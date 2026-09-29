#ifndef NEXUS_KPRINTF_H
#define NEXUS_KPRINTF_H

#include <stddef.h>
#include <stdarg.h>

/* Minimal formatted output.
 *
 * Supported: %s %c %d %i %u %x %X %p %%, length modifiers l / ll / z,
 * a field width with optional '0' pad or '-' left-justify (%016lx, %-8s).
 * %p prints 0x followed by 16 hex digits. */

/* Print to the serial console. */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Format into buf (always NUL-terminated if size > 0). Returns the length
 * the full output would have had, like snprintf. */
size_t ksnprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
size_t kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif /* NEXUS_KPRINTF_H */
