#include "kprintf.h"
#include "serial.h"
#include <stdarg.h>
#include <stdbool.h>

/* A negative width left-justifies (pads with spaces on the right). */
static void put_padded(const char *s, int len, int width, char pad)
{
    if (width < 0) {
        serial_write(s, (uint64_t)len);
        for (int i = len; i < -width; i++)
            serial_putchar(' ');
        return;
    }
    for (int i = len; i < width; i++)
        serial_putchar(pad);
    serial_write(s, (uint64_t)len);
}

static void put_unsigned(uint64_t v, unsigned base, bool upper, int width, char pad)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int n = 0;
    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v != 0);

    char out[24];
    for (int i = 0; i < n; i++)
        out[i] = buf[n - 1 - i];
    put_padded(out, n, width, pad);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            serial_write(p, 1);
            continue;
        }
        p++;

        char pad = ' ';
        bool left = false;
        if (*p == '-') {
            left = true;
            p++;
        }
        if (*p == '0') {
            pad = '0';
            p++;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9')
            width = width * 10 + (*p++ - '0');
        if (left) {
            width = -width;
            pad = ' ';
        }

        int longs = 0;
        while (*p == 'l' || *p == 'z') {
            longs++;
            p++;
        }

        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL)
                s = "(null)";
            int len = 0;
            while (s[len])
                len++;
            put_padded(s, len, width, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            put_padded(&c, 1, width, ' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                serial_putchar('-');
                if (width > 0)
                    width--;
                else if (width < 0)
                    width++;
                put_unsigned((uint64_t)0 - (uint64_t)v, 10, false, width, pad);
            } else {
                put_unsigned((uint64_t)v, 10, false, width, pad);
            }
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            put_unsigned(v, *p == 'u' ? 10 : 16, *p == 'X', width, pad);
            break;
        }
        case 'p':
            serial_write("0x", 2);
            put_unsigned((uint64_t)va_arg(ap, void *), 16, false, 16, '0');
            break;
        case '%':
            serial_putchar('%');
            break;
        case '\0':
            p--;
            break;
        default:
            serial_putchar('%');
            serial_putchar(*p);
            break;
        }
    }

    va_end(ap);
}
