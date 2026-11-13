#ifndef BHARAT_ACPI_H
#define BHARAT_ACPI_H

/* ACPI table discovery: what hardware the machine has.
 *
 * The firmware leaves a set of tables in memory describing the machine.
 * Limine hands us the root pointer (RSDP). From it we find the list of
 * tables (XSDT, or RSDT on old firmware) and read three of them:
 *
 *   MADT ("APIC")  the CPUs, the interrupt controllers (local APIC address,
 *                  I/O APICs) and how legacy IRQs map onto I/O APIC pins
 *   HPET           address of the high-precision event timer
 *   MCFG           address of PCI Express configuration space (ECAM)
 *
 * Every table's checksum is verified; a bad table is ignored and reported.
 * Everything is parsed once at boot into `struct acpi_info`.
 *
 * Limits: no AML interpreter (no power buttons, sleep states or
 * device-specific methods); fixed maximum counts below. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ACPI_MAX_CPUS       64
#define ACPI_MAX_IOAPICS    8
#define ACPI_MAX_OVERRIDES  16

struct acpi_cpu {
    uint8_t acpi_id;
    uint8_t apic_id;
    bool enabled;               /* usable now (or may be brought online) */
};

struct acpi_ioapic {
    uint8_t id;
    uint64_t phys;              /* register window */
    uint32_t gsi_base;          /* first global interrupt number it serves */
};

/* "Legacy IRQ n is wired to global interrupt g", with polarity/trigger. */
struct acpi_override {
    uint8_t irq;
    uint32_t gsi;
    bool active_low;
    bool level_triggered;
};

struct acpi_info {
    bool present;
    uint8_t revision;           /* 0 = ACPI 1.0 (RSDT), 2+ = XSDT */
    char oem_id[7];
    unsigned table_count;

    bool madt_present;
    uint64_t lapic_phys;
    bool has_legacy_pics;       /* MADT PCAT_COMPAT flag */
    unsigned cpu_count;
    struct acpi_cpu cpus[ACPI_MAX_CPUS];
    unsigned ioapic_count;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    unsigned override_count;
    struct acpi_override overrides[ACPI_MAX_OVERRIDES];

    bool hpet_present;
    uint64_t hpet_phys;

    bool ecam_present;
    uint64_t ecam_phys;
    uint16_t ecam_segment;
    uint8_t ecam_bus_start, ecam_bus_end;
};

/* Parse the tables. `rsdp` is the address from Limine (physical, or a
 * direct-map virtual address on older protocol revisions); 0 means the
 * firmware gave none, which is reported and leaves acpi_get()->present
 * false. */
void acpi_init(uint64_t rsdp);

const struct acpi_info *acpi_get(void);

/* The global interrupt number a legacy ISA IRQ arrives on, applying the
 * MADT overrides (identity if there is none). */
uint32_t acpi_irq_to_gsi(uint8_t irq, bool *active_low, bool *level);

/* Sum of `len` bytes is 0 mod 256 (the ACPI checksum rule). */
bool acpi_checksum_ok(const void *p, size_t len);

#endif /* BHARAT_ACPI_H */
