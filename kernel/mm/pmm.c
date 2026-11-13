#include "pmm.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"

/* One record per frame, from physical address 0 to the end of the highest
 * usable region. Free blocks are linked through their first frame's
 * record by frame number (32 bits is plenty: 2^32 frames = 16 TiB). */
struct pmm_page {
    uint32_t next, prev;      /* free-list links (block heads only) */
    uint16_t refcount;        /* users of an allocated single page */
    uint8_t order;            /* block order (free heads, allocated heads) */
    uint8_t flags;
};

#define PG_MANAGED    0x01    /* usable RAM owned by the allocator */
#define PG_ALLOCATED  0x02    /* part of an allocated block */
#define PG_FREE_HEAD  0x04    /* first frame of a free block */
#define PG_HEAD       0x08    /* first frame of an allocated block */
#define NONE          0xFFFFFFFFu

_Static_assert(sizeof(struct pmm_page) == 12, "frame record must stay small");

static struct pmm_page *pages;    /* HHDM pointer to the records */
static uint64_t pages_phys, pages_bytes;
static uint64_t frame_count;
static uint32_t free_head[PMM_MAX_ORDER + 1];
static uint64_t free_blocks[PMM_MAX_ORDER + 1];
static uint64_t free_pages;
static uint64_t managed_pages;
static uint64_t total_bytes;
static uint64_t usable_bytes;
static uint64_t non_ram_bytes;

/* Hot-page cache: recently freed single pages, kept off the buddy lists
 * and handed out first. Most page allocations are single pages that are
 * soon freed again; without the cache each one would split a large block
 * down to one page and merge it all the way back up. The cache drains
 * into the buddy lists when it is full or a larger block is needed.
 * Its pages count as free. */
#define HOT_MAX 64
static uint32_t hot[HOT_MAX];
static unsigned hot_count;

/* ---- free lists ---------------------------------------------------------- */

static void list_push(uint32_t f, unsigned order)
{
    struct pmm_page *p = &pages[f];
    p->flags |= PG_FREE_HEAD;
    p->order = (uint8_t)order;
    p->prev = NONE;
    p->next = free_head[order];
    if (free_head[order] != NONE)
        pages[free_head[order]].prev = f;
    free_head[order] = f;
    free_blocks[order]++;
}

static void list_remove(uint32_t f, unsigned order)
{
    struct pmm_page *p = &pages[f];
    if (p->prev != NONE)
        pages[p->prev].next = p->next;
    else
        free_head[order] = p->next;
    if (p->next != NONE)
        pages[p->next].prev = p->prev;
    p->flags &= ~PG_FREE_HEAD;
    p->next = p->prev = NONE;
    free_blocks[order]--;
}

/* Put block (f, order) on the free lists, merging with its buddy for as
 * long as the buddy is a free block of the same size. */
static void release_block(uint32_t f, unsigned order)
{
    while (order < PMM_MAX_ORDER) {
        uint32_t buddy = f ^ (1u << order);
        if (buddy >= frame_count || !(pages[buddy].flags & PG_FREE_HEAD)
            || pages[buddy].order != order)
            break;
        list_remove(buddy, order);
        f = f < buddy ? f : buddy;          /* merged block starts at the lower */
        order++;
    }
    list_push(f, order);
}

/* ---- init --------------------------------------------------------------- */

