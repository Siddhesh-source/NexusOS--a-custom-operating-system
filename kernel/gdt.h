#ifndef NEXUS_GDT_H
#define NEXUS_GDT_H

#include <stdint.h>

/* GDT descriptor */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

/* 16-byte long-mode system-segment descriptor (used for the 64-bit TSS) */
struct gdt_entry_long {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;   /* [7:4] flags, [3:0] limit high */
    uint8_t  base_high;
    uint32_t base_upper;     /* base[63:32] */
    uint32_t zero;           /* must be 0, type nibble 0 for 64-bit system seg */
} __attribute__((packed));

/* GDT pointer */
struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* GDT access flags */
#define GDT_ACCESS_PRESENT      0x00
#define GDT_CODE        0x9A  /* executable, readable, accessed */
#define GDT_DATA        0x92  /* writable, accessed */
#define GDT_TSS         0x89  /* available TSS */
#define GDT_LDT         0x82  /* LDT */

/* GDT granularity flags */
#define GDT_GRAN_4KB    0x80  /* 4KB granularity */
#define GDT_GRAN_BYTE   0x00  /* byte granularity */
#define GDT_GRAN_32BIT  0x40  /* 32-bit protected mode */
#define GDT_GRAN_16BIT  0x00  /* 16-bit protected mode */

/* Standard GDT entries */
#define GDT_NULL_SEL    0x00
#define GDT_CODE_SEL    0x08
#define GDT_DATA_SEL    0x10
#define GDT_TSS_SEL     0x18  /* Third descriptor, index 3 */

void gdt_init(void);
void gdt_flush(uint64_t);
void gdt_load_tr(uint16_t selector);

#endif /* NEXUS_GDT_H */