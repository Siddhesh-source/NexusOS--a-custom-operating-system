#ifndef BHARAT_APIC_H
#define BHARAT_APIC_H

/* Interrupt controllers of modern PCs.
 *
 *   local APIC  one per CPU: its timer, end-of-interrupt, and (Phase 9)
 *               messages between CPUs
 *   I/O APIC    takes device interrupt lines (keyboard, disk, network...)
 *               and sends each to a chosen vector on a chosen CPU
 *   8259 PIC    the 1980s controller the PC starts with; once the APICs
 *               are up it is fully masked and never used again
 *
 * Addresses come from ACPI's MADT. Device lines are numbered by "global
 * system interrupt" (GSI); acpi_irq_to_gsi() maps old ISA IRQ numbers.
 *
 * Limits: xAPIC (memory-mapped) mode only, not x2APIC; device interrupts
 * go to the boot CPU. */

#include <stdint.h>
#include <stdbool.h>

#define APIC_SPURIOUS_VECTOR 0xFF

/* Mask the 8259 PICs and turn on the local APIC and every I/O APIC (with
 * all their lines masked). Returns false if ACPI did not describe them. */
bool apic_init(void);

/* Turn on the calling CPU's local APIC (apic_init does the boot CPU). */
void apic_init_cpu(void);

/* Send an inter-processor interrupt: `command` is the low word of the
 * interrupt command register (vector | delivery mode | shorthand). */
void lapic_send_ipi(uint32_t apic_id, uint32_t command);

void lapic_eoi(void);
uint32_t lapic_id(void);
uint32_t lapic_read(uint32_t reg);
void lapic_write(uint32_t reg, uint32_t value);

/* Local APIC register offsets used by the clock. */
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_TIMER_INIT  0x380
#define LAPIC_TIMER_COUNT 0x390
#define LAPIC_TIMER_DIV   0x3E0
#define LAPIC_LVT_MASKED  (1u << 16)

/* Send legacy ISA IRQ `irq` (after ACPI overrides) to `vector` on this
 * CPU, unmasked. Returns false if no I/O APIC serves that line. */
bool ioapic_route_irq(uint8_t irq, uint8_t vector);
bool ioapic_mask_irq(uint8_t irq, bool masked);

/* Number of input lines across all I/O APICs. */
unsigned ioapic_line_count(void);

/* True once the PICs are masked (no legacy interrupts at all). */
bool pic_is_masked(void);

#endif /* BHARAT_APIC_H */