void pmm_init(const struct boot_mem_info *info)
{
    struct { uint64_t start, end; } usable[MM_MAX_REGIONS];
    size_t usable_count = 0;
    uint64_t highest = 0;

    kprintf("[pmm] Physical memory map (%lu regions):\n", (uint64_t)info->region_count);
    for (size_t i = 0; i < info->region_count; i++) {
        const struct mem_region *r = &info->regions[i];
        kprintf("[pmm]   %016lx - %016lx  %8lu KiB  %s\n",
                r->base, r->base + r->length, r->length / 1024,
                mem_region_type_name(r->type));
        /* "Reserved" and framebuffer entries describe address ranges
         * (firmware, MMIO), not installed RAM; keep them out of the total. */
        if (r->type == MEM_RESERVED || r->type == MEM_FRAMEBUFFER)
            non_ram_bytes += r->length;
        else
            total_bytes += r->length;
        if (r->type != MEM_USABLE)
            continue;
        usable_bytes += r->length;
        uint64_t start = PAGE_ALIGN_UP(r->base);
        uint64_t end = PAGE_ALIGN_DOWN(r->base + r->length);
        if (end <= start)
            continue;
        if (usable_count == MM_MAX_REGIONS)
            panic("pmm: too many usable regions");
        usable[usable_count].start = start;
        usable[usable_count].end = end;
        usable_count++;
        if (end > highest)
            highest = end;
    }
    if (usable_count == 0)
        panic("pmm: no usable memory reported by the bootloader");
    if ((highest >> PAGE_SHIFT) >= NONE)
        panic("pmm: more physical memory than the frame records can index");

    frame_count = highest >> PAGE_SHIFT;
    pages_bytes = frame_count * sizeof(struct pmm_page);

    /* Place the records at the start of the first usable region big enough. */
    pages_phys = 0;
    for (size_t i = 0; i < usable_count; i++) {
        uint64_t start = usable[i].start == 0 ? PAGE_SIZE : usable[i].start;
        if (usable[i].end > start && usable[i].end - start >= PAGE_ALIGN_UP(pages_bytes)) {
            pages_phys = start;
            break;
        }
    }
    if (pages_phys == 0)
        panic("pmm: no usable region large enough for the frame records");
    pages = phys_to_virt(pages_phys);
    memset(pages, 0, pages_bytes);
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++) {
        free_head[o] = NONE;
        free_blocks[o] = 0;
    }

    /* Mark usable frames managed, except page 0 (the failure value) and
     * the frames holding the records, then free them one by one: merging
     * builds the large blocks automatically. */
    uint64_t rec_end = pages_phys + PAGE_ALIGN_UP(pages_bytes);
    free_pages = managed_pages = 0;
    for (size_t i = 0; i < usable_count; i++) {
        for (uint64_t p = usable[i].start; p < usable[i].end; p += PAGE_SIZE) {
            if (p == 0 || (p >= pages_phys && p < rec_end))
                continue;
            uint32_t f = (uint32_t)(p >> PAGE_SHIFT);
            pages[f].flags = PG_MANAGED;
            pages[f].next = pages[f].prev = NONE;
            release_block(f, 0);
            free_pages++;
            managed_pages++;
        }
    }

    kprintf("[pmm] Buddy allocator: %lu frames tracked, %lu KiB of frame records at phys %p\n",
            frame_count, pages_bytes / 1024, (void *)pages_phys);
    pmm_print_stats();
}

/* ---- allocation ---------------------------------------------------------- */

static void drain_hot(unsigned keep)
{
    while (hot_count > keep)
        release_block(hot[--hot_count], 0);
}

static uint32_t alloc_block(unsigned order)
{
    if (order == 0 && hot_count > 0) {
        uint32_t f = hot[--hot_count];
        pages[f].flags = PG_MANAGED | PG_ALLOCATED | PG_HEAD;
        pages[f].order = 0;
        pages[f].refcount = 1;
        free_pages--;
        return f;
    }
    unsigned o = order;
    while (o <= PMM_MAX_ORDER && free_head[o] == NONE)
        o++;
    if (o > PMM_MAX_ORDER && hot_count > 0) {
        drain_hot(0);                       /* cached pages may merge into a fit */
        for (o = order; o <= PMM_MAX_ORDER && free_head[o] == NONE; o++)
            ;
    }
    if (o > PMM_MAX_ORDER)
        return NONE;
    uint32_t f = free_head[o];
    list_remove(f, o);
    while (o > order) {                     /* split, keeping the lower half */
        o--;
        list_push(f + (1u << o), o);
    }
    uint32_t n = 1u << order;
    for (uint32_t i = 0; i < n; i++)
        pages[f + i].flags = PG_MANAGED | PG_ALLOCATED;
    pages[f].flags |= PG_HEAD;
    pages[f].order = (uint8_t)order;
    pages[f].refcount = 1;
    free_pages -= n;
    return f;
}

