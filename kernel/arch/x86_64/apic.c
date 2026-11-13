#include "apic.h"
#include "acpi.h"
#include "vmm.h"
#include "cpu.h"
#include "kprintf.h"
#include "spinlock.h"

#ifndef NULL
#define NULL ((void *)0)
#endif

#define MSR_APIC_BASE       0x1B
#define APIC_BASE_ENABLE    (1ULL << 11)

#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0
#define LAPIC_ESR           0x280
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310
#define LAPIC_ICR_PENDING   (1u << 12)
#define LAPIC_SVR_ENABLE    (1u << 8)

#define IOAPIC_REGSEL       0x00
#define IOAPIC_WINDOW       0x10
#define IOAPIC_REG_VERSION  0x01
#define IOAPIC_REDIR(n)     (0x10 + 2 * (n))
#define IOAPIC_ACTIVE_LOW   (1u << 13)
#define IOAPIC_LEVEL        (1u << 15)
#define IOAPIC_MASKED       (1u << 16)

#define PIC_MASTER_DATA     0x21
#define PIC_SLAVE_DATA      0xA1

static volatile uint32_t *lapic;
static uint64_t lapic_phys_base;

struct ioapic {
    volatile uint32_t *regs;
    uint32_t gsi_base;
    unsigned lines;
};
static struct ioapic ioapics[ACPI_MAX_IOAPICS];
static unsigned ioapic_count;
static bool pic_masked;
static struct spinlock ioapic_lock = SPINLOCK_INIT(LOCK_CLASS_IOAPIC);

uint32_t lapic_read(uint32_t reg)
{
    return lapic[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t value)
{
    lapic[reg / 4] = value;
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_id(void)
{
    return lapic_read(LAPIC_ID) >> 24;
}

static uint32_t ioapic_read(struct ioapic *io, uint32_t reg)
{
    io->regs[IOAPIC_REGSEL / 4] = reg;
    return io->regs[IOAPIC_WINDOW / 4];
}

static void ioapic_write(struct ioapic *io, uint32_t reg, uint32_t value)
{
    io->regs[IOAPIC_REGSEL / 4] = reg;
    io->regs[IOAPIC_WINDOW / 4] = value;
}

bool pic_is_masked(void)
{
    return pic_masked;
}

unsigned ioapic_line_count(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < ioapic_count; i++)
        n += ioapics[i].lines;
    return n;
}

/* Turn on this CPU's local APIC. Every CPU has its own at the same
 * address, so the one mapping serves them all. */
void apic_init_cpu(void)
{
    wrmsr(MSR_APIC_BASE, (rdmsr(MSR_APIC_BASE) & ~0xFFFULL) | APIC_BASE_ENABLE
          | (lapic_phys_base & ~0xFFFULL));
    lapic_write(LAPIC_TPR, 0);                      /* accept every priority */
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_SVR, LAPIC_SVR_ENABLE | APIC_SPURIOUS_VECTOR);
    lapic_eoi();
}

void lapic_send_ipi(uint32_t apic_id, uint32_t command)
{
    /* The two halves of the command register must not be split by an
     * interrupt that sends an IPI of its own. */
    uint64_t flags = irq_save();
    while (lapic_read(LAPIC_ICR_LOW) & LAPIC_ICR_PENDING)
        __asm__ volatile("pause");
    lapic_write(LAPIC_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_ICR_LOW, command);
    irq_restore(flags);
}

bool apic_init(void)
{
    const struct acpi_info *a = acpi_get();
    if (!a->madt_present || a->lapic_phys == 0) {
        kprintf("[apic] no MADT: cannot use the APIC\n");
        return false;
    }
    IRQ_GUARD();

    /* The PICs were remapped to vectors 32-47 at boot; masking every line
     * stops them for good. (A rare spurious IRQ 7/15 can still arrive and
     * is ignored by the interrupt dispatcher.) */
    outb(PIC_MASTER_DATA, 0xFF);
    outb(PIC_SLAVE_DATA, 0xFF);
    pic_masked = true;

    lapic = vmm_map_phys(a->lapic_phys, 4096, VMM_WRITE | VMM_NOCACHE);
    if (lapic == NULL)
        return false;
    lapic_phys_base = a->lapic_phys;
    apic_init_cpu();

    ioapic_count = 0;
    for (unsigned i = 0; i < a->ioapic_count; i++) {
        struct ioapic *io = &ioapics[ioapic_count];
        io->regs = vmm_map_phys(a->ioapics[i].phys, 4096, VMM_WRITE | VMM_NOCACHE);
        if (io->regs == NULL)
            continue;
        io->gsi_base = a->ioapics[i].gsi_base;
        io->lines = ((ioapic_read(io, IOAPIC_REG_VERSION) >> 16) & 0xFF) + 1;
        for (unsigned n = 0; n < io->lines; n++) {
            ioapic_write(io, IOAPIC_REDIR(n) + 1, 0);
            ioapic_write(io, IOAPIC_REDIR(n), IOAPIC_MASKED);
        }
        ioapic_count++;
    }

    kprintf("[apic] Local APIC id %u (version 0x%x) on; PICs masked; "
            "%u I/O APIC(s), %u interrupt lines\n",
            lapic_id(), lapic_read(LAPIC_VERSION) & 0xFF, ioapic_count, ioapic_line_count());
    return true;
}

static struct ioapic *ioapic_for(uint32_t gsi, unsigned *line)
{
    for (unsigned i = 0; i < ioapic_count; i++) {
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].lines) {
            *line = gsi - ioapics[i].gsi_base;
            return &ioapics[i];
        }
    }
    return NULL;
}

bool ioapic_route_irq(uint8_t irq, uint8_t vector)
{
    bool active_low, level;
    unsigned line;
    uint32_t gsi = acpi_irq_to_gsi(irq, &active_low, &level);
    struct ioapic *io = ioapic_for(gsi, &line);
    if (io == NULL || vector < 32)
        return false;
    SPIN_GUARD(&ioapic_lock);
    uint32_t low = vector | (active_low ? IOAPIC_ACTIVE_LOW : 0) | (level ? IOAPIC_LEVEL : 0);
    ioapic_write(io, IOAPIC_REDIR(line) + 1, lapic_id() << 24);   /* destination CPU */
    ioapic_write(io, IOAPIC_REDIR(line), low);
    return true;
}

bool ioapic_mask_irq(uint8_t irq, bool masked)
{
    unsigned line;
    struct ioapic *io = ioapic_for(acpi_irq_to_gsi(irq, NULL, NULL), &line);
    if (io == NULL)
        return false;
    SPIN_GUARD(&ioapic_lock);
    uint32_t low = ioapic_read(io, IOAPIC_REDIR(line));
    ioapic_write(io, IOAPIC_REDIR(line), masked ? low | IOAPIC_MASKED : low & ~IOAPIC_MASKED);
    return true;
}
