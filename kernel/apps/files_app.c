/* Files: one folder at a time, as a list, with "Places" on the left
 * (Home, Code, Videos, Web pages, Documents, the whole disk) and how full
 * the disk is under them. Click a row to select it, click it again (or
 * press Enter) to open it: a folder opens in place, a file opens in the
 * app that suits it (a video in Player, a web page in Browser, code in
 * Code, anything else in the text editor). Up (or Backspace) closes the
 * folder and goes back to the one holding it. New file and New folder ask
 * for a name on the bottom line; Delete removes the selected file or empty
 * folder. Keys: arrows, Enter, Backspace (up), n (new file), d (new
 * folder), Delete. */

#include "apps.h"
#include "theme.h"
#include "fs.h"
#include "input.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define FILES_MAX 64
#define ROW_H     32
#define LIST_ROWS 12
#define LIST_Y    (UI_BAR_H + 36)        /* first row, from the body top */
#define SIDE      176                    /* the Places column */
#define PLACE_Y   (UI_BAR_H + 30)
#define PLACE_H   30

static const char *const place_names[] = { "Home", "Code", "Videos", "Web pages", "Documents", "Whole disk" };
static const char *const place_paths[] = { NULL, "/code", "/media", "/www", "/docs", "/" };
#define PLACES 6

enum { NORMAL, NEW_FILE, NEW_FOLDER };

struct files {
    char cwd[FS_PATH_MAX];
    struct fs_dirent ents[FILES_MAX];
    unsigned n, sel, top;
    int mode;
    char name[FS_NAME_MAX];
    char msg[80];
};

static const char *const files_buttons[] = { "Up", "Open", "New file", "New folder", "Delete" };

static void load(struct files *f)
{
    f->n = 0;
    while (f->n < FILES_MAX && fs_list(f->cwd, f->n, &f->ents[f->n]) == FS_OK)
        f->n++;
    if (f->sel >= f->n)
        f->sel = f->n > 0 ? f->n - 1 : 0;
}

static bool child(struct files *f, const char *name, char *out)
{
    return fs_path_resolve(f->cwd, name, out, FS_PATH_MAX) == FS_OK;
}

static void go(struct files *f, const char *name)
{
    char p[FS_PATH_MAX];
    if (child(f, name, p)) {
        strlcpy(f->cwd, p, sizeof(f->cwd));
        f->sel = f->top = 0;
        f->msg[0] = '\0';
        load(f);
    }
}

static bool files_open(struct window *w, const char *arg)
{
    struct files *f = kzalloc(sizeof(*f));
    if (f == NULL)
        return false;
    w->st = f;
    ksnprintf(f->cwd, sizeof(f->cwd), "/home/%s", desktop_user());
    struct fs_stat s;
    if (arg != NULL)
        strlcpy(f->cwd, arg, sizeof(f->cwd));
    if (fs_stat(f->cwd, &s) != FS_OK || s.type != FS_DIR)
        strlcpy(f->cwd, "/", sizeof(f->cwd));
    load(f);
    return true;
}

static void open_selected(struct window *w)
{
    struct files *f = w->st;
    if (f->n == 0)
        return;
    struct fs_dirent *e = &f->ents[f->sel];
    if (e->type == FS_DIR) {
        go(f, e->name);
        return;
    }
    char p[FS_PATH_MAX];
    if (child(f, e->name, p) && wm_open(app_for_file(p), p) == NULL)
        ksnprintf(f->msg, sizeof(f->msg), "Cannot open %s (too many windows?)", e->name);
}

static void delete_selected(struct files *f)
{
    char p[FS_PATH_MAX];
    if (f->n == 0 || !child(f, f->ents[f->sel].name, p))
        return;
    fs_status_t st = fs_unlink(p);
    if (st == FS_OK)
        ksnprintf(f->msg, sizeof(f->msg), "Deleted %s.", f->ents[f->sel].name);
    else
        ksnprintf(f->msg, sizeof(f->msg), "Cannot delete %s: %s.", f->ents[f->sel].name, fs_status_str(st));
    load(f);
}

static void create(struct files *f)
{
    char p[FS_PATH_MAX];
    fs_status_t st = FS_ERR_INVALID;
    if (f->name[0] != '\0' && child(f, f->name, p))
        st = f->mode == NEW_FILE ? fs_create(p) : fs_mkdir(p);
    if (st == FS_OK)
        ksnprintf(f->msg, sizeof(f->msg), "Created %s.", f->name);
    else
        ksnprintf(f->msg, sizeof(f->msg), "Cannot create '%s': %s.", f->name, fs_status_str(st));
    f->mode = NORMAL;
    load(f);
    for (unsigned i = 0; st == FS_OK && i < f->n; i++)
        if (strcmp(f->ents[i].name, f->name) == 0)
            f->sel = i;
}

