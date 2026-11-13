#include "idt.h"
#include "interrupts.h"
#include "serial.h"
#include "gdt.h"

static struct idt_entry idt_entries[256];
static struct idt_ptr idt_ptr;

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t ist, uint8_t flags)
{
    idt_entries[num].offset_low = (base & 0xFFFF);
    idt_entries[num].selector = sel;
    idt_entries[num].ist = ist;
    idt_entries[num].type_attr = flags;
    idt_entries[num].offset_middle = (base >> 16) & 0xFFFF;
    idt_entries[num].offset_high = (base >> 32) & 0xFFFFFFFF;
    idt_entries[num].zero = 0;
}

void idt_init(void)
{
    /* Setup IDT pointer */
    idt_ptr.limit = (sizeof(struct idt_entry) * 256) - 1;
    idt_ptr.base = (uint64_t)&idt_entries;

    /* Clear the entire IDT */
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0, 0);
    }

    /* All CPU exceptions (0-31) and the timer IRQ (32) use 64-bit interrupt
     * gates (type 0xE; 0x6 is not a valid gate type in long mode). */
    for (int v = 0; v < ISR_STUB_COUNT; v++) {
        /* Double fault runs on its own IST1 stack so a corrupt/overflowed
         * kernel stack still produces a report instead of a triple fault. */
        uint8_t ist = (v == DF_VECTOR) ? 1 : 0;
        idt_set_gate(v, isr_stub_table[v], GDT_CODE_SEL, ist,
                     IDT_ATTR_PRESENT | IDT_INTERRUPT_16 | IDT_ATTR_RING0);
    }

    /* Load IDT */
    idt_flush((uint64_t)&idt_ptr);

    serial_puts("[idt] IDT initialized.\n");
}

void idt_flush(uint64_t ptr)
{
    __asm__ volatile("lidt (%0)" : : "r"(ptr));
}
