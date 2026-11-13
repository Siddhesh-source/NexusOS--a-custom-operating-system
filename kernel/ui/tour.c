/* The tour: a scripted walk through the desktop, run by the desktop
 * thread one step at a time between frames, so every step is drawn as it
 * happens. It types into the apps exactly as a person would (same key
 * handling), drags a window with simulated mouse moves, and checks the
 * results on the way: a failed check prints a [FAIL] line, which fails
 * the boot test that runs the quick version. */

#include "desktop.h"
#include "apps.h"
#include "theme.h"
#include "input.h"
#include "fs.h"
#include "sched.h"
#include "kprintf.h"
#include "string.h"

enum op { SAY, LOGIN, OPEN, TYPE, KEY, SELECT, DRAG, EXPECT, EXPECT_FILE, EXPECT_LOAD, END };

struct step {
    uint16_t wait_ms;              /* pause before this step */
    uint8_t op;
    const char *text;
    const struct app *app;
    uint16_t key;
};

static const struct step steps[] = {
    { 300, LOGIN, "demo", 0, 0 },
    { 600, SAY, "Terminal: files and CPUs", 0, 0 },
    { 200, OPEN, 0, &app_terminal, 0 },
    { 600, TYPE, "ls /\n", 0, 0 },
    { 900, TYPE, "cat /etc/motd\n", 0, 0 },
    { 1200, TYPE, "echo hello from the tour > tour.txt\n", 0, 0 },
    { 400, TYPE, "cat tour.txt\n", 0, 0 },
    { 300, EXPECT, "hello from the tour", 0, 0 },
    { 600, TYPE, "cpus\n", 0, 0 },
    { 1500, DRAG, 0, 0, 0 },
    { 600, SAY, "Files: create, open, edit and delete", 0, 0 },
    { 200, OPEN, 0, &app_files, 0 },
    { 800, TYPE, "nnotes.txt\n", 0, 0 },
    { 800, SELECT, "tour.txt", 0, 0 },
    { 700, KEY, 0, 0, '\n' },
    { 700, TYPE, "and edited in the text editor.\n", 0, 0 },
    { 500, KEY, 0, 0, 19 },                       /* Ctrl+S */
    { 300, EXPECT_FILE, "edited in the text editor", 0, 0 },
    { 1200, KEY, 0, 0, 23 },                      /* Ctrl+W */
    { 600, SELECT, "notes.txt", 0, 0 },
    { 700, KEY, 0, 0, INPUT_KEY_DELETE },
    { 1000, SAY, "Monitor: a busy thread on every CPU", 0, 0 },
    { 200, OPEN, 0, &app_monitor, 0 },
    { 1500, TYPE, "l", 0, 0 },
    { 6000, EXPECT_LOAD, 0, 0, 0 },
    { 600, SAY, "Health: checks, the lock-order checker, a contained crash", 0, 0 },
    { 200, OPEN, 0, &app_health, 0 },
    { 800, TYPE, "1", 0, 0 },
    { 300, EXPECT, "All good", 0, 0 },
    { 2500, TYPE, "2", 0, 0 },
    { 300, EXPECT, "The checker caught it", 0, 0 },
    { 3000, TYPE, "3", 0, 0 },
    { 300, EXPECT, "The crash was contained", 0, 0 },
    { 3000, SAY, "About", 0, 0 },
    { 200, OPEN, 0, &app_about, 0 },
    { 3000, END, 0, 0, 0 },
};
#define NSTEPS (sizeof(steps) / sizeof(steps[0]))

static struct {
    bool on, fast;
    unsigned step;
    uint64_t at;                   /* tick when the current step may run */
    const char *typing;            /* rest of a TYPE step's text */
    unsigned drag;                 /* DRAG: moves left to make */
    int32_t dx, dy;
    unsigned failed;
} t;

static uint64_t ms_to_ticks(unsigned ms)
{
    if (t.fast)
        ms /= 4;
    return (uint64_t)ms * SCHED_TIMER_HZ / 1000;
}

