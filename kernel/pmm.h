#ifndef NEXUS_PMM_H
#define NEXUS_PMM_H

/* Physical memory manager: a bitmap of 4 KiB page frames.
 *
 * One bit per frame from physical address 0 up to the end of the highest
 * usable region (1 = allocated/reserved, 0 = free). Only frames inside
 * MEM_USABLE regions are ever handed out; the kernel image, bootloader
 * structures, ACPI tables, MMIO holes and the bitmap itself stay reserved.
 *
 * Frames are identified by physical address. Physical page 0 is never
 * allocated, so 0 doubles as the "out of memory" return value.
 *
 * Not SMP-safe; callers are single-core kernel code for now. */

#include "mm.h"

typedef enum {
    PMM_OK = 0,
    PMM_ERR_UNALIGNED,     /* address is not page-aligned */
    PMM_ERR_NOT_MANAGED,   /* frame is not usable RAM owned by the PMM */
    PMM_ERR_DOUBLE_FREE,   /* frame is already free */
} pmm_status_t;

struct pmm_stats {
    uint64_t total_bytes;     /* RAM reported by the firmware (all regions
                                 except reserved ranges and framebuffers) */
    uint64_t usable_bytes;    /* sum of MEM_USABLE regions */
    uint64_t reserved_bytes;  /* RAM not available for allocation
                                 (kernel, bootloader, ACPI, bad) */
    uint64_t non_ram_bytes;   /* firmware-reserved and MMIO address ranges */
    uint64_t managed_pages;   /* usable frames the allocator can hand out
                                 (excludes page 0 and the bitmap itself) */
    uint64_t free_pages;
    uint64_t allocated_pages; /* managed - free */
};

void pmm_init(const struct boot_mem_info *info);

/* Allocate one frame. Returns its physical address, or 0 when exhausted.
 * The frame's contents are undefined. */
uint64_t pmm_alloc_page(void);

/* Allocate one frame and zero it through the HHDM. Returns 0 on failure. */
uint64_t pmm_alloc_zeroed_page(void);

/* Return a frame. Invalid and double frees are rejected and reported,
 * leaving allocator state unchanged. */
pmm_status_t pmm_free_page(uint64_t phys);

/* True if the frame is managed by the PMM and currently allocated. */
bool pmm_is_allocated(uint64_t phys);

uint64_t pmm_free_page_count(void);
void pmm_get_stats(struct pmm_stats *out);
void pmm_print_stats(void);
const char *pmm_status_str(pmm_status_t s);

#endif /* NEXUS_PMM_H */
