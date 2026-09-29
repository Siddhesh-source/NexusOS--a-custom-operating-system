#include "vmm.h"
#include "pmm.h"
#include "cpu.h"
#include "kprintf.h"
#include "panic.h"

/* Hardware page-table entry bits */
#define PTE_PRESENT   (1ULL << 0)
#define PTE_WRITE     (1ULL << 1)
#define PTE_USER      (1ULL << 2)
#define PTE_PWT       (1ULL << 3)
#define PTE_PCD       (1ULL << 4)
#define PTE_HUGE      (1ULL << 7)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)
#define PTE_ADDR_MASK 0x000ffffffffff000ULL

#define ENTRIES_PER_TABLE 512
#define KERNEL_PML4_FIRST 256
#define MAX_PHYS_ADDR     (1ULL << 52)

/* Linker-script symbols delimiting the kernel image sections. */
extern char __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[];
extern char __data_start[], __kernel_end[];
extern char stack_bottom[];

static address_space_t kernel_space;
static address_space_t *current_space;
static uint64_t hhdm_limit;          /* end of the highest HHDM-mapped phys */

bool vmm_is_canonical(uint64_t virt)
{
    return (uint64_t)((int64_t)(virt << 16) >> 16) == virt;
}

static inline bool is_kernel_half(uint64_t virt)
{
    return virt >= KERNEL_HALF_BASE;
}

static inline uint64_t *table_at(uint64_t phys)
{
    return phys_to_virt(phys);
}

static uint64_t flags_to_pte(uint32_t flags)
{
    uint64_t pte = PTE_PRESENT;
    if (flags & VMM_WRITE)   pte |= PTE_WRITE;
    if (flags & VMM_USER)    pte |= PTE_USER;
    if (!(flags & VMM_EXEC)) pte |= PTE_NX;
    if (flags & VMM_GLOBAL)  pte |= PTE_GLOBAL;
    if (flags & VMM_NOCACHE) pte |= PTE_PCD | PTE_PWT;
    return pte;
}

static uint32_t pte_to_flags(uint64_t pte)
{
    uint32_t flags = 0;
    if (pte & PTE_WRITE)   flags |= VMM_WRITE;
    if (pte & PTE_USER)    flags |= VMM_USER;
    if (!(pte & PTE_NX))   flags |= VMM_EXEC;
    if (pte & PTE_GLOBAL)  flags |= VMM_GLOBAL;
    if (pte & PTE_PCD)     flags |= VMM_NOCACHE;
    return flags;
}

/* Walk to the level-1 PTE for `virt`, optionally creating missing tables.
 * Intermediate entries are permissive (present, writable, user for the user
 * half); the leaf PTE alone decides the effective permissions. */
static uint64_t *walk(address_space_t *as, uint64_t virt, bool create, vmm_status_t *err)
{
    uint64_t *table = table_at(as->pml4_phys);
    uint64_t inter_flags = PTE_PRESENT | PTE_WRITE | (is_kernel_half(virt) ? 0 : PTE_USER);

    for (int level = 3; level >= 1; level--) {
        uint64_t idx = (virt >> (PAGE_SHIFT + 9 * level)) & (ENTRIES_PER_TABLE - 1);
        uint64_t entry = table[idx];

        if (!(entry & PTE_PRESENT)) {
            if (!create) {
                *err = VMM_ERR_NOT_MAPPED;
                return NULL;
            }
            uint64_t phys = pmm_alloc_zeroed_page();
            if (phys == 0) {
                *err = VMM_ERR_NO_MEMORY;
                return NULL;
            }
            entry = phys | inter_flags;
            table[idx] = entry;
        } else if (entry & PTE_HUGE) {
            /* We never create large pages; finding one means corruption. */
            *err = VMM_ERR_INVALID;
            return NULL;
        }
        table = table_at(entry & PTE_ADDR_MASK);
    }
    return &table[(virt >> PAGE_SHIFT) & (ENTRIES_PER_TABLE - 1)];
}

static void flush_page(address_space_t *as, uint64_t virt)
{
    /* Kernel-half entries are shared by every space; always flush them. */
    if (is_kernel_half(virt) || as->pml4_phys == (read_cr3() & PTE_ADDR_MASK))
        invlpg(virt);
}

static vmm_status_t check_virt(address_space_t *as, uint64_t virt)
{
    if (as == NULL || as->pml4_phys == 0)
        return VMM_ERR_INVALID;
    if (!vmm_is_canonical(virt))
        return VMM_ERR_NONCANONICAL;
    if (!IS_PAGE_ALIGNED(virt))
        return VMM_ERR_UNALIGNED;
    return VMM_OK;
}

