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
#define ROW_H   40
#define MON_CPUS 8                        /* rows shown */

struct monitor {
    struct sched_stats prev[MON_CPUS];
    uint64_t prev_ticks;
    uint8_t busy[MON_CPUS][HISTORY];      /* newest last */
    unsigned threads;
    struct shell sh;
    char msg[64];
};

static const char *const mon_buttons[] = { "L Add load (10 s)" };

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
    w->r.h = UI_TITLE_H + 1 + UI_BAR_H + (int32_t)rows * ROW_H + 64;   /* fit the CPUs */
    return true;
}

static void mon_draw(struct window *w, struct gfx_rect body)
{
    struct monitor *m = w->st;
    ui_buttons(body, mon_buttons, 1, true, -1, -1);
    int32_t x = body.x + UI_PAD, y = body.y + UI_BAR_H;
    char line[96];
    gfx_text(x + 180, body.y + UI_PAD + 4, m->msg, COL_MUTED, 1);
    for (unsigned c = 0; c < MON_CPUS; c++) {
        if (!cpu_at(c)->online)
            continue;
        struct sched_stats *s = &m->prev[c];
        unsigned busy = m->busy[c][HISTORY - 1];
        ksnprintf(line, sizeof(line), "CPU %u %3u%%", c, busy);
        gfx_text(x, y + 4, line, COL_TEXT, 1);
        struct gfx_rect bar = gfx_rect(x + 96, y + 4, 160, 16);
        gfx_fill(bar, COL_BORDER);
        gfx_fill(gfx_rect(bar.x, bar.y, bar.w * (int32_t)busy / 100, bar.h),
                 busy > 80 ? COL_SAFFRON : COL_ACCENT);
        /* The last minute: one 3-pixel column per second. */
        struct gfx_rect g = gfx_rect(x + 272, y, HISTORY * 3, 28);
        gfx_fill(g, COL_BG);
        for (unsigned i = 0; i < HISTORY; i++) {
            int32_t h = (g.h * m->busy[c][i]) / 100;
            gfx_fill(gfx_rect(g.x + (int32_t)i * 3, g.y + g.h - h, 2, h), COL_ACCENT);
        }
        ksnprintf(line, sizeof(line), "steals %lu  IPIs %lu", s->steals, s->ipis);
        gfx_text(g.x + g.w + 12, y + 4, line, COL_MUTED, 1);
        y += ROW_H;
    }
    struct pmm_stats ps;
    pmm_get_stats(&ps);
    uint64_t mib = 1024 * 1024, total = ps.managed_pages * 4096 / mib, used = ps.allocated_pages * 4096 / mib;
    ksnprintf(line, sizeof(line), "Memory  %lu of %lu MiB in use", used, total);
    gfx_text(x, y + 8, line, COL_TEXT, 1);
    struct gfx_rect bar = gfx_rect(x + 272, y + 8, HISTORY * 3, 16);
    gfx_fill(bar, COL_BORDER);
    gfx_fill(gfx_rect(bar.x, bar.y, total == 0 ? 0 : bar.w * (int32_t)(used * 100 / total) / 100, bar.h), COL_OK);
    ksnprintf(line, sizeof(line), "Threads %u   Busy %% is over the last second; the graph is the last minute.",
              m->threads);
    gfx_text(x, y + 36, line, COL_MUTED, 1);
}

static void add_load(struct window *w)
{
    struct monitor *m = w->st;
    if (shell_run(&m->sh, "run load 10") == 0)
        ksnprintf(m->msg, sizeof(m->msg), "Started %u busy threads for 10 s.", smp_cpu_count());
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
    if (ui_buttons(gfx_rect(0, 0, b.w, b.h), mon_buttons, 1, false, x, y) == 0)
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
    .w = 2 * UI_PAD + 272 + HISTORY * 3 + 12 + 24 * GFX_CHAR_W,
    .h = UI_BAR_H + 4 * ROW_H + 64,
    .open = mon_open,
    .draw = mon_draw,
    .key = mon_key,
    .click = mon_click,
    .tick = mon_tick,
};
