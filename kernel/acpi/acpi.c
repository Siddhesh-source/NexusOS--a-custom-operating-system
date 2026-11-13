#include "acpi.h"
#include "vmm.h"
#include "kprintf.h"
#include "string.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

struct rsdp {
    char signature[8];          /* "RSD PTR " */
    uint8_t checksum;           /* covers the first 20 bytes */
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_phys;
    /* ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt_phys;
    uint8_t ext_checksum;       /* covers all `length` bytes */
    uint8_t reserved[3];
} __attribute__((packed));

struct sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt {
    struct sdt_header h;
    uint32_t lapic_phys;
    uint32_t flags;             /* bit 0: legacy 8259 PICs present */
} __attribute__((packed));

struct hpet_table {
    struct sdt_header h;
    uint32_t block_id;
    uint8_t space_id, bit_width, bit_offset, access_size;
    uint64_t address;
} __attribute__((packed));

struct mcfg_entry {
    uint64_t base;
    uint16_t segment;
    uint8_t bus_start, bus_end;
    uint32_t reserved;
} __attribute__((packed));

static struct acpi_info info;

const struct acpi_info *acpi_get(void)
{
    return &info;
}

bool acpi_checksum_ok(const void *p, size_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += b[i];
    return sum == 0;
}

/* Map a table: first its header (to learn the length), then all of it. */
static const struct sdt_header *map_table(uint64_t phys)
{
    const struct sdt_header *h = vmm_map_phys(phys, sizeof(*h), 0);
    if (h == NULL || h->length < sizeof(*h) || h->length > (1u << 20))
        return NULL;
    return vmm_map_phys(phys, h->length, 0);
}

static void parse_madt(const struct madt *m)
{
    info.madt_present = true;
    info.lapic_phys = m->lapic_phys;
    info.has_legacy_pics = m->flags & 1;

    const uint8_t *p = (const uint8_t *)(m + 1);
    const uint8_t *end = (const uint8_t *)m + m->h.length;
    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        uint8_t type = p[0], len = p[1];
        switch (type) {
        case 0:   /* processor local APIC */
            if (len >= 8 && info.cpu_count < ACPI_MAX_CPUS) {
                uint32_t flags;
                memcpy(&flags, p + 4, 4);
                /* bit 0: enabled; bit 1: can be enabled later */
                if (flags & 3) {
                    struct acpi_cpu *c = &info.cpus[info.cpu_count++];
                    c->acpi_id = p[2];
                    c->apic_id = p[3];
                    c->enabled = flags & 1;
                }
            }
            break;
        case 1:   /* I/O APIC */
            if (len >= 12 && info.ioapic_count < ACPI_MAX_IOAPICS) {
                struct acpi_ioapic *io = &info.ioapics[info.ioapic_count++];
                uint32_t addr;
                io->id = p[2];
                memcpy(&addr, p + 4, 4);
                memcpy(&io->gsi_base, p + 8, 4);
                io->phys = addr;
            }
            break;
        case 2:   /* interrupt source override */
            if (len >= 10 && info.override_count < ACPI_MAX_OVERRIDES) {
                struct acpi_override *o = &info.overrides[info.override_count++];
                uint16_t flags;
                o->irq = p[3];
                memcpy(&o->gsi, p + 4, 4);
                memcpy(&flags, p + 8, 2);
                /* polarity bits 0-1: 3 = active low; trigger bits 2-3: 3 = level */
                o->active_low = (flags & 3) == 3;
                o->level_triggered = ((flags >> 2) & 3) == 3;
            }
            break;
        case 5:   /* 64-bit local APIC address override */
            if (len >= 12)
                memcpy(&info.lapic_phys, p + 4, 8);
            break;
        default:  /* NMI wiring, x2APIC entries, ...: not needed yet */
            break;
        }
        p += len;
    }
}

