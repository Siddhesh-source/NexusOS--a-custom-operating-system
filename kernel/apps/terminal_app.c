/* Terminal: the Phase 10 shell and line editor, with a text buffer as
 * their screen. Health and About reuse the same idea: they run shell
 * commands and show what they print. Page Up / Page Down look back
 * through the last TEXTBUF_BACK lines that scrolled off the top. */

#include "apps.h"
#include "theme.h"
#include "shell.h"
#include "lineedit.h"
#include "heap.h"
#include "string.h"
#include "input.h"
#include "kprintf.h"

#define TERM_COLS 80
#define TERM_ROWS 25

struct term {
    struct textbuf tb;            /* first: app_text() relies on it */
    struct shell sh;
    struct lineedit le;
};

static void prompt(struct term *t)
{
    char p[FS_PATH_MAX + 40];
    shell_prompt(&t->sh, p, sizeof(p));
    textbuf_puts(&t->tb, p);
    le_begin(&t->le, false);
}

static bool term_open(struct window *w, const char *arg)
{
    (void)arg;
    struct term *t = kzalloc(sizeof(*t));
    if (t == NULL || !textbuf_init(&t->tb, TERM_COLS, TERM_ROWS)) {
        kfree(t);
        return false;
    }
    w->st = t;
    shell_init(&t->sh, desktop_user(), textbuf_out, &t->tb);
    le_init(&t->le, textbuf_out, &t->tb);
    t->le.complete = shell_complete;
    t->le.cctx = &t->sh;
    textbuf_puts(&t->tb, "BharatOS terminal. Type 'help' to see the commands; Tab completes names.\n");
    prompt(t);
    return true;
}

static void term_close(struct window *w)
{
    textbuf_free(&((struct term *)w->st)->tb);
}

static void term_draw(struct window *w, struct gfx_rect body)
{
    struct term *t = w->st;              /* the window manager paints the dark body */
    if (t->tb.view == 0)
        gfx_fill(gfx_rect(body.x + UI_PAD + (int32_t)t->tb.col * FONT_MONO_W,
                          body.y + 12 + (int32_t)t->tb.row * FONT_MONO_H + 1,
                          FONT_MONO_W, FONT_MONO_H - 2), COL_SAFFRON);     /* block caret */
    textbuf_draw(&t->tb, body.x + UI_PAD, body.y + 12, COL_TERM_TEXT);
    if (t->tb.view > 0) {
        char s[48];
        ksnprintf(s, sizeof(s), "%u lines back  (Page Down)", t->tb.view);
        gfx_text(body.x + body.w - UI_PAD - gfx_text_width(s, FONT_SMALL), body.y + 4, s, COL_SAFFRON, FONT_SMALL);
    }
}

static void term_key(struct window *w, uint16_t key)
{
    struct term *t = w->st;
    /* Page Up/Down look through the scrollback; any other key goes back
     * to the live rows first. */
    if (key == INPUT_KEY_PAGE_UP || key == INPUT_KEY_PAGE_DOWN) {
        textbuf_scroll(&t->tb, key == INPUT_KEY_PAGE_UP ? TERM_ROWS - 2 : -(TERM_ROWS - 2));
        wm_redraw(w);
        return;
    }
    t->tb.view = 0;
    enum le_result r = le_key(&t->le, key);
    if (r == LE_DONE) {
        char line[LE_MAX];
        strlcpy(line, t->le.buf, sizeof(line));
        le_add_history(&t->le, line);
        shell_run(&t->sh, line);
        if (t->sh.logout) {
            wm_close(w);                  /* `logout` closes the terminal */
            return;
        }
    }
    if (r != LE_MORE)
        prompt(t);
    wm_redraw(w);
}

const struct app app_terminal = {
    .name = "Terminal",
    .w = TERM_COLS * FONT_MONO_W + 2 * UI_PAD,
    .h = TERM_ROWS * FONT_MONO_H + 24,
    .dark = true,
    .tile = 0x1A212B,
    .icon = icon_terminal,
    .open = term_open,
    .close = term_close,
    .draw = term_draw,
    .key = term_key,
};

struct textbuf *app_text(struct window *w)
{
    if (w == NULL || !w->used || w->st == NULL)
        return NULL;
    if (w->app == &app_terminal || w->app == &app_health || w->app == &app_about)
        return w->st;                     /* each state starts with its textbuf */
    return NULL;
}
