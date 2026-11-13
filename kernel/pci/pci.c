#include "pci.h"
#include "spinlock.h"
#include "acpi.h"
#include "vmm.h"
#include "cpu.h"
#include "kprintf.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static struct pci_device devices[PCI_MAX_DEVICES];
static unsigned device_count;
static bool use_ecam;
static unsigned skipped;           /* functions found beyond the table size */

const char *pci_access_method(void)
{
    return use_ecam ? "ECAM" : "I/O ports";
}

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t offset)
{
    if (dev >= 32 || fn >= 8 || (offset & 3))
        return 0xFFFFFFFF;
    const struct acpi_info *a = acpi_get();
    if (use_ecam) {
        if (bus < a->ecam_bus_start || bus > a->ecam_bus_end)
            return 0xFFFFFFFF;
        uint64_t phys = a->ecam_phys + (((uint64_t)(bus - a->ecam_bus_start) << 20)
                                        | ((uint64_t)dev << 15) | ((uint64_t)fn << 12));
        volatile uint32_t *cfg = vmm_map_phys(phys, 4096, VMM_WRITE | VMM_NOCACHE);
        return cfg != NULL ? cfg[offset / 4] : 0xFFFFFFFF;
    }
    /* The address and data ports are one shared pair: keep the two
     * accesses together, even against other CPUs. */
    static struct spinlock pci_lock = SPINLOCK_INIT(LOCK_CLASS_PCI);
    SPIN_GUARD(&pci_lock);
    outl(PCI_CONFIG_ADDRESS, (1u << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                             | ((uint32_t)fn << 8) | offset);
    return inl(PCI_CONFIG_DATA);
}

static void record(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t id)
{
    if (device_count == PCI_MAX_DEVICES) {
        skipped++;
        return;
    }
    struct pci_device *d = &devices[device_count++];
    uint32_t class_reg = pci_config_read32(bus, dev, fn, 0x08);
    uint32_t hdr_reg = pci_config_read32(bus, dev, fn, 0x0C);
    uint32_t irq_reg = pci_config_read32(bus, dev, fn, 0x3C);
    d->bus = bus;
    d->dev = dev;
    d->fn = fn;
    d->vendor_id = id & 0xFFFF;
    d->device_id = id >> 16;
    d->revision = class_reg & 0xFF;
    d->prog_if = (class_reg >> 8) & 0xFF;
    d->subclass = (class_reg >> 16) & 0xFF;
    d->class_code = class_reg >> 24;
    d->header_type = (hdr_reg >> 16) & 0x7F;
    d->irq_line = irq_reg & 0xFF;
    d->irq_pin = (irq_reg >> 8) & 0xFF;
    /* Type 0 headers have 6 BARs, bridges 2. */
    unsigned bars = d->header_type == 0 ? 6 : d->header_type == 1 ? 2 : 0;
    for (unsigned i = 0; i < 6; i++)
        d->bar[i] = i < bars ? pci_config_read32(bus, dev, fn, (uint8_t)(0x10 + 4 * i)) : 0;
}

void pci_init(void)
{
    const struct acpi_info *a = acpi_get();
    use_ecam = a->present && a->ecam_present;
    device_count = 0;
    skipped = 0;

    /* Check every slot. Simple and complete (it also finds devices behind
     * bridges, since their buses are numbered); 8,192 reads at boot. */
    for (unsigned bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = pci_config_read32((uint8_t)bus, dev, 0, 0);
            if ((id & 0xFFFF) == 0xFFFF)
                continue;
            record((uint8_t)bus, dev, 0, id);
            /* Bit 7 of the header type: the device has functions 1-7. */
            bool multi = (pci_config_read32((uint8_t)bus, dev, 0, 0x0C) >> 23) & 1;
            for (uint8_t fn = 1; multi && fn < 8; fn++) {
                id = pci_config_read32((uint8_t)bus, dev, fn, 0);
                if ((id & 0xFFFF) != 0xFFFF)
                    record((uint8_t)bus, dev, fn, id);
            }
        }
    }
    kprintf("[pci] %u device(s) found via %s%s\n", device_count, pci_access_method(),
            skipped ? " (table full, some not recorded)" : "");
    pci_print_devices();
}

unsigned pci_device_count(void)
{
    return device_count;
}

const struct pci_device *pci_device_at(unsigned i)
{
    return i < device_count ? &devices[i] : NULL;
}

const struct pci_device *pci_find(uint16_t vendor, uint16_t device)
{
    for (unsigned i = 0; i < device_count; i++)
        if (devices[i].vendor_id == vendor && devices[i].device_id == device)
            return &devices[i];
    return NULL;
}

const char *pci_class_name(uint8_t c, uint8_t s)
{
    switch (c) {
    case 0x01:
        switch (s) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "storage controller";
        }
    case 0x02: return s == 0x00 ? "Ethernet controller" : "network controller";
    case 0x03: return s == 0x00 ? "VGA display" : "display controller";
    case 0x04: return "multimedia device";
    case 0x05: return "memory controller";
    case 0x06:
        switch (s) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        case 0x80: return "bridge";
        default:   return "bridge";
        }
    case 0x07: return "communication controller";
    case 0x08: return "system peripheral";
    case 0x0C: return s == 0x03 ? "USB controller" : "serial bus controller";
    default:   return "other device";
    }
}

void pci_print_devices(void)
{
    for (unsigned i = 0; i < device_count; i++) {
        const struct pci_device *d = &devices[i];
        kprintf("[pci]   %02x:%02x.%x  %04x:%04x  class %02x.%02x  %s",
                d->bus, d->dev, d->fn, d->vendor_id, d->device_id,
                d->class_code, d->subclass, pci_class_name(d->class_code, d->subclass));
        if (d->irq_pin)
            kprintf("  (IRQ %u)", d->irq_line);
        kprintf("\n");
    }
}