static void keep_visible(struct files *f)
{
    if (f->sel < f->top)
        f->top = f->sel;
    if (f->sel >= f->top + LIST_ROWS)
        f->top = f->sel - LIST_ROWS + 1;
}

static void files_draw(struct window *w, struct gfx_rect body)
{
    struct files *f = w->st;
    ui_buttons(body, files_buttons, 5, -1, true, -1, -1);
    /* Places and the disk */
    struct gfx_rect side = gfx_rect(body.x + 8, body.y + UI_BAR_H, SIDE - 16, body.h - UI_BAR_H - 56);
    gfx_round(side, UI_RADIUS_SM, COL_SURFACE_2, 255);
    gfx_text(side.x + 12, side.y + 8, "PLACES", COL_MUTED, FONT_SMALL);
    for (unsigned i = 0; i < PLACES; i++) {
        char home[FS_PATH_MAX];
        const char *pp = place_paths[i];
        if (pp == NULL) {
            ksnprintf(home, sizeof(home), "/home/%s", desktop_user());
            pp = home;
        }
        int32_t py = body.y + PLACE_Y + (int32_t)i * PLACE_H;
        if (strcmp(f->cwd, pp) == 0)
            gfx_round(gfx_rect(side.x + 4, py, side.w - 8, PLACE_H - 4), UI_RADIUS_SM, COL_ACCENT_SOFT, 255);
        gfx_text(side.x + 14, py + 5, place_names[i], COL_TEXT, FONT_UI);
    }
    struct fs_usage u;
    fs_get_usage(&u);
    int32_t dy = side.y + side.h - 74;
    gfx_text(side.x + 12, dy, "DISK", COL_MUTED, FONT_SMALL);
    gfx_text(side.x + 12, dy + 18, "BharatOS files (RAM)", COL_TEXT, FONT_SMALL);
    struct gfx_rect bar = gfx_rect(side.x + 12, dy + 38, side.w - 24, 6);
    gfx_round(bar, 3, COL_LINE, 255);
    int32_t used = u.max_bytes ? (int32_t)((uint64_t)bar.w * u.bytes / u.max_bytes) : 0;
    gfx_round(gfx_rect(bar.x, bar.y, used < 4 ? 4 : used, bar.h), 3, COL_TEAL, 255);
    char du[48];
    ksnprintf(du, sizeof(du), "%lu KB of %lu MB used", u.bytes / 1024, u.max_bytes >> 20);
    gfx_text(side.x + 12, dy + 50, du, COL_MUTED, FONT_SMALL);

    body.x += SIDE - 8;
    body.w -= SIDE - 8;
    int32_t x = body.x + UI_PAD, y = body.y + UI_BAR_H;
    gfx_text(x, y + 4, f->cwd, COL_MUTED, FONT_MEDIUM);
    y = body.y + LIST_Y;
    if (f->n == 0) {
        const char *e = "This folder is empty";
        gfx_text(body.x + (body.w - gfx_text_width(e, FONT_UI)) / 2, y + 80, e, COL_MUTED, FONT_UI);
    }
    for (unsigned i = f->top; i < f->n && i < f->top + LIST_ROWS; i++, y += ROW_H) {
        struct fs_dirent *e = &f->ents[i];
        if (i == f->sel)
            gfx_round(gfx_rect(body.x + 8, y, body.w - 16, ROW_H), UI_RADIUS_SM, COL_ACCENT_SOFT, 255);
        if (e->type == FS_DIR) {
            gfx_round(gfx_rect(x, y + 9, 8, 5), 1, 0xC98A3A, 255);          /* a folder */
            gfx_round(gfx_rect(x, y + 11, 18, 12), 2, 0xE0A458, 255);
        } else {
            gfx_round(gfx_rect(x + 2, y + 7, 14, 18), 2, COL_LINE, 255);   /* a page */
            gfx_round(gfx_rect(x + 3, y + 8, 12, 16), 2, COL_SURFACE, 255);
            gfx_fill(gfx_rect(x + 6, y + 13, 6, 1), COL_MUTED);
            gfx_fill(gfx_rect(x + 6, y + 17, 6, 1), COL_MUTED);
        }
        gfx_text(x + 30, y + 5, e->name, COL_TEXT, FONT_UI);
        char size[24];
        if (e->type == FS_DIR)
            ksnprintf(size, sizeof(size), "%lu item%s", e->size, e->size == 1 ? "" : "s");
        else
            ksnprintf(size, sizeof(size), "%lu bytes", e->size);
        gfx_text(body.x + body.w - UI_PAD - 8 - gfx_text_width(size, FONT_SMALL), y + 7, size,
                 COL_MUTED, FONT_SMALL);
    }
    int32_t by = body.y + body.h - 44;
    body.x -= SIDE - 8;
    body.w += SIDE - 8;
    x = body.x + UI_PAD;
    gfx_fill(gfx_rect(body.x, by - 8, body.w, 1), COL_LINE);
    if (f->mode != NORMAL) {
        const char *label = f->mode == NEW_FILE ? "New file name" : "New folder name";
        int32_t lw = gfx_text_width(label, FONT_SMALL);
        gfx_text(x, by + 9, label, COL_MUTED, FONT_SMALL);
        struct gfx_rect field = gfx_rect(x + lw + 12, by + 2, body.w - 2 * UI_PAD - lw - 12, 32);
        gfx_round(field, UI_RADIUS_SM, COL_PRIMARY, 255);                  /* focus ring */
        gfx_round(gfx_grow(field, -2), UI_RADIUS_SM - 2, COL_SURFACE, 255);
        int32_t tw = gfx_text(field.x + 10, field.y + 5, f->name, COL_TEXT, FONT_UI);
        gfx_fill(gfx_rect(field.x + 11 + tw, field.y + 8, 2, 16), COL_PRIMARY);
    } else {
        gfx_text(x, by + 8, f->msg[0] ? f->msg : "Click a file twice to open it", COL_MUTED, FONT_SMALL);
    }
}

