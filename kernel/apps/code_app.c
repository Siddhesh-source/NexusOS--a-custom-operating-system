/* Code: a small code editor in the style of the popular ones.
 *
 *   left     Explorer: the files of a folder; click one to open it in a
 *            new tab, click a folder to go in, ".." to go up
 *   top      Save and Run, then one tab per open file (click to switch)
 *   middle   the file, with line numbers and colours for C, Python, shell
 *            and Markdown (keywords, strings, comments, numbers)
 *   bottom   Output: what Run printed, and a status line (line, column,
 *            language, saved or not)
 *
 * Run runs a shell script (.sh) line by line in a private shell and shows
 * what it printed; for other files it says what they are. Ctrl+S saves,
 * Tab puts in 4 spaces, Enter keeps the indent of the line above.
 * Ctrl+T moves to the next tab, Ctrl+Q closes the tab (twice if it is not
 * saved). Ctrl+F finds text: type it, Enter jumps to the next match
 * (upper/lower case ignored, wrapping round at the end), Esc stops.
 *
 * Colours are found again for the lines on screen at every draw (one pass
 * over the file to know whether the first line starts inside a comment):
 * files are small, and nothing has to be kept in step with edits.
 *
 * Limits: files up to CODE_MAX bytes; CODE_TABS files open; no undo; long
 * lines are cut at the right edge (scroll with the cursor). */

#include "apps.h"
#include "theme.h"
#include "fs.h"
#include "shell.h"
#include "input.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define CODE_MAX   (64 * 1024)
#define SIDE_W     190
#define GUTTER     (5 * FONT_MONO_W)
#define ROWS       20
#define COLS       64
#define OUT_ROWS   5
#define EXP_MAX    32
#define TOP        UI_BAR_H
#define OUT_Y      (TOP + ROWS * FONT_MONO_H + 10)
#define TAB_X      160                  /* tabs start after Save and Run */
#define TAB_W      112
#define FIND_MAX   40

/* VS Code-like dark colours, used only inside this window. */
#define C_BG       0x1E1E1Eu
#define C_SIDE     0x252526u
#define C_GUTTER   0x858585u
#define C_TEXT     0xD4D4D4u
#define C_KEY      0x569CD6u
#define C_STR      0xCE9178u
#define C_COM      0x6A9955u
#define C_NUM      0xB5CEA8u
#define C_PRE      0xC586C0u
#define C_SEL      0x37373Du
#define C_STATUS   0x007ACCu
#define C_FIND     0x623F00u

enum lang { L_TEXT, L_C, L_PY, L_SH, L_MD };

/* One open file. */
struct doc {
    char path[FS_PATH_MAX];
    char *text;
    size_t len, cur;
    unsigned top, left;                 /* first line and column shown */
    bool dirty;
    enum lang lang;
};

struct code {
    struct doc tab[CODE_TABS];
    unsigned ntabs, at;                 /* open tabs, the one shown */
    bool close_asked;                   /* Ctrl+Q once on an unsaved tab */
    bool finding;                       /* the find bar is open */
    char find[FIND_MAX + 1];
    char dir[FS_PATH_MAX];
    struct fs_dirent ents[EXP_MAX];
    unsigned nents;
    struct textbuf out;
    char msg[80];
};

static const char *const code_buttons[] = { "Save", "Run" };

static struct doc *doc_of(struct code *c)
{
    return &c->tab[c->at];
}

static enum lang lang_of(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL)
        return L_TEXT;
    if (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0)
        return L_C;
    if (strcmp(dot, ".py") == 0)
        return L_PY;
    if (strcmp(dot, ".sh") == 0)
        return L_SH;
    if (strcmp(dot, ".md") == 0)
        return L_MD;
    return L_TEXT;
}

static const char *lang_name(enum lang l)
{
    static const char *const n[] = { "Plain text", "C", "Python", "Shell", "Markdown" };
    return n[l];
}

static const char *base_name(const char *path)
{
    const char *name = strrchr(path, '/');
    return name != NULL ? name + 1 : path;
}

static void load_dir(struct code *c)
{
    c->nents = 0;
    while (c->nents < EXP_MAX && fs_list(c->dir, c->nents, &c->ents[c->nents]) == FS_OK)
        c->nents++;
}

