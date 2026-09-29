#ifndef NEXUS_VMM_H
#define NEXUS_VMM_H

/* Virtual memory manager: x86-64 4-level paging with 4 KiB pages.
 *
 * An address space is just the physical address of its PML4. Every address
 * space shares the kernel half (PML4 entries 256-511): the PDPTs for those
 * entries are allocated once at boot and copied by reference into each new
 * PML4, so a kernel mapping created later is instantly visible everywhere.
 * The user half (entries 0-255) is private to each address space.
 *
 *   Process A -> address_space A -> PML4 A --+-- user half (private)
 *   Process B -> address_space B -> PML4 B --+-- user half (private)
 *                                            +-- kernel half (shared PDPTs)
 *
 * Page frames come from the PMM; the VMM never owns the frames it maps,
 * only the page-table pages it allocates.
 *
 * Not SMP-safe; single-core kernel code only for now. */

#include "mm.h"

/* Mapping permission flags. A page is always readable when present. */
#define VMM_WRITE    (1u << 0)   /* writable */
#define VMM_USER     (1u << 1)   /* accessible from ring 3 (user half only) */
#define VMM_EXEC     (1u << 2)   /* executable (otherwise NX) */
#define VMM_GLOBAL   (1u << 3)   /* survives CR3 switches (kernel half only) */
#define VMM_NOCACHE  (1u << 4)   /* uncached (PCD|PWT), for MMIO */

typedef enum {
    VMM_OK = 0,
    VMM_ERR_UNALIGNED,       /* virtual or physical address not page-aligned */
    VMM_ERR_NONCANONICAL,    /* virtual address outside the canonical halves */
    VMM_ERR_ALREADY_MAPPED,  /* map over an existing mapping */
    VMM_ERR_NOT_MAPPED,      /* unmap/protect/translate of an unmapped page */
    VMM_ERR_NO_MEMORY,       /* page-table allocation failed */
    VMM_ERR_INVALID,         /* bad argument (flags, address space, phys) */
} vmm_status_t;

typedef struct address_space {
    uint64_t pml4_phys;
} address_space_t;

/* Build the kernel page tables from the boot memory map and switch to them. */
void vmm_init(const struct boot_mem_info *info);

address_space_t *vmm_kernel_space(void);
address_space_t *vmm_current_space(void);

vmm_status_t vmm_map_page(address_space_t *as, uint64_t virt, uint64_t phys, uint32_t flags);
vmm_status_t vmm_unmap_page(address_space_t *as, uint64_t virt, uint64_t *old_phys);
vmm_status_t vmm_translate(address_space_t *as, uint64_t virt, uint64_t *phys);
vmm_status_t vmm_get_flags(address_space_t *as, uint64_t virt, uint32_t *flags);
vmm_status_t vmm_protect_page(address_space_t *as, uint64_t virt, uint32_t flags);

/* Map `size` bytes (page-granular) of contiguous physical memory. On failure
 * every page mapped by this call is unmapped again. */
vmm_status_t vmm_map_range(address_space_t *as, uint64_t virt, uint64_t phys,
                           uint64_t size, uint32_t flags);

/* Make sure every page-table page needed to map [virt, virt+size) exists,
 * without mapping anything. Lets a subsystem pay for its tables once at init
 * so later map calls in that range cannot fail for lack of page tables. */
vmm_status_t vmm_reserve_tables(address_space_t *as, uint64_t virt, uint64_t size);

/* Create an address space: empty user half, shared kernel half. */
vmm_status_t vmm_create_address_space(address_space_t *out);

/* Free the user-half page tables and the PML4 (not the mapped frames).
 * Refuses the kernel space and the currently active space. */
vmm_status_t vmm_destroy_address_space(address_space_t *as);

/* Load `as` into CR3. */
void vmm_switch(address_space_t *as);

bool vmm_is_canonical(uint64_t virt);
const char *vmm_status_str(vmm_status_t s);

/* Human-readable name of the kernel region containing `virt` (fault reports). */
const char *vmm_describe_address(uint64_t virt);

#endif /* NEXUS_VMM_H */
