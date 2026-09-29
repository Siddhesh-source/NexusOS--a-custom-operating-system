#include "mm_test.h"
#include "mm.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "pagefault.h"
#include "cpu.h"
#include "kprintf.h"
#include "string.h"

extern char __text_start[], __rodata_start[], __data_start[];
extern char stack_bottom[];

static unsigned passed, failed;

/* A `ret` instruction placed in writable, non-executable .data. */
static volatile uint8_t data_ret_insn[16] = { 0xC3 };
static const char rodata_string[] = "NEXUS read-only data";

#define CHECK(cond, desc) check((cond), (desc))

static void check(bool ok, const char *desc)
{
    if (ok) {
        passed++;
        kprintf("[test]   PASS  %s\n", desc);
    } else {
        failed++;
        kprintf("[test]   FAIL  %s\n", desc);
    }
}

static void section(const char *name)
{
    kprintf("\n[test] --- %s ---\n", name);
}

/* True if the most recent page fault matches the expectation. */
static bool pf_matches(uint64_t addr, bool present, bool write, bool ifetch)
{
    return pf_last.address == addr && pf_last.present == present
        && pf_last.write == write && pf_last.instruction_fetch == ifetch
        && !pf_last.user && pf_last.recovered;
}

/* ---- physical memory ---------------------------------------------------- */

static void test_pmm_basic(void)
{
    section("PMM: allocate / release");
    address_space_t *ks = vmm_kernel_space();
    uint64_t free0 = pmm_free_page_count();

    uint64_t a = pmm_alloc_page();
    uint64_t b = pmm_alloc_page();
    CHECK(a != 0 && b != 0, "allocate two physical pages");
    CHECK(IS_PAGE_ALIGNED(a) && IS_PAGE_ALIGNED(b), "frames are 4 KiB aligned");
    CHECK(a != b, "frames are distinct");
    CHECK(pmm_is_allocated(a) && pmm_is_allocated(b), "frames marked allocated");
    CHECK(pmm_free_page_count() == free0 - 2, "free count dropped by 2");

    volatile uint64_t *pa = phys_to_virt(a);
    *pa = 0xA5A5A5A5DEADBEEFULL;
    CHECK(*pa == 0xA5A5A5A5DEADBEEFULL, "frame usable through the HHDM");

    CHECK(pmm_free_page(a) == PMM_OK, "release a page");
    CHECK(!pmm_is_allocated(a), "released frame marked free");
    CHECK(pmm_free_page(a) == PMM_ERR_DOUBLE_FREE, "double free detected");
    CHECK(pmm_free_page(b + 8) == PMM_ERR_UNALIGNED, "unaligned free rejected");
    CHECK(pmm_free_page(0) == PMM_ERR_NOT_MANAGED, "free of physical page 0 rejected");

    uint64_t kphys = 0;
    vmm_translate(ks, (uint64_t)__text_start, &kphys);
    CHECK(pmm_free_page(kphys) == PMM_ERR_NOT_MANAGED, "free of kernel-image frame rejected");
    CHECK(pmm_free_page(1ULL << 45) == PMM_ERR_NOT_MANAGED, "free beyond physical memory rejected");

    CHECK(pmm_free_page(b) == PMM_OK, "release second page");
    CHECK(pmm_free_page_count() == free0, "free count restored after rejected frees");

    uint64_t z = pmm_alloc_zeroed_page();
    bool zero = z != 0;
    for (uint64_t i = 0; zero && i < PAGE_SIZE / 8; i++)
        zero = ((uint64_t *)phys_to_virt(z))[i] == 0;
    CHECK(zero, "zeroed allocation is all zero");
    pmm_free_page(z);
}

/* Pops one page from a chain threaded through the pages themselves. */
static uint64_t chain_pop(uint64_t *head)
{
    uint64_t p = *head;
    if (p != 0)
        *head = *(uint64_t *)phys_to_virt(p);
    return p;
}