static void set_title(struct window *w)
{
    struct doc *d = doc_of(w->st);
    char t[48];
    ksnprintf(t, sizeof(t), "Code: %s%s", base_name(d->path), d->dirty ? " *" : "");
    wm_set_title(w, t);
    wm_redraw(w);
}

/* Read `path` into `d`; a missing file starts empty (saved on Ctrl+S). */
static bool load_file(struct doc *d, const char *path, bool may_be_new)
{
    struct fs_stat s;
    size_t len = 0;
    fs_status_t st = fs_stat(path, &s);
    if (st == FS_ERR_NOT_FOUND && may_be_new) {
        len = 0;
    } else if (st != FS_OK || s.type != FS_FILE || s.size >= CODE_MAX
               || fs_read_file(path, d->text, CODE_MAX - 1, &len) != FS_OK) {
        return false;
    }
    d->len = len;
    d->text[len] = '\0';
    strlcpy(d->path, path, sizeof(d->path));
    d->cur = 0;
    d->top = d->left = 0;
    d->dirty = false;
    d->lang = lang_of(path);
    return true;
}

bool code_open_tab(struct window *w, const char *path)
{
    if (w == NULL || w->app != &app_code || path == NULL)
        return false;
    struct code *c = w->st;
    for (unsigned i = 0; i < c->ntabs; i++)
        if (strcmp(c->tab[i].path, path) == 0) {
            c->at = i;                       /* already open: show it */
            set_title(w);
            return true;
        }
    if (c->ntabs == CODE_TABS) {
        strlcpy(c->msg, "All tabs are in use; close one with Ctrl+Q", sizeof(c->msg));
        wm_redraw(w);
        return false;
    }
    struct doc *d = &c->tab[c->ntabs];
    if ((d->text = kmalloc(CODE_MAX)) == NULL || !load_file(d, path, c->ntabs == 0)) {
        kfree(d->text);
        d->text = NULL;
        strlcpy(c->msg, "Cannot open that file here", sizeof(c->msg));
        wm_redraw(w);
        return false;
    }
    c->at = c->ntabs++;
    c->close_asked = false;
    set_title(w);
    return true;
}

static bool code_open(struct window *w, const char *arg)
{
    struct code *c = kzalloc(sizeof(*c));
    if (c == NULL || !textbuf_init(&c->out, (SIDE_W + GUTTER + COLS * FONT_MONO_W) / FONT_MONO_W, OUT_ROWS)) {
        kfree(c);
        return false;
    }
    w->st = c;
    if (!code_open_tab(w, arg != NULL ? arg : "/code/hello.c")) {
        textbuf_free(&c->out);
        kfree(c);
        w->st = NULL;
        return false;
    }
    strlcpy(c->dir, c->tab[0].path, sizeof(c->dir));
    char *slash = strrchr(c->dir, '/');
    if (slash != NULL)
        slash[slash == c->dir ? 1 : 0] = '\0';
    load_dir(c);
    textbuf_puts(&c->out, "Output of Run appears here. Ctrl+S saves, Ctrl+F finds.\n");
    set_title(w);
    return true;
}

static void code_close(struct window *w)
{
    struct code *c = w->st;
    textbuf_free(&c->out);
    for (unsigned i = 0; i < c->ntabs; i++)
        kfree(c->tab[i].text);
}

/* Close tab `i`; the last tab cannot be closed (close the window). */
static void close_tab(struct window *w, unsigned i)
{
    struct code *c = w->st;
    if (c->ntabs <= 1 || i >= c->ntabs) {
        strlcpy(c->msg, "This is the last tab; Ctrl+W closes the window", sizeof(c->msg));
        return;
    }
    if (c->tab[i].dirty && !c->close_asked) {
        c->close_asked = true;
        strlcpy(c->msg, "Not saved: Ctrl+Q again closes it anyway", sizeof(c->msg));
        return;
    }
    kfree(c->tab[i].text);
    memmove(&c->tab[i], &c->tab[i + 1], sizeof(struct doc) * (c->ntabs - i - 1));
    c->ntabs--;
    memset(&c->tab[c->ntabs], 0, sizeof(struct doc));
    if (c->at >= c->ntabs || c->at > i)
        c->at = c->at > 0 ? c->at - 1 : 0;
    c->close_asked = false;
    set_title(w);
}

