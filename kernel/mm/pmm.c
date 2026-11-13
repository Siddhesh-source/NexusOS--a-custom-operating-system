#include "pmm.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"

#define BITS_PER_WORD 64

static uint64_t *bitmap;          /* HHDM pointer to the bitmap */
static uint64_t bitmap_words;
static uint64_t bitmap_phys;
static uint64_t bitmap_bytes;
static uint64_t frame_count;      /* frames covered by the bitmap */
static uint64_t next_word;        /* next-fit search hint */

static uint64_t free_pages;
static uint64_t managed_pages;
static uint64_t total_bytes;
static uint64_t usable_bytes;
static uint64_t non_ram_bytes;

/* Copy of the usable ranges (page-aligned inward), used to validate frees. */
static struct { uint64_t start, end; } usable[MM_MAX_REGIONS];
static size_t usable_count;

static inline bool bit_test(uint64_t frame)
{
    return (bitmap[frame / BITS_PER_WORD] >> (frame % BITS_PER_WORD)) & 1;
}

static inline void bit_set(uint64_t frame)
{
    bitmap[frame / BITS_PER_WORD] |= 1ULL << (frame % BITS_PER_WORD);
}

static inline void bit_clear(uint64_t frame)
{
    bitmap[frame / BITS_PER_WORD] &= ~(1ULL << (frame % BITS_PER_WORD));
}

/* A frame is managed if it lies in a usable region and is not one of the
 * permanently reserved frames (page 0, the bitmap). */
static bool frame_is_managed(uint64_t phys)
{
    if (phys == 0)
        return false;
    if (phys >= bitmap_phys && phys < bitmap_phys + PAGE_ALIGN_UP(bitmap_bytes))
        return false;
    for (size_t i = 0; i < usable_count; i++) {
        if (phys >= usable[i].start && phys < usable[i].end)
            return true;
    }
    return false;
}

/* Mark a frame permanently reserved (used by init only). */
static void reserve_frame(uint64_t phys)
{
    uint64_t frame = phys >> PAGE_SHIFT;
    if (frame < frame_count && !bit_test(frame)) {
        bit_set(frame);
        free_pages--;
        managed_pages--;
    }
}

void pmm_init(const struct boot_mem_info *info)
{
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

    frame_count = highest >> PAGE_SHIFT;
    bitmap_words = (frame_count + BITS_PER_WORD - 1) / BITS_PER_WORD;
    bitmap_bytes = bitmap_words * sizeof(uint64_t);

    /* Place the bitmap at the start of the first usable region big enough. */
    bitmap_phys = 0;
    for (size_t i = 0; i < usable_count; i++) {
        uint64_t start = usable[i].start == 0 ? PAGE_SIZE : usable[i].start;
        if (usable[i].end > start && usable[i].end - start >= PAGE_ALIGN_UP(bitmap_bytes)) {
            bitmap_phys = start;
            break;
        }
    }
    if (bitmap_phys == 0)
        panic("pmm: no usable region large enough for the frame bitmap");

    bitmap = phys_to_virt(bitmap_phys);

    /* Start with everything reserved, then release usable frames. */
    memset(bitmap, 0xFF, bitmap_bytes);
    for (size_t i = 0; i < usable_count; i++) {
        for (uint64_t p = usable[i].start; p < usable[i].end; p += PAGE_SIZE) {
            bit_clear(p >> PAGE_SHIFT);
            free_pages++;
            managed_pages++;
        }
    }

    reserve_frame(0);   /* keeps 0 usable as the failure sentinel */
    for (uint64_t off = 0; off < PAGE_ALIGN_UP(bitmap_bytes); off += PAGE_SIZE)
        reserve_frame(bitmap_phys + off);

    next_word = 0;

    kprintf("[pmm] Bitmap: %lu frames tracked, %lu bytes at phys %p\n",
            frame_count, bitmap_bytes, (void *)bitmap_phys);
    pmm_print_stats();
}

uint64_t pmm_alloc_page(void)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    if (free_pages == 0)
        return 0;

    for (uint64_t n = 0; n < bitmap_words; n++) {
        uint64_t w = (next_word + n) % bitmap_words;
        if (bitmap[w] == ~0ULL)
            continue;

        uint64_t bit = (uint64_t)__builtin_ctzll(~bitmap[w]);
        uint64_t frame = w * BITS_PER_WORD + bit;
        if (frame >= frame_count)
            continue;   /* padding bits in the last word */

        bit_set(frame);
        free_pages--;
        next_word = w;
        return frame << PAGE_SHIFT;
    }

    /* free_pages said otherwise: the bitmap and counter disagree. */
    panic("pmm: free page counter out of sync with bitmap");
}

uint64_t pmm_alloc_zeroed_page(void)
{
    uint64_t phys = pmm_alloc_page();
    if (phys != 0)
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

pmm_status_t pmm_free_page(uint64_t phys)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    pmm_status_t st = PMM_OK;

    if (!IS_PAGE_ALIGNED(phys))
        st = PMM_ERR_UNALIGNED;
    else if (!frame_is_managed(phys))
        st = PMM_ERR_NOT_MANAGED;
    else if (!bit_test(phys >> PAGE_SHIFT))
        st = PMM_ERR_DOUBLE_FREE;

    if (st != PMM_OK) {
        kprintf("[pmm] rejected free of %p: %s\n", (void *)phys, pmm_status_str(st));
        return st;
    }

    bit_clear(phys >> PAGE_SHIFT);
    free_pages++;
    return PMM_OK;
}

bool pmm_is_allocated(uint64_t phys)
{
    IRQ_GUARD();   /* preemption-safe: see cpu.h */
    return IS_PAGE_ALIGNED(phys) && frame_is_managed(phys) && bit_test(phys >> PAGE_SHIFT);
}

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
}

const char *pmm_status_str(pmm_status_t s)
{
    switch (s) {
    case PMM_OK:              return "ok";
    case PMM_ERR_UNALIGNED:   return "address not page-aligned";
    case PMM_ERR_NOT_MANAGED: return "frame not managed by the PMM";
    case PMM_ERR_DOUBLE_FREE: return "double free";
    }
    return "unknown";
}