static void test_pmm_exhaustion(void)
{
    section("PMM: exhaustion and out-of-memory handling");
    address_space_t *ks = vmm_kernel_space();
    uint64_t free0 = pmm_free_page_count();

    /* Grab every free frame, linking each into a list stored in the frame
     * itself so no extra memory is needed to remember them. */
    uint64_t head = 0, count = 0, p;
    while ((p = pmm_alloc_page()) != 0) {
        *(uint64_t *)phys_to_virt(p) = head;
        head = p;
        count++;
    }
    kprintf("[test]   allocated %lu pages (%lu KiB) before exhaustion\n",
            count, count * PAGE_SIZE / 1024);
    CHECK(count == free0, "every free page could be allocated");
    CHECK(pmm_free_page_count() == 0, "free count is zero when exhausted");
    CHECK(pmm_alloc_page() == 0, "allocation fails cleanly when exhausted");
    CHECK(pmm_alloc_zeroed_page() == 0, "zeroed allocation fails cleanly when exhausted");

    /* Consumers must degrade gracefully too. MM_TEST_VBASE + 1 GiB has no
     * page directory yet, so mapping it needs a new page-table page. */
    CHECK(vmm_map_page(ks, MM_TEST_VBASE + (1ULL << 30), 0x1000, VMM_WRITE) == VMM_ERR_NO_MEMORY,
          "vmm_map_page reports out-of-memory for page tables");
    address_space_t as;
    CHECK(vmm_create_address_space(&as) == VMM_ERR_NO_MEMORY,
          "address-space creation reports out-of-memory");

    struct heap_stats hs;
    heap_get_stats(&hs);
    CHECK(kmalloc(hs.largest_free + 32 * 1024) == NULL, "kmalloc returns NULL when heap cannot grow");

    /* With a few frames free, a heap growth that needs more must roll back
     * the frames it did get. (Growth stays inside the heap's existing page
     * table, so no page-table pages are involved.) */
    for (int i = 0; i < 3; i++)
        pmm_free_page(chain_pop(&head));
    CHECK(kmalloc(hs.largest_free + 32 * 1024) == NULL, "partial heap growth fails...");
    CHECK(pmm_free_page_count() == 3, "...and returns every frame it took");

    while ((p = chain_pop(&head)) != 0)
        pmm_free_page(p);
    CHECK(pmm_free_page_count() == free0, "all pages released, free count restored");
}

/* ---- virtual memory ----------------------------------------------------- */