/* ---- positions ------------------------------------------------------------ */

static size_t line_start(const struct doc *c, size_t i)
{
    while (i > 0 && c->text[i - 1] != '\n')
        i--;
    return i;
}

static size_t line_end(const struct doc *c, size_t i)
{
    while (i < c->len && c->text[i] != '\n')
        i++;
    return i;
}

static unsigned line_of(const struct doc *c, size_t idx)
{
    unsigned n = 0;
    for (size_t i = 0; i < idx && i < c->len; i++)
        n += c->text[i] == '\n';
    return n;
}

static size_t start_of_line(const struct doc *c, unsigned line)
{
    size_t i = 0;
    for (unsigned n = 0; n < line && i < c->len; i++)
        if (c->text[i] == '\n')
            n++;
    return i;
}

/* ---- colours -------------------------------------------------------------- */

static bool is_word(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_';
}

static bool keyword(enum lang l, const char *s, size_t n)
{
    static const char *const kc[] = { "int", "char", "void", "return", "if", "else", "for", "while",
        "struct", "static", "const", "unsigned", "long", "bool", "true", "false", "sizeof", "switch",
        "case", "break", "continue", "do", "enum", "typedef", "uint8_t", "uint16_t", "uint32_t",
        "uint64_t", "size_t", "NULL", NULL };
    static const char *const kp[] = { "def", "return", "if", "elif", "else", "for", "while", "in",
        "import", "from", "class", "True", "False", "None", "print", "and", "or", "not", "with", "as", NULL };
    static const char *const ks[] = { "echo", "ls", "cd", "cat", "mkdir", "rm", "touch", "cpus",
        "mem", "uptime", "whoami", "ping", "fetch", "tree", "df", "cp", "mv", "grep", "wc", "head", NULL };
    const char *const *k = l == L_C ? kc : l == L_PY ? kp : l == L_SH ? ks : NULL;
    for (; k != NULL && *k != NULL; k++)
        if (strlen(*k) == n && memcmp(*k, s, n) == 0)
            return true;
    return false;
}

/* Colour of each character of one line into `col`; `in_com` carries a C
 * block comment from line to line. */
static void colour_line(enum lang l, const char *s, size_t n, uint32_t *col, bool *in_com)
{
    size_t i = 0;
    if (l == L_MD) {
        uint32_t c = n > 0 && s[0] == '#' ? C_KEY : n > 1 && (s[0] == '-' || s[0] == '*') && s[1] == ' '
                     ? C_PRE : C_TEXT;
        for (; i < n; i++)
            col[i] = c;
        return;
    }
    if (l == L_C) {
        size_t j = 0;
        while (j < n && s[j] == ' ')
            j++;
        if (!*in_com && j < n && s[j] == '#') {
            for (; i < n; i++)
                col[i] = C_PRE;
            return;
        }
    }
    while (i < n) {
        if (*in_com) {
            col[i] = C_COM;
            if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') {
                col[++i] = C_COM;
                *in_com = false;
            }
            i++;
        } else if (l == L_C && s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            *in_com = true;
            col[i++] = C_COM;
        } else if ((l == L_C && s[i] == '/' && i + 1 < n && s[i + 1] == '/')
                   || ((l == L_PY || l == L_SH) && s[i] == '#')) {
            for (; i < n; i++)
                col[i] = C_COM;
        } else if (l != L_TEXT && (s[i] == '"' || s[i] == '\'')) {
            char q = s[i];
            col[i++] = C_STR;
            while (i < n && s[i] != q) {
                if (s[i] == '\\' && i + 1 < n)
                    col[i++] = C_STR;
                col[i++] = C_STR;
            }
            if (i < n)
                col[i++] = C_STR;
        } else if (s[i] >= '0' && s[i] <= '9' && (i == 0 || !is_word(s[i - 1]))) {
            while (i < n && is_word(s[i]))
                col[i++] = C_NUM;
        } else if (is_word(s[i])) {
            size_t b = i;
            while (i < n && is_word(s[i]))
                i++;
            uint32_t c = keyword(l, s + b, i - b) ? C_KEY : C_TEXT;
            for (size_t k = b; k < i; k++)
                col[k] = c;
        } else {
            col[i++] = C_TEXT;
        }
    }
}

