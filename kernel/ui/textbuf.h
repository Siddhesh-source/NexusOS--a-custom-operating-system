#ifndef BHARAT_TEXTBUF_H
#define BHARAT_TEXTBUF_H

/* Text buffer: a grid of characters that text is written into, the way a
 * terminal screen works. Used by the Terminal, Health and About windows:
 * the shell's output goes in here, and the window draws the grid.
 *
 * Understands newline, carriage return, backspace, tab, and the few
 * escape sequences the shell sends (ESC [2J clear, ESC [H home, ESC [K
 * clear to end of line, ESC [<n>m colours 30-37 and 90-97, 0 to reset);
 * other escape sequences are dropped. When the last row is full,
 * everything moves up one row (cheap here because it is ordinary memory)
 * and the top row goes into the scrollback: the last TEXTBUF_BACK rows
 * that went off the top, which textbuf_scroll() brings back into view.
 * UTF-8 box-drawing characters become + - |, other non-ASCII '?'.
 *
 * Limits: 16 colours; no cursor movement other than the above; the
 * scrollback is cleared by ESC [2J. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TEXTBUF_BACK 200

struct textbuf {
    char *cells;                 /* (TEXTBUF_BACK + rows) * cols characters */
    uint8_t *attr;               /* a colour (0 = default, 1..16) per cell */
    unsigned cols, rows;
    unsigned col, row;           /* where the next character goes */
    unsigned hist;               /* rows in the scrollback so far */
    unsigned view;               /* rows scrolled back (0: the live rows) */
    uint8_t esc;                 /* 0 normal, 1 after ESC, 2 inside ESC [ */
    uint8_t colour;              /* colour of the next character */
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

/* Live row `r` (cols characters, not 0-terminated). */
const char *textbuf_row(const struct textbuf *tb, unsigned r);
/* Row `r` as shown now (scrolled back by `view`), and the colour of one
 * of its cells. */
const char *textbuf_shown_row(const struct textbuf *tb, unsigned r);
uint8_t textbuf_shown_colour(const struct textbuf *tb, unsigned r, unsigned c);

/* Scroll back (positive) or forward (negative) by `rows`, kept between
 * the live rows and the oldest scrollback row. */
void textbuf_scroll(struct textbuf *tb, int rows);

/* True if `text` appears within one live row (self-tests). */
bool textbuf_has(const struct textbuf *tb, const char *text);

/* Draw the rows as shown in the monospace font (FONT_MONO_W x FONT_MONO_H
 * cells) with the top-left corner at (x, y); `rgb` is the default colour. */
void textbuf_draw(const struct textbuf *tb, int32_t x, int32_t y, uint32_t rgb);

#endif /* BHARAT_TEXTBUF_H */
