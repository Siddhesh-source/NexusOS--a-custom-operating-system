/* Browser: opens web pages over HTTP (net.c) and local HTML files.
 *
 * Addresses it understands:
 *   http://host[:port]/path   fetched with the network stack
 *   /www/home.html            a file in BharatOS (also file:///www/...)
 *   about:home                the start page (/www/home.html)
 *   about:bookmarks           your bookmarks (Ctrl+D adds the page shown;
 *                             kept in /home/<you>/bookmarks.txt)
 *   about:history             the pages visited, newest first
 * https:// pages say plainly that secure sites need TLS, which is a later
 * step.
 *
 * The page is turned into lines of words: tags only change the style
 * (headings bigger, links teal and underlined, <pre> in the fixed font) or
 * start a new line (<p>, <br>, <li>, <h1>...). <script> and <style> are
 * skipped, &amp; and friends are decoded. Words wrap at the window edge.
 * Links can be clicked; Back returns to the previous page.
 *
 * Keys: Ctrl+L (or click the address) to type an address, Enter to go,
 * arrows / Page Up / Page Down / Space to scroll, Backspace for Back.
 *
 * Limits: no images, CSS, JavaScript or forms; one page at a time;
 * BOOKMARK_MAX bookmarks; History keeps BROWSER_HISTORY_MAX pages and is
 * forgotten at restart;
 * fetching runs on the desktop thread, so the desktop waits while a page
 * loads (a few seconds at most, then it gives up). */

#include "apps.h"
#include "theme.h"
#include "fs.h"
#include "net.h"
#include "input.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define BR_W        860
#define BR_H        600
#define PAGE_Y      (UI_BAR_H + 6)
#define TEXT_MAX    (128 * 1024)
#define SPAN_MAX    6000
#define LINK_MAX    128
#define HIST_MAX    8
#define URL_MAX     256

struct span {
    int32_t x, y;                       /* y from the top of the page */
    uint32_t off;
    uint16_t len;
    uint8_t font;
    int16_t link;                       /* -1: not a link */
    uint32_t rgb;
};

struct browser {
    char url[URL_MAX];
    char edit[URL_MAX];
    bool editing;
    char title[64];
    char msg[96];
    char *raw;                          /* the page as fetched */
    char *text;                         /* the words, back to back */
    size_t text_len;
    struct span *spans;
    unsigned nspans;
    char links[LINK_MAX][URL_MAX];
    unsigned nlinks;
    int32_t page_h, scroll;
    char hist[HIST_MAX][URL_MAX];
    unsigned nhist;
};

static const char *const br_buttons[] = { "Back", "Home", "Bookmarks", "History", "Go" };
#define BR_BUTTONS 5

/* Pages visited, newest first, shared by every Browser window. */
static struct {
    char url[URL_MAX];
    char title[64];
} visited[BROWSER_HISTORY_MAX];
static unsigned nvisited;

static void remember_visit(const char *url, const char *title)
{
    if (strncmp(url, "about:", 6) == 0 || (nvisited > 0 && strcmp(visited[0].url, url) == 0))
        return;
    unsigned n = nvisited < BROWSER_HISTORY_MAX ? nvisited : BROWSER_HISTORY_MAX - 1;
    memmove(&visited[1], &visited[0], n * sizeof(visited[0]));
    strlcpy(visited[0].url, url, URL_MAX);
    strlcpy(visited[0].title, title, sizeof(visited[0].title));
    nvisited = n + 1;
}

/* Append `s` to a generated page, with <, > and & written as entities so
 * a page title can never add tags of its own. */
static size_t put_text(char *out, size_t at, size_t cap, const char *s, bool escape)
{
    for (; *s != '\0' && at + 6 < cap; s++) {
        const char *e = !escape ? NULL : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '&' ? "&amp;"
                        : *s == '"' ? "&quot;" : NULL;
        if (e != NULL)
            while (*e != '\0')
                out[at++] = *e++;
        else
            out[at++] = *s;
    }
    out[at] = '\0';
    return at;
}

