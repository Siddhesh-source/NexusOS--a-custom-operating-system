#include "wm.h"
#include "theme.h"
#include "rtc.h"
#include "selftest.h"
#include "percpu.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"
#include "input.h"

static struct gfx_surface *screen;
static struct gfx_surface back;
static struct window wins[WM_MAX_WINDOWS];
static struct window *order[WM_MAX_WINDOWS];   /* bottom to top */
static unsigned nwin;
static const struct app *const *apps;
static unsigned napps;
static char user[24];
static void (*logout_fn)(void);
static void (*backdrop)(void);
static void topbar_refresh(void);
static struct gfx_rect damage;
static struct wm_stats stats;
static unsigned opened;                         /* for placing new windows */

/* The mouse pointer and a window being dragged. */
static int32_t px, py;
static uint8_t buttons;
static struct window *dragging;
static int32_t drag_dx, drag_dy;

#define CURSOR_W 12
#define CURSOR_H 19
#define CLOSE_W  28

bool wm_init(struct gfx_surface *scr, int32_t w, int32_t h)
{
    if (w < 640 || h < 480)
        return false;
    if (back.px == NULL || back.w != w || back.h != h) {
        kfree(back.px);
        back.px = kmalloc((size_t)w * (size_t)h * 4);
        if (back.px == NULL)
            return false;
    }
    back.w = back.stride = w;
    back.h = h;
    screen = scr;
    memset(wins, 0, sizeof(wins));
    nwin = 0;
    dragging = NULL;
    px = w / 2;
    py = h / 2;
    wm_damage_all();
    return true;
}

void wm_set_apps(const struct app *const *a, unsigned n)
{
    apps = a;
    napps = n < WM_MAX_APPS ? n : WM_MAX_APPS;
    wm_damage_all();
}

void wm_set_user(const char *u)
{
    strlcpy(user, u != NULL ? u : "", sizeof(user));
    topbar_refresh();
}

void wm_on_logout(void (*fn)(void))
{
    logout_fn = fn;
}

void wm_set_backdrop(void (*fn)(void))
{
    backdrop = fn;
    wm_damage_all();
}

struct gfx_surface *wm_back(void)
{
    return &back;
}

void wm_get_stats(struct wm_stats *out)
{
    *out = stats;
    out->windows = nwin;
}

void wm_damage(struct gfx_rect r)
{
    damage = gfx_union(damage, gfx_intersect(r, gfx_rect(0, 0, back.w, back.h)));
}

void wm_damage_all(void)
{
    wm_damage(gfx_rect(0, 0, back.w, back.h));
}

void wm_redraw(struct window *w)
{
    if (w != NULL && w->used)
        wm_damage(w->r);
}

struct gfx_rect wm_body(const struct window *w)
{
    return gfx_rect(w->r.x + 1, w->r.y + UI_TITLE_H, w->r.w - 2, w->r.h - UI_TITLE_H - 1);
}

/* ---- windows ------------------------------------------------------------- */

static int index_of(struct window *w)
{
    for (unsigned i = 0; i < nwin; i++)
        if (order[i] == w)
            return (int)i;
    return -1;
}

struct window *wm_focused(void)
{
    return nwin > 0 ? order[nwin - 1] : NULL;
}

struct window *wm_find(const struct app *a)
{
    for (unsigned i = 0; i < WM_MAX_WINDOWS; i++)
        if (wins[i].used && wins[i].app == a)
            return &wins[i];
    return NULL;
}

void wm_focus(struct window *w)
{
    int i = index_of(w);
    if (i < 0 || (unsigned)i == nwin - 1)
        return;
    struct window *old = wm_focused();
    for (unsigned j = (unsigned)i; j + 1 < nwin; j++)
        order[j] = order[j + 1];
    order[nwin - 1] = w;
    wm_redraw(old);                         /* its title bar loses the colour */
    wm_redraw(w);
}

