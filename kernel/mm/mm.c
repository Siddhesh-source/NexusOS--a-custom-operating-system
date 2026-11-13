#include "mm.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "kprintf.h"

uint64_t hhdm_offset;

const char *mem_region_type_name(enum mem_region_type type)
{
    switch (type) {
    case MEM_USABLE:                 return "usable";
    case MEM_RESERVED:               return "reserved";
    case MEM_ACPI_RECLAIMABLE:       return "ACPI reclaimable";
    case MEM_ACPI_NVS:               return "ACPI NVS";
    case MEM_BAD:                    return "bad memory";
    case MEM_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
    case MEM_KERNEL_AND_MODULES:     return "kernel and modules";
    case MEM_FRAMEBUFFER:            return "framebuffer";
    }
    return "unknown";
}

void mm_init(const struct boot_mem_info *info)
{
    hhdm_offset = info->hhdm_offset;

    kprintf("[mm] Initialising physical memory manager...\n");
    pmm_init(info);

    kprintf("[mm] Initialising virtual memory manager...\n");
    vmm_init(info);

    kprintf("[mm] Initialising kernel heap...\n");
    heap_init();

    kprintf("[mm] Memory management online.\n");
    pmm_print_stats();
}