uint64_t pmm_alloc_pages(unsigned order)
{
    if (order > PMM_MAX_ORDER)
        return 0;
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    uint32_t f = alloc_block(order);
    return f == NONE ? 0 : (uint64_t)f << PAGE_SHIFT;
}

uint64_t pmm_alloc_page(void)
{
    return pmm_alloc_pages(0);
}

uint64_t pmm_alloc_zeroed_page(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys != 0)
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

/* ---- freeing ------------------------------------------------------------ */

static pmm_status_t check_block(uint64_t phys, unsigned order)
{
    if (!IS_PAGE_ALIGNED(phys))
        return PMM_ERR_UNALIGNED;
    uint64_t f = phys >> PAGE_SHIFT;
    if (phys == 0 || f >= frame_count || !(pages[f].flags & PG_MANAGED))
        return PMM_ERR_NOT_MANAGED;
    if (!(pages[f].flags & PG_ALLOCATED))
        return PMM_ERR_DOUBLE_FREE;
    if (order > PMM_MAX_ORDER || !(pages[f].flags & PG_HEAD) || pages[f].order != order)
        return PMM_ERR_BAD_ORDER;
    return PMM_OK;
}

static void free_block_checked(uint32_t f, unsigned order)
{
    if (order == 0) {
        pages[f].flags = PG_MANAGED;
        pages[f].refcount = 0;
        free_pages++;
        if (hot_count == HOT_MAX)
            drain_hot(HOT_MAX / 2);         /* full: send the older half on */
        hot[hot_count++] = f;
        return;
    }
    uint32_t n = 1u << order;
    for (uint32_t i = 0; i < n; i++) {
        pages[f + i].flags = PG_MANAGED;
        pages[f + i].refcount = 0;
    }
    free_pages += n;
    release_block(f, order);
}

pmm_status_t pmm_free_pages(uint64_t phys, unsigned order)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    pmm_status_t st = check_block(phys, order);
    if (st != PMM_OK) {
        kprintf("[pmm] rejected free of %p (order %u): %s\n", (void *)phys, order,
                pmm_status_str(st));
        return st;
    }
    free_block_checked((uint32_t)(phys >> PAGE_SHIFT), order);
    return PMM_OK;
}

pmm_status_t pmm_free_page(uint64_t phys)
{
    return pmm_free_pages(phys, 0);
}

/* ---- sharing ------------------------------------------------------------ */

pmm_status_t pmm_page_ref(uint64_t phys)
{
    IRQ_GUARD();
    pmm_status_t st = check_block(phys, 0);
    if (st != PMM_OK)
        return st;
    struct pmm_page *p = &pages[phys >> PAGE_SHIFT];
    if (p->refcount == 0xFFFF)
        panic("pmm: page reference count overflow");
    p->refcount++;
    return PMM_OK;
}

bool pmm_page_unref(uint64_t phys)
{
    IRQ_GUARD();
    if (check_block(phys, 0) != PMM_OK) {
        kprintf("[pmm] rejected unref of %p: not an allocated page\n", (void *)phys);
        return false;
    }
    uint32_t f = (uint32_t)(phys >> PAGE_SHIFT);
    if (--pages[f].refcount > 0)
        return false;
    free_block_checked(f, 0);
    return true;
}

unsigned pmm_page_refcount(uint64_t phys)
{
    IRQ_GUARD();
    return check_block(phys, 0) == PMM_OK ? pages[phys >> PAGE_SHIFT].refcount : 0;
}

bool pmm_is_allocated(uint64_t phys)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    if (!IS_PAGE_ALIGNED(phys) || phys == 0 || (phys >> PAGE_SHIFT) >= frame_count)
        return false;
    uint8_t fl = pages[phys >> PAGE_SHIFT].flags;
    return (fl & PG_MANAGED) && (fl & PG_ALLOCATED);
}

/* ---- statistics and checks ------------------------------------------------ */

uint64_t pmm_free_page_count(void)
{
    return free_pages;
}

