/* Files: one folder at a time, as a list. Click a row to select it,
 * click it again (or press Enter) to open it: a folder opens in place, a
 * file opens in the text editor. Up goes to the parent folder. New file
 * and New folder ask for a name on the bottom line; Delete removes the
 * selected file or empty folder. Keys: arrows, Enter, Backspace (up),
 * n (new file), d (new folder), Delete. */

#include "apps.h"
#include "theme.h"
#include "fs.h"
#include "input.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define FILES_MAX 64
#define ROW_H     20
#define LIST_ROWS 18

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
    if (child(f, e->name, p) && wm_open(&app_editor, p) == NULL)
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
    ui_buttons(body, files_buttons, 5, true, -1, -1);
    int32_t x = body.x + UI_PAD, y = body.y + UI_BAR_H;
    gfx_text(x, y, f->cwd, COL_MUTED, 1);
    y += 24;
    if (f->n == 0)
        gfx_text(x, y + 2, "(this folder is empty)", COL_MUTED, 1);
    for (unsigned i = f->top; i < f->n && i < f->top + LIST_ROWS; i++, y += ROW_H) {
        struct fs_dirent *e = &f->ents[i];
        if (i == f->sel)
            gfx_fill(gfx_rect(body.x, y, body.w, ROW_H), COL_SELECT);
        if (e->type == FS_DIR) {
            gfx_fill(gfx_rect(x, y + 5, 16, 11), COL_SAFFRON);       /* a folder */
            gfx_fill(gfx_rect(x, y + 3, 7, 3), COL_SAFFRON);
        } else {
            gfx_frame(gfx_rect(x + 2, y + 2, 12, 16), COL_MUTED);     /* a page */
        }
        gfx_text(x + 24, y + 2, e->name, COL_TEXT, 1);
        char size[24];
        if (e->type == FS_DIR)
            ksnprintf(size, sizeof(size), "%lu items", e->size);
        else
            ksnprintf(size, sizeof(size), "%lu bytes", e->size);
        gfx_text(body.x + body.w - UI_PAD - (int32_t)strlen(size) * GFX_CHAR_W, y + 2, size, COL_MUTED, 1);
    }
    char line[96];
    if (f->mode != NORMAL)
        ksnprintf(line, sizeof(line), "Name of the new %s: %s_   (Enter: create, Esc: cancel)",
                  f->mode == NEW_FILE ? "file" : "folder", f->name);
    else
        strlcpy(line, f->msg[0] ? f->msg : "Enter opens, Backspace goes up, n new file, d new folder",
                sizeof(line));
    gfx_text(x, body.y + body.h - 24, line, f->mode != NORMAL ? COL_ACCENT : COL_MUTED, 1);
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
    int hit = ui_buttons(gfx_rect(0, 0, b.w, b.h), files_buttons, 5, false, x, y);
    if (hit >= 0) {
        action(w, hit);
    } else if (y >= UI_BAR_H + 24) {
        unsigned i = f->top + (unsigned)(y - UI_BAR_H - 24) / ROW_H;
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
    .w = 560,
    .h = UI_BAR_H + 24 + LIST_ROWS * ROW_H + 40,
    .open = files_open,
    .draw = files_draw,
    .key = files_key,
    .click = files_click,
    .tick = files_tick,
};
