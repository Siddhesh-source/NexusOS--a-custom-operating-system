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
#define INFO_ROWS 28
#define SUMMARY_H 48

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

static const char *const health_buttons[] = { "1 Run checks", "2 Lock-order demo", "3 Crash demo" };
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
        textbuf_puts(&s->tb, "Press a button above (or 1, 2, 3) to run a check or a demo.\n");
    }
    return true;
}

static void info_close(struct window *w)
{
    textbuf_free(&((struct info *)w->st)->tb);
}

static void info_draw(struct window *w, struct gfx_rect body)
{
    struct info *s = w->st;
    int32_t y = body.y + UI_PAD;
    if (w->app == &app_health) {
        ui_buttons(body, health_buttons, 3, true, -1, -1);
        y = body.y + UI_BAR_H;
        unsigned passed, failed;
        selftest_totals(&passed, &failed);
        char line[96];
        ksnprintf(line, sizeof(line), "Boot self-checks: %u passed, %u failed", passed, failed);
        gfx_text(body.x + UI_PAD, y, line, failed == 0 ? COL_OK : COL_DANGER, 1);
        struct crash_info c;
        crash_get_last(&c);
        if (c.count == 0)
            ksnprintf(line, sizeof(line), "Crashes contained since boot: none");
        else
            ksnprintf(line, sizeof(line), "Crashes contained since boot: %lu (last: thread \"%s\", TID %u)",
                      c.count, c.name, c.tid);
        gfx_text(body.x + UI_PAD, y + 20, line, c.count == 0 ? COL_MUTED : COL_WARN, 1);
        y += SUMMARY_H;
        gfx_fill(gfx_rect(body.x, y - 6, body.w, 1), COL_BORDER);
    }
    textbuf_draw(&s->tb, body.x + UI_PAD, y, COL_TEXT);
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
    health_run(w, ui_buttons(gfx_rect(0, 0, b.w, b.h), health_buttons, 3, false, x, y));
}

static void health_tick(struct window *w)
{
    /* The crash line can change at any time (a crash anywhere). */
    struct gfx_rect b = wm_body(w);
    wm_damage(gfx_rect(b.x, b.y + UI_BAR_H, b.w, SUMMARY_H));
}

const struct app app_health = {
    .name = "Health",
    .w = INFO_COLS * GFX_CHAR_W + 2 * UI_PAD,
    .h = UI_BAR_H + SUMMARY_H + INFO_ROWS * GFX_CHAR_H + UI_PAD,
    .open = info_open,
    .close = info_close,
    .draw = info_draw,
    .key = health_key,
    .click = health_click,
    .tick = health_tick,
};

const struct app app_about = {
    .name = "About",
    .w = INFO_COLS * GFX_CHAR_W + 2 * UI_PAD,
    .h = 22 * GFX_CHAR_H + 2 * UI_PAD,
    .open = info_open,
    .close = info_close,
    .draw = info_draw,
};
