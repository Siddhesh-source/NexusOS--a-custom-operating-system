/* Text editor: opens one file, wraps long lines, and saves with the Save
 * button or Ctrl+S. Typing inserts at the cursor; Backspace and Delete
 * remove; arrows, Home and End move. The whole file is one block of
 * memory (files are small), so every edit is a simple move of bytes. */

#include "apps.h"
#include "theme.h"
#include "fs.h"
#include "input.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define EDIT_MAX  (16 * 1024)
#define EDIT_COLS 72
#define EDIT_ROWS 24

struct editor {
    char path[FS_PATH_MAX];
    char *text;                     /* EDIT_MAX bytes, 0-terminated */
    size_t len, cur;
    unsigned top;                   /* first row shown */
    bool dirty, readonly;
    char msg[80];
};

static const char *const edit_buttons[] = { "Save" };

static void set_title(struct window *w)
{
    struct editor *e = w->st;
    const char *name = strrchr(e->path, '/');
    char t[48];
    ksnprintf(t, sizeof(t), "Text editor: %s%s", name != NULL ? name + 1 : e->path, e->dirty ? " *" : "");
    wm_set_title(w, t);
}

/* Row and column where the character at `idx` is drawn (lines wrap at
 * EDIT_COLS). */
static void pos_of(const struct editor *e, size_t idx, unsigned *row, unsigned *col)
{
    unsigned r = 0, c = 0;
    for (size_t i = 0; i < idx && i < e->len; i++) {
        if (e->text[i] == '\n' || ++c >= EDIT_COLS) {
            r++;
            c = 0;
        }
    }
    *row = r;
    *col = c;
}

/* The index nearest to (row, col), staying on that row. */
static size_t index_at(const struct editor *e, unsigned row, unsigned col)
{
    unsigned r = 0, c = 0;
    size_t best = e->len;
    bool found = false;
    for (size_t i = 0; i <= e->len; i++) {
        if (r == row && c <= col) {
            best = i;
            found = true;
        }
        if (r > row || i == e->len)
            break;
        if (e->text[i] == '\n' || ++c >= EDIT_COLS) {
            r++;
            c = 0;
        }
    }
    return found ? best : e->len;
}

static bool edit_open(struct window *w, const char *arg)
{
    struct editor *e = kzalloc(sizeof(*e));
    struct fs_stat s;
    if (e == NULL || arg == NULL || fs_stat(arg, &s) != FS_OK || s.type != FS_FILE
        || (e->text = kmalloc(EDIT_MAX)) == NULL) {
        kfree(e);
        return false;
    }
    w->st = e;
    strlcpy(e->path, arg, sizeof(e->path));
    if (fs_read_file(arg, e->text, EDIT_MAX - 1, &e->len) != FS_OK)
        e->len = 0;
    e->text[e->len] = '\0';
    e->readonly = s.size > EDIT_MAX - 1;
    strlcpy(e->msg, e->readonly ? "This file is too big to edit here; showing the start only."
                                : "Ctrl+S saves.", sizeof(e->msg));
    e->cur = e->len;                      /* ready to add at the end */
    set_title(w);
    return true;
}

static void edit_close(struct window *w)
{
    kfree(((struct editor *)w->st)->text);
}

static void edit_draw(struct window *w, struct gfx_rect body)
{
    struct editor *e = w->st;
    ui_buttons(body, edit_buttons, 1, true, -1, -1);
    int32_t x0 = body.x + UI_PAD, y0 = body.y + UI_BAR_H;
    unsigned r = 0, c = 0;
    for (size_t i = 0; i < e->len && r < e->top + EDIT_ROWS; i++) {
        char ch = e->text[i];
        if (ch != '\n' && r >= e->top) {
            char shown = ch >= 0x20 && ch < 0x7F ? ch : '.';
            gfx_text_n(x0 + (int32_t)c * GFX_CHAR_W, y0 + (int32_t)(r - e->top) * GFX_CHAR_H,
                       &shown, 1, COL_TEXT, 1);
        }
        if (ch == '\n' || ++c >= EDIT_COLS) {
            r++;
            c = 0;
        }
    }
    pos_of(e, e->cur, &r, &c);
    if (r >= e->top && r < e->top + EDIT_ROWS)
        gfx_fill(gfx_rect(x0 + (int32_t)c * GFX_CHAR_W, y0 + (int32_t)(r - e->top) * GFX_CHAR_H,
                          2, GFX_CHAR_H), COL_ACCENT);
    gfx_fill(gfx_rect(body.x, body.y + body.h - 30, body.w, 1), COL_BORDER);
    gfx_text(x0, body.y + body.h - 22, e->msg, COL_MUTED, 1);
}