void pmm_get_stats(struct pmm_stats *out)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    out->total_bytes = total_bytes;
    out->usable_bytes = usable_bytes;
    out->reserved_bytes = total_bytes - usable_bytes;
    out->non_ram_bytes = non_ram_bytes;
    out->managed_pages = managed_pages;
    out->free_pages = free_pages;
    out->allocated_pages = managed_pages - free_pages;
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++)
        out->free_blocks[o] = free_blocks[o];
}

void pmm_print_stats(void)
{
    struct pmm_stats s;
    pmm_get_stats(&s);
    kprintf("[pmm] Total physical memory : %8lu KiB\n", s.total_bytes / 1024);
    kprintf("[pmm] Usable memory         : %8lu KiB\n", s.usable_bytes / 1024);
    kprintf("[pmm] Reserved memory       : %8lu KiB (kernel, bootloader, ACPI)\n",
            s.reserved_bytes / 1024);
    kprintf("[pmm] Free memory           : %8lu KiB (%lu pages)\n",
            s.free_pages * PAGE_SIZE / 1024, s.free_pages);
    kprintf("[pmm] Allocated pages       : %8lu of %lu managed\n",
            s.allocated_pages, s.managed_pages);
    kprintf("[pmm] Non-RAM ranges        : %8lu KiB (firmware/MMIO, excluded)\n",
            s.non_ram_bytes / 1024);
    kprintf("[pmm] Free blocks by order  :");
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++)
        kprintf(" %lu", s.free_blocks[o]);
    kprintf("\n");
}

void pmm_drain_cache(void)
{
    IRQ_GUARD();
    drain_hot(0);
}

bool pmm_check(void)
{
    IRQ_GUARD();
    uint64_t counted = hot_count;
    for (unsigned i = 0; i < hot_count; i++) {
        const struct pmm_page *p = &pages[hot[i]];
        if (p->flags != PG_MANAGED) {
            kprintf("[pmm] check: cached page %u has flags 0x%x\n", hot[i], p->flags);
            return false;
        }
    }
    for (unsigned o = 0; o <= PMM_MAX_ORDER; o++) {
        uint64_t n = 0;
        uint32_t prev = NONE;
        for (uint32_t f = free_head[o]; f != NONE; f = pages[f].next) {
            const struct pmm_page *p = &pages[f];
            if (f >= frame_count || !(p->flags & PG_FREE_HEAD) || p->order != o
                || p->prev != prev || (f & ((1u << o) - 1)) != 0) {
                kprintf("[pmm] check: bad free block %u at order %u\n", f, o);
                return false;
            }
            for (uint32_t i = 0; i < (1u << o); i++) {
                if (f + i >= frame_count || pages[f + i].flags & PG_ALLOCATED
                    || !(pages[f + i].flags & PG_MANAGED)
                    || (i > 0 && (pages[f + i].flags & PG_FREE_HEAD))) {
                    kprintf("[pmm] check: free block %u (order %u) overlaps frame %u\n",
                            f, o, f + i);
                    return false;
                }
            }
            uint32_t buddy = f ^ (1u << o);
            if (o < PMM_MAX_ORDER && buddy < frame_count
                && (pages[buddy].flags & PG_FREE_HEAD) && pages[buddy].order == o) {
                kprintf("[pmm] check: free buddies %u and %u not merged\n", f, buddy);
                return false;
            }
            counted += 1u << o;
            prev = f;
            n++;
        }
        if (n != free_blocks[o]) {
            kprintf("[pmm] check: order %u list has %lu blocks, counter says %lu\n",
                    o, n, free_blocks[o]);
            return false;
        }
    }
    if (counted != free_pages) {
        kprintf("[pmm] check: free lists hold %lu pages, counter says %lu\n", counted, free_pages);
        return false;
    }
    return true;
}

const char *pmm_status_str(pmm_status_t s)
{
    switch (s) {
    case PMM_OK:              return "ok";
    case PMM_ERR_UNALIGNED:   return "address not page-aligned";
    case PMM_ERR_NOT_MANAGED: return "frame not managed by the PMM";
    case PMM_ERR_DOUBLE_FREE: return "double free";
    case PMM_ERR_BAD_ORDER:   return "not the start of a block of that size";
    }
    return "unknown";
}
