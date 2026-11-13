/* Shared support for unit-test programs: reporting, random numbers, and
 * fakes for the kernel pieces the tested code calls. */
#include "unit.h"
#include "kprintf.h"
#include "panic.h"
#include "serial.h"
#include "pmm.h"
#include "vmm.h"

unsigned unit_passed, unit_failed;
uint64_t unit_fake_tsc;
bool unit_panic_seen;

/* Heap backing store for heap.c (see unit_config.h). */
__attribute__((aligned(4096))) unsigned char unit_heap_area[8 << 20];

int unit_report(const char *suite)
{
    kprintf("[unit] %s: %u checks passed, %u failed\n", suite, unit_passed, unit_failed);
    return unit_failed == 0 ? 0 : 1;
}

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

void unit_seed(uint64_t seed)
{
    rng_state = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

uint64_t unit_rand(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545f4914f6cdd1dULL;
}

uint64_t unit_rand_below(uint64_t n)
{
    return n ? unit_rand() % n : 0;
}

/* ---- serial port: straight to the host's standard output --------------- */

void serial_write(const char *buf, uint64_t n)
{
    host_write(buf, (size_t)n);
}

/* ---- panic: either expected by a test (jump back) or a real failure ----- */

static void *panic_jump[5];
static bool panic_armed;

bool unit_expect_panic(void (*fn)(void))
{
    unit_panic_seen = false;
    panic_armed = true;
    if (__builtin_setjmp(panic_jump) == 0)
        fn();
    panic_armed = false;
    return unit_panic_seen;
}

void panic(const char *msg)
{
    unit_panic_seen = true;
    if (panic_armed)
        __builtin_longjmp(panic_jump, 1);
    kprintf("[unit] unexpected panic: %s\n", msg);
    host_exit(2);
}

/* ---- page allocator and page tables ------------------------------------
 * The heap's memory is already there (unit_heap_area), so mapping only
 * has to succeed or fail. Tests can make the Nth next frame allocation
 * fail to check the heap's all-or-nothing growth. */

uint64_t unit_frames_in_use;
int64_t unit_fail_frame_in = -1;   /* <0: never; 0: next allocation fails */
static address_space_t fake_space;

uint64_t pmm_alloc_page(void)
{
    if (unit_fail_frame_in == 0) {
        unit_fail_frame_in = -1;
        return 0;
    }
    if (unit_fail_frame_in > 0)
        unit_fail_frame_in--;
    unit_frames_in_use++;
    return 0x100000 + unit_frames_in_use * 0x1000;
}

pmm_status_t pmm_free_page(uint64_t phys)
{
    (void)phys;
    unit_frames_in_use--;
    return PMM_OK;
}

address_space_t *vmm_kernel_space(void)
{
    return &fake_space;
}

vmm_status_t vmm_map_page(address_space_t *as, uint64_t virt, uint64_t phys, uint32_t flags)
{
    (void)as; (void)virt; (void)phys; (void)flags;
    return VMM_OK;
}

vmm_status_t vmm_unmap_page(address_space_t *as, uint64_t virt, uint64_t *old_phys)
{
    (void)as;
    *old_phys = 0x100000 + virt % 0x1000;
    return VMM_OK;
}
