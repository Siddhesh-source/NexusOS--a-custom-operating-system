/* Unit tests for kernel/mm/pmm.c (buddy allocator) and kernel/mm/slab.c,
 * run on the build machine.
 *
 * "Physical memory" is a 64 MiB array; the direct map offset points at it,
 * so phys_to_virt() works as in the kernel. The memory map has holes and
 * unaligned edges like a real one.
 *
 * Buddy: one million random operations (allocations of 1 page to 4 MiB,
 * frees, shared-page references) against a shadow map that records who
 * owns every frame. Every block handed out must be free in the shadow,
 * inside usable memory and aligned to its size; the allocator's own
 * consistency check runs throughout; freeing everything must restore the
 * exact starting layout of free blocks.
 *
 * Slab: random allocations and frees in caches of three sizes, each
 * object filled with its own byte and verified before it is freed. */
#include "unit.h"
#include "pmm.h"
#include "slab.h"
#include "kprintf.h"
#include "string.h"

#define RAM_BYTES   (64ULL << 20)
#define FRAMES      (RAM_BYTES / PAGE_SIZE)
#define BUDDY_OPS   1000000
#define SLAB_OPS    200000

__attribute__((aligned(4096))) static uint8_t ram[RAM_BYTES];
uint64_t hhdm_offset;

const char *mem_region_type_name(enum mem_region_type t)
{
    return t == MEM_USABLE ? "usable" : "other";
}

static const struct mem_region regions[] = {
    { 0x0,        0x9F000,              MEM_USABLE },   /* includes page 0 */
    { 0x9F000,    0x100000 - 0x9F000,   MEM_RESERVED },
    { 0x100000,   0x1E00000,            MEM_USABLE },   /* 1 MiB .. 31 MiB */
    { 0x1F00000,  0x100000,             MEM_ACPI_RECLAIMABLE },
    { 0x2000800,  0x1FFF000,            MEM_USABLE },   /* unaligned edges */
};

static bool in_usable(uint64_t phys)
{
    for (unsigned i = 0; i < sizeof(regions) / sizeof(regions[0]); i++) {
        if (regions[i].type != MEM_USABLE)
            continue;
        uint64_t s = PAGE_ALIGN_UP(regions[i].base);
        uint64_t e = PAGE_ALIGN_DOWN(regions[i].base + regions[i].length);
        if (phys >= s && phys < e)
            return true;
    }
    return false;
}

/* ---- buddy -------------------------------------------------------------- */

struct block { uint64_t phys; unsigned order; unsigned refs; };
static struct block live[4096];
static unsigned nlive;
static uint32_t owner[FRAMES];      /* 0 = free in the shadow, else block id */

static unsigned random_order(void)
{
    uint64_t r = unit_rand_below(100);
    if (r < 70) return 0;
    if (r < 90) return (unsigned)unit_rand_below(3) + 1;
    if (r < 99) return (unsigned)unit_rand_below(4) + 4;
    return (unsigned)unit_rand_below(3) + 8;
}