void wm_move(struct window *w, int32_t x, int32_t y)
{
    if (w == NULL || !w->used)
        return;
    /* Keep the title bar reachable: below the top bar, and at least 64
     * pixels of it on the screen. */
    if (x > back.w - 64) x = back.w - 64;
    if (x < 64 - w->r.w) x = 64 - w->r.w;
    if (y > back.h - UI_TITLE_H) y = back.h - UI_TITLE_H;
    if (y < UI_TOPBAR_H) y = UI_TOPBAR_H;
    wm_damage(w->r);
    w->r.x = x;
    w->r.y = y;
    wm_damage(w->r);
}

void wm_set_title(struct window *w, const char *title)
{
    strlcpy(w->title, title, sizeof(w->title));
    wm_damage(gfx_rect(w->r.x, w->r.y, w->r.w, UI_TITLE_H));
}

struct window *wm_open(const struct app *a, const char *arg)
{
    struct window *w = NULL;
    for (unsigned i = 0; i < WM_MAX_WINDOWS && w == NULL; i++)
        if (!wins[i].used)
            w = &wins[i];
    if (a == NULL || w == NULL)
        return NULL;
    memset(w, 0, sizeof(*w));
    w->app = a;
    strlcpy(w->title, a->name, sizeof(w->title));
    int32_t ww = a->w + 2, wh = a->h + UI_TITLE_H + 1;
    if (ww > back.w) ww = back.w;
    if (wh > back.h - UI_TOPBAR_H) wh = back.h - UI_TOPBAR_H;
    unsigned k = opened++ % 6;
    w->r = gfx_rect(48 + 40 * (int32_t)k, UI_TOPBAR_H + 24 + 32 * (int32_t)k, ww, wh);
    if (w->r.x + ww > back.w) w->r.x = back.w - ww;
    if (w->r.y + wh > back.h) w->r.y = back.h - wh;
    w->used = true;                          /* open() may set the title */
    if (a->open != NULL && !a->open(w, arg)) {
        w->used = false;
        return NULL;
    }
    order[nwin++] = w;
    wm_redraw(wm_focused() == w && nwin > 1 ? order[nwin - 2] : NULL);
    wm_redraw(w);
    return w;
}

void wm_close(struct window *w)
{
    int i = index_of(w);
    if (i < 0)
        return;
    if (w->app->close != NULL)
        w->app->close(w);
    kfree(w->st);
    w->st = NULL;
    for (unsigned j = (unsigned)i; j + 1 < nwin; j++)
        order[j] = order[j + 1];
    nwin--;
    if (dragging == w)
        dragging = NULL;
    wm_damage(w->r);
    wm_redraw(wm_focused());
    w->used = false;
}

void wm_close_all(void)
{
    while (nwin > 0)
        wm_close(order[nwin - 1]);
    opened = 0;
}

struct window *wm_show(const struct app *a)
{
    struct window *w = wm_find(a);
    if (w == NULL)
        return wm_open(a, NULL);
    wm_focus(w);
    return w;
}

/* ---- the top bar ------------------------------------------------------------ */

/* The badge and the text right of it, worked out once a second (the
 * clock) rather than at every draw. */
static char badge[40], right[64];
static bool healthy;

static void topbar_refresh(void)
{
    unsigned passed, failed, h, m, s;
    selftest_totals(&passed, &failed);
    healthy = failed == 0;
    if (healthy)
        ksnprintf(badge, sizeof(badge), "Healthy: %u checks", passed);
    else
        ksnprintf(badge, sizeof(badge), "%u checks failed", failed);
    char clock[16] = "";
    if (rtc_time(&h, &m, &s))
        ksnprintf(clock, sizeof(clock), "  %02u:%02u:%02u", h, m, s);
    ksnprintf(right, sizeof(right), "%s  |  %u CPUs%s", user, smp_cpu_count(), clock);
    wm_damage(gfx_rect(0, 0, back.w, UI_TOPBAR_H));
}

/* Items: apps 0..napps-1 from the left, then "Log out" and the health
 * badge from the right (after the user, CPUs and clock). */
