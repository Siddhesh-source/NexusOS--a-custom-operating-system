#include "serial.h"

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

void serial_putchar(char c)
{
    uint16_t port = SERIAL_COM1;
    while ((inb(port + REG_LSR) & LSR_THRE) == 0)
        __asm__ volatile("pause");
    outb(port + REG_THR, (uint8_t)c);
}

void serial_puts(const char *s)
{
    if (s == NULL)
        return;
    while (*s) {
        if (*s == '\n')
            serial_putchar('\r');
        serial_putchar(*s++);
    }
}

void serial_write(const char *buf, uint64_t n)
{
    if (buf == NULL)
        return;
    for (uint64_t i = 0; i < n; i++) {
        if (buf[i] == '\n')
            serial_putchar('\r');
        serial_putchar(buf[i]);
    }
}

void serial_put_hex(uint64_t value)
{
    static const char d[] = "0123456789abcdef";
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putchar(d[(value >> i) & 0xF]);
}
