#include "desktop.h"
#include "apps.h"
#include "theme.h"
#include "splash.h"
#include "fbcon.h"
#include "console.h"
#include "input.h"
#include "sched.h"
#include "proc.h"
#include "rcu.h"
#include "kprintf.h"
#include "string.h"
#include "rtc.h"

const struct app *const desktop_apps[] = {
    &app_terminal, &app_files, &app_monitor, &app_health, &app_about,
};
const unsigned desktop_app_count = sizeof(desktop_apps) / sizeof(desktop_apps[0]);

static char user[LOGIN_NAME_MAX + 1];
static bool logged_in, announce;

const char *desktop_user(void)
{
    return user;
}

bool desktop_logged_in(void)
{
    return logged_in;
}

/* ---- the login screen -------------------------------------------------------- */

static struct {
    bool password;                      /* asking for the password now */
    char name[LOGIN_NAME_MAX + 1];
    char pass[33];
    char msg[64];
} ls;

void login_reset(void)
{
    memset(&ls, 0, sizeof(ls));
}

const char *login_message(void)
{
    return ls.msg;
}

static void centred(int32_t y, const char *s, uint32_t rgb, enum font_id f)
{
    gfx_text((wm_back()->w - gfx_text_width(s, f)) / 2, y, s, rgb, f);
}

/* A lock screen on the wallpaper: the time, a round avatar with the
 * first letter of the name, and a soft name (or password) field. */
void login_draw(void)
{
    struct gfx_surface *b = wm_back();
    int32_t cx = b->w / 2, cy = b->h / 2;
    unsigned h, m, sec;
    char clock[8] = "";
    if (rtc_time(&h, &m, &sec))
        ksnprintf(clock, sizeof(clock), "%02u:%02u", h, m);
    centred(cy - 250, clock, COL_ON_DARK, FONT_DISPLAY);
    centred(cy - 170, "BharatOS", COL_ON_DARK_MUTED, FONT_MEDIUM);

    struct gfx_rect av = gfx_rect(cx - 40, cy - 100, 80, 80);
    gfx_round(gfx_grow(av, 1), 41, 0xFFFFFF, 40);
    gfx_round(av, 40, ls.name[0] ? COL_PRIMARY : COL_WALL_BOTTOM, 255);
    char initial[2] = { ls.name[0] >= 'a' && ls.name[0] <= 'z' ? (char)(ls.name[0] - 32) : ls.name[0], 0 };
    if (initial[0] != '\0')
        gfx_text(cx - gfx_text_width(initial, FONT_HEADING) / 2, av.y + 25, initial, COL_SURFACE, FONT_HEADING);

    char label[48];
    if (ls.password)
        ksnprintf(label, sizeof(label), "Password for %s", ls.name);
    else
        strlcpy(label, "Your name", sizeof(label));
    centred(cy + 4, label, COL_ON_DARK_MUTED, FONT_SMALL);
    struct gfx_rect field = gfx_rect(cx - 150, cy + 28, 300, 42);
    gfx_round(gfx_grow(field, 1), 22, 0xFFFFFF, 50);
    gfx_round(field, 21, COL_WALL_TOP, 200);
    char shown[40];
    size_t n = strlen(ls.password ? ls.pass : ls.name);
    if (ls.password) {
        memset(shown, '*', n);
        shown[n] = '\0';
    } else {
        strlcpy(shown, ls.name, sizeof(shown));
    }
    int32_t tw = gfx_text_width(shown, FONT_UI);
    gfx_text(cx - tw / 2, field.y + 10, shown, COL_ON_DARK, FONT_UI);
    gfx_fill(gfx_rect(cx + (tw + 1) / 2 + 2, field.y + 11, 2, 20), COL_SAFFRON);
    if (ls.msg[0] != '\0')
        centred(cy + 84, ls.msg, 0xF0A096, FONT_SMALL);
    else
        centred(cy + 84, "Press Enter to sign in", COL_ON_DARK_MUTED, FONT_SMALL);
}

static void enter_desktop(void)
{
    strlcpy(user, ls.name, sizeof(user));
    logged_in = true;
    announce = true;
    login_reset();
    wm_set_backdrop(NULL);
    wm_set_apps(desktop_apps, desktop_app_count);
    wm_set_user(user);
    kprintf("[desktop] %s logged in.\n", user);
}

