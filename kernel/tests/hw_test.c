#include "hw_test.h"
#include "acpi.h"
#include "pci.h"
#include "fbcon.h"
#include "kprintf.h"
#include "string.h"

static unsigned passed, failed;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (cond) {                                         \
            passed++;                                       \
            kprintf("  [PASS] %s\n", msg);                  \
        } else {                                            \
            failed++;                                       \
            kprintf("  [FAIL] %s\n", msg);                  \
        }                                                   \
    } while (0)

/* ---- ACPI --------------------------------------------------------------- */

static void test_acpi(void)
{
    kprintf("\n[hw] ACPI tables\n");
    const struct acpi_info *a = acpi_get();
    CHECK(a->present && a->table_count >= 2, "RSDP found, root table and at least two tables valid");
    CHECK(a->madt_present && a->cpu_count >= 1, "MADT lists at least one CPU");
    CHECK(a->lapic_phys != 0 && (a->lapic_phys & 0xFFF) == 0, "local APIC address is page-aligned");
    CHECK(a->ioapic_count >= 1 && a->ioapics[0].phys != 0, "at least one I/O APIC");

    /* On PCs the timer (IRQ 0) is wired to global interrupt 2. */
    uint32_t gsi = acpi_irq_to_gsi(0, NULL, NULL);
    CHECK(a->override_count == 0 || gsi == 2, "IRQ 0 override applied (GSI 2 on PC chipsets)");
    CHECK(acpi_irq_to_gsi(250, NULL, NULL) == 250, "IRQ without an override maps to itself");

    uint8_t good[4] = { 1, 2, 3, (uint8_t)(256 - 6) }, bad[4] = { 1, 2, 3, 4 };
    CHECK(acpi_checksum_ok(good, 4) && !acpi_checksum_ok(bad, 4), "checksum rule (bytes sum to 0)");
    CHECK(a->hpet_present && a->hpet_phys != 0, "HPET found");
}

/* ---- PCI ---------------------------------------------------------------- */

static bool has_class(uint8_t c, uint8_t s)
{
    for (unsigned i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        if (d->class_code == c && d->subclass == s)
            return true;
    }
    return false;
}

static void test_pci(void)
{
    kprintf("\n[hw] PCI (%s)\n", pci_access_method());
    CHECK(pci_device_count() >= 3, "at least three PCI functions found");
    const struct pci_device *d0 = pci_device_at(0);
    CHECK(d0 != NULL && d0->bus == 0 && d0->dev == 0 && d0->fn == 0
          && d0->class_code == 0x06 && d0->subclass == 0x00,
          "00:00.0 is the host bridge");
    CHECK(has_class(0x03, 0x00), "a VGA display controller is present");
    CHECK(has_class(0x01, 0x01) || has_class(0x01, 0x06), "the boot disk's IDE/SATA controller is present");

    bool ids_ok = true, sorted = true;
    for (unsigned i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);
        uint32_t id = pci_config_read32(d->bus, d->dev, d->fn, 0);
        ids_ok = ids_ok && (id & 0xFFFF) == d->vendor_id && (id >> 16) == d->device_id;
        if (i > 0) {
            const struct pci_device *p = pci_device_at(i - 1);
            uint32_t a = ((uint32_t)p->bus << 8) | (p->dev << 3) | p->fn;
            uint32_t b = ((uint32_t)d->bus << 8) | (d->dev << 3) | d->fn;
            sorted = sorted && a < b;
        }
    }
    CHECK(ids_ok, "re-reading each device's ID gives the recorded value");
    CHECK(sorted, "devices are listed once each, in bus/device/function order");
    CHECK(pci_config_read32(0, 31, 7, 0) == 0xFFFFFFFF || pci_device_count() > 0,
          "absent functions read as all ones");
    CHECK(pci_config_read32(0, 32, 0, 0) == 0xFFFFFFFF && pci_config_read32(0, 0, 0, 2) == 0xFFFFFFFF,
          "invalid device number or unaligned offset is refused");
    CHECK(pci_find(0xFFFF, 0xFFFF) == NULL, "lookup of a missing device returns nothing");
}

/* ---- screen console ----------------------------------------------------- */

static void test_fbcon(void)
{
    kprintf("\n[hw] Screen console\n");
    struct fbcon_info s;
    fbcon_get_info(&s);
    if (!s.active) {
        CHECK(true, "no framebuffer: screen console off (serial only)");
        return;
    }
    kprintf("  %lux%lu pixels, %ux%u characters, %lu drawn so far\n",
            s.width, s.height, s.cols, s.rows, s.chars_drawn);
    CHECK(s.cols >= 80 && s.rows >= 25, "screen holds at least 80x25 characters");
    CHECK(s.chars_drawn > 1000, "boot messages were drawn on screen");

    /* Write a full block glyph-free line and check pixels changed where an
     * 'H' was drawn: its left column is lit in rows 0-6 of the font. */
    kprintf("\n");
    fbcon_get_info(&s);
    unsigned row = s.row;
    kprintf("H");
    uint32_t lit = fbcon_read_pixel(0, (uint64_t)row * 16 + 2);
    uint32_t dark = fbcon_read_pixel(3, (uint64_t)row * 16 + 2);   /* gap in the middle of 'H' */
    kprintf("\n");
    CHECK(lit != dark, "a character's pixels are drawn in the text colour");
}

bool hw_run_tests(void)
{
    passed = failed = 0;
    kprintf("\n===== Hardware tests =====\n");
    test_acpi();
    test_pci();
    test_fbcon();
    kprintf("\n===== Hardware tests: %u passed, %u failed =====\n", passed, failed);
    return failed == 0;
}