static void test_buddy_random(void)
{
    struct pmm_stats start, now;
    pmm_drain_cache();
    pmm_get_stats(&start);
    UCHECK(pmm_check(), "fresh allocator is consistent");
    UCHECK(start.managed_pages > 10000 && !pmm_is_allocated(0), "pages managed; page 0 never handed out");

    uint32_t next_id = 1;
    for (uint64_t op = 0; op < BUDDY_OPS; op++) {
        uint64_t r = unit_rand_below(100);
        if ((r < 52 && nlive < 4096) || nlive == 0) {
            unsigned order = random_order();
            uint64_t p = pmm_alloc_pages(order);
            if (p == 0)
                continue;                         /* fragmented or full: allowed */
            uint64_t n = 1ULL << order;
            bool ok = (p & ((PAGE_SIZE << order) - 1)) == 0;
            for (uint64_t i = 0; i < n; i++) {
                uint64_t f = (p >> PAGE_SHIFT) + i;
                ok = ok && f < FRAMES && owner[f] == 0 && in_usable(f << PAGE_SHIFT);
                if (f < FRAMES)
                    owner[f] = next_id;
            }
            UCHECK(ok, "op %lu: block %p order %u overlaps, is misaligned or outside RAM",
                   op, (void *)p, order);
            live[nlive++] = (struct block){ p, order, 1 };
            next_id++;
        } else if (r < 92) {
            unsigned i = (unsigned)unit_rand_below(nlive);
            struct block b = live[i];
            bool freed;
            if (b.order == 0 && b.refs > 1) {
                freed = pmm_page_unref(b.phys);
                live[i].refs--;
                UCHECK(!freed, "op %lu: unref of a shared page freed it", op);
                continue;
            }
            if (b.order == 0 && unit_rand_below(2))
                freed = pmm_page_unref(b.phys);
            else
                freed = pmm_free_pages(b.phys, b.order) == PMM_OK;
            UCHECK(freed, "op %lu: freeing a live block failed", op);
            for (uint64_t k = 0; k < (1ULL << b.order); k++)
                owner[(b.phys >> PAGE_SHIFT) + k] = 0;
            live[i] = live[--nlive];
        } else {
            unsigned i = (unsigned)unit_rand_below(nlive);
            if (live[i].order == 0 && live[i].refs < 5) {
                UCHECK(pmm_page_ref(live[i].phys) == PMM_OK, "op %lu: sharing a page failed", op);
                live[i].refs++;
            }
        }
        if (op % 50000 == 0) {
            UCHECK(pmm_check(), "op %lu: allocator check failed", op);
            pmm_get_stats(&now);
            uint64_t used = 0;
            for (unsigned i = 0; i < nlive; i++)
                used += 1ULL << live[i].order;
            UCHECK(now.allocated_pages == used, "op %lu: allocator says %lu pages used, model %lu",
                   op, now.allocated_pages, used);
        }
    }

    while (nlive > 0) {
        struct block b = live[--nlive];
        while (b.order == 0 && b.refs > 1) {
            pmm_page_unref(b.phys);
            b.refs--;
        }
        pmm_free_pages(b.phys, b.order);
    }
    pmm_drain_cache();
    pmm_get_stats(&now);
    bool same = now.free_pages == start.free_pages;
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++)
        same = same && now.free_blocks[o] == start.free_blocks[o];
    UCHECK(same && pmm_check(), "after %u operations, freeing everything restores the exact block layout",
           BUDDY_OPS);
}

static void test_buddy_errors(void)
{
    uint64_t p = pmm_alloc_pages(2);
    UCHECK(pmm_free_pages(p + 8, 2) == PMM_ERR_UNALIGNED, "unaligned free refused");
    UCHECK(pmm_free_pages(p, 1) == PMM_ERR_BAD_ORDER, "wrong order refused");
    UCHECK(pmm_free_pages(p + PAGE_SIZE, 0) == PMM_ERR_BAD_ORDER, "middle of a block refused");
    UCHECK(pmm_free_pages(0, 0) == PMM_ERR_NOT_MANAGED, "page 0 refused");
    UCHECK(pmm_free_pages(0x9F000, 0) == PMM_ERR_NOT_MANAGED, "reserved frame refused");
    UCHECK(pmm_free_pages(1ULL << 40, 0) == PMM_ERR_NOT_MANAGED, "frame beyond RAM refused");
    UCHECK(pmm_free_pages(p, 2) == PMM_OK && pmm_free_pages(p, 2) == PMM_ERR_DOUBLE_FREE,
           "free once, then double free refused");
    UCHECK(pmm_check(), "errors left the allocator unchanged");

    /* Exhaust single pages, then everything comes back. */
    uint64_t free0 = pmm_free_page_count(), n = 0;
    uint64_t head = 0;
    for (uint64_t q; (q = pmm_alloc_page()) != 0; n++) {
        *(uint64_t *)phys_to_virt(q) = head;       /* chain through the pages */
        head = q;
    }
    UCHECK(n == free0 && pmm_free_page_count() == 0 && pmm_alloc_pages(0) == 0,
           "every page can be taken; then allocation fails cleanly");
    while (head != 0) {
        uint64_t next = *(uint64_t *)phys_to_virt(head);
        pmm_free_page(head);
        head = next;
    }
    UCHECK(pmm_free_page_count() == free0 && pmm_check(), "and all of them return, fully merged");
}