struct gfx_rect wm_topbar_item(unsigned i)
{
    if (i < napps) {
        int32_t x = 8 + 8 * GFX_CHAR_W + 16;    /* after the "BharatOS" mark */
        for (unsigned j = 0; j < i; j++)
            x += (int32_t)strlen(apps[j]->name) * GFX_CHAR_W + 16 + 4;
        return gfx_rect(x, 4, (int32_t)strlen(apps[i]->name) * GFX_CHAR_W + 16, 24);
    }
    int32_t x = back.w - 8 - (7 * GFX_CHAR_W + 16);
    if (i == napps)
        return gfx_rect(x, 4, 7 * GFX_CHAR_W + 16, 24);
    x -= 16 + (int32_t)strlen(right) * GFX_CHAR_W + 16 + (int32_t)strlen(badge) * GFX_CHAR_W + 16;
    return gfx_rect(x, 4, (int32_t)strlen(badge) * GFX_CHAR_W + 16, 24);
}

static void button(struct gfx_rect r, const char *label, uint32_t bg, uint32_t fg)
{
    gfx_fill(r, bg);
    gfx_text(r.x + 8, r.y + (r.h - GFX_CHAR_H) / 2, label, fg, 1);
}

static void draw_topbar(void)
{
    gfx_fill(gfx_rect(0, 0, back.w, UI_TOPBAR_H - 1), COL_SURFACE);
    gfx_fill(gfx_rect(0, UI_TOPBAR_H - 1, back.w, 1), COL_BORDER);
    gfx_text(8, 8, "BharatOS", COL_SAFFRON, 1);
    struct window *f = wm_focused();
    for (unsigned i = 0; i < napps; i++) {
        bool on = f != NULL && f->app == apps[i];
        button(wm_topbar_item(i), apps[i]->name, on ? COL_ACCENT : COL_BG, on ? COL_SURFACE : COL_TEXT);
    }
    button(wm_topbar_item(napps), "Log out", COL_BG, COL_TEXT);
    struct gfx_rect b = wm_topbar_item(napps + 1);
    button(b, badge, healthy ? COL_OK : COL_DANGER, COL_SURFACE);
    gfx_text(b.x + b.w + 16, 8, right, COL_TEXT, 1);
}

static void topbar_click(int32_t x, int32_t y)
{
    for (unsigned i = 0; i < napps + 2; i++) {
        if (!gfx_inside(wm_topbar_item(i), x, y))
            continue;
        if (i < napps)
            wm_show(apps[i]);
        else if (i == napps && logout_fn != NULL)
            logout_fn();
        else if (i == napps + 1)
            for (unsigned j = 0; j < napps; j++)
                if (strcmp(apps[j]->name, "Health") == 0)
                    wm_show(apps[j]);
        wm_damage(gfx_rect(0, 0, back.w, UI_TOPBAR_H));
        return;
    }
}

/* ---- drawing ------------------------------------------------------------- */

static void draw_window(struct window *w, bool focused)
{
    struct gfx_rect r = w->r, clip = gfx_get_clip();
    if (gfx_empty(gfx_intersect(r, clip)))
        return;
    gfx_frame(r, focused ? COL_ACCENT : COL_BORDER);
    struct gfx_rect bar = gfx_rect(r.x + 1, r.y + 1, r.w - 2, UI_TITLE_H - 1);
    gfx_fill(bar, focused ? COL_ACCENT : COL_BORDER);
    uint32_t tc = focused ? COL_SURFACE : COL_TEXT;
    gfx_text(bar.x + UI_PAD, bar.y + 5, w->title, tc, 1);
    gfx_text(r.x + r.w - CLOSE_W + 10, bar.y + 5, "x", tc, 1);
    struct gfx_rect body = wm_body(w);
    gfx_fill(body, COL_SURFACE);
    if (w->app->draw != NULL) {
        gfx_clip(gfx_intersect(clip, body));
        w->app->draw(w, body);
        gfx_clip(clip);
    }
}

static struct gfx_rect cursor_rect(void)
{
    return gfx_rect(px, py, CURSOR_W, CURSOR_H);
}

/* An arrow: a dark outline around a white inside. */
static void draw_cursor(void)
{
    for (int32_t i = 0; i < 16; i++) {
        int32_t w = i < 11 ? i + 1 : 11 - (i - 11) * 2;
        if (w > 0)
            gfx_fill(gfx_rect(px, py + i, w, 1), COL_TEXT);
        if (w > 2 && i < 15)
            gfx_fill(gfx_rect(px + 1, py + i, w - 2, 1), COL_SURFACE);
    }
    gfx_fill(gfx_rect(px + 4, py + 13, 3, 6), COL_TEXT);
    gfx_fill(gfx_rect(px + 5, py + 13, 1, 5), COL_SURFACE);
}

