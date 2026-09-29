#ifndef NEXUS_IDT_H
#define NEXUS_IDT_H

#include <stdint.h>

/* IDT entry structure */
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

/* IDT pointer */
struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* IDT entry types */
#define IDT_TASK_GATE   0x5
#define IDT_INTERRUPT_8 0x6
#define IDT_TRAP_8      0x7
#define IDT_INTERRUPT_16 0xE
#define IDT_TRAP_16     0xF

/* IDT entry attributes */
#define IDT_ATTR_PRESENT    0x80
#define IDT_ATTR_RING0      0x00
#define IDT_ATTR_RING1      0x20
#define IDT_ATTR_RING2      0x40
#define IDT_ATTR_RING3      0x60
#define IDT_ATTR_IST_SHIFT  0

/* Standard interrupt vectors */
#define IRQ0  32
#define IRQ1  33
#define IRQ2  34
#define IRQ3  35
#define IRQ4  36
#define IRQ5  37
#define IRQ6  38
#define IRQ7  39
#define IRQ8  40
#define IRQ9  41
#define IRQ10 42
#define IRQ11 43
#define IRQ12 44
#define IRQ13 45
#define IRQ14 46
#define IRQ15 47

/* Exception vectors */
#define DE_VECTOR      0  /* Divide Error */
#define DB_VECTOR      1  /* Debug */
#define NMI_VECTOR     2  /* Non-Maskable Interrupt */
#define BP_VECTOR      3  /* Breakpoint */
#define OF_VECTOR      4  /* Overflow */
#define BR_VECTOR      5  /* BOUND Range Exceeded */
#define UD_VECTOR      6  /* Invalid Opcode */
#define NM_VECTOR      7  /* Device Not Available */
#define DF_VECTOR      8  /* Double Fault */
#define TS_VECTOR     10  /* Invalid TSS */
#define NP_VECTOR     11  /* Segment Not Present */
#define SS_VECTOR     12  /* Stack Segment Fault */
#define GP_VECTOR     13  /* General Protection */
#define PF_VECTOR     14  /* Page Fault */
#define MF_VECTOR     16  /* x87 FPU Floating-Point Error */
#define AC_VECTOR     17  /* Alignment Check */
#define MC_VECTOR     18  /* Machine Check */
#define XM_VECTOR     19  /* SIMD Floating-Point Exception */
#define VE_VECTOR     20  /* Virtualization Exception */
#define CP_VECTOR     21  /* Control Protection Exception */

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t ist, uint8_t flags);
void idt_flush(uint64_t);

#endif /* NEXUS_IDT_H */