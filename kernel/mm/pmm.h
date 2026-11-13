#ifndef BHARAT_PMM_H
#define BHARAT_PMM_H

/* Physical memory manager: a buddy allocator over 4 KiB page frames.
 *
 * Free memory is kept as blocks of 2^order contiguous frames (order 0 =
 * one page, PMM_MAX_ORDER = 4 MiB), one free list per order. Allocating
 * takes the smallest free block that fits and splits it in halves until
 * it is the right size; freeing merges a block with its "buddy" (the
 * other half of the block it was split from) whenever that is free too,
 * so free memory stays in large blocks. Both are a handful of list
 * operations: no scanning.
 *
 * Every frame has a small record (struct pmm_page) holding its state, its
 * block order and a reference count. The reference count lets one frame
 * be shared, e.g. by copy-on-write mappings: pmm_page_ref() adds a user,
 * pmm_page_unref() removes one and frees the frame when none are left.
 *
 * Only frames inside MEM_USABLE regions are ever handed out; the kernel
 * image, bootloader structures, ACPI tables, MMIO holes and the frame
 * records themselves stay reserved. Physical page 0 is never allocated,
 * so 0 doubles as the "out of memory" return value.
 *
 * A small hot-page cache in front of the lists keeps recently freed single
 * pages for reuse, so the common "one page, soon freed" case skips the
 * split and merge work entirely.
 *
 * Not SMP-safe yet (IRQ_GUARD only); per-CPU caches come in Phase 9. */

#include "mm.h"

#define PMM_MAX_ORDER 10            /* largest block: 2^10 pages = 4 MiB */

typedef enum {
    PMM_OK = 0,
    PMM_ERR_UNALIGNED,     /* address is not page-aligned */
    PMM_ERR_NOT_MANAGED,   /* frame is not usable RAM owned by the PMM */
    PMM_ERR_DOUBLE_FREE,   /* frame is already free */
    PMM_ERR_BAD_ORDER,     /* not the start of a block of that order */
} pmm_status_t;

struct pmm_stats {
    uint64_t total_bytes;     /* RAM reported by the firmware (all regions
                                 except reserved ranges and framebuffers) */
    uint64_t usable_bytes;    /* sum of MEM_USABLE regions */
    uint64_t reserved_bytes;  /* RAM not available for allocation
                                 (kernel, bootloader, ACPI, bad) */
    uint64_t non_ram_bytes;   /* firmware-reserved and MMIO address ranges */
    uint64_t managed_pages;   /* usable frames the allocator can hand out
                                 (excludes page 0 and the frame records) */
    uint64_t free_pages;
    uint64_t allocated_pages; /* managed - free */
    uint64_t free_blocks[PMM_MAX_ORDER + 1];   /* free blocks per order */
};

void pmm_init(const struct boot_mem_info *info);

/* Allocate one frame. Returns its physical address, or 0 when exhausted.
 * The frame's contents are undefined; its reference count is 1. */
uint64_t pmm_alloc_page(void);

/* Allocate one frame and zero it through the HHDM. Returns 0 on failure. */
uint64_t pmm_alloc_zeroed_page(void);

/* Allocate 2^order contiguous frames, aligned to their size. Returns the
 * first frame's address, or 0 if no block that large is free. */
uint64_t pmm_alloc_pages(unsigned order);

/* Return a frame (whatever its reference count). Invalid and double frees
 * are rejected and reported, leaving allocator state unchanged. */
pmm_status_t pmm_free_page(uint64_t phys);

/* Return a block from pmm_alloc_pages(order); `order` must match. */
pmm_status_t pmm_free_pages(uint64_t phys, unsigned order);

/* Shared frames (single pages only). ref adds a user; unref removes one
 * and frees the frame when the count reaches 0 (returns true then). */
pmm_status_t pmm_page_ref(uint64_t phys);
bool pmm_page_unref(uint64_t phys);
unsigned pmm_page_refcount(uint64_t phys);   /* 0 if free or unmanaged */

/* True if the frame is managed by the PMM and currently allocated. */
bool pmm_is_allocated(uint64_t phys);

uint64_t pmm_free_page_count(void);
void pmm_get_stats(struct pmm_stats *out);
void pmm_print_stats(void);
const char *pmm_status_str(pmm_status_t s);

/* Move the hot-page cache back into the buddy lists, so the free-block
 * counts show the fully merged layout (tests compare it). */
void pmm_drain_cache(void);

/* Walk every free list and frame record and verify the allocator's
 * invariants (counts match, blocks aligned and not overlapping, every
 * possible merge done). Returns true if consistent. */
bool pmm_check(void);

#endif /* BHARAT_PMM_H */
