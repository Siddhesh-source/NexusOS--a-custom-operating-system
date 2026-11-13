/* Terminal: the Phase 10 shell and line editor, with a text buffer as
 * their screen. Health and About reuse the same idea: they run shell
 * commands and show what they print. */

#include "apps.h"
#include "theme.h"
#include "shell.h"
#include "lineedit.h"
#include "heap.h"
#include "string.h"

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
    textbuf_puts(&t->tb, "BharatOS terminal. Type 'help' to see the commands.\n");
    prompt(t);
    return true;
}

static void term_close(struct window *w)
{
    textbuf_free(&((struct term *)w->st)->tb);
}

static void term_draw(struct window *w, struct gfx_rect body)
{
    struct term *t = w->st;
    gfx_fill(body, COL_TERM_BG);
    textbuf_draw(&t->tb, body.x + UI_PAD, body.y + UI_PAD, COL_TERM_TEXT);
    gfx_fill(gfx_rect(body.x + UI_PAD + (int32_t)t->tb.col * GFX_CHAR_W,
                      body.y + UI_PAD + (int32_t)t->tb.row * GFX_CHAR_H + GFX_CHAR_H - 3,
                      GFX_CHAR_W, 2), COL_SAFFRON);
}

static void term_key(struct window *w, uint16_t key)
{
    struct term *t = w->st;
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
    .w = TERM_COLS * GFX_CHAR_W + 2 * UI_PAD,
    .h = TERM_ROWS * GFX_CHAR_H + 2 * UI_PAD,
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