static void save(struct window *w)
{
    struct editor *e = w->st;
    if (e->readonly) {
        strlcpy(e->msg, "Not saved: this file is too big to edit here.", sizeof(e->msg));
        return;
    }
    fs_status_t st = fs_write_file(e->path, e->text, e->len, false);
    if (st == FS_OK) {
        e->dirty = false;
        ksnprintf(e->msg, sizeof(e->msg), "Saved %lu bytes to %s.", e->len, e->path);
    } else {
        ksnprintf(e->msg, sizeof(e->msg), "Not saved: %s.", fs_status_str(st));
    }
    set_title(w);
}

static void edit_key(struct window *w, uint16_t key)
{
    struct editor *e = w->st;
    unsigned r, c;
    pos_of(e, e->cur, &r, &c);
    bool was_dirty = e->dirty;
    if (key == 19) {                                    /* Ctrl+S */
        save(w);
    } else if ((key == '\n' || (key >= 0x20 && key < 0x7F)) && !e->readonly) {
        if (e->len + 1 < EDIT_MAX) {
            memmove(e->text + e->cur + 1, e->text + e->cur, e->len - e->cur + 1);
            e->text[e->cur++] = (char)key;
            e->len++;
            e->dirty = true;
        } else {
            strlcpy(e->msg, "The file is full (16 KiB).", sizeof(e->msg));
        }
    } else if ((key == '\b' && e->cur > 0) || (key == INPUT_KEY_DELETE && e->cur < e->len)) {
        if (!e->readonly) {
            if (key == '\b')
                e->cur--;
            memmove(e->text + e->cur, e->text + e->cur + 1, e->len - e->cur);
            e->len--;
            e->dirty = true;
        }
    } else if (key == INPUT_KEY_LEFT && e->cur > 0) {
        e->cur--;
    } else if (key == INPUT_KEY_RIGHT && e->cur < e->len) {
        e->cur++;
    } else if (key == INPUT_KEY_UP && r > 0) {
        e->cur = index_at(e, r - 1, c);
    } else if (key == INPUT_KEY_DOWN) {
        e->cur = index_at(e, r + 1, c);
    } else if (key == INPUT_KEY_HOME || key == INPUT_KEY_END) {
        e->cur = index_at(e, r, key == INPUT_KEY_HOME ? 0 : EDIT_COLS);
    }
    pos_of(e, e->cur, &r, &c);
    if (r < e->top)
        e->top = r;
    if (r >= e->top + EDIT_ROWS)
        e->top = r - EDIT_ROWS + 1;
    if (e->dirty != was_dirty)
        set_title(w);
    wm_redraw(w);
}

static void edit_click(struct window *w, int32_t x, int32_t y)
{
    struct gfx_rect b = wm_body(w);
    if (ui_buttons(gfx_rect(0, 0, b.w, b.h), edit_buttons, 1, false, x, y) == 0) {
        save(w);
        wm_redraw(w);
    }
}

const char *editor_text(struct window *w, bool *unsaved)
{
    if (w == NULL || w->app != &app_editor)
        return NULL;
    struct editor *e = w->st;
    if (unsaved != NULL)
        *unsaved = e->dirty;
    return e->text;
}

const struct app app_editor = {
    .name = "Text editor",
    .w = EDIT_COLS * GFX_CHAR_W + 2 * UI_PAD,
    .h = UI_BAR_H + EDIT_ROWS * GFX_CHAR_H + 40,
    .open = edit_open,
    .close = edit_close,
    .draw = edit_draw,
    .key = edit_key,
    .click = edit_click,
};
