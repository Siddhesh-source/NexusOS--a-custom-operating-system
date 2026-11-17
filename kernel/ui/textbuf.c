#include "textbuf.h"
#include "gfx.h"
#include "heap.h"
#include "string.h"

/* Rows kept in memory: the scrollback, then the live rows. */
#define ALL(tb) ((size_t)(TEXTBUF_BACK + (tb)->rows))

bool textbuf_init(struct textbuf *tb, unsigned cols, unsigned rows)
{
    memset(tb, 0, sizeof(*tb));
    if (cols == 0 || rows == 0 || cols > 512 || rows > 256)
        return false;
    tb->cols = cols;
    tb->rows = rows;
    tb->cells = kmalloc(ALL(tb) * cols);
    tb->attr = kmalloc(ALL(tb) * cols);
    if (tb->cells == NULL || tb->attr == NULL) {
        textbuf_free(tb);
        return false;
    }
    textbuf_clear(tb);
    return true;
}

void textbuf_free(struct textbuf *tb)
{
    kfree(tb->cells);
    kfree(tb->attr);
    tb->cells = NULL;
    tb->attr = NULL;
}

void textbuf_clear(struct textbuf *tb)
{
    memset(tb->cells, ' ', ALL(tb) * tb->cols);
    memset(tb->attr, 0, ALL(tb) * tb->cols);
    tb->col = tb->row = tb->hist = tb->view = 0;
}

/* Index of live row `r`, column 0. */
static size_t at(const struct textbuf *tb, unsigned r)
{
    return (size_t)(TEXTBUF_BACK + r) * tb->cols;
}

static void newline(struct textbuf *tb)
{
    tb->col = 0;
    if (tb->row + 1 < tb->rows) {
        tb->row++;
        return;
    }
    /* The top live row joins the scrollback; the oldest one is dropped. */
    size_t move = (ALL(tb) - 1) * tb->cols;
    memmove(tb->cells, tb->cells + tb->cols, move);
    memmove(tb->attr, tb->attr + tb->cols, move);
    memset(tb->cells + move, ' ', tb->cols);
    memset(tb->attr + move, 0, tb->cols);
    if (tb->hist < TEXTBUF_BACK)
        tb->hist++;
    if (tb->view > 0 && tb->view < tb->hist)
        tb->view++;                        /* keep looking at the same lines */
}

static void put(struct textbuf *tb, char c)
{
    if (tb->col >= tb->cols)
        newline(tb);
    size_t i = at(tb, tb->row) + tb->col++;
    tb->cells[i] = c;
    tb->attr[i] = tb->colour;
}

/* ESC [ n m: 30-37 normal and 90-97 bright colours, 0 or 39 default. */
static void set_colour(struct textbuf *tb, unsigned n)
{
    if (n == 0 || n == 39)
        tb->colour = 0;
    else if (n >= 30 && n <= 37)
        tb->colour = (uint8_t)(1 + n - 30);
    else if (n >= 90 && n <= 97)
        tb->colour = (uint8_t)(9 + n - 90);
}

static void escape(struct textbuf *tb, uint8_t c)
{
    if (tb->esc == 1) {
        tb->esc = c == '[' ? 2 : 0;
        tb->esc_num = 0;
    } else if (c >= '0' && c <= '9') {
        if (tb->esc_num < 1000)
            tb->esc_num = tb->esc_num * 10 + (c - '0');
    } else if (c == ';') {
        set_colour(tb, tb->esc_num);       /* ESC [1;34m: each part in turn */
        tb->esc_num = 0;
    } else if (c != '?') {
        tb->esc = 0;
        if (c == 'J' && tb->esc_num == 2)
            textbuf_clear(tb);
        else if (c == 'H')
            tb->col = tb->row = 0;
        else if (c == 'K' && tb->col < tb->cols) {
            memset(tb->cells + at(tb, tb->row) + tb->col, ' ', tb->cols - tb->col);
            memset(tb->attr + at(tb, tb->row) + tb->col, 0, tb->cols - tb->col);
        }
        else if (c == 'm')
            set_colour(tb, tb->esc_num);
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
    return tb->cells + at(tb, r < tb->rows ? r : 0);
}

const char *textbuf_shown_row(const struct textbuf *tb, unsigned r)
{
    return tb->cells + at(tb, r < tb->rows ? r : 0) - (size_t)tb->view * tb->cols;
}

uint8_t textbuf_shown_colour(const struct textbuf *tb, unsigned r, unsigned c)
{
    if (r >= tb->rows || c >= tb->cols)
        return 0;
    return tb->attr[at(tb, r) - (size_t)tb->view * tb->cols + c];
}

void textbuf_scroll(struct textbuf *tb, int rows)
{
    int v = (int)tb->view + rows;
    tb->view = v < 0 ? 0 : (unsigned)v > tb->hist ? tb->hist : (unsigned)v;
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

/* Colours for a dark background (index 1..16 = ANSI 30-37, then 90-97). */
static const uint32_t palette[17] = {
    0, 0x5C6370, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xDCDFE4,
    0x7F848E, 0xFF7A85, 0xB5E890, 0xFFD58A, 0x8CC8FF, 0xDE9BF0, 0x7FD8E2, 0xFFFFFF,
};

void textbuf_draw(const struct textbuf *tb, int32_t x, int32_t y, uint32_t rgb)
{
    for (unsigned r = 0; r < tb->rows; r++) {
        const char *row = textbuf_shown_row(tb, r);
        int32_t ry = y + (int32_t)r * FONT_MONO_H;
        /* One gfx_text_n call per run of cells with the same colour. */
        for (unsigned c = 0; c < tb->cols;) {
            uint8_t a = textbuf_shown_colour(tb, r, c);
            unsigned e = c + 1;
            while (e < tb->cols && textbuf_shown_colour(tb, r, e) == a)
                e++;
            gfx_text_n(x + (int32_t)c * FONT_MONO_W, ry, row + c, e - c, a == 0 ? rgb : palette[a], FONT_MONO);
            c = e;
        }
    }
}