/* ---- drawing -------------------------------------------------------------- */

static struct gfx_rect tab_rect(int32_t main_x, int32_t y, unsigned i)
{
    return gfx_rect(main_x + TAB_X + (int32_t)i * (TAB_W + 4), y + 12, TAB_W, 32);
}

static void code_draw(struct window *w, struct gfx_rect body)
{
    struct code *c = w->st;
    struct doc *d = doc_of(c);
    gfx_fill(body, C_BG);
    /* Explorer */
    struct gfx_rect side = gfx_rect(body.x, body.y, SIDE_W, body.h);
    gfx_fill(side, C_SIDE);
    gfx_text(side.x + 12, side.y + 10, "EXPLORER", C_GUTTER, FONT_SMALL);
    gfx_text_n(side.x + 12, side.y + 28, c->dir, 22, C_TEXT, FONT_SMALL);
    int32_t ey = side.y + 52;
    gfx_text(side.x + 12, ey, "..", C_TEXT, FONT_UI);
    ey += 22;
    for (unsigned i = 0; i < c->nents && ey + 22 < body.y + body.h - 30; i++, ey += 22) {
        if (c->ents[i].type == FS_FILE && strcmp(c->ents[i].name, base_name(d->path)) == 0)
            gfx_fill(gfx_rect(side.x, ey - 2, SIDE_W, 22), C_SEL);
        gfx_text(side.x + 12, ey, c->ents[i].type == FS_DIR ? ">" : " ", C_GUTTER, FONT_UI);
        gfx_text_n(side.x + 26, ey, c->ents[i].name, 18,
                   c->ents[i].type == FS_DIR ? C_STR : C_TEXT, FONT_UI);
    }
    /* Buttons and tabs over the code */
    struct gfx_rect main = gfx_rect(body.x + SIDE_W, body.y, body.w - SIDE_W, body.h);
    ui_buttons(gfx_rect(main.x, main.y, main.w, UI_BAR_H), code_buttons, 2, 1, true, -1, -1);
    for (unsigned i = 0; i < c->ntabs; i++) {
        struct gfx_rect t = tab_rect(main.x, main.y, i);
        gfx_round(t, 6, i == c->at ? 0x3C3C3Cu : C_SIDE, 255);
        if (i == c->at)
            gfx_fill(gfx_rect(t.x + 6, t.y + t.h - 2, t.w - 12, 2), C_STATUS);
        char name[16];
        ksnprintf(name, sizeof(name), "%s%s", c->tab[i].dirty ? "*" : "", base_name(c->tab[i].path));
        gfx_text_n(t.x + 10, t.y + 8, name, 13, i == c->at ? 0xFFFFFFu : C_GUTTER, FONT_SMALL);
    }
    /* Code with line numbers and colours */
    int32_t x0 = main.x + 8, y0 = body.y + TOP;
    bool in_com = false;
    size_t i = 0;
    unsigned line = 0;
    unsigned cur_line = line_of(d, d->cur);
    size_t flen = strlen(c->find);
    uint32_t col[512];
    while (line < d->top + ROWS) {
        size_t e = line_end(d, i), n = e - i;
        size_t cn = n < 512 ? n : 512;
        colour_line(d->lang, d->text + i, cn, col, &in_com);
        if (line >= d->top) {
            int32_t y = y0 + (int32_t)(line - d->top) * FONT_MONO_H;
            if (line == cur_line)
                gfx_fill(gfx_rect(main.x, y, main.w, FONT_MONO_H), 0x2A2D2Eu);
            char num[8];
            ksnprintf(num, sizeof(num), "%4u", line + 1);
            gfx_text(x0, y, num, line == cur_line ? C_TEXT : C_GUTTER, FONT_MONO);
            for (size_t k = d->left; k < cn && k < d->left + COLS; k++) {
                int32_t x = x0 + GUTTER + (int32_t)(k - d->left) * FONT_MONO_W;
                /* Matches of the find text get a dark orange background. */
                if (c->finding && flen > 0 && k + flen <= n && strncasecmp(d->text + i + k, c->find, flen) == 0)
                    gfx_fill(gfx_rect(x, y, (int32_t)flen * FONT_MONO_W, FONT_MONO_H), C_FIND);
                char ch = d->text[i + k] >= 0x20 && d->text[i + k] < 0x7F ? d->text[i + k] : ' ';
                gfx_text_n(x, y, &ch, 1, col[k], FONT_MONO);
            }
        }
        if (e >= d->len)
            break;
        i = e + 1;
        line++;
    }
    /* Caret */
    size_t ls = line_start(d, d->cur);
    unsigned ccol = (unsigned)(d->cur - ls);
    if (cur_line >= d->top && cur_line < d->top + ROWS && ccol >= d->left && ccol < d->left + COLS)
        gfx_fill(gfx_rect(x0 + GUTTER + (int32_t)(ccol - d->left) * FONT_MONO_W,
                          y0 + (int32_t)(cur_line - d->top) * FONT_MONO_H + 1, 2, FONT_MONO_H - 2), 0xAEAFADu);
    /* The find bar, top right of the code */
    if (c->finding) {
        struct gfx_rect f = gfx_rect(main.x + main.w - 300, y0 + 4, 288, 30);
        gfx_round(f, 4, 0x3C3C3Cu, 255);
        gfx_frame(f, C_STATUS);
        char s[FIND_MAX + 16];
        ksnprintf(s, sizeof(s), "Find: %s_", c->find);
        gfx_text(f.x + 10, f.y + 6, s, C_TEXT, FONT_SMALL);
    }
    /* Output panel and status bar */
    int32_t oy = body.y + OUT_Y;
    gfx_fill(gfx_rect(main.x, oy - 6, main.w, 1), 0x3C3C3Cu);
    gfx_text(main.x + 8, oy - 2, "OUTPUT", C_GUTTER, FONT_SMALL);
    textbuf_draw(&c->out, main.x + 8, oy + 16, C_TEXT);
    struct gfx_rect st = gfx_rect(body.x, body.y + body.h - 24, body.w, 24);
    gfx_fill(st, C_STATUS);
    char s[120];
    ksnprintf(s, sizeof(s), "  Ln %u, Col %u     %s     UTF-8     %s", cur_line + 1, ccol + 1,
              lang_name(d->lang), c->msg[0] ? c->msg : d->dirty ? "Not saved" : "Saved");
    gfx_text(st.x + 4, st.y + 4, s, 0xFFFFFFu, FONT_SMALL);
}