bool wm_compose(void)
{
    if (gfx_empty(damage) || back.px == NULL)
        return false;
    struct gfx_rect d = damage;
    damage = gfx_rect(0, 0, 0, 0);
    gfx_target(&back);
    gfx_clip(d);
    gfx_fill(d, COL_BG);
    if (backdrop != NULL)
        backdrop();
    if (napps > 0)
        gfx_text(16, back.h - 28, "F1-F5 open the apps   Ctrl+W closes a window   F12 shows a tour",
                 COL_MUTED, 1);
    for (unsigned i = 0; i < nwin; i++)
        draw_window(order[i], i == nwin - 1);
    if (napps > 0)
        draw_topbar();
    draw_cursor();
    if (screen != NULL)
        gfx_copy(screen, &back, d);
    stats.frames++;
    stats.pixels += (uint64_t)d.w * (uint64_t)d.h;
    return true;
}

/* ---- input --------------------------------------------------------------- */

static struct window *window_at(int32_t x, int32_t y)
{
    for (unsigned i = nwin; i-- > 0;)
        if (gfx_inside(order[i]->r, x, y))
            return order[i];
    return NULL;
}

void wm_key(uint16_t key)
{
    if (key >= INPUT_KEY_F1 && key < INPUT_KEY_F1 + napps && key < INPUT_KEY_F1 + 5) {
        wm_show(apps[key - INPUT_KEY_F1]);
        return;
    }
    struct window *w = wm_focused();
    if (key == 23) {                              /* Ctrl+W */
        wm_close(w);
        return;
    }
    if (w != NULL && w->app->key != NULL)
        w->app->key(w, key);
}

void wm_mouse(int32_t x, int32_t y, uint8_t b)
{
    if (x != px || y != py) {
        wm_damage(cursor_rect());
        px = x;
        py = y;
        wm_damage(cursor_rect());
        if (dragging != NULL)
            wm_move(dragging, x - drag_dx, y - drag_dy);
    }
    bool press = (b & INPUT_BUTTON_LEFT) && !(buttons & INPUT_BUTTON_LEFT);
    if (!(b & INPUT_BUTTON_LEFT))
        dragging = NULL;
    buttons = b;
    if (!press)
        return;
    if (y < UI_TOPBAR_H) {
        topbar_click(x, y);
        return;
    }
    struct window *w = window_at(x, y);
    if (w == NULL)
        return;
    wm_focus(w);
    if (y < w->r.y + UI_TITLE_H) {
        if (x >= w->r.x + w->r.w - CLOSE_W) {
            wm_close(w);
        } else {
            dragging = w;
            drag_dx = x - w->r.x;
            drag_dy = y - w->r.y;
        }
        return;
    }
    struct gfx_rect body = wm_body(w);
    if (gfx_inside(body, x, y) && w->app->click != NULL)
        w->app->click(w, x - body.x, y - body.y);
}

void wm_tick(void)
{
    for (unsigned i = 0; i < nwin; i++)
        if (order[i]->app->tick != NULL)
            order[i]->app->tick(order[i]);
    topbar_refresh();                                   /* the clock */
}

/* ---- widgets ------------------------------------------------------------- */

int ui_buttons(struct gfx_rect body, const char *const *labels, unsigned n,
               bool draw, int32_t x, int32_t y)
{
    int32_t bx = body.x + UI_PAD;
    int hit = -1;
    for (unsigned i = 0; i < n; i++) {
        struct gfx_rect r = gfx_rect(bx, body.y + UI_PAD, (int32_t)strlen(labels[i]) * GFX_CHAR_W + 16, 24);
        if (draw)
            button(r, labels[i], COL_ACCENT, COL_SURFACE);
        if (gfx_inside(r, x, y))
            hit = (int)i;
        bx += r.w + UI_PAD;
    }
    return hit;
}