static size_t put_link(char *out, size_t at, size_t cap, const char *url, const char *title)
{
    at = put_text(out, at, cap, "<li><a href=\"", false);
    at = put_text(out, at, cap, url, true);
    at = put_text(out, at, cap, "\">", false);
    at = put_text(out, at, cap, title[0] != '\0' ? title : url, true);
    at = put_text(out, at, cap, "</a> ", false);
    at = put_text(out, at, cap, url, true);
    return put_text(out, at, cap, "</li>", false);
}

static bool bookmarks_path(char *path)
{
    if (desktop_user()[0] == '\0')
        return false;
    ksnprintf(path, FS_PATH_MAX, "/home/%s/bookmarks.txt", desktop_user());
    return true;
}

/* The bookmarks file: one "address<Tab>title" line per bookmark. Returns
 * its text (kfree it) and its number of lines, or NULL if there is none. */
static char *read_bookmarks(unsigned *lines)
{
    char path[FS_PATH_MAX];
    size_t len = 0;
    char *buf = kmalloc(BOOKMARK_MAX * (URL_MAX + 70) + 1);
    *lines = 0;
    if (buf == NULL || !bookmarks_path(path)
        || fs_read_file(path, buf, BOOKMARK_MAX * (URL_MAX + 70), &len) != FS_OK) {
        kfree(buf);
        return NULL;
    }
    buf[len] = '\0';
    for (size_t i = 0; i < len; i++)
        *lines += buf[i] == '\n';
    return buf;
}

/* True if a line of `text` starts with `start`. */
static bool line_in(const char *text, const char *start)
{
    size_t n = strlen(start);
    for (const char *l = strchr(text, '\n'); l != NULL; l = strchr(l + 1, '\n'))
        if (strncmp(l + 1, start, n) == 0)
            return true;
    return false;
}

/* Ctrl+D: add the page shown to the bookmarks. */
static void add_bookmark(struct window *w)
{
    struct browser *b = w->st;
    char path[FS_PATH_MAX], line[URL_MAX + 72];
    unsigned n = 0;
    char *have = read_bookmarks(&n);
    ksnprintf(line, sizeof(line), "%s\t", b->url);
    if (!bookmarks_path(path) || strncmp(b->url, "about:", 6) == 0)
        strlcpy(b->msg, "This page cannot be bookmarked", sizeof(b->msg));
    else if (have != NULL && (strncmp(have, line, strlen(line)) == 0 || line_in(have, line)))
        strlcpy(b->msg, "Already in your bookmarks", sizeof(b->msg));
    else if (n >= BOOKMARK_MAX)
        ksnprintf(b->msg, sizeof(b->msg), "Bookmarks are full (%u); remove some from bookmarks.txt", BOOKMARK_MAX);
    else {
        char title[64];
        strlcpy(title, b->title, sizeof(title));
        for (char *t = title; *t != '\0'; t++)
            if (*t == '\t' || *t == '\n')
                *t = ' ';
        ksnprintf(line, sizeof(line), "%s\t%s\n", b->url, title);
        fs_status_t st = fs_write_file(path, line, strlen(line), true);
        ksnprintf(b->msg, sizeof(b->msg), st == FS_OK ? "Bookmarked (see Bookmarks)" : "Not bookmarked: %s",
                  fs_status_str(st));
    }
    kfree(have);
    wm_redraw(w);
}

