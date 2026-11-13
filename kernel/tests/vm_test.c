#include "vm_test.h"
#include "pmm.h"
#include "slab.h"
#include "vm.h"
#include "vmm.h"
#include "heap.h"
#include "pagefault.h"
#include "proc.h"
#include "cpu.h"
#include "kprintf.h"
#include "string.h"

static unsigned passed, failed;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) {                                         \
            passed++;                                       \
            kprintf("  [PASS] %s\n", msg);                  \
        } else {                                            \
            failed++;                                       \
            kprintf("  [FAIL] %s\n", msg);                  \
        }                                                   \
    } while (0)

static bool same_blocks(const struct pmm_stats *a, const struct pmm_stats *b)
{
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++)
        if (a->free_blocks[o] != b->free_blocks[o])
            return false;
    return a->free_pages == b->free_pages;
}

/* ---- buddy allocator ---------------------------------------------------- */

static void test_buddy(void)
{
    kprintf("\n[vm] Buddy page allocator\n");
    struct pmm_stats s0, s1;
    pmm_drain_cache();          /* compare fully merged layouts below */
    CHECK(pmm_check(), "free lists are consistent and fully merged");
    pmm_get_stats(&s0);

    bool aligned = true, marked = true;
    for (unsigned o = 0; o <= 5; o++) {
        uint64_t p = pmm_alloc_pages(o);
        aligned = aligned && p != 0 && (p & ((PAGE_SIZE << o) - 1)) == 0;
        for (uint64_t i = 0; p != 0 && i < (1u << o); i++)
            marked = marked && pmm_is_allocated(p + i * PAGE_SIZE);
        if (p != 0)
            pmm_free_pages(p, o);
    }
    CHECK(aligned, "a block of 2^n pages is aligned to its own size (n = 0..5)");
    CHECK(marked, "every page of an allocated block is marked allocated");

    uint64_t a = pmm_alloc_page();
    pmm_get_stats(&s1);
    CHECK(a != 0 && s1.free_pages == s0.free_pages - 1 && !same_blocks(&s0, &s1),
          "a single page is split off a larger free block");
    pmm_free_page(a);
    uint64_t again = pmm_alloc_page();
    CHECK(again == a, "a page freed a moment ago is reused at once (hot-page cache)");
    pmm_free_page(again);
    pmm_drain_cache();
    pmm_get_stats(&s1);
    CHECK(same_blocks(&s0, &s1) && pmm_check(), "draining the cache merges the buddies back exactly");

    uint64_t b = pmm_alloc_pages(1);
    CHECK(b != 0 && pmm_free_page(b) == PMM_ERR_BAD_ORDER, "freeing a 2-page block as 1 page is refused");
    CHECK(pmm_free_pages(b + PAGE_SIZE, 0) == PMM_ERR_BAD_ORDER, "freeing the middle of a block is refused");
    CHECK(pmm_free_pages(b, 1) == PMM_OK && pmm_free_pages(b, 1) == PMM_ERR_DOUBLE_FREE,
          "freeing the block works once, then is a double free");
    CHECK(pmm_alloc_pages(PMM_MAX_ORDER + 1) == 0, "an order above the maximum is refused");

    uint64_t big = pmm_alloc_pages(PMM_MAX_ORDER);
    CHECK(big != 0 && (big & ((PAGE_SIZE << PMM_MAX_ORDER) - 1)) == 0, "a 4 MiB block can be allocated");
    pmm_free_pages(big, PMM_MAX_ORDER);

    /* Take every 4 MiB block; smaller blocks must still be available. */
    static uint64_t bigs[512];
    unsigned nb = 0;
    while (nb < 512 && (bigs[nb] = pmm_alloc_pages(PMM_MAX_ORDER)) != 0)
        nb++;
    uint64_t small = pmm_alloc_page();
    CHECK(nb > 0 && small != 0, "with every 4 MiB block taken, single pages still come from smaller blocks");
    if (small != 0)
        pmm_free_page(small);
    while (nb > 0)
        pmm_free_pages(bigs[--nb], PMM_MAX_ORDER);
    pmm_drain_cache();
    pmm_get_stats(&s1);
    CHECK(s1.free_pages == s0.free_pages && pmm_check(), "everything returns and merges again");

    uint64_t r = pmm_alloc_page();
    CHECK(pmm_page_refcount(r) == 1 && pmm_page_ref(r) == PMM_OK && pmm_page_refcount(r) == 2,
          "a page starts with one user; sharing adds one");
    CHECK(!pmm_page_unref(r) && pmm_is_allocated(r), "dropping one of two users keeps the page");
    CHECK(pmm_page_unref(r) && !pmm_is_allocated(r), "dropping the last user frees it");
    CHECK(pmm_page_ref(r) != PMM_OK && pmm_page_refcount(r) == 0, "a free page cannot be shared");
}

