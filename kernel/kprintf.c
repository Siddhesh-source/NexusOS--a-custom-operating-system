#include "kprintf.h"
#include "serial.h"
#include <stdbool.h>

/* Output goes through a sink: either the serial port or a bounded buffer. */
struct sink {
    char *buf;       /* NULL = serial */
    size_t cap;
    size_t len;      /* characters produced (may exceed cap) */
};

static void emit(struct sink *s, const char *str, size_t n)
{
    if (s->buf == NULL) {
        serial_write(str, n);
    } else {
        for (size_t i = 0; i < n; i++) {
            if (s->len + i + 1 < s->cap)
                s->buf[s->len + i] = str[i];
        }
    }
    s->len += n;
}

static void emit_char(struct sink *s, char c)
{
    emit(s, &c, 1);
}

/* A negative width left-justifies (pads with spaces on the right). */
static void put_padded(struct sink *s, const char *str, int len, int width, char pad)
{
    if (width < 0) {
        emit(s, str, (size_t)len);
        for (int i = len; i < -width; i++)
            emit_char(s, ' ');
        return;
    }
    for (int i = len; i < width; i++)
        emit_char(s, pad);
    emit(s, str, (size_t)len);
}

static void put_unsigned(struct sink *s, uint64_t v, unsigned base, bool upper,
                         int width, char pad)
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
    put_padded(s, out, n, width, pad);
}

static void format(struct sink *s, const char *fmt, va_list ap)
{
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit(s, p, 1);
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
            const char *str = va_arg(ap, const char *);
            if (str == NULL)
                str = "(null)";
            int len = 0;
            while (str[len])
                len++;
            put_padded(s, str, len, width, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            put_padded(s, &c, 1, width, ' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = longs ? va_arg(ap, int64_t) : va_arg(ap, int);
            if (v < 0) {
                emit_char(s, '-');
                if (width > 0)
                    width--;
                else if (width < 0)
                    width++;
                put_unsigned(s, (uint64_t)0 - (uint64_t)v, 10, false, width, pad);
            } else {
                put_unsigned(s, (uint64_t)v, 10, false, width, pad);
            }
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = longs ? va_arg(ap, uint64_t) : va_arg(ap, unsigned int);
            put_unsigned(s, v, *p == 'u' ? 10 : 16, *p == 'X', width, pad);
            break;
        }
        case 'p':
            emit(s, "0x", 2);
            put_unsigned(s, (uint64_t)va_arg(ap, void *), 16, false, 16, '0');
            break;
        case '%':
            emit_char(s, '%');
            break;
        case '\0':
            p--;
            break;
        default:
            emit_char(s, '%');
            emit_char(s, *p);
            break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    struct sink s = { .buf = NULL };
    va_list ap;
    va_start(ap, fmt);
    format(&s, fmt, ap);
    va_end(ap);
}

size_t kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct sink s = { .buf = buf, .cap = size, .len = 0 };
    format(&s, fmt, ap);
    if (size > 0)
        buf[s.len < size ? s.len : size - 1] = '\0';
    return s.len;
}

size_t ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