/* about:bookmarks and about:history, written as HTML into b->raw. */
static size_t make_list_page(struct browser *b, bool bookmarks)
{
    size_t at = put_text(b->raw, 0, NET_PAGE_MAX, bookmarks ? "<title>Bookmarks</title><h1>Bookmarks</h1><ul>"
                                                            : "<title>History</title><h1>History</h1><ul>", false);
    unsigned count = 0;
    if (bookmarks) {
        char *list = read_bookmarks(&count);
        for (char *l = list; l != NULL && *l != '\0';) {
            char *nl = strchr(l, '\n'), *tab = strchr(l, '\t');
            if (nl == NULL)
                break;
            *nl = '\0';
            if (tab != NULL && tab < nl)
                *tab = '\0';
            at = put_link(b->raw, at, NET_PAGE_MAX, l, tab != NULL && tab < nl ? tab + 1 : "");
            l = nl + 1;
        }
        kfree(list);
    } else {
        for (unsigned i = 0; i < nvisited; i++)
            at = put_link(b->raw, at, NET_PAGE_MAX, visited[i].url, visited[i].title);
        count = nvisited;
    }
    at = put_text(b->raw, at, NET_PAGE_MAX, "</ul>", false);
    if (count == 0)
        at = put_text(b->raw, at, NET_PAGE_MAX, bookmarks ? "<p>No bookmarks yet. Press Ctrl+D on a page to add it.</p>"
                                                          : "<p>No pages visited yet.</p>", false);
    return at;
}

/* ---- layout ----------------------------------------------------------------- */

struct layout {
    struct browser *b;
    int32_t width, x, y, line_h;
    enum font_id font;
    uint32_t rgb;
    int link;
    bool pre, skip, in_title, space;
    unsigned list_depth;
};

static void newline(struct layout *l, int32_t extra)
{
    if (l->x > 0 || extra > 0) {
        l->y += (l->x > 0 ? l->line_h : 0) + extra;
        l->x = 0;
    }
    l->line_h = gfx_line_height(l->font) + 4;
    l->space = false;
}

static void emit(struct layout *l, const char *w, size_t n)
{
    struct browser *b = l->b;
    if (n == 0 || b->nspans >= SPAN_MAX || b->text_len + n > TEXT_MAX)
        return;
    char tmp[160];
    size_t tn = n < sizeof(tmp) - 1 ? n : sizeof(tmp) - 1;
    memcpy(tmp, w, tn);
    tmp[tn] = '\0';
    int32_t ww = gfx_text_width(tmp, l->font), sp = l->space && l->x > 0 ? gfx_text_width(" ", l->font) : 0;
    if (l->x > 0 && l->x + sp + ww > l->width) {
        newline(l, 0);
        sp = 0;
    }
    int32_t lh = gfx_line_height(l->font) + 4;
    if (lh > l->line_h)
        l->line_h = lh;
    struct span *s = &b->spans[b->nspans++];
    s->x = l->x + sp + (int32_t)l->list_depth * 18;
    s->y = l->y;
    s->off = (uint32_t)b->text_len;
    s->len = (uint16_t)tn;
    s->font = (uint8_t)l->font;
    s->link = (int16_t)l->link;
    s->rgb = l->rgb;
    memcpy(b->text + b->text_len, tmp, tn);
    b->text_len += tn;
    l->x += sp + ww;
    l->space = false;
}

static char entity(const char *p, size_t n, size_t *used)
{
    static const struct { const char *name; char ch; } ents[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' },
        { "&#39;", '\'' }, { "&apos;", '\'' }, { "&nbsp;", ' ' }, { "&copy;", 'c' },
    };
    for (unsigned i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        size_t l = strlen(ents[i].name);
        if (l <= n && strncasecmp(p, ents[i].name, l) == 0) {
            *used = l;
            return ents[i].ch;
        }
    }
    *used = 1;
    return '&';
}

static bool tag_is(const char *t, const char *name)
{
    size_t n = strlen(name);
    return strncasecmp(t, name, n) == 0 && (t[n] == '\0' || t[n] == ' ' || t[n] == '>' || t[n] == '/');
}

/* Read attribute `name` from a tag's text. */
static bool attr(const char *t, const char *name, char *out, size_t cap)
{
    size_t n = strlen(name);
    for (const char *p = t; *p != '\0'; p++) {
        if ((p == t || p[-1] == ' ') && strncasecmp(p, name, n) == 0 && p[n] == '=') {
            p += n + 1;
            char q = (*p == '"' || *p == '\'') ? *p++ : ' ';
            size_t k = 0;
            while (*p != '\0' && *p != q && *p != '>' && k + 1 < cap)
                out[k++] = *p++;
            out[k] = '\0';
            return true;
        }
    }
    return false;
}

