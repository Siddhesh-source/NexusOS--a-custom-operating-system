/* Monitor: how busy each CPU is, live. Once a second it samples every
 * CPU's scheduler counters (idle time, work steals, IPIs) and shows the
 * busy % as a bar plus the last minute as a small graph. "Add load"
 * starts one busy thread per CPU for 10 s (the shell's `run load`), so
 * the bars fill up and steals show work spreading between CPUs. */

#include "apps.h"
#include "theme.h"
#include "shell.h"
#include "sched.h"
#include "percpu.h"
#include "proc.h"
#include "rcu.h"
#include "pmm.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define HISTORY 60
#define ROW_H   56
#define BAR_X   150                    /* from the left padding */
#define BAR_W   220
#define GRAPH_X (BAR_X + BAR_W + 24)
#define MON_CPUS 8                        /* rows shown */

struct monitor {
    struct sched_stats prev[MON_CPUS];
    uint64_t prev_ticks;
    uint8_t busy[MON_CPUS][HISTORY];      /* newest last */
    unsigned threads;
    struct shell sh;
    char msg[64];
};

static const char *const mon_buttons[] = { "Add load" };

/* The shell's own messages are not shown: the window says it in one line. */
static void discard(void *ctx, const char *s, size_t n)
{
    (void)ctx; (void)s; (void)n;
}

static void sample(struct monitor *m)
{
    uint64_t now = sched_ticks(), dt = now - m->prev_ticks;
    for (unsigned c = 0; c < MON_CPUS; c++) {
        if (!cpu_at(c)->online)
            continue;
        struct sched_stats s;
        sched_get_cpu_stats(c, &s);
        uint64_t idle = s.idle_ticks - m->prev[c].idle_ticks;
        unsigned busy = dt == 0 || idle >= dt ? 0 : (unsigned)(100 - idle * 100 / dt);
        memmove(m->busy[c], m->busy[c] + 1, HISTORY - 1);
        m->busy[c][HISTORY - 1] = (uint8_t)busy;
        m->prev[c] = s;
    }
    m->prev_ticks = now;
    unsigned n = 0;
    uint64_t f = rcu_read_lock();
    for (unsigned slot = 0; slot < THREAD_MAX; slot++)
        n += thread_at(slot) != NULL;
    rcu_read_unlock(f);
    m->threads = n;
}

static bool mon_open(struct window *w, const char *arg)
{
    (void)arg;
    struct monitor *m = kzalloc(sizeof(*m));
    if (m == NULL)
        return false;
    w->st = m;
    shell_init(&m->sh, desktop_user(), discard, NULL);
    sample(m);
    unsigned rows = smp_cpu_count() < MON_CPUS ? smp_cpu_count() : MON_CPUS;
    w->r.h = UI_TITLE_H + UI_BAR_H + (int32_t)rows * ROW_H + 72;   /* fit the CPUs */
    return true;
}

/* A thin rounded bar: track, then the filled part. */
static void bar(int32_t x, int32_t y, int32_t w, unsigned pct, uint32_t rgb)
{
    gfx_round(gfx_rect(x, y, w, 6), 3, COL_LINE, 255);
    if (pct > 0)
        gfx_round(gfx_rect(x, y, w * (int32_t)pct / 100 < 6 ? 6 : w * (int32_t)pct / 100, 6), 3, rgb, 255);
}

