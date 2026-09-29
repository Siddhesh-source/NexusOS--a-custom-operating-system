#ifndef NEXUS_MM_H
#define NEXUS_MM_H

/* Common memory-management definitions shared by the PMM, VMM and heap.
 *
 * Kernel virtual address layout (all in the upper canonical half):
 *
 *   0x0000000000000000 - 0x00007fffffffffff   user space (per address space;
 *                                             empty until processes exist)
 *   0xffff800000000000 + phys                 HHDM: direct map of RAM-backed
 *                                             physical memory (RW, NX)
 *   0xffffc00000000000 (KERNEL_HEAP_BASE)     kernel heap, grows upward to
 *                                             KERNEL_HEAP_BASE + KERNEL_HEAP_MAX
 *   0xffffd00000000000 (MM_TEST_VBASE)        scratch window for memory tests
 *   0xffffffff80000000 +                      kernel image (text RX, rodata R,
 *                                             data/bss RW) as linked
 *
 * Everything else is deliberately left unmapped, so stray accesses fault. */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PAGE_SIZE           ((uint64_t)4096)
#define PAGE_SHIFT          12
#define PAGE_MASK           (PAGE_SIZE - 1)
#define PAGE_ALIGN_DOWN(x)  ((uint64_t)(x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)    (((uint64_t)(x) + PAGE_MASK) & ~PAGE_MASK)
#define IS_PAGE_ALIGNED(x)  (((uint64_t)(x) & PAGE_MASK) == 0)

#define KERNEL_HALF_BASE    ((uint64_t)0xffff800000000000)
#define KERNEL_HEAP_BASE    ((uint64_t)0xffffc00000000000)
#define KERNEL_HEAP_MAX     ((uint64_t)256 << 20)         /* 256 MiB of address space */
#define MM_TEST_VBASE       ((uint64_t)0xffffd00000000000)

/* Physical memory region types, independent of the boot protocol. */
enum mem_region_type {
    MEM_USABLE = 0,
    MEM_RESERVED,
    MEM_ACPI_RECLAIMABLE,
    MEM_ACPI_NVS,
    MEM_BAD,
    MEM_BOOTLOADER_RECLAIMABLE,
    MEM_KERNEL_AND_MODULES,
    MEM_FRAMEBUFFER,
};

struct mem_region {
    uint64_t base;
    uint64_t length;
    enum mem_region_type type;
};

#define MM_MAX_REGIONS 128

/* Everything the memory subsystem needs from the boot environment. */
struct boot_mem_info {
    const struct mem_region *regions;
    size_t region_count;
    uint64_t hhdm_offset;       /* virtual = physical + hhdm_offset */
    uint64_t kernel_phys_base;  /* where the kernel image was loaded */
    uint64_t kernel_virt_base;  /* where it was linked */
};

extern uint64_t hhdm_offset;

static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(phys + hhdm_offset);
}

const char *mem_region_type_name(enum mem_region_type type);

/* Bring up PMM, kernel page tables and heap, in that order. */
void mm_init(const struct boot_mem_info *info);

#endif /* NEXUS_MM_H */
