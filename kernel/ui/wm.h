#ifndef BHARAT_WM_H
#define BHARAT_WM_H

/* Window manager: windows, the top bar, the mouse pointer, and drawing
 * them all.
 *
 * Each window belongs to an app (struct app: a name, a size and a few
 * callbacks). Windows are kept in stacking order; the top one has the
 * focus and gets the keys. With the mouse, a window is focused by
 * clicking it, moved by dragging its title bar and closed with the x at
 * its top right. The top bar shows the apps (click to open or bring to
 * the front), the user's name, the CPU count, the health badge, the clock
 * and "Log out". Keys F1..F5 open the apps too, Ctrl+W closes a window.
 *
 * Drawing: everything is drawn into a back buffer in memory. Any change
 * marks a damage rectangle (wm_damage); wm_compose() redraws only that
 * part (background, windows bottom to top, top bar, pointer, all clipped
 * to it) and copies just that part to the screen.
 *
 * The window manager is used by one thread only (the desktop; the boot
 * thread during self-tests), so it needs no lock.
 *
 * Limits: WM_MAX_WINDOWS windows; windows are not resizable and cannot be
 * minimised; one damage rectangle (the union of all changes). */

#include <stdbool.h>
#include <stdint.h>
#include "gfx.h"

#define WM_MAX_WINDOWS 8
#define WM_MAX_APPS    8

struct window;

struct app {
    const char *name;
    int32_t w, h;                                  /* body size */
    bool (*open)(struct window *w, const char *arg);
    void (*close)(struct window *w);
    void (*draw)(struct window *w, struct gfx_rect body);
    void (*key)(struct window *w, uint16_t key);
    void (*click)(struct window *w, int32_t x, int32_t y);   /* body-relative */
    void (*tick)(struct window *w);                /* about once a second */
};

struct window {
    bool used;
    const struct app *app;
    struct gfx_rect r;             /* the whole window, title bar included */
    char title[48];
    void *st;                      /* the app's own state */
};

struct wm_stats {
    uint64_t frames;               /* wm_compose calls that drew something */
    uint64_t pixels;               /* pixels copied to the screen */
    unsigned windows;
};

/* Start with `screen` (NULL: draw into memory only, for self-tests) of
 * the given size. False if the back buffer cannot be allocated. */
bool wm_init(struct gfx_surface *screen, int32_t w, int32_t h);
void wm_set_apps(const struct app *const *apps, unsigned n);
void wm_set_user(const char *user);
/* Called when "Log out" is clicked. */
void wm_on_logout(void (*fn)(void));
/* Something drawn on the background under the windows (the login
 * screen); NULL for none. */
void wm_set_backdrop(void (*fn)(void));

struct window *wm_open(const struct app *a, const char *arg);
void wm_close(struct window *w);
void wm_close_all(void);
void wm_focus(struct window *w);
struct window *wm_focused(void);
struct window *wm_find(const struct app *a);
/* Open the app, or bring its window to the front if one is open. */
struct window *wm_show(const struct app *a);
void wm_move(struct window *w, int32_t x, int32_t y);    /* kept on screen */
void wm_set_title(struct window *w, const char *title);

void wm_damage(struct gfx_rect r);
void wm_damage_all(void);
void wm_redraw(struct window *w);
/* The window's body (where the app draws), in screen coordinates. */
struct gfx_rect wm_body(const struct window *w);

void wm_key(uint16_t key);
void wm_mouse(int32_t x, int32_t y, uint8_t buttons);
void wm_tick(void);
bool wm_compose(void);

struct gfx_surface *wm_back(void);
void wm_get_stats(struct wm_stats *out);
/* Rectangle of top-bar item `i`: apps 0..n-1, then "Log out", then the
 * health badge (self-tests click them). */
struct gfx_rect wm_topbar_item(unsigned i);

/* ---- small shared widgets ------------------------------------------------ */

#define UI_BAR_H 40                /* a row of buttons at the top of a body */

/* A row of buttons along the top of `body`. With draw = true, draws them;
 * either way returns the index of the button at (px, py), or -1. */
int ui_buttons(struct gfx_rect body, const char *const *labels, unsigned n,
               bool draw, int32_t px, int32_t py);

#endif /* BHARAT_WM_H */