/* ---- slab caches -------------------------------------------------------- */

static void test_slab(void)
{
    kprintf("\n[vm] Slab caches\n");
    CHECK(slab_create("bad", 0, 16, 0) == NULL && slab_create("bad", SLAB_MAX_OBJECT + 1, 16, 0) == NULL
          && slab_create("bad", 32, 24, 0) == NULL, "bad sizes and alignments are refused");

    struct slab_cache *c = slab_create("test48", 48, 16, 0);
    struct slab_stats st;
    slab_get_stats(c, &st);
    CHECK(c != NULL && st.object_size == 48 && st.per_slab > 50, "a 48-byte cache packs many objects per page");

    enum { N = 200 };
    static uint8_t *objs[N];
    bool ok = true;
    for (unsigned i = 0; i < N; i++) {
        objs[i] = slab_alloc(c);
        ok = ok && objs[i] != NULL && ((uint64_t)objs[i] & 15) == 0;
        if (objs[i] != NULL)
            memset(objs[i], (int)i, 48);
    }
    slab_get_stats(c, &st);
    CHECK(ok && st.in_use == N && st.slabs >= 3, "200 objects, 16-byte aligned, spread over several slabs");
    for (unsigned i = 0; ok && i < N; i++)
        for (unsigned j = 0; j < 48; j++)
            ok = ok && objs[i][j] == (uint8_t)i;
    CHECK(ok, "objects do not overlap (each keeps its own fill)");

    for (unsigned i = 0; i < N; i += 2)
        slab_free(c, objs[i]);
    CHECK(slab_check(c), "after freeing half, lists and poison are consistent");
    uint8_t *again = slab_alloc(c);
    bool reused = false;
    for (unsigned i = 0; i < N; i += 2)
        reused = reused || again == objs[i];
    CHECK(reused, "a freed slot is reused before a new page is taken");
    slab_free(c, again);

    CHECK(slab_free(c, objs[0]) == SLAB_ERR_DOUBLE_FREE, "double free is refused");
    void *heap_obj = kmalloc(48);
    CHECK(slab_free(c, heap_obj) == SLAB_ERR_INVALID_PTR, "a heap pointer is refused");
    kfree(heap_obj);
    CHECK(slab_free(c, objs[1] + 8) == SLAB_ERR_INVALID_PTR, "a pointer into the middle of an object is refused");

    uint8_t saved = objs[2][5];
    objs[2][5] = 0x77;                      /* write after free */
    kprintf("  (a use-after-free report is expected next)\n");
    CHECK(!slab_check(c), "a write into a freed object is caught");
    objs[2][5] = saved;
    CHECK(slab_check(c), "consistent again once repaired");

    for (unsigned i = 1; i < N; i += 2)
        slab_free(c, objs[i]);
    slab_get_stats(c, &st);
    CHECK(st.in_use == 0 && st.slabs <= 1, "with all objects freed, at most one empty page is kept");
    slab_shrink(c);
    slab_get_stats(c, &st);
    CHECK(st.slabs == 0, "shrinking returns it to the page allocator");

    struct slab_cache *lim = slab_create("limit5", 64, 16, 5);
    void *l[6];
    for (unsigned i = 0; i < 6; i++)
        l[i] = slab_alloc(lim);
    slab_get_stats(lim, &st);
    CHECK(l[4] != NULL && l[5] == NULL && st.failures == 1, "a cache limited to 5 objects refuses the 6th");
    slab_free(lim, l[0]);
    l[0] = slab_alloc(lim);
    CHECK(l[0] != NULL, "after one is freed, the next allocation succeeds");
    for (unsigned i = 0; i < 5; i++)
        slab_free(lim, l[i]);

    CHECK(proc_cache_pages() > 0, "threads and processes now live in their own slab caches");
}

