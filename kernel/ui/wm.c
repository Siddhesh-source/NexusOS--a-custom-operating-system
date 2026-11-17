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
static struct gfx_surface back, wall;          /* wall: the wallpaper, drawn once */
static struct window wins[WM_MAX_WINDOWS];
static struct window *order[WM_MAX_WINDOWS];   /* bottom to top */
static unsigned nwin;
static const struct app *const *apps;
static unsigned napps;
static char user[24];
static void (*logout_fn)(void);
static void (*backdrop)(void);
static void topbar_refresh(void);
static struct gfx_rect dock_rect(void);
static struct gfx_rect damage;
static struct wm_stats stats;

/* The mouse pointer and a window being dragged. */
static int32_t px, py;
static bool pointer_shown;                     /* hidden until the mouse moves */
static uint8_t buttons;
static struct window *dragging;
static int32_t drag_dx, drag_dy;

#define CURSOR_W 12
#define CURSOR_H 19
#define CLOSE_W  44                 /* the close button's click area */
#define SHADOW   34                 /* how far a window's shadow reaches */
#define SLOT_W   76                 /* one app in the dock */
#define TILE     44

static bool alloc_surface(struct gfx_surface *s, int32_t w, int32_t h)
{
    if (s->px == NULL || s->w != w || s->h != h) {
        kfree(s->px);
        s->px = kmalloc((size_t)w * (size_t)h * 4);
        if (s->px == NULL)
            return false;
    }
    s->w = s->stride = w;
    s->h = h;
    return true;
}