static void mon_draw(struct window *w, struct gfx_rect body)
{
    struct monitor *m = w->st;
    struct gfx_rect b0 = gfx_rect(body.x, body.y, body.w, body.h);
    ui_buttons(b0, mon_buttons, 1, 0, true, -1, -1);
    gfx_text(body.x + UI_PAD + gfx_text_width(mon_buttons[0], FONT_MEDIUM) + 44,
             body.y + 18, m->msg, COL_MUTED, FONT_SMALL);
    int32_t x = body.x + UI_PAD, y = body.y + UI_BAR_H;
    char line[96];
    for (unsigned c = 0; c < MON_CPUS; c++) {
        if (!cpu_at(c)->online)
            continue;
        struct sched_stats *s = &m->prev[c];
        unsigned busy = m->busy[c][HISTORY - 1];
        ksnprintf(line, sizeof(line), "CPU %u", c);
        gfx_text(x, y + 4, line, COL_TEXT, FONT_MEDIUM);
        ksnprintf(line, sizeof(line), "%u%%", busy);
        gfx_text(x + BAR_X - 16 - gfx_text_width(line, FONT_HEADING), y, line, COL_TEXT, FONT_HEADING);
        bar(x + BAR_X, y + 10, BAR_W, busy, busy > 80 ? COL_PRIMARY : COL_TEAL);
        ksnprintf(line, sizeof(line), "%lu steals  /  %lu IPIs", s->steals, s->ipis);
        gfx_text(x + BAR_X, y + 22, line, COL_MUTED, FONT_SMALL);
        /* The last minute: one column per second. */
        struct gfx_rect g = gfx_rect(x + GRAPH_X, y, HISTORY * 3, 40);
        gfx_round(g, 6, COL_SURFACE_2, 255);
        for (unsigned i = 0; i < HISTORY; i++) {
            int32_t h = (g.h - 8) * m->busy[c][i] / 100;
            if (h > 0)
                gfx_fill(gfx_rect(g.x + (int32_t)i * 3, g.y + g.h - 4 - h, 2, h), COL_TEAL);
        }
        y += ROW_H;
    }
    struct pmm_stats ps;
    pmm_get_stats(&ps);
    uint64_t mib = 1024 * 1024, total = ps.managed_pages * 4096 / mib, used = ps.allocated_pages * 4096 / mib;
    gfx_fill(gfx_rect(x, y - 4, body.w - 2 * UI_PAD, 1), COL_LINE);
    gfx_text(x, y + 10, "Memory", COL_TEXT, FONT_MEDIUM);
    bar(x + BAR_X, y + 18, BAR_W, total == 0 ? 0 : (unsigned)(used * 100 / total), COL_OK);
    ksnprintf(line, sizeof(line), "%lu of %lu MiB in use  /  %u threads", used, total, m->threads);
    gfx_text(x + BAR_X, y + 30, line, COL_MUTED, FONT_SMALL);
}

static void add_load(struct window *w)
{
    struct monitor *m = w->st;
    if (shell_run(&m->sh, "run load 10") == 0)
        ksnprintf(m->msg, sizeof(m->msg), "%u busy threads for 10 seconds", smp_cpu_count());
    else
        strlcpy(m->msg, "Could not start the busy threads.", sizeof(m->msg));
    wm_redraw(w);
}

static void mon_key(struct window *w, uint16_t key)
{
    if (key == 'l' || key == 'L')
        add_load(w);
}

static void mon_click(struct window *w, int32_t x, int32_t y)
{
    struct gfx_rect b = wm_body(w);
    if (ui_buttons(gfx_rect(0, 0, b.w, b.h), mon_buttons, 1, 0, false, x, y) == 0)
        add_load(w);
}

static void mon_tick(struct window *w)
{
    sample(w->st);
    wm_redraw(w);
}

unsigned monitor_busy(struct window *w, unsigned cpu)
{
    if (w == NULL || w->app != &app_monitor || cpu >= MON_CPUS)
        return 0;
    return ((struct monitor *)w->st)->busy[cpu][HISTORY - 1];
}

const struct app app_monitor = {
    .name = "Monitor",
    .w = 2 * UI_PAD + GRAPH_X + HISTORY * 3,
    .h = UI_BAR_H + 4 * ROW_H + 72,
    .tile = COL_TEAL,
    .icon = icon_monitor,
    .open = mon_open,
    .draw = mon_draw,
    .key = mon_key,
    .click = mon_click,
    .tick = mon_tick,
};
