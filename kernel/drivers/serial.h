#ifndef BHARAT_SERIAL_H
#define BHARAT_SERIAL_H

#include <stdint.h>

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

/* Send a copy of every byte written to the serial port to `fn` as well
 * (used by the screen console). NULL turns it off. */
void serial_set_mirror(void (*fn)(char c));

#endif