/* ---- demand paging ------------------------------------------------------ */

#define UBASE 0x400000ULL

static uint64_t peek(uint64_t va)
{
    return *(volatile uint64_t *)va;
}

static void poke(uint64_t va, uint64_t v)
{
    *(volatile uint64_t *)va = v;
}

static void test_demand_paging(void)
{
    kprintf("\n[vm] Demand paging\n");
    IRQ_GUARD();                            /* we switch page tables below */
    address_space_t as;
    struct vm_stats v0, v1;
    uint64_t free0 = pmm_free_page_count();
    if (vmm_create_address_space(&as) != VMM_OK) {
        CHECK(false, "create an address space");
        return;
    }
    vmm_switch(&as);
    vm_get_stats(&v0);

    CHECK(vm_reserve(&as, UBASE, 16, VM_WRITE) == VM_OK, "reserve 16 pages at 0x400000");
    CHECK(vmm_translate(&as, UBASE, NULL) == VMM_ERR_NOT_MAPPED, "nothing is mapped yet");
    vm_get_stats(&v1);
    CHECK(v1.pages_committed == v0.pages_committed, "no memory is used yet");

    uint64_t first = peek(UBASE + 8);
    poke(UBASE + 3 * PAGE_SIZE, 0x1234);
    vm_get_stats(&v1);
    CHECK(first == 0 && peek(UBASE + 3 * PAGE_SIZE) == 0x1234, "first touch gives a zero-filled page that works");
    CHECK(v1.demand_faults - v0.demand_faults == 2 && v1.pages_committed - v0.pages_committed == 2,
          "two touched pages cost exactly two page faults and two frames");
    CHECK(vmm_translate(&as, UBASE + 5 * PAGE_SIZE, NULL) == VMM_ERR_NOT_MAPPED,
          "untouched pages stay unmapped");

    uint64_t v;
    kprintf("  (two page-fault reports are expected next)\n");
    CHECK(mm_probe_read(UBASE + 16 * PAGE_SIZE, &v) == 1, "an access just past the region still faults");
    CHECK(vm_reserve(&as, 0x800000, 2, VM_READ) == VM_OK && mm_probe_read(0x800000, &v) == 0
          && mm_probe_write(0x800000, 1) == 1, "a read-only region can be read but not written");

    CHECK(vm_reserve(&as, UBASE + PAGE_SIZE, 1, VM_WRITE) == VM_ERR_OVERLAP, "overlapping regions are refused");
    CHECK(vm_reserve(&as, UBASE + 100, 1, VM_WRITE) == VM_ERR_INVALID
          && vm_reserve(&as, 0xffff900000000000ULL, 1, VM_WRITE) == VM_ERR_INVALID
          && vm_reserve(vmm_kernel_space(), UBASE, 1, VM_WRITE) == VM_ERR_INVALID,
          "unaligned, kernel-half or kernel-space regions are refused");

    CHECK(vm_unmap(&as, UBASE) == VM_OK && vm_unmap(&as, UBASE) == VM_ERR_NOT_FOUND,
          "a region can be removed once");
    CHECK(vmm_translate(&as, UBASE + 3 * PAGE_SIZE, NULL) == VMM_ERR_NOT_MAPPED, "its pages are unmapped");
    vm_space_release(&as);
    vmm_switch(vmm_kernel_space());
    vmm_destroy_address_space(&as);
    vm_get_stats(&v1);
    CHECK(v1.pages_committed == v0.pages_committed && v1.regions == v0.regions,
          "all frames and regions are released");
    heap_check();
    CHECK(pmm_free_page_count() + 2 >= free0, "no page frames leaked (bookkeeping pages aside)");
}

/* ---- copy-on-write and shared objects ----------------------------------- */

