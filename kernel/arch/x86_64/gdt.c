#include "gdt.h"
#include "serial.h"
#include <stdint.h>

/* GDT entries: slots 0-2 are 8-byte code/data descriptors; slot 3 onward
 * holds the 16-byte long-mode TSS descriptor. Index 4 is reserved as the
 * upper half of that 16-byte descriptor (unused, must read as zero). */
static struct gdt_entry gdt_entries[5];
static struct gdt_ptr gdt_ptr;

/* TSS structure */
struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;  /* 8-byte reserved at 0x1C (AMD64 TSS layout) */
    uint64_t ist1;       /* IST1 for double fault (at 0x24) */
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;  /* 8-byte reserved at 0x5C */
    uint16_t reserved3;  /* 2-byte reserved at 0x64 (fixed part = 0x68 bytes) */
    uint16_t io_map_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss_entry) == 0x68, "64-bit TSS must be 104 bytes");

/* IST stack for double fault (4KB aligned) */
static uint8_t ist1_stack[4096] __attribute__((aligned(4096)));
static struct tss_entry tss_entry;

void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt_entries[num].base_low = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low = (limit & 0xFFFF);
    gdt_entries[num].granularity = ((limit >> 16) & 0x0F);

    gdt_entries[num].granularity |= (gran & 0xF0);
    gdt_entries[num].access = access;
}

/* Write a 16-byte long-mode system-segment descriptor (for the 64-bit TSS).
 * Writes to slot `num` AND slot `num+1` (the upper 8 bytes), since a long-mode
 * system descriptor is two consecutive 8-byte GDT entries. */
static void gdt_set_tss_gate(int num, uint64_t base, uint32_t limit, uint8_t access)
{
    struct gdt_entry_long *desc = (struct gdt_entry_long *)&gdt_entries[num];
    desc->limit_low   = (uint16_t)(limit & 0xFFFF);
    desc->base_low    = (uint16_t)(base & 0xFFFF);
    desc->base_middle = (uint8_t)((base >> 16) & 0xFF);
    desc->access      = access;                 /* 0x89: Present | 64-bit Available TSS */
    desc->granularity = (uint8_t)((limit >> 16) & 0x0F); /* G=0, L=0 (system seg) */
    desc->base_high   = (uint8_t)((base >> 24) & 0xFF);
    desc->base_upper  = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    desc->zero        = 0;
}

void gdt_init(void)
{
    /* Setup GDT pointer: covers 5 entries (slots 0..4), the last slot being
     * the upper half of the 16-byte TSS descriptor. */
    gdt_ptr.limit = (sizeof(struct gdt_entry) * 5) - 1;
    gdt_ptr.base = (uint64_t)&gdt_entries;

    /* Null descriptor */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* Kernel code segment */
    /* 64-bit code segment: base=0, limit=0xFFFFF, access=0x9A, granularity=0xBE (G=1, D/B=0, L=1, limit high=0xF) */
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xBE); /* executable, readable */

    /* Kernel data segment */
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xCF); /* writable */

    /* TSS descriptor (long-mode, 16-byte system descriptor at index 3) */
    uint64_t tss_base = (uint64_t)&tss_entry;
    uint32_t tss_limit = sizeof(struct tss_entry) - 1;
    /* access=0x89: Present | 64-bit Available TSS. Limit is byte-granular. */
    gdt_set_tss_gate(3, tss_base, tss_limit, 0x89);

    /* Initialize TSS */
    /* Zero the TSS */
    for (unsigned i = 0; i < sizeof(tss_entry); i++) {
        ((uint8_t*)&tss_entry)[i] = 0;
    }
    /* Set IST1 to point to the top of the IST1 stack */
    tss_entry.ist1 = (uint64_t)&ist1_stack + sizeof(ist1_stack);

    /* Flush the old GDT and install the new one */
    gdt_flush((uint64_t)&gdt_ptr);

    /* Load the TSS selector into TR */
    gdt_load_tr(GDT_TSS_SEL);

    serial_puts("[gdt] GDT and TSS initialized.\n");
}

/* Load the GDT and reload every segment register. Limine leaves CS=0x28 and
 * SS/DS=0x30, which lie beyond our GDT limit; their cached descriptors keep
 * working until the first iretq reloads them from the new GDT and #GPs. */
void gdt_flush(uint64_t ptr)
{
    __asm__ volatile(
        "lgdt (%0)\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw %2, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        : : "r"(ptr), "i"(GDT_CODE_SEL), "i"(GDT_DATA_SEL)
        : "rax", "memory");
}

void gdt_load_tr(uint16_t selector)
{
    __asm__ volatile("ltr %0" : : "r"(selector));
}