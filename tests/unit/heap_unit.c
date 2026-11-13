/* Unit tests for kernel/mm/heap.c, run on the build machine.
 *
 * The main test is a random walk of thousands of allocations and frees
 * checked against a simple model: every live block keeps its own fill
 * byte, its alignment, and its red zone; the heap's own consistency check
 * passes throughout; freeing everything leaves one free block. */
#include "unit.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define MAX_LIVE   256
#define RANDOM_OPS 20000

struct live {
    uint8_t *p;
    size_t size;
    size_t align;
    uint8_t fill;
};

static struct live live[MAX_LIVE];
static unsigned nlive;

static bool filled(const uint8_t *p, size_t n, uint8_t v)
{
    for (size_t i = 0; i < n; i++)
        if (p[i] != v)
            return false;
    return true;
}

static bool all_live_intact(void)
{
    for (unsigned i = 0; i < nlive; i++)
        if (!filled(live[i].p, live[i].size, live[i].fill))
            return false;
    return true;
}

static size_t random_size(void)
{
    switch (unit_rand_below(10)) {
    case 0:  return 1 + unit_rand_below(64 * 1024);    /* occasionally big */
    case 1:  return 1 + unit_rand_below(16);           /* tiny */
    default: return 1 + unit_rand_below(2000);
    }
}

static size_t random_align(void)
{
    /* Mostly default alignment, sometimes 32 .. 4096. */
    return unit_rand_below(4) ? HEAP_MIN_ALIGN : (size_t)32 << unit_rand_below(8);
}

static void free_all_live(void)
{
    while (nlive > 0)
        kfree(live[--nlive].p);
}

static void test_random_walk(uint64_t seed)
{
    unit_seed(seed);
    heap_init();
    nlive = 0;

    for (unsigned op = 0; op < RANDOM_OPS; op++) {
        bool do_alloc = nlive == 0 || (nlive < MAX_LIVE && unit_rand_below(100) < 55);
        if (do_alloc) {
            size_t size = random_size(), align = random_align();
            uint8_t *p = kmalloc_aligned(size, align);
            UCHECK(p != NULL, "seed %lu op %u: kmalloc_aligned(%lu, %lu) failed",
                   seed, op, (uint64_t)size, (uint64_t)align);
            if (p == NULL)
                return;
            UCHECK(((uint64_t)p % align) == 0, "seed %lu op %u: bad alignment", seed, op);
            UCHECK(filled(p + size, HEAP_REDZONE, HEAP_POISON_REDZONE),
                   "seed %lu op %u: red zone missing", seed, op);
            uint8_t fill = (uint8_t)(op * 7 + 1);
            memset(p, fill, size);
            live[nlive++] = (struct live){ p, size, align, fill };
        } else {
            unsigned i = (unsigned)unit_rand_below(nlive);
            UCHECK(filled(live[i].p, live[i].size, live[i].fill),
                   "seed %lu op %u: block %p was overwritten", seed, op, (void *)live[i].p);
            UCHECK(kfree(live[i].p) == HEAP_OK, "seed %lu op %u: kfree failed", seed, op);
            live[i] = live[--nlive];
        }
        if (op % 97 == 0) {
            UCHECK(heap_check(), "seed %lu op %u: heap_check failed", seed, op);
            UCHECK(all_live_intact(), "seed %lu op %u: a live block changed", seed, op);
            struct heap_stats s;
            heap_get_stats(&s);
            UCHECK(s.block_count - s.free_block_count == nlive,
                   "seed %lu op %u: %lu used blocks, model has %u", seed, op,
                   s.block_count - s.free_block_count, nlive);
        }
    }
    free_all_live();
    struct heap_stats s;
    heap_get_stats(&s);
    UCHECK(heap_check() && s.block_count == 1 && s.used_bytes == 0,
           "seed %lu: freeing everything leaves one free block", seed);
}

static void test_bad_frees(void)
{
    heap_init();
    uint8_t *a = kmalloc(64), *b = kmalloc(64);
    int on_stack;
    UCHECK(kfree(b) == HEAP_OK, "free b");
    UCHECK(kfree(b) == HEAP_ERR_DOUBLE_FREE, "double free reported");
    UCHECK(kfree(&on_stack) == HEAP_ERR_INVALID_PTR, "stack pointer rejected");
    UCHECK(kfree(a + 16) == HEAP_ERR_INVALID_PTR, "interior pointer rejected");
    UCHECK(kfree(NULL) == HEAP_OK, "kfree(NULL) is a no-op");
    UCHECK(kfree(a) == HEAP_OK && heap_check(), "heap consistent after bad frees");
    UCHECK(kmalloc(0) == NULL, "kmalloc(0) is NULL");
    UCHECK(kmalloc_aligned(8, 48) == NULL, "non-power-of-two alignment rejected");
    UCHECK(kmalloc(KERNEL_HEAP_MAX) == NULL, "allocation as big as the heap fails");
    UCHECK(heap_check(), "heap consistent after failed allocations");
}

static void test_growth_failure(void)
{
    heap_init();
    struct heap_stats before;
    heap_get_stats(&before);
    uint64_t frames = unit_frames_in_use;

    unit_fail_frame_in = 3;   /* the 4th frame of the growth fails */
    void *p = kmalloc(256 * 1024);
    UCHECK(p == NULL, "allocation fails when the page allocator runs out mid-growth");
    UCHECK(unit_frames_in_use == frames, "growth is all-or-nothing: no frames kept (%lu vs %lu)",
           unit_frames_in_use, frames);
    struct heap_stats after;
    heap_get_stats(&after);
    UCHECK(after.mapped_bytes == before.mapped_bytes && heap_check(),
           "heap unchanged after failed growth");
    p = kmalloc(256 * 1024);
    UCHECK(p != NULL, "the same allocation succeeds once frames are available");
    kfree(p);
}

static uint8_t *victim;

static void use_after_free(void)
{
    victim[5] = 0x42;
    for (int i = 0; i < 10000; i++)
        if (kmalloc(48) == NULL)
            break;
}

static void overflow_then_free(void)
{
    victim[48] = 0x42;
    kfree(victim);
}

static void test_detection(void)
{
    heap_init();
    victim = kmalloc(48);
    kfree(victim);
    UCHECK(filled(victim, 48, HEAP_POISON_FREE), "freed block is poisoned");
    victim[5] = 0x42;
    UCHECK(!heap_check(), "heap_check reports a write after free");
    victim[5] = HEAP_POISON_FREE;
    UCHECK(heap_check(), "repaired heap passes");
    UCHECK(unit_expect_panic(use_after_free), "reusing written freed memory panics");

    heap_init();
    victim = kmalloc(48);
    UCHECK(unit_expect_panic(overflow_then_free), "kfree panics on a damaged red zone");

    heap_init();
    unit_panic_seen = false;
    victim = kmalloc(48);
    memset(victim, 1, 48);
    UCHECK(kfree(victim) == HEAP_OK && !unit_panic_seen,
           "writing exactly the requested size is not an overflow");
}

int unit_main(void)
{
    kprintf("[unit] heap: %u random operations per seed\n", RANDOM_OPS);
    static const uint64_t seeds[] = { 1, 2, 3, 0xbeef, 0x5eed1234 };
    for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
        test_random_walk(seeds[i]);
    test_bad_frees();
    test_growth_failure();
    test_detection();
    return unit_report("heap");
}