static void test_vmm_mapping(void)
{
    section("VMM: map / translate / protect / unmap");
    address_space_t *ks = vmm_kernel_space();
    uint64_t va = MM_TEST_VBASE;
    uint64_t pa = pmm_alloc_page();
    uint64_t pa2 = pmm_alloc_page();
    uint64_t t = 0, v = 0;
    uint32_t flags = 0;

    CHECK(vmm_map_page(ks, va, pa, VMM_WRITE) == VMM_OK, "map a virtual page");
    CHECK(vmm_translate(ks, va, &t) == VMM_OK && t == pa, "translate returns the mapped frame");
    CHECK(vmm_translate(ks, va + 0x123, &t) == VMM_OK && t == pa + 0x123,
          "translate preserves the page offset");

    *(volatile uint64_t *)va = 0x0123456789ABCDEFULL;
    CHECK(*(volatile uint64_t *)phys_to_virt(pa) == 0x0123456789ABCDEFULL,
          "write through mapping is visible through the HHDM alias");

    CHECK(vmm_get_flags(ks, va, &flags) == VMM_OK
          && (flags & VMM_WRITE) && !(flags & VMM_EXEC) && !(flags & VMM_USER),
          "mapping flags read back as RW, NX, supervisor");

    section("VMM: invalid mapping requests");
    CHECK(vmm_map_page(ks, va, pa2, VMM_WRITE) == VMM_ERR_ALREADY_MAPPED, "remap of mapped page rejected");
    CHECK(vmm_map_page(ks, va + 0x10, pa2, VMM_WRITE) == VMM_ERR_UNALIGNED, "unaligned virtual address rejected");
    CHECK(vmm_map_page(ks, va + PAGE_SIZE, pa2 + 1, VMM_WRITE) == VMM_ERR_UNALIGNED, "unaligned physical address rejected");
    CHECK(vmm_map_page(ks, 0x0000800000000000ULL, pa2, VMM_WRITE) == VMM_ERR_NONCANONICAL, "non-canonical address rejected");
    CHECK(vmm_map_page(ks, va + PAGE_SIZE, 1ULL << 52, VMM_WRITE) == VMM_ERR_INVALID, "physical address beyond 52 bits rejected");
    CHECK(vmm_map_page(ks, va + PAGE_SIZE, pa2, VMM_USER) == VMM_ERR_INVALID, "user mapping in kernel half rejected");
    CHECK(vmm_map_page(ks, va + PAGE_SIZE, pa2, 1u << 20) == VMM_ERR_INVALID, "unknown flag rejected");
    CHECK(vmm_translate(ks, 0x0000800000000000ULL, &t) == VMM_ERR_NONCANONICAL, "translate of non-canonical address rejected");
    CHECK(vmm_translate(ks, va + PAGE_SIZE, &t) == VMM_ERR_NOT_MAPPED, "translate of unmapped page fails");

    section("VMM: permission changes");
    CHECK(vmm_protect_page(ks, va, 0) == VMM_OK, "change page to read-only");
    CHECK(mm_probe_write(va, 1) == 1 && pf_matches(va, true, true, false),
          "write to read-only page faults (present, write)");
    CHECK(mm_probe_read(va, &v) == 0 && v == 0x0123456789ABCDEFULL, "read-only page still readable");
    CHECK(vmm_protect_page(ks, va, VMM_WRITE) == VMM_OK && mm_probe_write(va, 2) == 0,
          "restoring write permission allows writes");

    *(volatile uint8_t *)va = 0xC3;   /* ret */
    CHECK(mm_probe_exec(va) == 1 && pf_matches(va, true, false, true),
          "executing an NX page faults (instruction fetch)");
    CHECK(vmm_protect_page(ks, va, VMM_EXEC) == VMM_OK && mm_probe_exec(va) == 0,
          "granting execute permission allows execution");
    CHECK(vmm_protect_page(ks, va, VMM_WRITE) == VMM_OK, "revoke execute permission");

    section("VMM: unmap");
    uint64_t old = 0;
    CHECK(vmm_unmap_page(ks, va, &old) == VMM_OK && old == pa, "unmap returns the old frame");
    CHECK(vmm_translate(ks, va, &t) == VMM_ERR_NOT_MAPPED, "unmapped page no longer translates");
    CHECK(mm_probe_read(va, &v) == 1 && pf_matches(va, false, false, false),
          "access after unmap faults (TLB entry was flushed)");
    CHECK(vmm_unmap_page(ks, va, NULL) == VMM_ERR_NOT_MAPPED, "double unmap rejected");
    CHECK(vmm_protect_page(ks, va, VMM_WRITE) == VMM_ERR_NOT_MAPPED, "protect of unmapped page rejected");

    *(volatile uint64_t *)phys_to_virt(pa2) = 0xFEEDFACECAFEBEEFULL;
    CHECK(vmm_map_page(ks, va, pa2, VMM_WRITE) == VMM_OK
          && *(volatile uint64_t *)va == 0xFEEDFACECAFEBEEFULL,
          "remapping to a new frame shows the new contents");
    vmm_unmap_page(ks, va, NULL);

    section("VMM: range mapping across a page-table boundary");
    uint64_t rva = MM_TEST_VBASE + (2ULL << 20) - PAGE_SIZE;   /* straddles 2 MiB */
    uint64_t frames[3];
    for (int i = 0; i < 3; i++)
        frames[i] = pmm_alloc_page();
    bool ok = true;
    for (int i = 0; i < 3; i++)
        ok = ok && vmm_map_page(ks, rva + i * PAGE_SIZE, frames[i], VMM_WRITE) == VMM_OK;
    for (int i = 0; ok && i < 3; i++)
        ok = vmm_translate(ks, rva + i * PAGE_SIZE, &t) == VMM_OK && t == frames[i];
    CHECK(ok, "three pages spanning two page tables map and translate");
    for (int i = 0; i < 3; i++) {
        vmm_unmap_page(ks, rva + i * PAGE_SIZE, NULL);
        pmm_free_page(frames[i]);
    }

    pmm_free_page(pa);
    pmm_free_page(pa2);
}