static void parse_table(uint64_t phys)
{
    const struct sdt_header *h = map_table(phys);
    if (h == NULL)
        return;
    char sig[5] = { h->signature[0], h->signature[1], h->signature[2], h->signature[3], 0 };
    if (!acpi_checksum_ok(h, h->length)) {
        kprintf("[acpi] table %s at %p has a bad checksum; ignored\n", sig, (void *)phys);
        return;
    }
    info.table_count++;
    kprintf("[acpi]   %s at %p, %u bytes\n", sig, (void *)phys, h->length);

    if (memcmp(sig, "APIC", 4) == 0 && h->length >= sizeof(struct madt)) {
        parse_madt((const struct madt *)h);
    } else if (memcmp(sig, "HPET", 4) == 0 && h->length >= sizeof(struct hpet_table)) {
        const struct hpet_table *t = (const struct hpet_table *)h;
        if (t->space_id == 0) {             /* memory-mapped */
            info.hpet_present = true;
            info.hpet_phys = t->address;
        }
    } else if (memcmp(sig, "MCFG", 4) == 0
               && h->length >= sizeof(*h) + 8 + sizeof(struct mcfg_entry)) {
        /* Only segment group 0 is used; one PCI segment is plenty here. */
        const struct mcfg_entry *e =
            (const struct mcfg_entry *)((const uint8_t *)h + sizeof(*h) + 8);
        if (e->segment == 0) {
            info.ecam_present = true;
            info.ecam_phys = e->base;
            info.ecam_segment = e->segment;
            info.ecam_bus_start = e->bus_start;
            info.ecam_bus_end = e->bus_end;
        }
    }
}

void acpi_init(uint64_t rsdp_addr)
{
    memset(&info, 0, sizeof(info));
    if (rsdp_addr == 0) {
        kprintf("[acpi] firmware provided no ACPI tables\n");
        return;
    }
    /* Older Limine revisions give a direct-map pointer, newer ones a
     * physical address; accept both. */
    uint64_t phys = rsdp_addr >= hhdm_offset ? rsdp_addr - hhdm_offset : rsdp_addr;
    const struct rsdp *r = vmm_map_phys(phys, sizeof(struct rsdp), 0);
    if (r == NULL || memcmp(r->signature, "RSD PTR ", 8) != 0 || !acpi_checksum_ok(r, 20)) {
        kprintf("[acpi] RSDP at %p is not valid; ACPI ignored\n", (void *)phys);
        return;
    }

    bool use_xsdt = r->revision >= 2 && r->xsdt_phys != 0
                    && acpi_checksum_ok(r, r->length < sizeof(*r) ? sizeof(*r) : r->length);
    uint64_t root_phys = use_xsdt ? r->xsdt_phys : r->rsdt_phys;
    const struct sdt_header *root = map_table(root_phys);
    if (root == NULL || !acpi_checksum_ok(root, root->length)) {
        kprintf("[acpi] root table at %p is not valid; ACPI ignored\n", (void *)root_phys);
        return;
    }

    info.present = true;
    info.revision = r->revision;
    memcpy(info.oem_id, r->oem_id, 6);
    kprintf("[acpi] ACPI %s, OEM \"%s\", %s at %p\n", r->revision >= 2 ? "2.0+" : "1.0",
            info.oem_id, use_xsdt ? "XSDT" : "RSDT", (void *)root_phys);

    size_t entry_size = use_xsdt ? 8 : 4;
    size_t n = (root->length - sizeof(*root)) / entry_size;
    const uint8_t *entries = (const uint8_t *)(root + 1);
    for (size_t i = 0; i < n; i++) {
        uint64_t table = 0;
        memcpy(&table, entries + i * entry_size, entry_size);
        parse_table(table);
    }

    kprintf("[acpi] %u CPU(s), local APIC at %p, %u I/O APIC(s), %u IRQ override(s)%s\n",
            info.cpu_count, (void *)info.lapic_phys, info.ioapic_count, info.override_count,
            info.has_legacy_pics ? ", legacy PICs present" : "");
    if (info.hpet_present)
        kprintf("[acpi] HPET at %p\n", (void *)info.hpet_phys);
    if (info.ecam_present)
        kprintf("[acpi] PCIe ECAM at %p, buses %u-%u\n", (void *)info.ecam_phys,
                info.ecam_bus_start, info.ecam_bus_end);
    else
        kprintf("[acpi] no MCFG table: PCI config space via I/O ports\n");
}

uint32_t acpi_irq_to_gsi(uint8_t irq, bool *active_low, bool *level)
{
    for (unsigned i = 0; i < info.override_count; i++) {
        if (info.overrides[i].irq == irq) {
            if (active_low)
                *active_low = info.overrides[i].active_low;
            if (level)
                *level = info.overrides[i].level_triggered;
            return info.overrides[i].gsi;
        }
    }
    /* ISA default: edge-triggered, active high, same number. */
    if (active_low)
        *active_low = false;
    if (level)
        *level = false;
    return irq;
}