/* ---- actions -------------------------------------------------------------- */

static void save(struct window *w)
{
    struct code *c = w->st;
    struct doc *d = doc_of(c);
    fs_status_t st = fs_write_file(d->path, d->text, d->len, false);
    if (st == FS_OK) {
        d->dirty = false;
        ksnprintf(c->msg, sizeof(c->msg), "Saved %lu bytes", d->len);
        load_dir(c);
    } else {
        ksnprintf(c->msg, sizeof(c->msg), "Not saved: %s", fs_status_str(st));
    }
    set_title(w);
}

/* Run: a shell script line by line in a private shell. */
static void run(struct window *w)
{
    struct code *c = w->st;
    struct doc *d = doc_of(c);
    textbuf_clear(&c->out);
    if (d->lang != L_SH) {
        textbuf_puts(&c->out, d->lang == L_C ? "This is C source. BharatOS runs shell scripts (.sh) here;\n"
                                               "C programs run as apps once user mode lands.\n"
                                             : "Run works on shell scripts (.sh). Try /code/demo.sh\n");
        return;
    }
    struct shell *sh = kzalloc(sizeof(*sh));
    if (sh == NULL) {
        textbuf_puts(&c->out, "Not enough memory to run.\n");
        return;
    }
    shell_init(sh, desktop_user()[0] ? desktop_user() : "user", textbuf_out, &c->out);
    char line[256];
    unsigned ran = 0;
    for (size_t i = 0; i < d->len;) {
        size_t e = line_end(d, i), n = e - i < sizeof(line) - 1 ? e - i : sizeof(line) - 1;
        memcpy(line, d->text + i, n);
        line[n] = '\0';
        size_t k = 0;
        while (line[k] == ' ')
            k++;
        if (line[k] != '\0' && line[k] != '#') {
            shell_run(sh, line + k);
            ran++;
        }
        i = e + 1;
    }
    kfree(sh);
    ksnprintf(c->msg, sizeof(c->msg), "Ran %u line%s", ran, ran == 1 ? "" : "s");
}