static void test_kernel_layout(void)
{
    section("VMM: kernel image permissions");
    address_space_t *ks = vmm_kernel_space();
    uint32_t tf = 0, rf = 0, df = 0;
    uint64_t text_phys = 0, data_phys = 0;

    CHECK(vmm_get_flags(ks, (uint64_t)__text_start, &tf) == VMM_OK
          && (tf & VMM_EXEC) && !(tf & VMM_WRITE), ".text is read + execute");
    CHECK(vmm_get_flags(ks, PAGE_ALIGN_DOWN((uint64_t)rodata_string), &rf) == VMM_OK
          && !(rf & VMM_EXEC) && !(rf & VMM_WRITE), ".rodata is read-only, NX");
    CHECK(vmm_get_flags(ks, (uint64_t)__data_start, &df) == VMM_OK
          && !(df & VMM_EXEC) && (df & VMM_WRITE), ".data is read-write, NX");

    vmm_translate(ks, (uint64_t)__text_start, &text_phys);
    vmm_translate(ks, (uint64_t)&passed, &data_phys);
    CHECK(data_phys - text_phys == (uint64_t)&passed - (uint64_t)__text_start,
          "kernel image is mapped to its contiguous physical load address");

    uint64_t hphys = 0;
    CHECK(vmm_translate(ks, (uint64_t)phys_to_virt(0x100000), &hphys) == VMM_OK && hphys == 0x100000,
          "HHDM maps physical memory at hhdm_offset + phys");

    section("Page faults: invalid accesses are reported");
    uint64_t v, faults0 = pf_count;

    CHECK(mm_probe_write((uint64_t)__text_start, 0) == 1
          && pf_matches((uint64_t)__text_start, true, true, false),
          "write to kernel .text faults");
    CHECK(mm_probe_write((uint64_t)rodata_string, 0) == 1
          && pf_matches((uint64_t)rodata_string, true, true, false),
          "write to kernel .rodata faults");
    CHECK(mm_probe_exec((uint64_t)data_ret_insn) == 1
          && pf_matches((uint64_t)data_ret_insn, true, false, true),
          "executing kernel .data faults (NX)");
    CHECK(mm_probe_read(0, &v) == 1 && pf_matches(0, false, false, false),
          "null-pointer read faults (page 0 unmapped)");
    CHECK(mm_probe_write(0x400000, 0) == 1 && pf_matches(0x400000, false, true, false),
          "write to unmapped user-half address faults");
    CHECK(mm_probe_read((uint64_t)stack_bottom, &v) == 1
          && pf_matches((uint64_t)stack_bottom, false, false, false),
          "boot-stack guard page faults");
    uint64_t hole = hhdm_offset + (1ULL << 45);
    CHECK(mm_probe_read(hole, &v) == 1 && pf_matches(hole, false, false, false),
          "HHDM beyond physical memory is unmapped");
    CHECK(mm_probe_read(KERNEL_HEAP_BASE + KERNEL_HEAP_MAX - PAGE_SIZE, &v) == 1,
          "unbacked kernel heap space is unmapped");

    uint64_t faults1 = pf_count;
    CHECK(mm_probe_read(0x0000800000000000ULL, &v) == 1,
          "non-canonical access faults (reported as #GP)");
    CHECK(pf_count == faults1, "non-canonical access raised #GP, not #PF");
    CHECK(faults1 - faults0 == 8, "every invalid access produced exactly one page-fault report");
}

/* ---- kernel heap -------------------------------------------------------- */

static bool in_heap(void *p)
{
    return (uint64_t)p >= KERNEL_HEAP_BASE && (uint64_t)p < KERNEL_HEAP_BASE + KERNEL_HEAP_MAX;
}

