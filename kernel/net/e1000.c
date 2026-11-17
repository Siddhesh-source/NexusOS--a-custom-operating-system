#include "e1000.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "mm.h"
#include "clock.h"
#include "kprintf.h"
#include "string.h"

/* Registers (offsets into the card's memory window). */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_TIPG   0x0410
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_MTA    0x5200
#define REG_RAL    0x5400
#define REG_RAH    0x5404

#define CTRL_ASDE  (1u << 5)
#define CTRL_SLU   (1u << 6)
#define CTRL_RST   (1u << 26)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN    (1u << 1)
#define TCTL_PSP   (1u << 3)
#define CMD_EOP    (1u << 0)
#define CMD_IFCS   (1u << 1)
#define CMD_RS     (1u << 3)
#define DESC_DD    (1u << 0)

#define RING 32

struct rx_desc {
    uint64_t addr;
    uint16_t length, csum;
    uint8_t status, errors;
    uint16_t special;
} __attribute__((packed));

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso, cmd, status, css;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t *regs;
static volatile struct rx_desc *rx;
static volatile struct tx_desc *tx;
static uint8_t *rx_buf, *tx_buf;
static uint64_t rx_buf_phys, tx_buf_phys;
static unsigned rx_next, tx_next;
static uint8_t mac[6];
static struct e1000_stats stats;

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(regs + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(regs + r) = v; }

static uint16_t eeprom_read(uint8_t addr)
{
    wr(REG_EERD, ((uint32_t)addr << 8) | 1);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(REG_EERD);
        if (v & (1u << 4))
            return (uint16_t)(v >> 16);
    }
    return 0;
}

bool e1000_present(void)
{
    return regs != NULL;
}

void e1000_mac(uint8_t out[6])
{
    memcpy(out, mac, 6);
}

bool e1000_link_up(void)
{
    return regs != NULL && (rd(REG_STATUS) & 2u) != 0;
}

