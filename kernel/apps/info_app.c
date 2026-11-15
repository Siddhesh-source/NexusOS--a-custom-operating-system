/* Health and About: windows that run shell commands and show their
 * output. Health adds a summary of the boot self-checks and the last
 * contained crash, and buttons (or keys 1-3) for the live checks, the
 * lock-order demo and the crash demo. While a Health command runs, the
 * window also catches the kernel's own messages (the lock checker's
 * report, the crash report with its call trace), which otherwise go only
 * to the serial log. */

#include "apps.h"
#include "theme.h"
#include "shell.h"
#include "crash.h"
#include "selftest.h"
#include "serial.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define INFO_COLS 84
#define INFO_ROWS 24
#define SUMMARY_H 64

struct info {
    struct textbuf tb;            /* first: app_text() relies on it */
    struct shell sh;
};

/* Text reaches the buffer only under the console lock, so the shell's
 * output and kernel messages (from any CPU) never write it at once. */
static struct textbuf *catching;

static void catch_byte(char c)
{
    textbuf_write(catching, &c, 1);       /* console lock held by the caller */
}

static void locked_out(void *ctx, const char *s, size_t n)
{
    uint64_t f = console_lock();
    textbuf_write(ctx, s, n);
    console_unlock(f);
}

static const char *const health_buttons[] = { "Run checks", "Lock-order demo", "Crash demo" };
static const char *const health_cmds[] = { "checks", "lockdemo", "crashdemo" };

static bool info_open(struct window *w, const char *arg)
{
    (void)arg;
    struct info *s = kzalloc(sizeof(*s));
    if (s == NULL || !textbuf_init(&s->tb, INFO_COLS, INFO_ROWS)) {
        kfree(s);
        return false;
    }
    w->st = s;
    shell_init(&s->sh, desktop_user(), locked_out, &s->tb);
    if (w->app == &app_about) {
        shell_run(&s->sh, "about");
        textbuf_puts(&s->tb,
            "\nThis desktop:\n"
            "  Windows are drawn into memory first, then only the changed part\n"
            "  is copied to the screen. Keyboard and mouse come in by interrupts.\n"
            "  Every app here is a kernel thread's code behind small APIs (files,\n"
            "  input, drawing), so it can move to user mode later unchanged.\n"
            "\nMade in India, from scratch: no Linux, no libraries.\n");
    } else {
        textbuf_puts(&s->tb, "Run the live checks, or watch the lock checker and a contained\ncrash at work. Keys 1, 2 and 3 do the same.\n");
    }
    return true;
}

static void info_close(struct window *w)
{
    textbuf_free(&((struct info *)w->st)->tb);
}

/* About: a title, then each "  Label:  value" line of the `about`
 * output as two columns; other lines as plain text. */
static void about_draw(struct info *s, struct gfx_rect body)
{
    int32_t x = body.x + 28, y = body.y + 20;
    gfx_text(x - 3, y, "BharatOS", COL_TEXT, FONT_DISPLAY);
    y += 76;
    for (unsigned r = 0; r < s->tb.rows; r++) {
        char line[INFO_COLS + 1];
        memcpy(line, textbuf_row(&s->tb, r), INFO_COLS);
        unsigned n = INFO_COLS;
        while (n > 0 && line[n - 1] == ' ')
            n--;
        line[n] = '\0';
        char *t = line;
        while (*t == ' ')
            t++;
        char *colon = strchr(t, ':');
        if (*t == '\0') {
            y += 10;                                     /* a blank line: a gap */
        } else if (t - line >= 8) {                      /* continues the value above */
            gfx_text(x + 140, y, t, COL_TEXT, FONT_UI);
            y += 22;
        } else if (t - line == 2 && colon != NULL && colon - t < 16) {
            *colon = '\0';
            char *v = colon + 1;
            while (*v == ' ')
                v++;
            gfx_text(x, y, t, COL_MUTED, FONT_UI);
            gfx_text(x + 140, y, v, COL_TEXT, FONT_UI);
            y += 22;
        } else {
            gfx_text(x, y, t, t == line ? COL_TEXT : COL_MUTED, t == line ? FONT_MEDIUM : FONT_UI);
            y += 24;
        }
    }
}