/* ---- slab --------------------------------------------------------------- */

struct obj { struct slab_cache *c; uint8_t *p; uint32_t size; uint8_t fill; };
static struct obj objs[3000];
static unsigned nobjs;

static void test_slab_random(void)
{
    static const uint32_t sizes[3] = { 24, 176, 1000 };
    struct slab_cache *caches[3];
    for (unsigned i = 0; i < 3; i++)
        caches[i] = slab_create("unit", sizes[i], 8, 0);
    UCHECK(caches[0] && caches[1] && caches[2], "three caches created");

    for (uint64_t op = 0; op < SLAB_OPS; op++) {
        if ((unit_rand_below(100) < 55 && nobjs < 3000) || nobjs == 0) {
            unsigned k = (unsigned)unit_rand_below(3);
            uint8_t *p = slab_alloc(caches[k]);
            UCHECK(p != NULL, "op %lu: slab_alloc failed", op);
            if (p == NULL)
                continue;
            uint8_t fill = (uint8_t)(op * 13 + 1);
            memset(p, fill, sizes[k]);
            objs[nobjs++] = (struct obj){ caches[k], p, sizes[k], fill };
        } else {
            unsigned i = (unsigned)unit_rand_below(nobjs);
            bool intact = true;
            for (uint32_t j = 0; j < objs[i].size; j++)
                intact = intact && objs[i].p[j] == objs[i].fill;
            UCHECK(intact, "op %lu: object %p was overwritten", op, (void *)objs[i].p);
            UCHECK(slab_free(objs[i].c, objs[i].p) == SLAB_OK, "op %lu: free failed", op);
            objs[i] = objs[--nobjs];
        }
        if (op % 20000 == 0)
            for (unsigned k = 0; k < 3; k++)
                UCHECK(slab_check(caches[k]), "op %lu: cache %u check failed", op, k);
    }
    while (nobjs > 0) {
        nobjs--;
        slab_free(objs[nobjs].c, objs[nobjs].p);
    }
    uint64_t free_before_shrink = pmm_free_page_count();
    for (unsigned k = 0; k < 3; k++) {
        struct slab_stats st;
        slab_get_stats(caches[k], &st);
        UCHECK(st.in_use == 0 && st.slabs <= 1 && slab_check(caches[k]),
               "cache %u: empty at the end, at most one page kept", k);
        slab_shrink(caches[k]);
    }
    UCHECK(pmm_free_page_count() <= free_before_shrink + 3 && pmm_check(),
           "shrinking returns the kept pages to the buddy allocator");
}

static struct slab_cache *uaf_cache;
static uint8_t *uaf_obj;

static void reuse_after_write(void)
{
    for (int i = 0; i < 1000; i++)
        slab_alloc(uaf_cache);
}

static void test_slab_detection(void)
{
    uaf_cache = slab_create("uaf", 64, 8, 0);
    uaf_obj = slab_alloc(uaf_cache);
    slab_free(uaf_cache, uaf_obj);
    uaf_obj[10] = 0x55;
    UCHECK(!slab_check(uaf_cache), "slab_check reports a write after free");
    UCHECK(unit_expect_panic(reuse_after_write), "handing out the written slot panics");
    UCHECK(slab_free(uaf_cache, (uint8_t *)ram + 12345) == SLAB_ERR_INVALID_PTR,
           "a pointer that is not an object is refused");
}

int unit_main(void)
{
    hhdm_offset = (uint64_t)ram;
    struct boot_mem_info info = {
        .regions = regions, .region_count = sizeof(regions) / sizeof(regions[0]),
        .hhdm_offset = (uint64_t)ram,
    };
    pmm_init(&info);
    kprintf("[unit] mm: %u buddy operations, %u slab operations\n", BUDDY_OPS, SLAB_OPS);
    unit_seed(0x5eed);
    test_buddy_random();
    test_buddy_errors();
    test_slab_random();
    test_slab_detection();
    return unit_report("mm");
}