static void set_font(struct layout *l, enum font_id f, uint32_t rgb)
{
    l->font = f;
    l->rgb = rgb;
}

static void handle_tag(struct layout *l, const char *t)
{
    bool close = t[0] == '/';
    const char *n = close ? t + 1 : t;
    struct browser *b = l->b;
    if (tag_is(n, "script") || tag_is(n, "style")) {
        l->skip = !close;
    } else if (tag_is(n, "title")) {
        l->in_title = !close;
    } else if (tag_is(n, "h1") || tag_is(n, "h2") || tag_is(n, "h3") || tag_is(n, "h4")) {
        newline(l, close ? 6 : 10);
        if (close)
            set_font(l, FONT_UI, COL_TEXT);
        else
            set_font(l, n[1] == '1' ? FONT_HEADING : FONT_MEDIUM, COL_TEXT);
        newline(l, 0);
    } else if (tag_is(n, "p") || tag_is(n, "div") || tag_is(n, "table") || tag_is(n, "blockquote")
               || tag_is(n, "section") || tag_is(n, "article") || tag_is(n, "header") || tag_is(n, "footer")) {
        newline(l, tag_is(n, "p") ? 8 : 2);
    } else if (tag_is(n, "br") || tag_is(n, "tr")) {
        if (l->x == 0)
            l->y += l->line_h;
        newline(l, 0);
    } else if (tag_is(n, "ul") || tag_is(n, "ol")) {
        newline(l, 4);
        if (close && l->list_depth > 0)
            l->list_depth--;
        else if (!close && l->list_depth < 6)
            l->list_depth++;
    } else if (tag_is(n, "li") && !close) {
        newline(l, 2);
        uint32_t keep = l->rgb;
        l->rgb = COL_MUTED;
        emit(l, "-", 1);
        l->rgb = keep;
        l->space = true;
    } else if (tag_is(n, "pre")) {
        newline(l, 6);
        l->pre = !close;
        set_font(l, close ? FONT_UI : FONT_MONO, COL_TEXT);
    } else if (tag_is(n, "hr")) {
        newline(l, 12);
    } else if (tag_is(n, "a")) {
        char href[URL_MAX];
        if (close) {
            l->link = -1;
            l->rgb = COL_TEXT;
        } else if (attr(t, "href", href, sizeof(href)) && b->nlinks < LINK_MAX) {
            strlcpy(b->links[b->nlinks], href, URL_MAX);
            l->link = (int)b->nlinks++;
            l->rgb = COL_TEAL;
        }
    } else if (tag_is(n, "td") || tag_is(n, "th")) {
        l->space = true;
    }
}