static void info_draw(struct window *w, struct gfx_rect body)
{
    struct info *s = w->st;
    if (w->app == &app_about) {
        about_draw(s, body);
        return;
    }
    ui_buttons(body, health_buttons, 3, 0, true, -1, -1);
    int32_t x = body.x + UI_PAD, y = body.y + UI_BAR_H;
    unsigned passed, failed;
    selftest_totals(&passed, &failed);
    char line[96];
    gfx_round(gfx_rect(x + 2, y + 10, 10, 10), 5, failed == 0 ? COL_OK : COL_DANGER, 255);
    gfx_text(x + 20, y, failed == 0 ? "All checks passed" : "Some checks failed", COL_TEXT, FONT_HEADING);
    struct crash_info c;
    crash_get_last(&c);
    if (c.count == 0)
        ksnprintf(line, sizeof(line), "%u boot checks passed, %u failed. No crashes so far.", passed, failed);
    else
        ksnprintf(line, sizeof(line), "%u boot checks passed, %u failed. %lu crash%s contained (last: \"%s\").",
                  passed, failed, c.count, c.count == 1 ? "" : "es", c.name);
    gfx_text(x + 20, y + 30, line, COL_MUTED, FONT_SMALL);
    struct gfx_rect panel = gfx_rect(x, y + SUMMARY_H, body.w - 2 * UI_PAD,
                                     INFO_ROWS * FONT_MONO_H + 16);
    gfx_round(panel, UI_RADIUS_SM, COL_SURFACE_2, 255);
    textbuf_draw(&s->tb, panel.x + 12, panel.y + 8, COL_TEXT);
}

static void health_run(struct window *w, int which)
{
    struct info *s = w->st;
    if (which < 0 || which > 2)
        return;
    textbuf_clear(&s->tb);
    textbuf_puts(&s->tb, "$ ");
    textbuf_puts(&s->tb, health_cmds[which]);
    textbuf_puts(&s->tb, "\n");
    uint64_t f = console_lock();
    catching = &s->tb;
    serial_mirror_t old = serial_set_mirror(catch_byte);
    console_unlock(f);
    shell_run(&s->sh, health_cmds[which]);
    f = console_lock();
    serial_set_mirror(old);
    catching = NULL;
    console_unlock(f);
    wm_redraw(w);
}

static void health_key(struct window *w, uint16_t key)
{
    health_run(w, (int)key - '1');
}

static void health_click(struct window *w, int32_t x, int32_t y)
{
    struct gfx_rect b = wm_body(w);
    health_run(w, ui_buttons(gfx_rect(0, 0, b.w, b.h), health_buttons, 3, 0, false, x, y));
}

static void health_tick(struct window *w)
{
    /* The crash line can change at any time (a crash anywhere). */
    struct gfx_rect b = wm_body(w);
    wm_damage(gfx_rect(b.x, b.y + UI_BAR_H, b.w, SUMMARY_H));
}

const struct app app_health = {
    .name = "Health",
    .w = INFO_COLS * FONT_MONO_W + 2 * UI_PAD + 24,
    .h = UI_BAR_H + SUMMARY_H + INFO_ROWS * FONT_MONO_H + 16 + UI_PAD,
    .tile = COL_OK,
    .icon = icon_health,
    .open = info_open,
    .close = info_close,
    .draw = info_draw,
    .key = health_key,
    .click = health_click,
    .tick = health_tick,
};

const struct app app_about = {
    .name = "About",
    .w = 660,
    .h = 540,
    .tile = 0x3A4A63,
    .icon = icon_about,
    .open = info_open,
    .close = info_close,
    .draw = info_draw,
};