static bool insert(struct doc *d, const char *s, size_t n)
{
    if (d->len + n >= CODE_MAX)
        return false;
    memmove(d->text + d->cur + n, d->text + d->cur, d->len - d->cur + 1);
    memcpy(d->text + d->cur, s, n);
    d->cur += n;
    d->len += n;
    d->dirty = true;
    return true;
}

static void keep_visible(struct doc *d)
{
    unsigned line = line_of(d, d->cur), col = (unsigned)(d->cur - line_start(d, d->cur));
    if (line < d->top)
        d->top = line;
    if (line >= d->top + ROWS)
        d->top = line - ROWS + 1;
    if (col < d->left)
        d->left = col;
    if (col >= d->left + COLS)
        d->left = col - COLS + 1;
}

/* The next match of the find text after the cursor, wrapping round. */
static void find_next(struct code *c)
{
    struct doc *d = doc_of(c);
    size_t n = strlen(c->find);
    if (n == 0 || n > d->len)
        return;
    for (size_t k = 1; k <= d->len; k++) {
        size_t i = (d->cur + k) % d->len;
        if (i + n <= d->len && strncasecmp(d->text + i, c->find, n) == 0) {
            d->cur = i;
            ksnprintf(c->msg, sizeof(c->msg), "Found on line %u", line_of(d, i) + 1);
            return;
        }
    }
    ksnprintf(c->msg, sizeof(c->msg), "'%s' is not in this file", c->find);
}

/* A key while the find bar is open. */
static void find_key(struct code *c, uint16_t key)
{
    size_t n = strlen(c->find);
    if (key == 0x1B)
        c->finding = false;
    else if (key == '\n')
        find_next(c);
    else if (key == '\b' && n > 0)
        c->find[n - 1] = '\0';
    else if (key >= 0x20 && key < 0x7F && n < FIND_MAX) {
        c->find[n] = (char)key;
        c->find[n + 1] = '\0';
    }
}

static void code_key(struct window *w, uint16_t key)
{
    struct code *c = w->st;
    struct doc *d = doc_of(c);
    bool was = d->dirty;
    size_t ls = line_start(d, d->cur), col = d->cur - ls;
    c->msg[0] = '\0';
    if (key != 17)
        c->close_asked = false;
    if (key == 6) {                                     /* Ctrl+F */
        c->finding = true;
    } else if (c->finding && key < 0x100) {
        find_key(c, key);
    } else if (key == 19) {                             /* Ctrl+S */
        save(w);
    } else if (key == 18) {                             /* Ctrl+R */
        run(w);
    } else if (key == 20) {                             /* Ctrl+T: next tab */
        c->at = (c->at + 1) % c->ntabs;
        set_title(w);
        return;
    } else if (key == 17) {                             /* Ctrl+Q: close tab */
        close_tab(w, c->at);
        wm_redraw(w);
        return;
    } else if (key == '\t') {
        insert(d, "    ", 4);
    } else if (key == '\n') {
        char ind[65] = "\n";
        size_t n = 1;
        for (size_t i = ls; i < d->len && d->text[i] == ' ' && n < 64; i++)
            ind[n++] = ' ';
        insert(d, ind, n);
    } else if (key >= 0x20 && key < 0x7F) {
        char ch = (char)key;
        if (!insert(d, &ch, 1))
            strlcpy(c->msg, "The file is full (64 KiB)", sizeof(c->msg));
    } else if (key == '\b' && d->cur > 0) {
        d->cur--;
        memmove(d->text + d->cur, d->text + d->cur + 1, d->len - d->cur);
        d->len--;
        d->dirty = true;
    } else if (key == INPUT_KEY_DELETE && d->cur < d->len) {
        memmove(d->text + d->cur, d->text + d->cur + 1, d->len - d->cur);
        d->len--;
        d->dirty = true;
    } else if (key == INPUT_KEY_LEFT && d->cur > 0) {
        d->cur--;
    } else if (key == INPUT_KEY_RIGHT && d->cur < d->len) {
        d->cur++;
    } else if (key == INPUT_KEY_UP && ls > 0) {
        size_t ps = line_start(d, ls - 1);
        d->cur = ps + col < ls - 1 ? ps + col : ls - 1;
    } else if (key == INPUT_KEY_DOWN) {
        size_t le = line_end(d, d->cur);
        if (le < d->len) {
            size_t ns = le + 1, ne = line_end(d, ns);
            d->cur = ns + col < ne ? ns + col : ne;
        }
    } else if (key == INPUT_KEY_HOME) {
        d->cur = ls;
    } else if (key == INPUT_KEY_END) {
        d->cur = line_end(d, d->cur);
    } else if (key == INPUT_KEY_PAGE_DOWN || key == INPUT_KEY_PAGE_UP) {
        unsigned line = line_of(d, d->cur);
        line = key == INPUT_KEY_PAGE_UP ? (line > ROWS ? line - ROWS : 0) : line + ROWS;
        d->cur = start_of_line(d, line);
    }
    keep_visible(d);
    if (was != d->dirty)
        set_title(w);
    wm_redraw(w);
}