static void layout_page(struct browser *b, const char *raw, size_t n, bool html, int32_t width)
{
    b->nspans = 0;
    b->text_len = 0;
    b->nlinks = 0;
    b->title[0] = '\0';
    struct layout l = { .b = b, .width = width, .font = html ? FONT_UI : FONT_MONO, .rgb = COL_TEXT,
                        .link = -1, .pre = !html };
    l.line_h = gfx_line_height(l.font) + 4;
    char word[160];
    size_t wn = 0;
    size_t tlen = 0;
    for (size_t i = 0; i < n; i++) {
        char ch = raw[i];
        if (html && ch == '<') {
            emit(&l, word, wn);
            wn = 0;
            if (strncmp(raw + i, "<!--", 4) == 0) {
                const char *e = strstr(raw + i + 4, "-->");
                i = e != NULL ? (size_t)(e - raw) + 2 : n;
                continue;
            }
            char tag[200];
            size_t k = 0;
            i++;
            while (i < n && raw[i] != '>' && k + 1 < sizeof(tag)) {
                char c = raw[i++];
                tag[k++] = c == '\n' || c == '\t' || c == '\r' ? ' ' : c;
            }
            while (i < n && raw[i] != '>')
                i++;
            tag[k] = '\0';
            if (k > 0 && tag[k - 1] == '/')
                tag[k - 1] = '\0';
            handle_tag(&l, tag);
            continue;
        }
        if (l.skip)
            continue;
        if (html && ch == '&') {
            size_t used;
            ch = entity(raw + i, n - i, &used);
            i += used - 1;
        }
        if (l.in_title) {
            if (tlen + 1 < sizeof(b->title) && !(tlen == 0 && (ch == ' ' || ch == '\n')))
                b->title[tlen++] = ch == '\n' ? ' ' : ch;
            b->title[tlen] = '\0';
            continue;
        }
        if (ch == '\r')
            continue;
        if (l.pre && ch == '\n') {
            emit(&l, word, wn);
            wn = 0;
            if (l.x == 0)
                l.y += l.line_h;
            newline(&l, 0);
            continue;
        }
        if (ch == ' ' || ch == '\n' || ch == '\t') {
            emit(&l, word, wn);
            wn = 0;
            l.space = true;
            continue;
        }
        if ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x7F)
            ch = (unsigned char)ch >= 0xC0 ? '?' : (unsigned char)ch >= 0x80 ? '\0' : ' ';
        if (ch == '\0')
            continue;
        if (wn + 1 < sizeof(word))
            word[wn++] = ch;
    }
    emit(&l, word, wn);
    newline(&l, 0);
    b->page_h = l.y + l.line_h;
}

/* ---- loading ----------------------------------------------------------------- */

/* `href` taken relative to the current page. */
static void resolve(const struct browser *b, const char *href, char *out)
{
    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0
        || strncmp(href, "about:", 6) == 0 || strncmp(href, "file://", 7) == 0) {
        strlcpy(out, href, URL_MAX);
        return;
    }
    if (strncmp(b->url, "http://", 7) == 0) {
        const char *slash = strchr(b->url + 7, '/');
        size_t origin = slash != NULL ? (size_t)(slash - b->url) : strlen(b->url);
        size_t keep = origin;
        if (href[0] != '/') {
            const char *last = strrchr(b->url + origin, '/');
            keep = last != NULL ? (size_t)(last - b->url) + 1 : origin;
        }
        if (keep >= URL_MAX - 2)
            keep = URL_MAX - 2;
        memcpy(out, b->url, keep);
        out[keep] = '\0';
        if (href[0] != '/' && out[keep - 1] != '/')
            strlcpy(out + keep++, "/", 2);
        strlcpy(out + keep, href, URL_MAX - keep);
        return;
    }
    /* A file: relative to its folder. */
    char dir[FS_PATH_MAX];
    const char *path = strncmp(b->url, "file://", 7) == 0 ? b->url + 7 : b->url;
    strlcpy(dir, path, sizeof(dir));
    char *s = strrchr(dir, '/');
    if (s != NULL)
        s[1] = '\0';
    if (fs_path_resolve(dir[0] == '/' ? dir : "/", href, out, URL_MAX) != FS_OK)
        strlcpy(out, href, URL_MAX);
}

