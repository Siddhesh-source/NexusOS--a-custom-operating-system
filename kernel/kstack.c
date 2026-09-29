#include "kstack.h"
#include "vmm.h"
#include "pmm.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"

#define KSTACK_PAGES (KSTACK_SIZE / PAGE_SIZE)

_Static_assert(KSTACK_SIZE % PAGE_SIZE == 0, "stack size must be whole pages");
_Static_assert(KSTACK_SLOT_SIZE > KSTACK_SIZE, "slot must leave room for a guard");

static bool slot_used[KSTACK_MAX_SLOTS];
static unsigned slots_in_use;

static inline uint64_t slot_top(int slot)
{
    return KSTACK_REGION_BASE + (uint64_t)(slot + 1) * KSTACK_SLOT_SIZE;
}

void kstack_init(void)
{
    /* Build all page tables for the region now, so stack allocation only
     * ever needs data frames and a failure can't leave tables half-built. */
    vmm_status_t st = vmm_reserve_tables(vmm_kernel_space(), KSTACK_REGION_BASE,
                                         (uint64_t)KSTACK_MAX_SLOTS * KSTACK_SLOT_SIZE);
    if (st != VMM_OK) {
        kprintf("[kstack] cannot reserve page tables: %s\n", vmm_status_str(st));
        panic("kstack: init failed");
    }
    kprintf("[kstack] %d slots of %d KiB (%d KiB stack + guard) at %p\n",
            KSTACK_MAX_SLOTS, KSTACK_SLOT_SIZE / 1024, KSTACK_SIZE / 1024,
            (void *)KSTACK_REGION_BASE);
}

kstack_status_t kstack_alloc(struct kstack *out)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    int slot = -1;
    for (int i = 0; i < KSTACK_MAX_SLOTS; i++) {
        if (!slot_used[i]) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return KSTACK_ERR_NO_SLOT;

    address_space_t *ks = vmm_kernel_space();
    uint64_t base = slot_top(slot) - KSTACK_SIZE;

    for (uint64_t i = 0; i < KSTACK_PAGES; i++) {
        uint64_t pa = pmm_alloc_page();
        if (pa == 0 || vmm_map_page(ks, base + i * PAGE_SIZE, pa, VMM_WRITE | VMM_GLOBAL) != VMM_OK) {
            if (pa != 0)
                pmm_free_page(pa);
            while (i-- > 0) {
                uint64_t old;
                if (vmm_unmap_page(ks, base + i * PAGE_SIZE, &old) == VMM_OK)
                    pmm_free_page(old);
            }
            return KSTACK_ERR_NO_MEMORY;
        }
    }

    memset((void *)base, 0, KSTACK_SIZE);
    *(uint64_t *)base = KSTACK_CANARY;

    slot_used[slot] = true;
    slots_in_use++;
    out->base = base;
    out->top = slot_top(slot);
    out->slot = slot;
    return KSTACK_OK;
}

kstack_status_t kstack_free(struct kstack *s)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    if (s == NULL || s->slot < 0 || s->slot >= KSTACK_MAX_SLOTS || !slot_used[s->slot]
        || s->top != slot_top(s->slot) || s->base != s->top - KSTACK_SIZE)
        return KSTACK_ERR_INVALID;

    /* Freeing the stack we are running on would pull the floor out from
     * under ourselves. */
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    if (rsp >= s->base && rsp < s->top)
        panic("kstack: attempt to free the active stack");

    address_space_t *ks = vmm_kernel_space();
    for (uint64_t i = 0; i < KSTACK_PAGES; i++) {
        uint64_t old;
        if (vmm_unmap_page(ks, s->base + i * PAGE_SIZE, &old) != VMM_OK)
            panic("kstack: stack page unexpectedly unmapped");
        pmm_free_page(old);
    }

    slot_used[s->slot] = false;
    slots_in_use--;
    s->slot = -1;
    s->base = s->top = 0;
    return KSTACK_OK;
}

bool kstack_canary_intact(const struct kstack *s)
{
    return *(const volatile uint64_t *)s->base == KSTACK_CANARY;
}

uint64_t kstack_high_water(const struct kstack *s)
{
    for (uint64_t p = s->base + 8; p < s->top; p += 8) {
        if (*(const volatile uint64_t *)p != 0)
            return s->top - p;
    }
    return 0;
}

unsigned kstack_slots_in_use(void)
{
    return slots_in_use;
}

const char *kstack_status_str(kstack_status_t s)
{
    switch (s) {
    case KSTACK_OK:            return "ok";
    case KSTACK_ERR_NO_SLOT:   return "no free stack slot";
    case KSTACK_ERR_NO_MEMORY: return "out of physical memory";
    case KSTACK_ERR_INVALID:   return "invalid stack";
    }
    return "unknown";
}
