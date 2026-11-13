#include "serial.h"
#include "percpu.h"
#include "ipi.h"
#include "panic.h"

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

#define REG_THR 0
#define REG_IER 1
#define REG_FCR 2
#define REG_LCR 3
#define REG_MCR 4
#define REG_LSR 5
#define LSR_THRE 0x20
#define LSR_DATA 0x01
#define IER_RX   0x01

void serial_init(void)
{
    uint16_t port = SERIAL_COM1;
    outb(port + REG_IER, 0x00);
    outb(port + REG_LCR, 0x80);
    outb(port + 0, (SERIAL_BAUD_DIVISOR & 0xFF));
    outb(port + 1, ((SERIAL_BAUD_DIVISOR >> 8) & 0xFF));
    outb(port + REG_LCR, 0x03);
    outb(port + REG_FCR, 0xC7);
    outb(port + REG_MCR, 0x0B);
    outb(port + REG_IER, 0x00);
}

/* Optional second output (the screen console), fed every byte. */
static void (*mirror)(char c);

void serial_set_mirror(void (*fn)(char c))
{
    mirror = fn;
}

void serial_putchar(char c)
{
    if (mirror != NULL)
        mirror(c);
    uint16_t port = SERIAL_COM1;
    while ((inb(port + REG_LSR) & LSR_THRE) == 0)
        __asm__ volatile("pause");
    outb(port + REG_THR, (uint8_t)c);
}

/* The console lock: one CPU writes at a time, so lines from different
 * CPUs (or preempted threads) never mix. It is "recursive": the CPU that
 * holds it may take it again (kprintf -> serial_write, or an interrupt
 * that prints while its CPU was printing). It is a leaf: nothing else is
 * locked while holding it, so it is outside the lock-order checker.
 * During a panic the panicking CPU takes it over (the others are being
 * stopped and may never let go). */
#define NO_OWNER 0xFFFFFFFFu
static volatile uint32_t con_owner = NO_OWNER;
static uint32_t con_depth;

uint64_t console_lock(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    uint32_t me = cpu_id();
    if (__atomic_load_n(&con_owner, __ATOMIC_RELAXED) == me) {
        con_depth++;
        return flags;
    }
    for (;;) {
        uint32_t expect = NO_OWNER;
        if (__atomic_compare_exchange_n(&con_owner, &expect, me, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
        if (kernel_panicking()) {
            __atomic_store_n(&con_owner, me, __ATOMIC_SEQ_CST);
            break;
        }
        cpu_relax();
    }
    con_depth = 1;
    return flags;
}

void console_unlock(uint64_t flags)
{
    if (--con_depth == 0)
        __atomic_store_n(&con_owner, NO_OWNER, __ATOMIC_RELEASE);
    if (flags & (1ULL << 9))
        __asm__ volatile("sti" : : : "memory");
}

void serial_puts(const char *s)
{
    if (s == NULL)
        return;
    uint64_t flags = console_lock();
    while (*s) {
        if (*s == '\n')
            serial_putchar('\r');
        serial_putchar(*s++);
    }
    console_unlock(flags);
}

void serial_write(const char *buf, uint64_t n)
{
    if (buf == NULL)
        return;
    uint64_t flags = console_lock();
    for (uint64_t i = 0; i < n; i++) {
        if (buf[i] == '\n')
            serial_putchar('\r');
        serial_putchar(buf[i]);
    }
    console_unlock(flags);
}

void serial_put_hex(uint64_t value)
{
    static const char d[] = "0123456789abcdef";
    uint64_t flags = console_lock();
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putchar(d[(value >> i) & 0xF]);
    console_unlock(flags);
}

void serial_enable_rx_irq(void)
{
    uint16_t port = SERIAL_COM1;
    while (inb(port + REG_LSR) & LSR_DATA)    /* drop anything typed during boot */
        (void)inb(port + REG_THR);
    outb(port + REG_IER, IER_RX);
}

bool serial_try_read(uint8_t *out)
{
    uint16_t port = SERIAL_COM1;
    if (!(inb(port + REG_LSR) & LSR_DATA))
        return false;
    *out = inb(port + REG_THR);              /* the receive register shares offset 0 */
    return true;
}
