#include "backtrace.h"
#include "kprintf.h"
#include "vmm.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

extern char __text_start[], __text_end[];
extern char stack_bottom[], stack_top[];   /* boot stack, entry.asm */

static bool is_kernel_text(uint64_t addr)
{
    return addr >= (uint64_t)__text_start && addr < (uint64_t)__text_end;
}

const struct ksym *ksym_lookup(uint64_t addr, uint64_t *offset)
{
    if (!is_kernel_text(addr) || ksym_count == 0 || addr < ksym_table[0].addr)
        return NULL;

    /* Binary search for the last symbol starting at or before addr. */
    uint64_t lo = 0, hi = ksym_count;
    while (hi - lo > 1) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (ksym_table[mid].addr <= addr)
            lo = mid;
        else
            hi = mid;
    }
    if (offset != NULL)
        *offset = addr - ksym_table[lo].addr;
    return &ksym_table[lo];
}

/* Can 16 bytes at `addr` (saved RBP + return address) be read safely? A
 * panic must never fault again, so check the page tables instead of just
 * trying. Before paging is ours, only the boot stack is trusted. */
static bool frame_readable(uint64_t addr)
{
    if (addr & 7)
        return false;
    if (addr >= (uint64_t)stack_bottom && addr + 16 <= (uint64_t)stack_top)
        return true;
    if (addr < KERNEL_HALF_BASE)
        return false;
    uint64_t phys;
    return vmm_translate_kernel(addr, &phys) == VMM_OK
        && vmm_translate_kernel(addr + 15, &phys) == VMM_OK;
}

unsigned backtrace_collect(uint64_t rbp, uint64_t *out, unsigned max)
{
    unsigned n = 0;
    while (n < max && frame_readable(rbp)) {
        const uint64_t *frame = (const uint64_t *)rbp;
        uint64_t next = frame[0], ret = frame[1];
        if (!is_kernel_text(ret))
            break;
        out[n++] = ret;
        /* Stacks grow down, so callers' frames are at higher addresses. A
         * frame that does not move up means the chain is broken. */
        if (next <= rbp)
            break;
        rbp = next;
    }
    return n;
}

static void print_line(unsigned idx, uint64_t addr, uint64_t lookup_addr)
{
    uint64_t off;
    const struct ksym *s = ksym_lookup(lookup_addr, &off);
    if (s != NULL)
        kprintf("  #%u %p  %s+0x%lx\n", idx, (void *)addr, s->name, addr - s->addr);
    else
        kprintf("  #%u %p  ?\n", idx, (void *)addr);
}

void backtrace_print_list(const uint64_t *rets, unsigned n)
{
    kprintf("Call trace (most recent call first):\n");
    for (unsigned i = 0; i < n; i++)
        print_line(i, rets[i], rets[i] - 1);
    if (n == 0)
        kprintf("  (no readable frames)\n");
}

void backtrace_print(uint64_t rip, uint64_t rbp)
{
    uint64_t rets[BACKTRACE_MAX_DEPTH];
    unsigned idx = 0;

    kprintf("Call trace (most recent call first):\n");
    if (rip != 0)
        print_line(idx++, rip, rip);
    unsigned n = backtrace_collect(rbp, rets, BACKTRACE_MAX_DEPTH);
    /* A return address points just past its call; look up addr-1 so a
     * call at the very end of a function is not blamed on the next one. */
    for (unsigned i = 0; i < n; i++)
        print_line(idx++, rets[i], rets[i] - 1);
    if (idx == 0)
        kprintf("  (no readable frames)\n");
}

__attribute__((noinline)) void backtrace_print_here(void)
{
    backtrace_print(0, (uint64_t)__builtin_frame_address(0));
}