static void action(struct window *w, int which)
{
    struct files *f = w->st;
    switch (which) {
    case 0: go(f, ".."); break;
    case 1: open_selected(w); break;
    case 2: case 3:
        f->mode = which == 2 ? NEW_FILE : NEW_FOLDER;
        f->name[0] = '\0';
        break;
    case 4: delete_selected(f); break;
    }
}

static void files_key(struct window *w, uint16_t key)
{
    struct files *f = w->st;
    if (f->mode != NORMAL) {
        size_t n = strlen(f->name);
        if (key == '\n')
            create(f);
        else if (key == 0x1B)
            f->mode = NORMAL;
        else if (key == '\b' && n > 0)
            f->name[n - 1] = '\0';
        else if (key > 0x20 && key < 0x7F && key != '/' && n + 1 < FS_NAME_MAX) {
            f->name[n] = (char)key;
            f->name[n + 1] = '\0';
        }
    } else if (key == INPUT_KEY_UP && f->sel > 0) {
        f->sel--;
    } else if (key == INPUT_KEY_DOWN && f->sel + 1 < f->n) {
        f->sel++;
    } else if (key == '\n') {
        action(w, 1);
    } else if (key == '\b') {
        action(w, 0);
    } else if (key == 'n' || key == 'd') {
        action(w, key == 'n' ? 2 : 3);
    } else if (key == INPUT_KEY_DELETE) {
        action(w, 4);
    }
    keep_visible(f);
    wm_redraw(w);
}

static void files_click(struct window *w, int32_t x, int32_t y)
{
    struct files *f = w->st;
    struct gfx_rect b = wm_body(w);
    int hit = ui_buttons(gfx_rect(0, 0, b.w, b.h), files_buttons, 5, -1, false, x, y);
    if (hit >= 0) {
        action(w, hit);
    } else if (x < SIDE) {
        if (y >= PLACE_Y && y < PLACE_Y + PLACES * PLACE_H) {
            unsigned i = (unsigned)(y - PLACE_Y) / PLACE_H;
            char home[FS_PATH_MAX];
            const char *pp = place_paths[i];
            if (pp == NULL) {
                ksnprintf(home, sizeof(home), "/home/%s", desktop_user());
                pp = home;
            }
            go(f, pp);
        }
    } else if (y >= LIST_Y) {
        unsigned i = f->top + (unsigned)(y - LIST_Y) / ROW_H;
        if (i < f->n && i < f->top + LIST_ROWS) {
            if (i == f->sel)
                open_selected(w);          /* a second click opens */
            else
                f->sel = i;
        }
    }
    wm_redraw(w);
}

static void files_tick(struct window *w)
{
    /* Files can change from elsewhere (the terminal): refresh the list. */
    struct files *f = w->st;
    unsigned n = f->n;
    load(f);
    if (n != f->n)
        wm_redraw(w);
}

bool files_select(struct window *w, const char *name)
{
    if (w == NULL || w->app != &app_files)
        return false;
    struct files *f = w->st;
    load(f);
    for (unsigned i = 0; i < f->n; i++)
        if (strcmp(f->ents[i].name, name) == 0) {
            f->sel = i;
            keep_visible(f);
            wm_redraw(w);
            return true;
        }
    return false;
}

const char *files_selected(struct window *w)
{
    struct files *f = w != NULL && w->app == &app_files ? w->st : NULL;
    return f != NULL && f->n > 0 ? f->ents[f->sel].name : "";
}

const char *files_folder(struct window *w)
{
    return w != NULL && w->app == &app_files ? ((struct files *)w->st)->cwd : "";
}

const struct app app_files = {
    .name = "Files",
    .w = 600 + SIDE,
    .h = LIST_Y + LIST_ROWS * ROW_H + 60,
    .tile = 0xEFE3CC,
    .icon = icon_files,
    .open = files_open,
    .draw = files_draw,
    .key = files_key,
    .click = files_click,
    .tick = files_tick,
};
