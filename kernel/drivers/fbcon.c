#include "fbcon.h"
#include "cpu.h"
#include "serial.h"
#include "string.h"
#include "font8x8.h"

#define CELL_W 8
#define CELL_H 16
#define FG 0x00E6E6E6u          /* near-white text */
#define BG 0x00102030u          /* dark blue-grey background */

static struct {
    uint8_t *base;
    uint64_t pitch;
    uint8_t rs, gs, bs;         /* colour channel bit positions */
    uint32_t fg, bg;            /* in the framebuffer's pixel format */
    uint32_t utf8_cp;           /* code point being decoded */
    unsigned utf8_left;         /* continuation bytes still expected */
    uint8_t esc;                /* 0 normal, 1 after ESC, 2 inside ESC [ */
    unsigned esc_num;           /* number inside ESC [ n X */
    bool cursor_shown;
    unsigned cursor_col, cursor_row;
    uint64_t screen_h;          /* pixels below the area's top */
    uint8_t *screen;            /* the whole screen (fb.base is the area) */
} fb;

static struct fbcon_info st;

static uint32_t pack(uint32_t rgb)
{
    return (((rgb >> 16) & 0xFF) << fb.rs) | (((rgb >> 8) & 0xFF) << fb.gs)
         | ((rgb & 0xFF) << fb.bs);
}

static inline uint32_t *pixel(uint64_t x, uint64_t y)
{
    return (uint32_t *)(fb.base + y * fb.pitch + x * 4);
}

static void clear_row(unsigned row)
{
    for (uint64_t y = (uint64_t)row * CELL_H; y < (uint64_t)(row + 1) * CELL_H; y++) {
        uint32_t *p = pixel(0, y);
        for (uint64_t x = 0; x < (uint64_t)st.cols * CELL_W; x++)
            p[x] = fb.bg;
    }
}

static void draw(char c)
{
    unsigned idx = (unsigned char)c >= 0x20 && (unsigned char)c < 0x7F ? (unsigned)c - 0x20 : '?' - 0x20;
    const uint8_t *g = font8x8[idx];
    uint64_t x0 = (uint64_t)st.col * CELL_W, y0 = (uint64_t)st.row * CELL_H;
    for (unsigned r = 0; r < CELL_H; r++) {
        uint8_t bits = g[r / 2];             /* each font row drawn twice */
        uint32_t *p = pixel(x0, y0 + r);
        for (unsigned b = 0; b < CELL_W; b++)
            p[b] = (bits >> b) & 1 ? fb.fg : fb.bg;
    }
    st.chars_drawn++;
}

static void newline(void)
{
    st.col = 0;
    st.row = (st.row + 1) % st.rows;   /* wrap to the top: no scrolling */
    clear_row(st.row);
    /* Keep the next row blank too: a visible gap between the newest line
     * and the oldest text still on screen. */
    clear_row((st.row + 1) % st.rows);
}

static void put_visible(char c)
{
    if (st.col >= st.cols)
        newline();
    draw(c);
    st.col++;
}

/* Box-drawing characters (U+2500-U+257F) become + - |; other non-ASCII
 * characters become '?'. */
static char from_code_point(uint32_t cp)
{
    if (cp >= 0x2500 && cp <= 0x257F) {
        if (cp == 0x2500 || cp == 0x2501 || cp == 0x2550)
            return '-';
        if (cp == 0x2502 || cp == 0x2503 || cp == 0x2551)
            return '|';
        return '+';
    }
    return '?';
}

/* The cursor: the bottom two pixel rows of the next character cell,
 * inverted. Inverting again removes it without knowing what is under. */
static void cursor_flip(unsigned col, unsigned row)
{
    if (col >= st.cols || row >= st.rows)
        return;
    for (unsigned r = CELL_H - 2; r < CELL_H; r++) {
        uint32_t *p = pixel((uint64_t)col * CELL_W, (uint64_t)row * CELL_H + r);
        for (unsigned b = 0; b < CELL_W; b++)
            p[b] = p[b] == fb.fg ? fb.bg : fb.fg;
    }
}

static void clear_screen(void)
{
    for (unsigned r = 0; r < st.rows; r++)
        clear_row(r);
    st.col = st.row = 0;
}

/* ESC [ n J (clear screen), ESC [ H (top left), ESC [ K (clear to the end
 * of the line). Other sequences are swallowed so they never show as text. */