static void test_cow(void)
{
    kprintf("\n[vm] Copy-on-write and shared memory objects\n");
    IRQ_GUARD();
    address_space_t a, b;
    struct vm_stats v0, v1;
    if (vmm_create_address_space(&a) != VMM_OK || vmm_create_address_space(&b) != VMM_OK) {
        CHECK(false, "create two address spaces");
        return;
    }
    vm_get_stats(&v0);

    struct vm_object *o = vm_object_create(4);
    CHECK(o != NULL && vm_map(&a, UBASE, o, 0, 4, VM_WRITE) == VM_OK, "an object of 4 pages, mapped in space A");
    vmm_switch(&a);
    poke(UBASE, 0xAAAA0000);
    poke(UBASE + PAGE_SIZE, 0xAAAA0001);

    struct vm_object *c = vm_object_clone_cow(o);
    uint64_t f0 = vm_object_frame(o, 0);
    CHECK(c != NULL && vm_object_frame(c, 0) == f0 && pmm_page_refcount(f0) == 2,
          "the clone shares the same frames (2 users each)");
    CHECK(vm_map(&b, UBASE, c, 0, 4, VM_WRITE) == VM_OK, "the clone is mapped in space B");
    CHECK(vm_map(&b, 0x900000, o, 0, 4, VM_WRITE) == VM_OK, "the original is also mapped (shared) in B at 0x900000");

    vmm_switch(&b);
    CHECK(peek(UBASE) == 0xAAAA0000 && peek(UBASE + PAGE_SIZE) == 0xAAAA0001,
          "B sees A's data through the clone");
    vm_get_stats(&v1);
    CHECK(v1.cow_copies == v0.cow_copies, "reading copies nothing");

    poke(UBASE, 0xBBBB0000);                /* write to the clone in B */
    vm_get_stats(&v1);
    CHECK(v1.cow_copies - v0.cow_copies == 1 && vm_object_frame(c, 0) != f0,
          "B's first write copies that one page");
    CHECK(pmm_page_refcount(f0) == 1, "the original frame has one user again");
    poke(UBASE + 2 * PAGE_SIZE, 7);         /* never-touched page in the clone */
    vm_get_stats(&v1);
    CHECK(v1.cow_copies - v0.cow_copies == 1, "an untouched page is demand-filled, not copied");

    vmm_switch(&a);
    CHECK(peek(UBASE) == 0xAAAA0000, "A still sees its own data");
    poke(UBASE, 0xAAAA1111);                /* last user: no copy needed */
    vm_get_stats(&v1);
    CHECK(v1.cow_reuses - v0.cow_reuses == 1 && v1.cow_copies - v0.cow_copies == 1,
          "A's write to a page it no longer shares reuses it without copying");
    poke(UBASE + PAGE_SIZE, 0xAAAA2222);    /* still shared with the clone: copy */
    vmm_switch(&b);
    CHECK(peek(0x900000) == 0xAAAA1111 && peek(0x900000 + PAGE_SIZE) == 0xAAAA2222,
          "B's shared view of the original sees both of A's writes");
    CHECK(peek(UBASE) == 0xBBBB0000 && peek(UBASE + PAGE_SIZE) == 0xAAAA0001,
          "while B's clone keeps its own values");

    vmm_switch(vmm_kernel_space());
    vm_space_release(&a);
    vm_space_release(&b);
    vm_object_release(o);
    vm_object_release(c);
    vmm_destroy_address_space(&a);
    vmm_destroy_address_space(&b);
    vm_get_stats(&v1);
    CHECK(v1.objects == v0.objects && v1.regions == v0.regions && v1.pages_committed == v0.pages_committed,
          "releasing everything frees every object, region and frame");
    CHECK(pmm_check(), "the page allocator is consistent afterwards");
}

bool vm_run_tests(void)
{
    passed = failed = 0;
    kprintf("\n===== Memory that scales: tests =====\n");
    test_buddy();
    test_slab();
    test_demand_paging();
    test_cow();
    kprintf("\n");
    slab_print_all();
    kprintf("\n===== Memory tests: %u passed, %u failed =====\n", passed, failed);
    return failed == 0;
}