static vmm_status_t check_flags(uint64_t virt, uint32_t flags)
{
    if (flags & ~(VMM_WRITE | VMM_USER | VMM_EXEC | VMM_GLOBAL | VMM_NOCACHE))
        return VMM_ERR_INVALID;
    /* User pages live only in the user half; global pages only in the
     * shared kernel half (a global user page would leak across spaces). */
    if ((flags & VMM_USER) && is_kernel_half(virt))
        return VMM_ERR_INVALID;
    if ((flags & VMM_GLOBAL) && !is_kernel_half(virt))
        return VMM_ERR_INVALID;
    return VMM_OK;
}

vmm_status_t vmm_map_page(address_space_t *as, uint64_t virt, uint64_t phys, uint32_t flags)
{
    vmm_status_t st = check_virt(as, virt);
    if (st != VMM_OK)
        return st;
    if (!IS_PAGE_ALIGNED(phys))
        return VMM_ERR_UNALIGNED;
    if (phys >= MAX_PHYS_ADDR)
        return VMM_ERR_INVALID;
    if ((st = check_flags(virt, flags)) != VMM_OK)
        return st;

    uint64_t *pte = walk(as, virt, true, &st);
    if (pte == NULL)
        return st;
    if (*pte & PTE_PRESENT)
        return VMM_ERR_ALREADY_MAPPED;

    *pte = phys | flags_to_pte(flags);
    flush_page(as, virt);
    return VMM_OK;
}