static void escape_byte(uint8_t c)
{
    if (fb.esc == 1) {
        fb.esc = c == '[' ? 2 : 0;
        fb.esc_num = 0;
        return;
    }
    if (c >= '0' && c <= '9') {
        fb.esc_num = fb.esc_num * 10 + (c - '0');
        return;
    }
    if (c == ';' || c == '?')
        return;
    fb.esc = 0;
    if (c == 'J' && fb.esc_num == 2) {
        clear_screen();
    } else if (c == 'H') {
        st.col = st.row = 0;
    } else if (c == 'K') {
        unsigned col = st.col;
        while (st.col < st.cols)
            put_visible(' ');
        st.col = col;
    }
}

static void putc_inner(uint8_t c)
{
    if (fb.esc != 0) {
        escape_byte(c);
        return;
    }
    if (fb.utf8_left > 0) {
        if ((c & 0xC0) == 0x80) {
            fb.utf8_cp = (fb.utf8_cp << 6) | (c & 0x3F);
            if (--fb.utf8_left == 0)
                put_visible(from_code_point(fb.utf8_cp));
            return;
        }
        fb.utf8_left = 0;                   /* broken sequence: drop it */
    }
    if (c >= 0xC0) {                        /* start of a multi-byte character */
        fb.utf8_left = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        fb.utf8_cp = c & (0x3F >> fb.utf8_left);
        return;
    }
    switch (c) {
    case 0x1B: fb.esc = 1; break;
    case '\b': if (st.col > 0) st.col--; break;
    case '\n': newline(); break;
    case '\r': st.col = 0; break;
    case '\t': do { put_visible(' '); } while (st.col % 8 != 0 && st.col < st.cols); break;
    default:
        if (c >= 0x20 && c < 0x7F)
            put_visible((char)c);
        break;
    }
}

void fbcon_putc(char ch)
{
    if (!st.active)
        return;
    /* Called with the console lock held (from serial_putchar's mirror).
     * The cursor is removed before anything changes and drawn again
     * after, so it never leaves marks behind. */
    if (fb.cursor_shown)
        cursor_flip(fb.cursor_col, fb.cursor_row);
    putc_inner((uint8_t)ch);
    fb.cursor_col = st.col;
    fb.cursor_row = st.row;
    cursor_flip(fb.cursor_col, fb.cursor_row);
    fb.cursor_shown = true;
}

bool fbcon_init(void *addr, uint64_t width, uint64_t height, uint64_t pitch,
                uint16_t bpp, uint8_t red_shift, uint8_t green_shift, uint8_t blue_shift)
{
    if (addr == 0 || bpp != 32 || width < CELL_W || height < CELL_H || pitch < width * 4)
        return false;
    fb.base = fb.screen = addr;
    fb.screen_h = height;
    fb.pitch = pitch;
    fb.rs = red_shift;
    fb.gs = green_shift;
    fb.bs = blue_shift;
    fb.fg = pack(FG);
    fb.bg = pack(BG);
    st.width = width;
    st.height = height;
    st.cols = (unsigned)(width / CELL_W);
    st.rows = (unsigned)(height / CELL_H);
    st.col = st.row = 0;
    for (unsigned r = 0; r < st.rows; r++)
        clear_row(r);
    st.active = true;
    serial_set_mirror(fbcon_putc);
    return true;
}

void fbcon_get_info(struct fbcon_info *out)
{
    uint64_t flags = console_lock();
    *out = st;
    console_unlock(flags);
}

uint32_t fbcon_read_pixel(uint64_t x, uint64_t y)
{
    if (!st.active || x >= st.width || y >= st.height)
        return 0;
    uint32_t v = *pixel(x, y);
    return (((v >> fb.rs) & 0xFF) << 16) | (((v >> fb.gs) & 0xFF) << 8) | ((v >> fb.bs) & 0xFF);
}

bool fbcon_set_top(uint64_t top)
{
    uint64_t flags = console_lock();
    bool ok = st.active && top + 25 * CELL_H <= fb.screen_h;
    if (ok) {
        fb.base = fb.screen + top * fb.pitch;
        st.height = fb.screen_h - top;
        st.rows = (unsigned)(st.height / CELL_H);
        fb.cursor_shown = false;
        clear_screen();
    }
    console_unlock(flags);
    return ok;
}

void fbcon_stop(void)
{
    uint64_t flags = console_lock();
    serial_set_mirror(NULL);
    st.active = false;
    console_unlock(flags);
}

void fbcon_resume(void)
{
    if (fb.screen == NULL || st.active)
        return;
    /* No lock: called while the machine is stopping (panic), when the
     * lock's holder may never let go. */
    fb.base = fb.screen;
    st.height = fb.screen_h;
    st.rows = (unsigned)(st.height / CELL_H);
    fb.cursor_shown = false;
    clear_screen();
    st.active = true;
    serial_set_mirror(fbcon_putc);
}