static void test_heap(void)
{
    section("Heap: allocation and release");
    struct heap_stats s;

    CHECK(kmalloc(0) == NULL, "kmalloc(0) returns NULL");
    uint8_t *p = kmalloc(24);
    CHECK(p != NULL && in_heap(p) && ((uint64_t)p % HEAP_MIN_ALIGN) == 0,
          "small allocation is in the heap and 16-byte aligned");
    memset(p, 0x5A, 24);
    CHECK(p[0] == 0x5A && p[23] == 0x5A, "allocation is writable");
    CHECK(kfree(p) == HEAP_OK, "free small allocation");

    enum { N = 64 };
    uint8_t *ptrs[N];
    size_t sizes[N];
    bool ok = true;
    for (int i = 0; i < N; i++) {
        sizes[i] = (size_t)(i * 37) % 700 + 1;
        ptrs[i] = kmalloc(sizes[i]);
        ok = ok && ptrs[i] != NULL;
        if (ptrs[i] != NULL)
            memset(ptrs[i], i, sizes[i]);
    }
    CHECK(ok, "64 allocations of varied sizes succeed");
    for (int i = 0; ok && i < N; i++) {
        for (size_t j = 0; ok && j < sizes[i]; j++)
            ok = ptrs[i][j] == (uint8_t)i;
    }
    CHECK(ok, "allocations do not overlap (patterns intact)");

    for (int i = 1; i < N; i += 2)
        kfree(ptrs[i]);
    ok = true;
    for (int i = 0; i < N; i += 2) {
        for (size_t j = 0; ok && j < sizes[i]; j++)
            ok = ptrs[i][j] == (uint8_t)i;
    }
    CHECK(ok, "freeing odd blocks leaves even blocks intact");
    CHECK(heap_check(), "heap consistent with interleaved free blocks");
    for (int i = 0; i < N; i += 2)
        kfree(ptrs[i]);
    heap_get_stats(&s);
    CHECK(heap_check() && s.used_bytes == 0 && s.block_count == 1,
          "all blocks coalesce back into one free block");

    section("Heap: alignment");
    void *a64 = kmalloc_aligned(100, 64);
    void *a4k = kmalloc_aligned(1, 4096);
    void *b4k = kmalloc_aligned(5000, 4096);
    CHECK(a64 != NULL && ((uint64_t)a64 % 64) == 0, "64-byte aligned allocation");
    CHECK(a4k != NULL && ((uint64_t)a4k % 4096) == 0, "page-aligned small allocation");
    CHECK(b4k != NULL && ((uint64_t)b4k % 4096) == 0, "page-aligned multi-page allocation");
    CHECK(heap_check(), "heap consistent after aligned splits");
    CHECK(kmalloc_aligned(8, 48) == NULL, "non-power-of-two alignment rejected");
    CHECK(kmalloc_aligned(8, 2 * PAGE_SIZE) == NULL, "alignment above page size rejected");
    kfree(a64);
    kfree(a4k);
    kfree(b4k);

    section("Heap: coalescing");
    void *x = kmalloc(1000), *y = kmalloc(1000), *z = kmalloc(1000);
    kfree(y);
    kfree(x);
    kfree(z);
    heap_get_stats(&s);
    CHECK(heap_check() && s.block_count == 1, "freeing middle, first, last coalesces fully");

    section("Heap: invalid and double frees");
    void *g1 = kmalloc(64), *q = kmalloc(64), *g2 = kmalloc(64);
    int local;
    CHECK(kfree(q) == HEAP_OK, "free between two live blocks");
    CHECK(kfree(q) == HEAP_ERR_DOUBLE_FREE, "double free detected");
    CHECK(kfree(&local) == HEAP_ERR_INVALID_PTR, "free of stack pointer rejected");
    CHECK(kfree((uint8_t *)g1 + 8) == HEAP_ERR_INVALID_PTR, "free of misaligned interior pointer rejected");
    CHECK(kfree((uint8_t *)g1 + 16) == HEAP_ERR_INVALID_PTR, "free of aligned interior pointer rejected");
    CHECK(kfree(NULL) == HEAP_OK, "kfree(NULL) is a no-op");
    kfree(g1);
    kfree(g2);
    CHECK(heap_check(), "heap consistent after rejected frees");

    section("Heap: growth and out-of-memory");
    heap_get_stats(&s);
    uint64_t mapped0 = s.mapped_bytes;
    size_t big_size = 1 << 20;
    uint8_t *big = kmalloc(big_size);
    heap_get_stats(&s);
    CHECK(big != NULL && s.mapped_bytes > mapped0, "1 MiB allocation grows the heap");
    ok = big != NULL;
    for (size_t off = 0; ok && off < big_size; off += PAGE_SIZE) {
        big[off] = (uint8_t)(off >> 12);
        ok = vmm_translate(vmm_kernel_space(), (uint64_t)big + off, NULL) == VMM_OK;
    }
    CHECK(ok, "every page of the large allocation is backed");
    kfree(big);

    uint64_t fail0 = s.alloc_failures;
    CHECK(kmalloc(KERNEL_HEAP_MAX + 1) == NULL, "allocation above heap limit returns NULL");
    CHECK(kmalloc((size_t)-1) == NULL, "SIZE_MAX allocation returns NULL (no overflow)");
    CHECK(kmalloc(KERNEL_HEAP_MAX - PAGE_SIZE) == NULL, "allocation exceeding remaining heap space returns NULL");
    heap_get_stats(&s);
    CHECK(s.alloc_failures == fail0 + 3, "failures counted");
    CHECK(heap_check(), "heap consistent after failed allocations");

    uint8_t *d = kmalloc(256);
    if (d != NULL)
        memset(d, 0xAA, 256);
    kfree(d);
    uint8_t *zb = kzalloc(256);
    ok = zb != NULL;
    for (int i = 0; ok && i < 256; i++)
        ok = zb[i] == 0;
    CHECK(ok, "kzalloc returns zeroed memory (even when reusing a dirty block)");
    kfree(zb);
}