void tour_start(bool fast)
{
    if (t.on)
        return;
    memset(&t, 0, sizeof(t));
    t.on = true;
    t.fast = fast;
    t.at = sched_ticks() + ms_to_ticks(steps[0].wait_ms);
    kprintf("[tour] Starting the %stour of the desktop.\n", fast ? "quick " : "");
}

bool tour_running(void)
{
    return t.on;
}

static void check(bool ok, const char *what)
{
    if (ok) {
        kprintf("[tour] check passed: %s\n", what);
    } else {
        kprintf("[FAIL] tour check: %s\n", what);
        t.failed++;
    }
}

static bool file_has(const char *name, const char *text)
{
    char path[FS_PATH_MAX], buf[512];
    size_t len = 0;
    ksnprintf(path, sizeof(path), "/home/%s/%s", desktop_user(), name);
    if (fs_read_file(path, buf, sizeof(buf) - 1, &len) != FS_OK)
        return false;
    buf[len] = '\0';
    return strstr(buf, text) != NULL;
}

/* Run the current step; false if it needs more ticks (typing, dragging). */
static bool run(const struct step *s)
{
    struct window *w = wm_focused();
    switch (s->op) {
    case SAY:
        kprintf("[tour] %s\n", s->text);
        return true;
    case LOGIN:
        if (desktop_logged_in())
            return true;
        if (t.typing == NULL) {
            kprintf("[tour] Logging in as %s\n", s->text);
            t.typing = s->text;
        }
        /* fall through: type the name, then Enter */
    case TYPE:
        if (t.typing == NULL)
            t.typing = s->text;
        if (*t.typing != '\0') {
            desktop_key((uint8_t)*t.typing++);
            return false;
        }
        t.typing = NULL;
        if (s->op == LOGIN)
            desktop_key('\n');
        return true;
    case OPEN:
        check(wm_show(s->app) != NULL, s->app->name);
        return true;
    case KEY:
        desktop_key(s->key);
        return true;
    case SELECT:
        check(files_select(w, s->text), "Files shows the file");
        return true;
    case DRAG: {
        /* Press on the title bar, move 16 times, let go. */
        if (w == NULL)
            return true;
        if (t.drag == 0) {
            t.dx = w->r.x + 60;
            t.dy = w->r.y + 10;
            wm_mouse(t.dx, t.dy, INPUT_BUTTON_LEFT);
            t.drag = 16;
            return false;
        }
        t.dx += 10;
        t.dy += 3;
        wm_mouse(t.dx, t.dy, --t.drag > 0 ? INPUT_BUTTON_LEFT : 0);
        return t.drag == 0;
    }
    case EXPECT: {
        struct textbuf *tb = app_text(w);
        check(tb != NULL && textbuf_has(tb, s->text), s->text);
        return true;
    }
    case EXPECT_FILE:
        check(file_has("tour.txt", s->text), "the edited file was saved");
        return true;
    case EXPECT_LOAD: {
        unsigned busiest = 0;
        for (unsigned c = 0; c < 8; c++)
            if (monitor_busy(w, c) > busiest)
                busiest = monitor_busy(w, c);
        kprintf("[tour] Busiest CPU over the last second: %u%%\n", busiest);
        check(busiest > 0, "the monitor shows the load");
        return true;
    }
    default:
        kprintf("[tour] Demo finished (%u check%s failed).\n", t.failed, t.failed == 1 ? "" : "s");
        t.on = false;
        return true;
    }
}

void tour_tick(void)
{
    if (!t.on || sched_ticks() < t.at)
        return;
    if (t.step > 0 && !desktop_logged_in() && steps[t.step].op != END) {
        kprintf("[tour] Stopped: logged out.\n");
        t.on = false;
        return;
    }
    if (!run(&steps[t.step])) {
        t.at = sched_ticks() + ms_to_ticks(t.fast ? 0 : 60);   /* typing speed */
        return;
    }
    if (++t.step < NSTEPS)
        t.at = sched_ticks() + ms_to_ticks(steps[t.step].wait_ms);
    else
        t.on = false;
}