/* One key on the login screen. */
static void login_key(uint16_t key)
{
    char *buf = ls.password ? ls.pass : ls.name;
    size_t cap = ls.password ? sizeof(ls.pass) : sizeof(ls.name), n = strlen(buf);
    wm_damage_all();
    if (key == '\b' && n > 0) {
        buf[n - 1] = '\0';
    } else if (key == 0x1B) {
        login_reset();
    } else if (key >= 0x20 && key < 0x7F && n + 1 < cap) {
        buf[n] = (char)key;
        buf[n + 1] = '\0';
    } else if (key == '\n') {
        bool needs;
        if (!ls.password && !login_name_ok(ls.name)) {
            strlcpy(ls.msg, "Please use 1 to 16 letters, digits, - or _.", sizeof(ls.msg));
        } else if (!ls.password && login_check(ls.name, NULL, &needs) && !needs) {
            enter_desktop();
        } else if (!ls.password) {
            ls.password = true;
            ls.msg[0] = '\0';
        } else if (login_check(ls.name, ls.pass, &needs)) {
            enter_desktop();
        } else {
            login_reset();
            strlcpy(ls.msg, "That password is not right.", sizeof(ls.msg));
        }
    }
}

void desktop_key(uint16_t key)
{
    if (logged_in)
        wm_key(key);
    else
        login_key(key);
}

void desktop_logout(void)
{
    if (!logged_in)
        return;
    kprintf("[desktop] %s logged out.\n", user);
    wm_close_all();
    wm_set_apps(NULL, 0);
    logged_in = false;
    user[0] = '\0';
    login_reset();
    wm_set_backdrop(login_draw);
}

/* ---- the desktop thread ----------------------------------------------------------- */

static tid_t desktop_tid;
static unsigned restarts;
static bool gave_up;          /* no memory for the screen: text mode only */

static void handle(const struct input_event *ev)
{
    if (ev->type == INPUT_MOUSE) {
        wm_mouse(ev->x, ev->y, ev->buttons);
    } else if (ev->key == INPUT_KEY_F1 + 11) {
        tour_start(ev->mods & INPUT_MOD_SHIFT);
    } else {
        desktop_key(ev->key);
    }
}

static int64_t desktop_main(void *arg)
{
    (void)arg;
    thread_current()->flags |= THREAD_CONTAIN;     /* a crash restarts it */
    struct gfx_surface *scr = splash_screen();
    if (!wm_init(scr, scr->w, scr->h)) {
        kprintf("[desktop] not enough memory for the screen; staying in text mode\n");
        gave_up = true;
        return 1;
    }
    fbcon_stop();                                  /* the screen is ours now */
    input_set_screen(scr->w, scr->h);
    input_set_desktop(true);
    wm_on_logout(desktop_logout);
    logged_in = announce = false;
    login_reset();
    wm_set_apps(NULL, 0);
    wm_set_backdrop(login_draw);
    kprintf("[desktop] Login screen ready (type your name in the QEMU window).\n");
#ifdef BHARAT_DEMO_AUTO
    tour_start(false);
#endif
    uint64_t next_tick = 0;
    for (;;) {
        struct input_event ev;
        while (input_desktop_poll(&ev))
            handle(&ev);
        tour_tick();
        uint64_t now = sched_ticks();
        if (now >= next_tick) {
            if (logged_in)
                wm_tick();
            else
                wm_damage_all();                   /* the lock screen's clock */
            next_tick = now + SCHED_TIMER_HZ;
        }
        if (wm_compose() && announce) {
            announce = false;
            kprintf("[desktop] Desktop ready for %s.\n", user);
        }
        sched_sleep(2);                            /* about 50 frames a second */
    }
}

void desktop_start(void)
{
    if (splash_screen() == NULL)
        return;
    proc_status_t err;
    struct thread *t = thread_create(process_kernel(), "desktop", desktop_main, NULL, &err);
    if (t == NULL) {
        kprintf("[desktop] cannot start the desktop: %s\n", proc_status_str(err));
        return;
    }
    desktop_tid = t->tid;
}

void desktop_watch(void)
{
    if (desktop_tid == 0 || gave_up)
        return;
    uint64_t f = rcu_read_lock();
    struct thread *t = thread_find(desktop_tid);
    bool gone = t == NULL || t->state == TASK_TERMINATED;
    rcu_read_unlock(f);
    if (!gone)
        return;
    restarts++;
    kprintf("\n[desktop] The desktop stopped unexpectedly; starting it again (%u).\n", restarts);
    desktop_tid = 0;
    if (restarts <= 3)
        desktop_start();
}
