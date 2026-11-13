#ifndef BHARAT_TEXTBUF_H
#define BHARAT_TEXTBUF_H

/* Text buffer: a grid of characters that text is written into, the way a
 * terminal screen works. Used by the Terminal, Health and About windows:
 * the shell's output goes in here, and the window draws the grid.
 *
 * Understands newline, carriage return, backspace, tab, and the few
 * escape sequences the shell sends (ESC [2J clear, ESC [H home, ESC [K
 * clear to end of line); other escape sequences are dropped. When the
 * last row is full, everything moves up one row (scrolling, cheap here
 * because it is ordinary memory). UTF-8 box-drawing characters become
 * + - |, other non-ASCII characters '?'.
 *
 * Limits: one colour; no cursor movement other than the above. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct textbuf {
    char *cells;                 /* rows * cols characters */
    unsigned cols, rows;
    unsigned col, row;           /* where the next character goes */
    uint8_t esc;                 /* 0 normal, 1 after ESC, 2 inside ESC [ */
    unsigned esc_num;
    uint32_t cp;                 /* UTF-8 character being decoded */
    unsigned utf8_left;
};

bool textbuf_init(struct textbuf *tb, unsigned cols, unsigned rows);
void textbuf_free(struct textbuf *tb);
void textbuf_clear(struct textbuf *tb);
void textbuf_write(struct textbuf *tb, const char *s, size_t n);
void textbuf_puts(struct textbuf *tb, const char *s);

/* Output function in the shape the shell and line editor use
 * (ctx is the textbuf). */
void textbuf_out(void *ctx, const char *s, size_t n);

/* Row `r` (cols characters, not 0-terminated). */
const char *textbuf_row(const struct textbuf *tb, unsigned r);

/* True if `text` appears within one row (self-tests). */
bool textbuf_has(const struct textbuf *tb, const char *text);

/* Draw the grid with its top-left corner at (x, y). */
void textbuf_draw(const struct textbuf *tb, int32_t x, int32_t y, uint32_t rgb);

#endif /* BHARAT_TEXTBUF_H */
