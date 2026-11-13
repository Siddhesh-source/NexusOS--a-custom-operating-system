#include "textbuf.h"
#include "gfx.h"
#include "heap.h"
#include "string.h"

bool textbuf_init(struct textbuf *tb, unsigned cols, unsigned rows)
{
    memset(tb, 0, sizeof(*tb));
    if (cols == 0 || rows == 0 || cols > 512 || rows > 256)
        return false;
    tb->cells = kmalloc((size_t)cols * rows);
    if (tb->cells == NULL)
        return false;
    tb->cols = cols;
    tb->rows = rows;
    textbuf_clear(tb);
    return true;
}

void textbuf_free(struct textbuf *tb)
{
    kfree(tb->cells);
    tb->cells = NULL;
}

void textbuf_clear(struct textbuf *tb)
{
    memset(tb->cells, ' ', (size_t)tb->cols * tb->rows);
    tb->col = tb->row = 0;
}

static void newline(struct textbuf *tb)
{
    tb->col = 0;
    if (tb->row + 1 < tb->rows) {
        tb->row++;
        return;
    }
    memmove(tb->cells, tb->cells + tb->cols, (size_t)tb->cols * (tb->rows - 1));
    memset(tb->cells + (size_t)tb->cols * (tb->rows - 1), ' ', tb->cols);
}

static void put(struct textbuf *tb, char c)
{
    if (tb->col >= tb->cols)
        newline(tb);
    tb->cells[(size_t)tb->row * tb->cols + tb->col++] = c;
}

static void escape(struct textbuf *tb, uint8_t c)
{
    if (tb->esc == 1) {
        tb->esc = c == '[' ? 2 : 0;
        tb->esc_num = 0;
    } else if (c >= '0' && c <= '9') {
        tb->esc_num = tb->esc_num * 10 + (c - '0');
    } else if (c != ';' && c != '?') {
        tb->esc = 0;
        if (c == 'J' && tb->esc_num == 2)
            textbuf_clear(tb);
        else if (c == 'H')
            tb->col = tb->row = 0;
        else if (c == 'K' && tb->col < tb->cols)
            memset(tb->cells + (size_t)tb->row * tb->cols + tb->col, ' ', tb->cols - tb->col);
    }
}

static void byte(struct textbuf *tb, uint8_t c)
{
    if (tb->esc != 0) {
        escape(tb, c);
        return;
    }
    if (tb->utf8_left > 0 && (c & 0xC0) == 0x80) {
        tb->cp = (tb->cp << 6) | (c & 0x3F);
        if (--tb->utf8_left == 0) {
            uint32_t cp = tb->cp;
            bool box = cp >= 0x2500 && cp <= 0x257F;
            put(tb, !box ? '?' : (cp == 0x2500 || cp == 0x2501 || cp == 0x2550) ? '-'
                    : (cp == 0x2502 || cp == 0x2503 || cp == 0x2551) ? '|' : '+');
        }
        return;
    }
    tb->utf8_left = 0;                     /* a broken sequence is dropped */
    if (c >= 0xC0) {
        tb->utf8_left = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        tb->cp = c & (0x3F >> tb->utf8_left);
        return;
    }
    switch (c) {
    case 0x1B: tb->esc = 1; break;
    case '\n': newline(tb); break;
    case '\r': tb->col = 0; break;
    case '\b': if (tb->col > 0) tb->col--; break;
    case '\t': do { put(tb, ' '); } while (tb->col % 8 != 0 && tb->col < tb->cols); break;
    default:
        if (c >= 0x20 && c < 0x7F)
            put(tb, (char)c);
    }
}

void textbuf_write(struct textbuf *tb, const char *s, size_t n)
{
    if (tb->cells == NULL || s == NULL)
        return;
    for (size_t i = 0; i < n; i++)
        byte(tb, (uint8_t)s[i]);
}

void textbuf_puts(struct textbuf *tb, const char *s)
{
    textbuf_write(tb, s, strlen(s));
}

void textbuf_out(void *ctx, const char *s, size_t n)
{
    textbuf_write(ctx, s, n);
}

const char *textbuf_row(const struct textbuf *tb, unsigned r)
{
    return tb->cells + (size_t)(r < tb->rows ? r : 0) * tb->cols;
}

bool textbuf_has(const struct textbuf *tb, const char *text)
{
    size_t n = strlen(text);
    for (unsigned r = 0; r < tb->rows && n <= tb->cols; r++)
        for (unsigned c = 0; c + n <= tb->cols; c++)
            if (memcmp(textbuf_row(tb, r) + c, text, n) == 0)
                return true;
    return false;
}

void textbuf_draw(const struct textbuf *tb, int32_t x, int32_t y, uint32_t rgb)
{
    for (unsigned r = 0; r < tb->rows; r++)
        gfx_text_n(x, y + (int32_t)r * GFX_CHAR_H, textbuf_row(tb, r), tb->cols, rgb, 1);
}