vmm_status_t vmm_unmap_page(address_space_t *as, uint64_t virt, uint64_t *old_phys)
{
    vmm_status_t st = check_virt(as, virt);
    if (st != VMM_OK)
        return st;

    uint64_t *pte = walk(as, virt, false, &st);
    if (pte == NULL)
        return st;
    if (!(*pte & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;

    if (old_phys != NULL)
        *old_phys = *pte & PTE_ADDR_MASK;
    *pte = 0;
    flush_page(as, virt);
    return VMM_OK;
}

vmm_status_t vmm_translate(address_space_t *as, uint64_t virt, uint64_t *phys)
{
    if (as == NULL || as->pml4_phys == 0)
        return VMM_ERR_INVALID;
    if (!vmm_is_canonical(virt))
        return VMM_ERR_NONCANONICAL;

    vmm_status_t st;
    uint64_t *pte = walk(as, PAGE_ALIGN_DOWN(virt), false, &st);
    if (pte == NULL)
        return st;
    if (!(*pte & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;

    if (phys != NULL)
        *phys = (*pte & PTE_ADDR_MASK) | (virt & PAGE_MASK);
    return VMM_OK;
}

vmm_status_t vmm_get_flags(address_space_t *as, uint64_t virt, uint32_t *flags)
{
    vmm_status_t st = check_virt(as, virt);
    if (st != VMM_OK)
        return st;

    uint64_t *pte = walk(as, virt, false, &st);
    if (pte == NULL)
        return st;
    if (!(*pte & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;

    *flags = pte_to_flags(*pte);
    return VMM_OK;
}

vmm_status_t vmm_protect_page(address_space_t *as, uint64_t virt, uint32_t flags)
{
    vmm_status_t st = check_virt(as, virt);
    if (st != VMM_OK)
        return st;
    if ((st = check_flags(virt, flags)) != VMM_OK)
        return st;

    uint64_t *pte = walk(as, virt, false, &st);
    if (pte == NULL)
        return st;
    if (!(*pte & PTE_PRESENT))
        return VMM_ERR_NOT_MAPPED;

    *pte = (*pte & PTE_ADDR_MASK) | flags_to_pte(flags);
    flush_page(as, virt);
    return VMM_OK;
}

vmm_status_t vmm_map_range(address_space_t *as, uint64_t virt, uint64_t phys,
                           uint64_t size, uint32_t flags)
{
    uint64_t pages = PAGE_ALIGN_UP(size) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        vmm_status_t st = vmm_map_page(as, virt + i * PAGE_SIZE, phys + i * PAGE_SIZE, flags);
        if (st != VMM_OK) {
            while (i-- > 0)
                vmm_unmap_page(as, virt + i * PAGE_SIZE, NULL);
            return st;
        }
    }
    return VMM_OK;
}

/* ---- address spaces ---------------------------------------------------- */

vmm_status_t vmm_create_address_space(address_space_t *out)
{
    if (out == NULL)
        return VMM_ERR_INVALID;

    uint64_t pml4 = pmm_alloc_zeroed_page();
    if (pml4 == 0)
        return VMM_ERR_NO_MEMORY;

    uint64_t *dst = table_at(pml4);
    uint64_t *src = table_at(kernel_space.pml4_phys);
    for (int i = KERNEL_PML4_FIRST; i < ENTRIES_PER_TABLE; i++)
        dst[i] = src[i];

    out->pml4_phys = pml4;
    return VMM_OK;
}

/* Free a page-table page and, recursively, the tables below it. Leaf frames
 * (level 1 entries) belong to whoever mapped them and are not freed. */
static void free_table(uint64_t phys, int level)
{
    if (level > 1) {
        uint64_t *t = table_at(phys);
        for (int i = 0; i < ENTRIES_PER_TABLE; i++) {
            if ((t[i] & PTE_PRESENT) && !(t[i] & PTE_HUGE))
                free_table(t[i] & PTE_ADDR_MASK, level - 1);
        }
    }
    pmm_free_page(phys);
}

vmm_status_t vmm_destroy_address_space(address_space_t *as)
{
    if (as == NULL || as->pml4_phys == 0 || as == &kernel_space
        || as->pml4_phys == kernel_space.pml4_phys
        || as->pml4_phys == (read_cr3() & PTE_ADDR_MASK))
        return VMM_ERR_INVALID;

    uint64_t *pml4 = table_at(as->pml4_phys);
    for (int i = 0; i < KERNEL_PML4_FIRST; i++) {
        if (pml4[i] & PTE_PRESENT)
            free_table(pml4[i] & PTE_ADDR_MASK, 3);
    }
    pmm_free_page(as->pml4_phys);
    as->pml4_phys = 0;
    return VMM_OK;
}

void vmm_switch(address_space_t *as)
{
    write_cr3(as->pml4_phys);
    current_space = as;
}

address_space_t *vmm_kernel_space(void)
{
    return &kernel_space;
}

address_space_t *vmm_current_space(void)
{
    return current_space;
}

/* ---- boot-time construction -------------------------------------------- */

static void map_or_panic(uint64_t virt, uint64_t phys, uint64_t size, uint32_t flags,
                         const char *what)
{
    vmm_status_t st = vmm_map_range(&kernel_space, virt, phys, size, flags);
    if (st != VMM_OK) {
        kprintf("[vmm] failed to map %s: %s\n", what, vmm_status_str(st));
        panic("vmm: cannot build kernel page tables");
    }
}

static void map_kernel_section(const struct boot_mem_info *info, const char *name,
                               uint64_t start, uint64_t end, uint32_t flags)
{
    start = PAGE_ALIGN_DOWN(start);
    end = PAGE_ALIGN_UP(end);
    uint64_t phys = start - info->kernel_virt_base + info->kernel_phys_base;
    map_or_panic(start, phys, end - start, flags | VMM_GLOBAL, name);
    kprintf("[vmm]   %-7s %p - %p -> phys %p  %c%c%c\n", name,
            (void *)start, (void *)end, (void *)phys,
            'R', (flags & VMM_WRITE) ? 'W' : '-', (flags & VMM_EXEC) ? 'X' : '-');
}

/* Physical regions reachable through the HHDM. Reserved/bad memory and the
 * holes between regions are deliberately left unmapped, as is the kernel
 * image (reachable only through its own, permission-checked mapping). */
static bool region_in_hhdm(enum mem_region_type t)
{
    return t == MEM_USABLE || t == MEM_BOOTLOADER_RECLAIMABLE
        || t == MEM_ACPI_RECLAIMABLE || t == MEM_ACPI_NVS
        || t == MEM_FRAMEBUFFER;
}

static void map_hhdm(const struct boot_mem_info *info)
{
    uint64_t mapped = 0;
    for (size_t i = 0; i < info->region_count; i++) {
        const struct mem_region *r = &info->regions[i];
        if (!region_in_hhdm(r->type))
            continue;

        uint32_t flags = VMM_WRITE | VMM_GLOBAL;
        if (r->type == MEM_FRAMEBUFFER)
            flags |= VMM_NOCACHE;

        /* Non-usable regions need not be page-aligned; round outward and
         * skip pages a neighbouring region already mapped. */
        uint64_t start = PAGE_ALIGN_DOWN(r->base);
        uint64_t end = PAGE_ALIGN_UP(r->base + r->length);
        for (uint64_t p = start; p < end; p += PAGE_SIZE) {
            vmm_status_t st = vmm_map_page(&kernel_space, p + hhdm_offset, p, flags);
            if (st == VMM_ERR_ALREADY_MAPPED)
                continue;
            if (st != VMM_OK) {
                kprintf("[vmm] HHDM map of phys %p failed: %s\n", (void *)p, vmm_status_str(st));
                panic("vmm: cannot build HHDM");
            }
            mapped += PAGE_SIZE;
        }
        if (end > hhdm_limit)
            hhdm_limit = end;
    }
    kprintf("[vmm]   HHDM    %p - %p  RW-  (%lu KiB of RAM-backed memory)\n",
            (void *)hhdm_offset, (void *)(hhdm_offset + hhdm_limit), mapped / 1024);
}

void vmm_init(const struct boot_mem_info *info)
{
    uint64_t free_before = pmm_free_page_count();

    kernel_space.pml4_phys = pmm_alloc_zeroed_page();
    if (kernel_space.pml4_phys == 0)
        panic("vmm: no memory for kernel PML4");

    /* Pre-allocate every kernel-half PDPT so that PML4 entries 256-511 never
     * change after boot and can be shared by all future address spaces. */
    uint64_t *pml4 = table_at(kernel_space.pml4_phys);
    for (int i = KERNEL_PML4_FIRST; i < ENTRIES_PER_TABLE; i++) {
        uint64_t pdpt = pmm_alloc_zeroed_page();
        if (pdpt == 0)
            panic("vmm: no memory for kernel PDPTs");
        pml4[i] = pdpt | PTE_PRESENT | PTE_WRITE;
    }

    kprintf("[vmm] Building kernel page tables (PML4 at phys %p)\n",
            (void *)kernel_space.pml4_phys);
    map_kernel_section(info, ".text", (uint64_t)__text_start, (uint64_t)__text_end, VMM_EXEC);
    map_kernel_section(info, ".rodata", (uint64_t)__rodata_start, (uint64_t)__rodata_end, 0);
    map_kernel_section(info, ".data", (uint64_t)__data_start, (uint64_t)__kernel_end, VMM_WRITE);
    map_hhdm(info);

    /* NX and supervisor write-protect must be on for the permissions above
     * to mean anything; global pages keep kernel TLB entries across CR3
     * switches. Limine normally enables NXE/WP already. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE);
    write_cr0(read_cr0() | CR0_WP);
    write_cr4(read_cr4() | CR4_PGE);

    vmm_switch(&kernel_space);
    /* Toggling PGE flushes global TLB entries left over from Limine's tables. */
    write_cr4(read_cr4() & ~CR4_PGE);
    write_cr4(read_cr4() | CR4_PGE);

    /* Guard page: unmap the lowest page of the boot stack so an overflow
     * faults (and escalates to #DF on the IST stack) instead of silently
     * corrupting whatever lies below it in .bss. */
    vmm_unmap_page(&kernel_space, (uint64_t)stack_bottom, NULL);

    kprintf("[vmm] Switched to kernel page tables (CR3=%p), %lu page-table pages used\n",
            (void *)read_cr3(), free_before - pmm_free_page_count());
    kprintf("[vmm] Boot stack guard page at %p\n", (void *)stack_bottom);
}

/* ---- diagnostics ------------------------------------------------------- */

const char *vmm_describe_address(uint64_t virt)
{
    if (!vmm_is_canonical(virt))
        return "non-canonical address";
    if (virt < PAGE_SIZE)
        return "null page";
    if (!is_kernel_half(virt))
        return "user space";
    if (virt >= PAGE_ALIGN_DOWN((uint64_t)stack_bottom)
        && virt < PAGE_ALIGN_DOWN((uint64_t)stack_bottom) + PAGE_SIZE)
        return "boot stack guard page";
    if (virt >= (uint64_t)__text_start && virt < PAGE_ALIGN_UP((uint64_t)__text_end))
        return "kernel .text";
    if (virt >= (uint64_t)__rodata_start && virt < PAGE_ALIGN_UP((uint64_t)__rodata_end))
        return "kernel .rodata";
    if (virt >= (uint64_t)__data_start && virt < PAGE_ALIGN_UP((uint64_t)__kernel_end))
        return "kernel .data/.bss";
    if (virt >= KERNEL_HEAP_BASE && virt < KERNEL_HEAP_BASE + KERNEL_HEAP_MAX)
        return "kernel heap";
    if (virt >= MM_TEST_VBASE && virt < MM_TEST_VBASE + (1ULL << 39))
        return "memory-test window";
    if (virt >= hhdm_offset && virt < hhdm_offset + hhdm_limit)
        return "HHDM (direct physical map)";
    return "unmapped kernel space";
}

const char *vmm_status_str(vmm_status_t s)
{
    switch (s) {
    case VMM_OK:                 return "ok";
    case VMM_ERR_UNALIGNED:      return "address not page-aligned";
    case VMM_ERR_NONCANONICAL:   return "non-canonical virtual address";
    case VMM_ERR_ALREADY_MAPPED: return "page already mapped";
    case VMM_ERR_NOT_MAPPED:     return "page not mapped";
    case VMM_ERR_NO_MEMORY:      return "out of physical memory for page tables";
    case VMM_ERR_INVALID:        return "invalid argument";
    }
    return "unknown";
}