bool wm_init(struct gfx_surface *scr, int32_t w, int32_t h)
{
    if (w < 640 || h < 480 || !alloc_surface(&back, w, h) || !alloc_surface(&wall, w, h))
        return false;
    gfx_target(&wall);
    gfx_gradient(gfx_rect(0, 0, w, h), COL_WALL_TOP, COL_WALL_BOTTOM);
    screen = scr;
    memset(wins, 0, sizeof(wins));
    nwin = 0;
    dragging = NULL;
    px = w / 2;
    py = h / 2;
    pointer_shown = false;
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

/* A window and its shadow. */
static struct gfx_rect outer(const struct window *w)
{
    return gfx_grow(w->r, SHADOW);
}

void wm_redraw(struct window *w)
{
    if (w != NULL && w->used)
        wm_damage(outer(w));
}

struct gfx_rect wm_body(const struct window *w)
{
    return gfx_rect(w->r.x, w->r.y + UI_TITLE_H, w->r.w, w->r.h - UI_TITLE_H);
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
    wm_redraw(old);                         /* its shadow and title go quiet */
    wm_redraw(w);
    topbar_refresh();                       /* the top bar names the focused app */
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
    wm_damage(outer(w));
    w->r.x = x;
    w->r.y = y;
    wm_damage(outer(w));
}

void wm_set_title(struct window *w, const char *title)
{
    strlcpy(w->title, title, sizeof(w->title));
    wm_damage(gfx_rect(w->r.x, w->r.y, w->r.w, UI_TITLE_H));
}

/* New windows open centred in the space between the top bar and the
 * dock, moved down and right only while another window sits there. */
static void place(struct window *w)
{
    int32_t top = UI_TOPBAR_H + 16, avail = back.h - top - UI_DOCK_H;
    int32_t x = (back.w - w->r.w) / 2, y = top + (avail - w->r.h) / 2;
    if (y < top) y = top;
    for (unsigned tries = 0; tries < WM_MAX_WINDOWS; tries++) {
        bool taken = false;
        for (unsigned i = 0; i < nwin; i++)
            taken |= order[i]->r.x == x && order[i]->r.y == y;
        if (!taken)
            break;
        x += 32;
        y += 28;
    }
    if (x + w->r.w > back.w) x = back.w - w->r.w;
    if (y + w->r.h > back.h) y = back.h - w->r.h;
    w->r.x = x > 0 ? x : 0;
    w->r.y = y > UI_TOPBAR_H ? y : UI_TOPBAR_H;
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
    int32_t ww = a->w, wh = a->h + UI_TITLE_H;
    if (ww > back.w) ww = back.w;
    if (wh > back.h - UI_TOPBAR_H) wh = back.h - UI_TOPBAR_H;
    w->r = gfx_rect(0, 0, ww, wh);
    w->used = true;                          /* open() may set the title or size */
    if (a->open != NULL && !a->open(w, arg)) {
        w->used = false;
        return NULL;
    }
    place(w);
    order[nwin++] = w;
    wm_redraw(nwin > 1 ? order[nwin - 2] : NULL);
    wm_redraw(w);
    topbar_refresh();
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
    wm_damage(outer(w));
    wm_redraw(wm_focused());
    w->used = false;
    topbar_refresh();
}

void wm_close_all(void)
{
    while (nwin > 0)
        wm_close(order[nwin - 1]);
}

struct window *wm_show(const struct app *a)
{
    struct window *w = wm_find(a);
    if (w == NULL)
        return wm_open(a, NULL);
    wm_focus(w);
    return w;
}

/* ---- top bar and dock --------------------------------------------------- */

/* Worked out once a second (the clock) rather than at every draw. */
static char status[40], right[64];
static bool healthy;

static void topbar_refresh(void)
{
    unsigned passed, failed, h, m, s;
    selftest_totals(&passed, &failed);
    healthy = failed == 0;
    if (healthy)
        ksnprintf(status, sizeof(status), "%u checks passed", passed);
    else
        ksnprintf(status, sizeof(status), "%u checks failed", failed);
    char clock[8] = "";
    if (rtc_time(&h, &m, &s))
        ksnprintf(clock, sizeof(clock), "%02u:%02u", h, m);
    ksnprintf(right, sizeof(right), "%s     %u CPUs     %s", user, smp_cpu_count(), clock);
    wm_damage(gfx_rect(0, 0, back.w, UI_TOPBAR_H));
    if (napps > 0)
        wm_damage(gfx_grow(dock_rect(), 2));      /* running dots, focus highlight */
}

static struct gfx_rect dock_rect(void)
{
    int32_t w = (int32_t)napps * SLOT_W + 24, h = 84;
    return gfx_rect((back.w - w) / 2, back.h - h - 12, w, h);
}

/* Items: the apps in the dock 0..n-1, then "Log out" and the health
 * status in the top bar. */
struct gfx_rect wm_ui_item(unsigned i)
{
    if (i < napps) {
        struct gfx_rect d = dock_rect();
        return gfx_rect(d.x + 12 + (int32_t)i * SLOT_W, d.y + 8, SLOT_W, d.h - 16);
    }
    int32_t lw = gfx_text_width("Log out", FONT_SMALL) + 20;
    int32_t x = back.w - 8 - lw;
    if (i == napps)
        return gfx_rect(x, 3, lw, UI_TOPBAR_H - 6);
    int32_t sw = gfx_text_width(status, FONT_SMALL) + 34;
    x -= 24 + gfx_text_width(right, FONT_SMALL) + 24 + sw;
    return gfx_rect(x, 3, sw, UI_TOPBAR_H - 6);
}

static void draw_topbar(void)
{
    gfx_blend(gfx_rect(0, 0, back.w, UI_TOPBAR_H), COL_CHROME, 170);
    gfx_blend(gfx_rect(0, UI_TOPBAR_H - 1, back.w, 1), 0xFFFFFF, 16);
    int32_t ty = (UI_TOPBAR_H - gfx_line_height(FONT_MEDIUM)) / 2;
    gfx_round(gfx_rect(16, UI_TOPBAR_H / 2 - 4, 8, 8), 4, COL_SAFFRON, 255);
    int32_t x = 32 + gfx_text(32, ty, "BharatOS", COL_ON_DARK, FONT_MEDIUM);
    struct window *f = wm_focused();
    if (f != NULL)
        gfx_text(x + 24, ty, f->app->name, COL_ON_DARK_MUTED, FONT_UI);
    int32_t sy = (UI_TOPBAR_H - gfx_line_height(FONT_SMALL)) / 2;
    struct gfx_rect lo = wm_ui_item(napps), st = wm_ui_item(napps + 1);
    gfx_round(lo, UI_RADIUS_SM, 0xFFFFFF, 22);
    gfx_text(lo.x + 10, sy, "Log out", COL_ON_DARK, FONT_SMALL);
    gfx_round(gfx_rect(st.x + 12, UI_TOPBAR_H / 2 - 4, 8, 8), 4, healthy ? COL_OK : COL_DANGER, 255);
    gfx_text(st.x + 26, sy, status, COL_ON_DARK, FONT_SMALL);
    gfx_text(st.x + st.w + 24, sy, right, COL_ON_DARK, FONT_SMALL);
}

static void draw_dock(void)
{
    struct gfx_rect d = dock_rect();
    gfx_round(gfx_grow(d, 1), 21, 0xFFFFFF, 26);           /* a faint rim */
    gfx_round(d, 20, COL_CHROME, 190);
    struct window *f = wm_focused();
    for (unsigned i = 0; i < napps; i++) {
        struct gfx_rect s = wm_ui_item(i);
        struct gfx_rect t = gfx_rect(s.x + (s.w - TILE) / 2, s.y + 2, TILE, TILE);
        if (f != NULL && f->app == apps[i])
            gfx_round(gfx_grow(t, 4), 15, 0xFFFFFF, 34);
        gfx_round(t, 11, apps[i]->tile, 255);
        if (apps[i]->icon != NULL)
            apps[i]->icon(t);
        bool running = wm_find(apps[i]) != NULL;
        int32_t lw = gfx_text_width(apps[i]->name, FONT_SMALL);
        gfx_text(s.x + (s.w - lw) / 2, t.y + TILE + 3, apps[i]->name,
                 running ? COL_ON_DARK : COL_ON_DARK_MUTED, FONT_SMALL);
        if (running)
            gfx_round(gfx_rect(s.x + s.w / 2 - 2, d.y + d.h - 7, 4, 4), 2, COL_SAFFRON, 255);
    }
}

static void topbar_click(int32_t x, int32_t y)
{
    for (unsigned i = 0; i < napps + 2; i++) {
        if (!gfx_inside(wm_ui_item(i), x, y))
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
        wm_damage(dock_rect());
        return;
    }
}

/* ---- drawing ------------------------------------------------------------- */

static struct gfx_rect close_rect(const struct window *w)
{
    return gfx_rect(w->r.x + w->r.w - 28, w->r.y + 13, 14, 14);
}

static void draw_window(struct window *w, bool focused)
{
    struct gfx_rect r = w->r, clip = gfx_get_clip();
    if (gfx_empty(gfx_intersect(outer(w), clip)))
        return;
    bool dark = w->app->dark;
    /* The shadow's own shape is smaller than the window, so only its soft
     * fade shows below it, never a solid band. */
    if (focused)
        gfx_shadow(gfx_grow(r, -10), UI_RADIUS, SHADOW, 12, 110);
    else
        gfx_shadow(gfx_grow(r, -8), UI_RADIUS, 20, 8, 60);
    if (dark)
        gfx_round(gfx_grow(r, 1), UI_RADIUS + 1, 0xFFFFFF, 30);
    gfx_round(r, UI_RADIUS, dark ? COL_TERM_TITLE : COL_SURFACE, 255);
    struct gfx_rect body = wm_body(w);
    if (dark) {
        gfx_round(body, UI_RADIUS, COL_TERM_BG, 255);
        gfx_fill(gfx_rect(body.x, body.y, body.w, UI_RADIUS), COL_TERM_BG);
    }
    gfx_fill(gfx_rect(r.x, body.y - 1, r.w, 1), dark ? 0x232B36 : COL_LINE);
    uint32_t tc = dark ? (focused ? COL_ON_DARK : COL_ON_DARK_MUTED) : (focused ? COL_TEXT : COL_MUTED);
    int32_t tw = gfx_text_width(w->title, FONT_MEDIUM);
    gfx_text(r.x + (r.w - tw) / 2, r.y + (UI_TITLE_H - gfx_line_height(FONT_MEDIUM)) / 2,
             w->title, tc, FONT_MEDIUM);
    struct gfx_rect c = close_rect(w);
    gfx_round(c, 7, focused ? COL_DANGER : (dark ? 0x3A4452 : COL_LINE), 255);
    if (w->app->draw != NULL) {
        gfx_clip(gfx_intersect(clip, body));
        w->app->draw(w, body);
        gfx_clip(clip);
    }
}

static struct gfx_rect cursor_rect(void)
{
    return gfx_rect(px - 1, py - 1, CURSOR_W + 2, CURSOR_H + 2);
}

/* An arrow: dark inside, light outline, so it shows on any colour. */
static void draw_cursor(void)
{
    for (int32_t i = 0; i < 16; i++) {
        int32_t w = i < 11 ? i + 1 : 11 - (i - 11) * 2;
        if (w > 0)
            gfx_fill(gfx_rect(px, py + i, w, 1), 0xFFFFFF);
        if (w > 2 && i < 15)
            gfx_fill(gfx_rect(px + 1, py + i, w - 2, 1), 0x11151B);
    }
    gfx_fill(gfx_rect(px + 4, py + 13, 3, 6), 0xFFFFFF);
    gfx_fill(gfx_rect(px + 5, py + 13, 1, 5), 0x11151B);
}

bool wm_compose(void)
{
    if (gfx_empty(damage) || back.px == NULL)
        return false;
    struct gfx_rect d = damage;
    damage = gfx_rect(0, 0, 0, 0);
    gfx_copy(&back, &wall, d);
    gfx_target(&back);
    gfx_clip(d);
    if (backdrop != NULL)
        backdrop();
    for (unsigned i = 0; i < nwin; i++)
        draw_window(order[i], i == nwin - 1);
    if (napps > 0) {
        draw_topbar();
        draw_dock();
    }
    if (pointer_shown)
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
    if (key >= INPUT_KEY_F1 && key < INPUT_KEY_F1 + napps && key < INPUT_KEY_F1 + 8) {
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
        pointer_shown = true;
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
    struct window *w = window_at(x, y);
    if (napps > 0 && (y < UI_TOPBAR_H || (w == NULL && gfx_inside(dock_rect(), x, y)))) {
        topbar_click(x, y);
        return;
    }
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

void wm_animate(void)
{
    for (unsigned i = 0; i < nwin; i++)
        if (order[i]->app->anim != NULL)
            order[i]->app->anim(order[i]);
}

void wm_tick(void)
{
    for (unsigned i = 0; i < nwin; i++)
        if (order[i]->app->tick != NULL)
            order[i]->app->tick(order[i]);
    topbar_refresh();                                   /* the clock */
}

/* ---- widgets ------------------------------------------------------------- */

int ui_buttons(struct gfx_rect body, const char *const *labels, unsigned n, int primary,
               bool draw, int32_t x, int32_t y)
{
    int32_t bx = body.x + UI_PAD, by = body.y + 12;
    int hit = -1;
    for (unsigned i = 0; i < n; i++) {
        struct gfx_rect r = gfx_rect(bx, by, gfx_text_width(labels[i], FONT_MEDIUM) + 28, 32);
        if (draw) {
            bool main = (int)i == primary;
            gfx_round(r, UI_RADIUS_SM, main ? COL_PRIMARY : COL_SURFACE_2, 255);
            gfx_text(r.x + 14, r.y + (r.h - gfx_line_height(FONT_MEDIUM)) / 2, labels[i],
                     main ? COL_SURFACE : COL_TEXT, FONT_MEDIUM);
        }
        if (gfx_inside(r, x, y))
            hit = (int)i;
        bx += r.w + 8;
    }
    return hit;
}
