#include "box.h"
#include "kprintf.h"
#include "serial.h"
#include "cpu.h"

static void rule(const char *left, const char *right)
{
    IRQ_GUARD();   /* whole line in one uninterrupted write */
    serial_puts(left);
    for (int i = 0; i < BOX_INNER_WIDTH + 2; i++)
        serial_puts("═");
    serial_puts(right);
    serial_puts("\n");
}

void box_top(void)       { rule("╔", "╗"); }
void box_separator(void) { rule("╠", "╣"); }
void box_bottom(void)    { rule("╚", "╝"); }

/* Width in display columns: count every byte except UTF-8 continuation
 * bytes (10xxxxxx), so multi-byte glyphs such as "→" count once. */
static int columns(const char *s, int *bytes_for_width)
{
    int cols = 0, i = 0, cut = -1;
    for (; s[i]; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (cols == BOX_INNER_WIDTH && cut < 0)
                cut = i;
            cols++;
        }
    }
    *bytes_for_width = cut < 0 ? i : cut;
    return cols;
}

void box_line(const char *fmt, ...)
{
    IRQ_GUARD();   /* whole line in one uninterrupted write */
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    int bytes;
    int cols = columns(text, &bytes);
    serial_puts("║ ");
    serial_write(text, (uint64_t)bytes);
    for (int i = cols; i < BOX_INNER_WIDTH; i++)
        serial_putchar(' ');
    serial_puts(" ║\n");
}