static void load(struct window *w, const char *where, bool remember)
{
    struct browser *b = w->st;
    char url[URL_MAX];
    strlcpy(url, strcmp(where, "about:home") == 0 ? "/www/home.html" : where, sizeof(url));
    if (strncmp(url, "file://", 7) == 0)
        memmove(url, url + 7, strlen(url + 7) + 1);
    if (remember && b->url[0] != '\0' && b->nhist < HIST_MAX)
        strlcpy(b->hist[b->nhist++], b->url, URL_MAX);
    else if (remember && b->url[0] != '\0') {
        memmove(b->hist[0], b->hist[1], (HIST_MAX - 1) * URL_MAX);
        strlcpy(b->hist[HIST_MAX - 1], b->url, URL_MAX);
    }
    size_t len = 0;
    bool html = true, good = true;          /* good: worth keeping in History */
    b->msg[0] = '\0';
    if (strcmp(url, "about:bookmarks") == 0 || strcmp(url, "about:history") == 0) {
        len = make_list_page(b, url[6] == 'b');
    } else if (url[0] == '/') {
        fs_status_t st = fs_read_file(url, b->raw, NET_PAGE_MAX, &len);
        if (st != FS_OK) {
            good = false;
            ksnprintf(b->msg, sizeof(b->msg), "Cannot open %s: %s", url, fs_status_str(st));
            len = 0;
        }
        const char *dot = strrchr(url, '.');
        html = dot != NULL && (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0);
    } else {
        if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
            char full[URL_MAX];
            ksnprintf(full, sizeof(full), "http://%s", url);
            strlcpy(url, full, sizeof(url));
        }
        int status = 0;
        char final_url[256];
        net_status_t st = http_get(url, b->raw, NET_PAGE_MAX, &len, &status, final_url);
        strlcpy(url, final_url, sizeof(url));
        good = st == NET_OK || len > 0;
        if (st == NET_ERR_UNSUPPORTED) {
            good = false;
            len = (size_t)ksnprintf(b->raw, NET_PAGE_MAX,
                "<h1>Secure page</h1><p>%s is a secure (https) address. Secure pages need "
                "encryption (TLS), which BharatOS does not have yet.</p><p>Try an http:// "
                "address, or <a href=\"about:home\">the start page</a>.</p>", url);
        } else if (st != NET_OK && len == 0) {
            good = false;
            len = (size_t)ksnprintf(b->raw, NET_PAGE_MAX,
                "<h1>Could not open the page</h1><p>%s: %s.</p><p>Is the network on? QEMU's "
                "built-in network reaches the internet through your PC. Pages on your own PC are "
                "at http://10.0.2.2:PORT/</p>", url, net_status_str(st));
        } else {
            ksnprintf(b->msg, sizeof(b->msg), "HTTP %d, %lu bytes", status, len);
            html = strstr(b->raw, "<") != NULL;
        }
    }
    b->raw[len] = '\0';
    strlcpy(b->url, url, sizeof(b->url));
    strlcpy(b->edit, url, sizeof(b->edit));
    b->editing = false;
    b->scroll = 0;
    layout_page(b, b->raw, len, html, BR_W - 2 * UI_PAD - 20);
    if (good)
        remember_visit(url, b->title);
    char t[48];
    ksnprintf(t, sizeof(t), "Browser: %s", b->title[0] ? b->title : url);
    wm_set_title(w, t);
    wm_redraw(w);
}

static bool br_open(struct window *w, const char *arg)
{
    struct browser *b = kzalloc(sizeof(*b));
    if (b == NULL || (b->raw = kmalloc(NET_PAGE_MAX + 1)) == NULL || (b->text = kmalloc(TEXT_MAX)) == NULL
        || (b->spans = kmalloc(SPAN_MAX * sizeof(struct span))) == NULL) {
        if (b != NULL) {
            kfree(b->raw);
            kfree(b->text);
        }
        kfree(b);
        return false;
    }
    w->st = b;
    load(w, arg != NULL ? arg : "about:home", false);
    return true;
}

static void br_close(struct window *w)
{
    struct browser *b = w->st;
    kfree(b->raw);
    kfree(b->text);
    kfree(b->spans);
}

/* ---- drawing and input -------------------------------------------------------- */

static struct gfx_rect url_field(struct gfx_rect body)
{
    int32_t x = body.x + UI_PAD + BR_BUTTONS * 8;
    for (unsigned i = 0; i < BR_BUTTONS; i++)
        x += gfx_text_width(br_buttons[i], FONT_MEDIUM) + 28;
    return gfx_rect(x, body.y + 12, body.x + body.w - UI_PAD - x, 32);
}

static int32_t view_h(struct gfx_rect body)
{
    return body.h - PAGE_Y - 30;
}

