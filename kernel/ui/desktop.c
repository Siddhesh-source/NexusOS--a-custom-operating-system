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

static void centred(int32_t y, const char *s, uint32_t rgb, int32_t scale)
{
    struct gfx_surface *b = wm_back();
    gfx_text((b->w - (int32_t)strlen(s) * GFX_CHAR_W * scale) / 2, y, s, rgb, scale);
}

void login_draw(void)
{
    struct gfx_surface *b = wm_back();
    int32_t cy = b->h / 2;
    centred(cy - 200, "BharatOS", COL_SAFFRON, 4);
    centred(cy - 124, "an operating system written from scratch", COL_MUTED, 1);
    struct gfx_rect box = gfx_rect(b->w / 2 - 220, cy - 80, 440, 176);
    gfx_fill(box, COL_SURFACE);
    gfx_frame(box, COL_BORDER);
    char label[64];
    if (ls.password)
        ksnprintf(label, sizeof(label), "Password for %s:", ls.name);
    else
        strlcpy(label, "Welcome! Type your name:", sizeof(label));
    gfx_text(box.x + 24, box.y + 24, label, COL_TEXT, 1);
    struct gfx_rect field = gfx_rect(box.x + 24, box.y + 52, box.w - 48, 32);
    gfx_frame(field, COL_ACCENT);
    char shown[40];
    size_t n = strlen(ls.password ? ls.pass : ls.name);
    if (ls.password) {
        memset(shown, '*', n);
        shown[n] = '\0';
    } else {
        strlcpy(shown, ls.name, sizeof(shown));
    }
    int32_t tw = gfx_text(field.x + 8, field.y + 8, shown, COL_TEXT, 1);
    gfx_fill(gfx_rect(field.x + 8 + tw, field.y + 6, 2, 20), COL_ACCENT);
    gfx_text(box.x + 24, box.y + 96, "Then press Enter.", COL_MUTED, 1);
    if (ls.msg[0] != '\0')
        gfx_text(box.x + 24, box.y + 128, ls.msg, COL_DANGER, 1);
    centred(b->h - 40, "Any name works: letters, digits, - or _. Names in /etc/users need their password.",
            COL_MUTED, 1);
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