bool e1000_init(void)
{
    static const uint16_t ids[] = { 0x100E, 0x100F, 0x10D3, 0x1004, 0x153A };
    const struct pci_device *d = NULL;
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]) && d == NULL; i++)
        d = pci_find(0x8086, ids[i]);
    if (d == NULL)
        return false;
    uint64_t bar = d->bar[0] & ~0xFull;
    if ((d->bar[0] & 1) || bar == 0) {
        kprintf("[net] network card has no memory window; not used\n");
        return false;
    }
    if (((d->bar[0] >> 1) & 3) == 2)
        bar |= (uint64_t)d->bar[1] << 32;
    /* Turn on memory access and let the card reach RAM by itself (DMA). */
    uint32_t cmd = pci_config_read32(d->bus, d->dev, d->fn, 4);
    pci_config_write32(d->bus, d->dev, d->fn, 4, (cmd & 0xFFFF) | 0x6);
    regs = vmm_map_phys(bar, 0x20000, VMM_WRITE | VMM_NOCACHE);
    if (regs == NULL)
        return false;

    wr(REG_IMC, 0xFFFFFFFF);                       /* no interrupts: we poll */
    wr(REG_CTRL, rd(REG_CTRL) | CTRL_RST);
    clock_delay_ns(2000000);
    wr(REG_IMC, 0xFFFFFFFF);
    wr(REG_CTRL, (rd(REG_CTRL) | CTRL_SLU | CTRL_ASDE) & ~CTRL_RST);

    uint32_t ral = rd(REG_RAL), rah = rd(REG_RAH);
    if ((rah & (1u << 31)) && (ral | (rah & 0xFFFF)) != 0) {
        for (int i = 0; i < 4; i++)
            mac[i] = (uint8_t)(ral >> (8 * i));
        mac[4] = (uint8_t)rah;
        mac[5] = (uint8_t)(rah >> 8);
    } else {
        for (int i = 0; i < 3; i++) {
            uint16_t w = eeprom_read((uint8_t)i);
            mac[2 * i] = (uint8_t)w;
            mac[2 * i + 1] = (uint8_t)(w >> 8);
        }
        wr(REG_RAL, (uint32_t)mac[0] | ((uint32_t)mac[1] << 8) | ((uint32_t)mac[2] << 16)
                    | ((uint32_t)mac[3] << 24));
        wr(REG_RAH, (uint32_t)mac[4] | ((uint32_t)mac[5] << 8) | (1u << 31));
    }
    for (uint32_t i = 0; i < 128; i++)
        wr(REG_MTA + i * 4, 0);

    /* One page holds both descriptor rings (32 x 16 bytes each); the
     * buffers are 32 x 2 KiB = 16 pages per ring. */
    uint64_t ring_phys = pmm_alloc_zeroed_page();
    rx_buf_phys = pmm_alloc_pages(4);
    tx_buf_phys = pmm_alloc_pages(4);
    if (ring_phys == 0 || rx_buf_phys == 0 || tx_buf_phys == 0) {
        kprintf("[net] not enough memory for the network card's buffers\n");
        regs = NULL;
        return false;
    }
    rx = phys_to_virt(ring_phys);
    tx = phys_to_virt(ring_phys + 2048);
    rx_buf = phys_to_virt(rx_buf_phys);
    tx_buf = phys_to_virt(tx_buf_phys);
    for (unsigned i = 0; i < RING; i++) {
        rx[i].addr = rx_buf_phys + (uint64_t)i * E1000_BUF;
        rx[i].status = 0;
        tx[i].addr = tx_buf_phys + (uint64_t)i * E1000_BUF;
        tx[i].status = DESC_DD;                   /* free */
    }
    wr(REG_RDBAL, (uint32_t)ring_phys);
    wr(REG_RDBAH, (uint32_t)(ring_phys >> 32));
    wr(REG_RDLEN, RING * sizeof(struct rx_desc));
    wr(REG_RDH, 0);
    wr(REG_RDT, RING - 1);
    rx_next = 0;
    wr(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);  /* 2 KiB buffers */

    wr(REG_TDBAL, (uint32_t)(ring_phys + 2048));
    wr(REG_TDBAH, (uint32_t)((ring_phys + 2048) >> 32));
    wr(REG_TDLEN, RING * sizeof(struct tx_desc));
    wr(REG_TDH, 0);
    wr(REG_TDT, 0);
    tx_next = 0;
    wr(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));
    wr(REG_TIPG, 0x0060200A);
    kprintf("[net] Network card %04x:%04x at %02x:%02x.%x, address %02x:%02x:%02x:%02x:%02x:%02x\n",
            d->vendor_id, d->device_id, d->bus, d->dev, d->fn,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

bool e1000_send(const void *frame, size_t len)
{
    if (regs == NULL || len < 14 || len > 1514)
        return false;
    volatile struct tx_desc *d = &tx[tx_next];
    if (!(d->status & DESC_DD)) {
        stats.dropped++;
        return false;                             /* the card is still sending it */
    }
    memcpy(tx_buf + (size_t)tx_next * E1000_BUF, frame, len);
    d->length = (uint16_t)(len < 60 ? 60 : len);  /* pad to the Ethernet minimum */
    if (len < 60)
        memset(tx_buf + (size_t)tx_next * E1000_BUF + len, 0, 60 - len);
    d->cmd = CMD_EOP | CMD_IFCS | CMD_RS;
    d->status = 0;
    tx_next = (tx_next + 1) % RING;
    __asm__ volatile("" ::: "memory");
    wr(REG_TDT, tx_next);
    stats.sent++;
    return true;
}

size_t e1000_receive(void *buf)
{
    if (regs == NULL)
        return 0;
    volatile struct rx_desc *d = &rx[rx_next];
    if (!(d->status & DESC_DD))
        return 0;
    __asm__ volatile("" ::: "memory");
    size_t len = d->length;
    bool ok = (d->status & 2u) && d->errors == 0 && len <= E1000_BUF;
    if (ok)
        memcpy(buf, rx_buf + (size_t)rx_next * E1000_BUF, len);
    else
        stats.dropped++;
    d->status = 0;
    wr(REG_RDT, rx_next);                         /* hand the buffer back */
    rx_next = (rx_next + 1) % RING;
    if (!ok)
        return 0;
    stats.received++;
    return len;
}

void e1000_get_stats(struct e1000_stats *out)
{
    *out = stats;
}
