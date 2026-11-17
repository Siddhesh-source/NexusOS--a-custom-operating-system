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
#include "heap.h"

const struct app *const desktop_apps[] = {
    &app_terminal, &app_files, &app_code, &app_browser, &app_player,
    &app_monitor, &app_health, &app_about,
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

static enum login_lang lang;           /* kept across sign-ins */

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

/* A line in the chosen language: the English text, or a Hindi or Marathi
 * phrase (a little taller, so moved up 2 pixels to sit on the same line). */
static void centred_lang(int32_t y, const char *en, enum phrase_id hi, enum phrase_id mr, uint32_t rgb)
{
    if (lang == LOGIN_ENGLISH) {
        centred(y, en, rgb, FONT_SMALL);
        return;
    }
    enum phrase_id id = lang == LOGIN_HINDI ? hi : mr;
    gfx_phrase((wm_back()->w - gfx_phrase_width(id)) / 2, y - 2, id, rgb);
}

void login_set_language(enum login_lang l)
{
    lang = (unsigned)l <= LOGIN_MARATHI ? l : LOGIN_ENGLISH;
    wm_damage_all();
}

enum login_lang login_language(void)
{
    return lang;
}

/* "F1   English  हिन्दी  मराठी", the one in use brighter. */
static void draw_languages(int32_t y)
{
    const char *key = "F1", *en = "English";
    int32_t gap = 18, kw = gfx_text_width(key, FONT_SMALL), ew = gfx_text_width(en, FONT_SMALL);
    int32_t total = kw + gap + ew + gap + gfx_phrase_width(PHRASE_HINDI) + gap + gfx_phrase_width(PHRASE_MARATHI);
    int32_t x = (wm_back()->w - total) / 2;
    uint32_t on = COL_ON_DARK, off = COL_ON_DARK_MUTED;
    x += gfx_text(x, y, key, COL_SAFFRON, FONT_SMALL) + gap;
    x += gfx_text(x, y, en, lang == LOGIN_ENGLISH ? on : off, FONT_SMALL) + gap;
    x += gfx_phrase(x, y - 2, PHRASE_HINDI, lang == LOGIN_HINDI ? on : off) + gap;
    gfx_phrase(x, y - 2, PHRASE_MARATHI, lang == LOGIN_MARATHI ? on : off);
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
    centred(cy - 270, clock, COL_ON_DARK, FONT_DISPLAY);
    centred(cy - 190, "BharatOS", COL_ON_DARK_MUTED, FONT_MEDIUM);
    /* Namaste (Hindi) and Namaskar (Marathi), always shown. */
    int32_t g1 = gfx_phrase_width(PHRASE_HI_GREETING), g2 = gfx_phrase_width(PHRASE_MR_GREETING);
    int32_t gx = cx - (g1 + 40 + g2) / 2;
    gfx_phrase(gx, cy - 160, PHRASE_HI_GREETING, COL_ON_DARK);
    gfx_round(gfx_rect(gx + g1 + 17, cy - 145, 6, 6), 3, COL_SAFFRON, 255);
    gfx_phrase(gx + g1 + 40, cy - 160, PHRASE_MR_GREETING, COL_ON_DARK);

    struct gfx_rect av = gfx_rect(cx - 40, cy - 100, 80, 80);
    gfx_round(gfx_grow(av, 1), 41, 0xFFFFFF, 40);
    gfx_round(av, 40, ls.name[0] ? COL_PRIMARY : COL_WALL_BOTTOM, 255);
    char initial[2] = { ls.name[0] >= 'a' && ls.name[0] <= 'z' ? (char)(ls.name[0] - 32) : ls.name[0], 0 };
    if (initial[0] != '\0')
        gfx_text(cx - gfx_text_width(initial, FONT_HEADING) / 2, av.y + 25, initial, COL_SURFACE, FONT_HEADING);

    char label[48];
    if (ls.password && lang != LOGIN_ENGLISH) {
        /* "पासवर्ड: name" */
        ksnprintf(label, sizeof(label), ": %s", ls.name);
        int32_t pw = gfx_phrase_width(PHRASE_PASSWORD), x = cx - (pw + gfx_text_width(label, FONT_SMALL)) / 2;
        gfx_phrase(x, cy + 2, PHRASE_PASSWORD, COL_ON_DARK_MUTED);
        gfx_text(x + pw, cy + 4, label, COL_ON_DARK_MUTED, FONT_SMALL);
    } else if (ls.password) {
        ksnprintf(label, sizeof(label), "Password for %s", ls.name);
        centred(cy + 4, label, COL_ON_DARK_MUTED, FONT_SMALL);
    } else {
        centred_lang(cy + 4, "Your name", PHRASE_HI_NAME, PHRASE_MR_NAME, COL_ON_DARK_MUTED);
    }
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
        centred_lang(cy + 84, "Press Enter to sign in", PHRASE_HI_ENTER, PHRASE_MR_ENTER, COL_ON_DARK_MUTED);
    draw_languages(cy + 124);
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
    if (key == INPUT_KEY_F1) {
        login_set_language((enum login_lang)((lang + 1) % 3));
    } else if (key == '\b' && n > 0) {
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

/* ---- screenshots ------------------------------------------------------------ */

fs_status_t desktop_screenshot(char *path, size_t cap)
{
    if (!logged_in || path == NULL || cap < FS_PATH_MAX)
        return FS_ERR_INVALID;
    /* The first free name: screenshot-1.ppm, screenshot-2.ppm, ... */
    struct fs_stat st;
    unsigned n = 1;
    for (; n <= DESKTOP_SHOTS_MAX; n++) {
        ksnprintf(path, cap, "/home/%s/screenshot-%u.ppm", user, n);
        if (fs_stat(path, &st) == FS_ERR_NOT_FOUND)
            break;
    }
    if (n > DESKTOP_SHOTS_MAX)
        return FS_ERR_EXISTS;
    /* PPM: a short text header, then red, green, blue bytes per pixel. */
    struct gfx_surface *b = wm_back();
    char head[32];
    int hl = ksnprintf(head, sizeof(head), "P6\n%d %d\n255\n", b->w, b->h);
    size_t len = (size_t)hl + (size_t)b->w * (size_t)b->h * 3;
    uint8_t *buf = kmalloc(len);
    if (buf == NULL)
        return FS_ERR_NO_SPACE;
    memcpy(buf, head, (size_t)hl);
    uint8_t *p = buf + hl;
    for (int32_t y = 0; y < b->h; y++)
        for (int32_t x = 0; x < b->w; x++) {
            uint32_t c = gfx_get(b, x, y);
            *p++ = (uint8_t)(c >> 16);
            *p++ = (uint8_t)(c >> 8);
            *p++ = (uint8_t)c;
        }
    fs_status_t r = fs_write_file(path, buf, len, false);
    kfree(buf);
    return r;
}

/* Print Screen: save the screen and say where it went. */
static void screenshot_key(void)
{
    char path[FS_PATH_MAX], msg[96];
    fs_status_t r = desktop_screenshot(path, sizeof(path));
    if (r == FS_OK)
        ksnprintf(msg, sizeof(msg), "Screenshot saved: %s", path);
    else if (r == FS_ERR_EXISTS)
        ksnprintf(msg, sizeof(msg), "Too many screenshots; delete some in Files");
    else
        ksnprintf(msg, sizeof(msg), "Could not save the screenshot: %s", fs_status_str(r));
    kprintf("[desktop] %s\n", msg);
    wm_toast(msg);
}

void desktop_key(uint16_t key)
{
    if (logged_in && key == INPUT_KEY_PRINT)
        screenshot_key();
    else if (logged_in)
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
        if (logged_in)
            wm_animate();
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
