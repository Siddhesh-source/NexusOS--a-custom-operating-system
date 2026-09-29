#ifndef NEXUS_KPRINTF_H
#define NEXUS_KPRINTF_H

/* Minimal formatted output to the serial console.
 *
 * Supported: %s %c %d %i %u %x %X %p %%, length modifiers l / ll / z,
 * a field width with optional '0' pad or '-' left-justify (%016lx, %-8s).
 * %p prints 0x followed by 16 hex digits. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif /* NEXUS_KPRINTF_H */
