#ifndef BHARAT_PCI_H
#define BHARAT_PCI_H

/* PCI device discovery.
 *
 * Every PCI device has a small "configuration space" holding its vendor
 * and device IDs, its class (storage, network, display, ...) and where
 * its registers are. We read it in one of two ways:
 *   - ECAM: memory-mapped, 4 KiB per function, when ACPI's MCFG table
 *     gives its address (PCI Express machines, QEMU `-M q35`);
 *   - the legacy I/O ports 0xCF8/0xCFC otherwise (QEMU's default `pc`).
 *
 * pci_init() checks every bus/device/function once and keeps a table of
 * what it found, printed at boot. Drivers (Phase 13) will look devices up
 * here.
 *
 * Limits: segment group 0 only; at most PCI_MAX_DEVICES functions; BARs
 * are recorded raw (sizes are not probed yet). */

#include <stdint.h>
#include <stdbool.h>

#define PCI_MAX_DEVICES 64

struct pci_device {
    uint8_t bus, dev, fn;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t header_type;        /* 0 = device, 1 = PCI-to-PCI bridge */
    uint8_t irq_line, irq_pin;
    uint32_t bar[6];
};

void pci_init(void);

/* How configuration space is being read: "ECAM" or "I/O ports". */
const char *pci_access_method(void);

unsigned pci_device_count(void);
const struct pci_device *pci_device_at(unsigned i);
const struct pci_device *pci_find(uint16_t vendor, uint16_t device);

/* Raw configuration-space read (offset is 4-byte aligned, < 256). Returns
 * 0xFFFFFFFF for a function that does not exist. */
uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset);

/* Short English name of a class/subclass, e.g. "IDE controller". */
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

void pci_print_devices(void);

#endif /* BHARAT_PCI_H */
