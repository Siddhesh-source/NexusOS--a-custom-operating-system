#ifndef BHARAT_SERIAL_H
#define BHARAT_SERIAL_H

#include <stdint.h>
#include <stdbool.h>

#ifndef NULL
#define NULL ((void *)0)
#endif

#define SERIAL_COM1 0x3F8
#define SERIAL_BAUD_DIVISOR 1

void serial_init(void);
void serial_putchar(char c);
void serial_puts(const char *s);
void serial_write(const char *buf, uint64_t n);
void serial_put_hex(uint64_t value);

/* The console lock (serial and screen): keeps each line in one piece when
 * several CPUs print. The holder may take it again. Interrupts are off
 * while it is held; pass the result to console_unlock. serial_puts,
 * serial_write and kprintf take it themselves. */
uint64_t console_lock(void);
void console_unlock(uint64_t flags);

/* Send a copy of every byte written to the serial port to `fn` as well
 * (used by the screen console). NULL turns it off. */
void serial_set_mirror(void (*fn)(char c));

/* Receiving: turn on the "byte received" interrupt (the caller routes
 * IRQ 4), and take one received byte if there is one. */
void serial_enable_rx_irq(void);
bool serial_try_read(uint8_t *out);

#endif