static void code_click(struct window *w, int32_t x, int32_t y)
{
    struct code *c = w->st;
    struct doc *d = doc_of(c);
    struct gfx_rect b = wm_body(w);
    if (x < SIDE_W) {
        if (y < 52 || y >= 52 + 22 * (int32_t)(c->nents + 1))
            return;
        int row = (y - 52) / 22;
        char p[FS_PATH_MAX];
        const char *target = row == 0 ? ".." : c->ents[row - 1].name;
        if (fs_path_resolve(c->dir, target, p, sizeof(p)) != FS_OK)
            return;
        struct fs_stat s;
        if (fs_stat(p, &s) == FS_OK && s.type == FS_DIR) {
            strlcpy(c->dir, p, sizeof(c->dir));
            load_dir(c);
        } else {
            code_open_tab(w, p);
        }
        set_title(w);
    } else if (y < TOP) {
        int hit = ui_buttons(gfx_rect(SIDE_W, 0, b.w - SIDE_W, UI_BAR_H), code_buttons, 2, 1, false, x, y);
        if (hit == 0)
            save(w);
        else if (hit == 1)
            run(w);
        for (unsigned i = 0; i < c->ntabs; i++)
            if (gfx_inside(tab_rect(SIDE_W, 0, i), x, y)) {
                c->at = i;
                set_title(w);
            }
    } else if (y < TOP + ROWS * FONT_MONO_H) {
        unsigned line = d->top + (unsigned)(y - TOP) / FONT_MONO_H;
        int32_t cx = x - SIDE_W - 8 - GUTTER;
        unsigned col = d->left + (cx > 0 ? (unsigned)cx / FONT_MONO_W : 0);
        size_t s = start_of_line(d, line), e = line_end(d, s);
        d->cur = s + col < e ? s + col : e;
    }
    wm_redraw(w);
}

/* Self-tests: the text, and the Run output. */
const char *code_text(struct window *w, bool *unsaved, struct textbuf **out)
{
    if (w == NULL || w->app != &app_code)
        return NULL;
    struct code *c = w->st;
    if (unsaved != NULL)
        *unsaved = doc_of(c)->dirty;
    if (out != NULL)
        *out = &c->out;
    return doc_of(c)->text;
}

unsigned code_tabs(struct window *w, unsigned *active, size_t *cursor, const char **msg)
{
    if (w == NULL || w->app != &app_code)
        return 0;
    struct code *c = w->st;
    if (active != NULL)
        *active = c->at;
    if (cursor != NULL)
        *cursor = doc_of(c)->cur;
    if (msg != NULL)
        *msg = c->msg;
    return c->ntabs;
}

const struct app app_code = {
    .name = "Code",
    .w = SIDE_W + 8 + GUTTER + COLS * FONT_MONO_W + 12,
    .h = OUT_Y + 16 + OUT_ROWS * FONT_MONO_H + 30,
    .dark = true,
    .tile = 0x1F6FB2,
    .icon = icon_code,
    .open = code_open,
    .close = code_close,
    .draw = code_draw,
    .key = code_key,
    .click = code_click,
};