/* ---- address spaces ----------------------------------------------------- */

static void test_address_spaces(void)
{
    section("Address spaces: isolation foundation");
    address_space_t *ks = vmm_kernel_space();
    const uint64_t user_va = 0x400000;
    const uint64_t shared_va = MM_TEST_VBASE;   /* tables already exist */
    uint64_t t;

    uint64_t free0 = pmm_free_page_count();

    address_space_t as_a, as_b;
    CHECK(vmm_create_address_space(&as_a) == VMM_OK
          && vmm_create_address_space(&as_b) == VMM_OK
          && as_a.pml4_phys != as_b.pml4_phys,
          "create two address spaces");

    uint64_t fa = pmm_alloc_page(), fb = pmm_alloc_page();
    *(volatile uint64_t *)phys_to_virt(fa) = 0xAAAAAAAAAAAAAAAAULL;
    *(volatile uint64_t *)phys_to_virt(fb) = 0xBBBBBBBBBBBBBBBBULL;

    CHECK(vmm_map_page(&as_a, user_va, fa, VMM_WRITE | VMM_USER) == VMM_OK
          && vmm_map_page(&as_b, user_va, fb, VMM_WRITE | VMM_USER) == VMM_OK,
          "map the same user address in both spaces");
    CHECK(vmm_translate(&as_a, user_va, &t) == VMM_OK && t == fa, "space A translates to frame A");
    CHECK(vmm_translate(&as_b, user_va, &t) == VMM_OK && t == fb, "space B translates to frame B");
    CHECK(vmm_translate(ks, user_va, &t) == VMM_ERR_NOT_MAPPED, "kernel space has no user mapping");
    CHECK(vmm_map_page(&as_a, user_va + PAGE_SIZE, fa, VMM_WRITE | VMM_GLOBAL) == VMM_ERR_INVALID,
          "global user mapping rejected");

    /* A kernel mapping made after the spaces were created shows up in both. */
    uint64_t fk = pmm_alloc_page();
    vmm_map_page(ks, shared_va, fk, VMM_WRITE);
    CHECK(vmm_translate(&as_a, shared_va, &t) == VMM_OK && t == fk
          && vmm_translate(&as_b, shared_va, &t) == VMM_OK && t == fk,
          "later kernel mappings are shared by all spaces");

    uint64_t flags = irq_save();
    vmm_switch(&as_a);
    uint64_t seen_a = *(volatile uint64_t *)user_va;
    bool kernel_ok_a = passed > 0;   /* touches kernel .bss while in space A */
    vmm_switch(&as_b);
    uint64_t seen_b = *(volatile uint64_t *)user_va;
    vmm_switch(ks);
    irq_restore(flags);

    CHECK(seen_a == 0xAAAAAAAAAAAAAAAAULL, "running in space A sees frame A");
    CHECK(seen_b == 0xBBBBBBBBBBBBBBBBULL, "running in space B sees frame B at the same address");
    CHECK(kernel_ok_a, "kernel remains accessible after switching spaces");

    CHECK(vmm_destroy_address_space(ks) == VMM_ERR_INVALID, "destroying the kernel space is refused");
    vmm_unmap_page(&as_a, user_va, NULL);
    vmm_unmap_page(&as_b, user_va, NULL);
    vmm_unmap_page(ks, shared_va, NULL);
    pmm_free_page(fa);
    pmm_free_page(fb);
    pmm_free_page(fk);
    CHECK(vmm_destroy_address_space(&as_a) == VMM_OK
          && vmm_destroy_address_space(&as_b) == VMM_OK, "destroy both address spaces");
    CHECK(pmm_free_page_count() == free0, "all page-table pages returned to the PMM");
}

bool mm_run_tests(void)
{
    passed = failed = 0;
    kprintf("\n[test] ===== Phase 3 memory-management tests =====\n");

    test_pmm_basic();
    test_pmm_exhaustion();
    test_vmm_mapping();
    test_kernel_layout();
    test_heap();
    test_address_spaces();

    kprintf("\n[test] ===== %u passed, %u failed =====\n", passed, failed);
    pmm_print_stats();
    heap_print_stats();
    return failed == 0;
}