static void br_draw(struct window *w, struct gfx_rect body)
{
    struct browser *b = w->st;
    ui_buttons(body, br_buttons, BR_BUTTONS, 4, true, -1, -1);
    struct gfx_rect f = url_field(body);
    gfx_round(f, UI_RADIUS_SM, b->editing ? COL_PRIMARY : COL_LINE, 255);
    gfx_round(gfx_grow(f, -2), UI_RADIUS_SM - 2, COL_SURFACE, 255);
    const char *shown = b->editing ? b->edit : b->url;
    size_t sl = strlen(shown), fit = (size_t)(f.w - 24) / 8;
    if (sl > fit)
        shown += sl - fit;
    int32_t tw = gfx_text(f.x + 10, f.y + 7, shown, b->editing ? COL_TEXT : COL_MUTED, FONT_UI);
    if (b->editing)
        gfx_fill(gfx_rect(f.x + 11 + tw, f.y + 8, 2, 16), COL_PRIMARY);

    struct gfx_rect view = gfx_rect(body.x, body.y + PAGE_Y, body.w, view_h(body));
    gfx_fill(gfx_rect(view.x, view.y - 1, view.w, 1), COL_LINE);
    struct gfx_rect old = gfx_get_clip();
    gfx_clip(gfx_intersect(old, view));
    int32_t x0 = view.x + UI_PAD + 6, y0 = view.y + 10 - b->scroll;
    for (unsigned i = 0; i < b->nspans; i++) {
        const struct span *s = &b->spans[i];
        int32_t y = y0 + s->y;
        if (y + 30 < view.y || y > view.y + view.h)
            continue;
        int32_t sw = gfx_text_n(x0 + s->x, y, b->text + s->off, s->len, s->rgb, (enum font_id)s->font);
        if (s->link >= 0)
            gfx_fill(gfx_rect(x0 + s->x, y + gfx_line_height((enum font_id)s->font) - 2, sw, 1), COL_TEAL);
    }
    gfx_clip(old);
    /* Scroll bar and status line */
    if (b->page_h > view.h) {
        int32_t th = view.h * view.h / b->page_h, ty = view.y + (view.h - th) * b->scroll / (b->page_h - view.h);
        gfx_round(gfx_rect(view.x + view.w - 8, ty, 4, th < 20 ? 20 : th), 2, COL_LINE, 255);
    }
    int32_t sy = body.y + body.h - 26;
    gfx_fill(gfx_rect(body.x, sy - 4, body.w, 1), COL_LINE);
    char st[140];
    ksnprintf(st, sizeof(st), "%s%s%u links     Ctrl+D bookmarks this page", b->msg, b->msg[0] ? "     " : "",
              b->nlinks);
    gfx_text(body.x + UI_PAD, sy + 2, st, COL_MUTED, FONT_SMALL);
}

static void scroll_by(struct window *w, int32_t dy)
{
    struct browser *b = w->st;
    int32_t max = b->page_h - view_h(wm_body(w)) + 20;
    b->scroll += dy;
    if (b->scroll > max)
        b->scroll = max;
    if (b->scroll < 0)
        b->scroll = 0;
}

static void back(struct window *w)
{
    struct browser *b = w->st;
    if (b->nhist == 0)
        return;
    char u[URL_MAX];
    strlcpy(u, b->hist[--b->nhist], sizeof(u));
    load(w, u, false);
}

static void br_key(struct window *w, uint16_t key)
{
    struct browser *b = w->st;
    if (b->editing) {
        size_t n = strlen(b->edit);
        if (key == '\n') {
            char u[URL_MAX];
            strlcpy(u, b->edit, sizeof(u));
            load(w, u, true);
            return;
        }
        if (key == 0x1B)
            b->editing = false;
        else if (key == '\b' && n > 0)
            b->edit[n - 1] = '\0';
        else if (key > 0x20 && key < 0x7F && n + 1 < URL_MAX) {
            b->edit[n] = (char)key;
            b->edit[n + 1] = '\0';
        }
    } else if (key == 4) {                      /* Ctrl+D */
        add_bookmark(w);
        return;
    } else if (key == 12) {                     /* Ctrl+L */
        b->editing = true;
        b->edit[0] = '\0';
    } else if (key == INPUT_KEY_DOWN) {
        scroll_by(w, 40);
    } else if (key == INPUT_KEY_UP) {
        scroll_by(w, -40);
    } else if (key == INPUT_KEY_PAGE_DOWN || key == ' ') {
        scroll_by(w, view_h(wm_body(w)) - 40);
    } else if (key == INPUT_KEY_PAGE_UP) {
        scroll_by(w, -(view_h(wm_body(w)) - 40));
    } else if (key == INPUT_KEY_HOME) {
        b->scroll = 0;
    } else if (key == '\b') {
        back(w);
        return;
    }
    wm_redraw(w);
}

static void br_click(struct window *w, int32_t x, int32_t y)
{
    struct browser *b = w->st;
    struct gfx_rect body = gfx_rect(0, 0, wm_body(w).w, wm_body(w).h);
    int hit = ui_buttons(body, br_buttons, BR_BUTTONS, 4, false, x, y);
    if (hit == 0) {
        back(w);
        return;
    }
    if (hit == 1) {
        load(w, "about:home", true);
        return;
    }
    if (hit == 2 || hit == 3) {
        load(w, hit == 2 ? "about:bookmarks" : "about:history", true);
        return;
    }
    if (hit == 4) {
        char u[URL_MAX];
        strlcpy(u, b->editing ? b->edit : b->url, sizeof(u));
        load(w, u, true);
        return;
    }
    if (gfx_inside(url_field(body), x, y)) {
        b->editing = true;
        strlcpy(b->edit, b->url, sizeof(b->edit));
        wm_redraw(w);
        return;
    }
    int32_t x0 = UI_PAD + 6, y0 = PAGE_Y + 10 - b->scroll;
    for (unsigned i = 0; i < b->nspans; i++) {
        const struct span *s = &b->spans[i];
        if (s->link < 0)
            continue;
        int32_t sw = gfx_text_n(-10000, -10000, b->text + s->off, s->len, 0, (enum font_id)s->font);
        if (gfx_inside(gfx_rect(x0 + s->x, y0 + s->y, sw, gfx_line_height((enum font_id)s->font)), x, y)) {
            char u[URL_MAX];
            resolve(b, b->links[s->link], u);
            load(w, u, true);
            return;
        }
    }
}

/* ---- self-test helpers ---------------------------------------------------------- */

bool browser_page_has(struct window *w, const char *text)
{
    if (w == NULL || w->app != &app_browser)
        return false;
    struct browser *b = w->st;
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= b->text_len; i++)
        if (memcmp(b->text + i, text, n) == 0)
            return true;
    return strstr(b->title, text) != NULL;
}

const char *browser_url(struct window *w, unsigned *links)
{
    if (w == NULL || w->app != &app_browser)
        return "";
    struct browser *b = w->st;
    if (links != NULL)
        *links = b->nlinks;
    return b->url;
}

/* Follow link number `i` (self-tests). */
bool browser_follow(struct window *w, unsigned i)
{
    if (w == NULL || w->app != &app_browser)
        return false;
    struct browser *b = w->st;
    if (i >= b->nlinks)
        return false;
    char u[URL_MAX];
    resolve(b, b->links[i], u);
    load(w, u, true);
    return true;
}

void browser_go(struct window *w, const char *url)
{
    if (w != NULL && w->app == &app_browser)
        load(w, url, true);
}

const char *browser_message(struct window *w)
{
    return w != NULL && w->app == &app_browser ? ((struct browser *)w->st)->msg : "";
}

void browser_back(struct window *w)
{
    if (w != NULL && w->app == &app_browser)
        back(w);
}

const struct app app_browser = {
    .name = "Browser",
    .w = BR_W,
    .h = BR_H,
    .tile = 0x2A7C76,
    .icon = icon_browser,
    .open = br_open,
    .close = br_close,
    .draw = br_draw,
    .key = br_key,
    .click = br_click,
};
