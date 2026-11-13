/* Phase 11 self-tests: the desktop without a screen. The window manager
 * draws into its back buffer only, and the tests drive it the way the
 * desktop thread does: keys, mouse moves and clicks, ticks. They check
 * what changed in the windows, the files and the pixels. */

#include "desktop_test.h"
#include "selftest.h"
#include "gfx.h"
#include "textbuf.h"
#include "wm.h"
#include "apps.h"
#include "desktop.h"
#include "theme.h"
#include "splash.h"
#include "fs.h"
#include "input.h"
#include "sched.h"
#include "heap.h"
#include "percpu.h"
#include "kprintf.h"
#include "string.h"

#define CHECK(c, d) st_check((c), (d))

static void type(const char *s)
{
    while (*s != '\0')
        desktop_key((uint8_t)*s++);
}

/* A full click: press and release at (x, y). */
static void click(int32_t x, int32_t y)
{
    wm_mouse(x, y, INPUT_BUTTON_LEFT);
    wm_mouse(x, y, 0);
}

static void test_gfx(void)
{
    st_section("Drawing");
    static uint32_t a[64 * 32], b[64 * 32];
    struct gfx_surface sa = { a, 64, 32, 64 }, sb = { b, 64, 32, 64 };
    gfx_target(&sa);
    gfx_fill(gfx_rect(0, 0, 64, 32), 0x000000);
    gfx_fill(gfx_rect(10, 10, 4, 4), 0x123456);
    CHECK(gfx_get(&sa, 10, 10) == 0x123456 && gfx_get(&sa, 13, 13) == 0x123456
          && gfx_get(&sa, 14, 10) == 0 && gfx_get(&sa, 9, 10) == 0,
          "a filled rectangle covers exactly its pixels");
    gfx_clip(gfx_rect(0, 0, 20, 20));
    gfx_fill(gfx_rect(15, 15, 30, 30), 0xFFFFFF);
    CHECK(gfx_get(&sa, 19, 19) == 0xFFFFFF && gfx_get(&sa, 20, 19) == 0 && gfx_get(&sa, 19, 20) == 0,
          "drawing stays inside the clip rectangle");
    gfx_fill(gfx_rect(-100, -100, 50, 50), 0xFFFFFF);
    CHECK(gfx_get(&sa, 0, 0) == 0, "a rectangle off the surface draws nothing (and does not crash)");
    gfx_target(&sa);
    int32_t w = gfx_text(0, 16, "H", 0xFF0000, 1);
    CHECK(w == 8 && gfx_get(&sa, 0, 18) == 0xFF0000 && gfx_get(&sa, 3, 18) == 0,
          "text: 'H' is 8 pixels wide, its stroke drawn, its middle gap left");
    struct gfx_rect u = gfx_union(gfx_rect(0, 0, 4, 4), gfx_rect(10, 10, 2, 2));
    struct gfx_rect i = gfx_intersect(gfx_rect(0, 0, 10, 10), gfx_rect(5, 5, 10, 10));
    CHECK(u.x == 0 && u.w == 12 && u.h == 12 && i.x == 5 && i.w == 5
          && gfx_empty(gfx_intersect(gfx_rect(0, 0, 2, 2), gfx_rect(5, 5, 2, 2))),
          "rectangle union and intersection");
    memset(b, 0, sizeof(b));
    gfx_copy(&sb, &sa, gfx_rect(8, 8, 8, 8));
    CHECK(gfx_get(&sb, 10, 10) == 0x123456 && gfx_get(&sb, 0, 18) == 0,
          "present copies only the damaged rectangle");
    CHECK(splash_steps_done() >= 10, "the boot splash moved on at every boot step");
}

static void test_textbuf(void)
{
    st_section("Text buffer");
    struct textbuf tb;
    CHECK(textbuf_init(&tb, 10, 3) && !textbuf_init(&(struct textbuf){0}, 0, 3),
          "a 10x3 buffer is made; a 0-wide one is refused");
    textbuf_puts(&tb, "ab\ncd");
    CHECK(memcmp(textbuf_row(&tb, 0), "ab ", 3) == 0 && memcmp(textbuf_row(&tb, 1), "cd", 2) == 0
          && tb.row == 1 && tb.col == 2, "text and newline land in the right cells");
    textbuf_puts(&tb, "\n123456789012");
    CHECK(textbuf_has(&tb, "1234567890") && textbuf_has(&tb, "12") && !textbuf_has(&tb, "ab"),
          "a long line wraps; the oldest line scrolls away");
    textbuf_puts(&tb, "x\bY\x1b[2J\x1b[Hok");
    CHECK(textbuf_has(&tb, "ok") && !textbuf_has(&tb, "1234") && tb.row == 0,
          "escape codes clear the screen and move home");
    textbuf_puts(&tb, "\xe2\x94\x80\xe2\x94\x82\xc3\xa9");
    CHECK(memcmp(textbuf_row(&tb, 0), "ok-|?", 5) == 0, "box-drawing characters become - | and others ?");
    textbuf_free(&tb);
}

static void test_login(void)
{
    st_section("Login screen");
    login_reset();
    type("no way!\n");
    CHECK(!desktop_logged_in() && login_message()[0] != '\0', "a name with spaces is refused with a message");
    desktop_key(0x1B);
    type("admin\n");
    CHECK(!desktop_logged_in(), "a user with a password is asked for it");
    type("wrong\n");
    CHECK(!desktop_logged_in() && strstr(login_message(), "password") != NULL, "a wrong password is refused");
    type("tester\n");
    CHECK(desktop_logged_in() && strcmp(desktop_user(), "tester") == 0, "a plain name logs in");
    struct wm_stats s;
    wm_compose();
    wm_get_stats(&s);
    struct gfx_rect bar = wm_topbar_item(0);
    CHECK(s.frames > 0 && gfx_get(wm_back(), bar.x + 2, bar.y + 2) == COL_BG,
          "the desktop is drawn with the app buttons on the top bar");
}

static void test_windows(void)
{
    st_section("Windows");
    struct window *t = wm_open(&app_terminal, NULL);
    struct window *f = wm_open(&app_files, NULL);
    CHECK(t != NULL && f != NULL && wm_focused() == f, "two windows open; the newer one has the focus");
    wm_move(f, 700, 400);
    wm_focus(t);
    CHECK(wm_focused() == t, "focus moves to the window brought forward");
    /* Click the title bar of the Files window where the terminal does not
     * cover it, then drag it by (40, 30). */
    struct gfx_rect fr = f->r;
    int32_t gx = fr.x + 100, gy = fr.y + 10;
    bool clear = !gfx_inside(t->r, gx, gy);
    wm_mouse(gx, gy, INPUT_BUTTON_LEFT);
    wm_mouse(gx + 40, gy + 30, INPUT_BUTTON_LEFT);
    wm_mouse(gx + 40, gy + 30, 0);
    CHECK(clear && wm_focused() == f && f->r.x == fr.x + 40 && f->r.y == fr.y + 30,
          "clicking a title bar focuses the window; dragging moves it");
    wm_move(f, -5000, -5000);
    CHECK(f->r.y >= UI_TOPBAR_H && f->r.x + f->r.w >= 64, "a window cannot be moved off the screen");
    wm_move(f, fr.x, fr.y);
    struct wm_stats s0, s1;
    wm_compose();
    wm_get_stats(&s0);
    wm_mouse(fr.x + 5, fr.y + 100, 0);         /* only the pointer moves */
    wm_compose();
    wm_get_stats(&s1);
    CHECK(s1.frames == s0.frames + 1 && s1.pixels - s0.pixels < 200 * 100,
          "moving the pointer redraws only a small damaged area");
    CHECK(wm_compose() == false, "nothing changed: nothing is redrawn");

    unsigned opened = 2;
    struct window *extra[WM_MAX_WINDOWS];
    unsigned n = 0;
    while (n < WM_MAX_WINDOWS && (extra[n] = wm_open(&app_about, NULL)) != NULL)
        n++;
    CHECK(opened + n == WM_MAX_WINDOWS, "windows can be opened up to the limit, then are refused");
    for (unsigned i = 0; i < n; i++)
        wm_close(extra[i]);
    struct gfx_rect close = gfx_rect(f->r.x + f->r.w - 10, f->r.y + 10, 1, 1);
    wm_focus(f);
    click(close.x, close.y);
    CHECK(wm_find(&app_files) == NULL && wm_focused() == t, "the x closes a window; focus goes back");
    struct gfx_rect item = wm_topbar_item(1);
    click(item.x + 4, item.y + 4);
    f = wm_find(&app_files);
    click(item.x + 4, item.y + 4);
    CHECK(f != NULL && wm_focused() == f && wm_find(&app_files) == f,
          "the top bar opens an app, and a second click does not open it twice");
    wm_key(INPUT_KEY_F1 + 2);
    CHECK(wm_focused() != NULL && wm_focused()->app == &app_monitor, "F3 opens the Monitor");
    wm_key(23);
    CHECK(wm_find(&app_monitor) == NULL, "Ctrl+W closes the focused window");
}

static void test_terminal(void)
{
    st_section("Terminal window");
    struct window *t = wm_show(&app_terminal);
    struct textbuf *tb = app_text(t);
    type("echo window says hi\n");
    CHECK(tb != NULL && textbuf_has(tb, "window says hi") && textbuf_has(tb, "tester@bharat:~$"),
          "a command typed in the window runs; its output and a new prompt appear");
    type("cd /etc\npwd\n");
    CHECK(textbuf_has(tb, "tester@bharat:/etc$"), "the shell's folder and prompt follow cd");
    type("clear\n");
    CHECK(!textbuf_has(tb, "window says hi"), "clear empties the window");
    type("logout\n");
    CHECK(wm_find(&app_terminal) == NULL, "logout in a terminal closes it");
}

static void test_files_editor(void)
{
    st_section("Files and the text editor");
    struct window *f = wm_show(&app_files);
    CHECK(f != NULL && strcmp(files_folder(f), "/home/tester") == 0, "Files opens in the user's home folder");
    type("nnew.txt\n");
    struct fs_stat s;
    CHECK(fs_stat("/home/tester/new.txt", &s) == FS_OK && strcmp(files_selected(f), "new.txt") == 0,
          "New file asks for a name, creates it and selects it");
    type("dsub\n");
    CHECK(fs_stat("/home/tester/sub", &s) == FS_OK && s.type == FS_DIR, "New folder creates a folder");
    type("dbad/name\n");
    CHECK(fs_stat("/home/tester/badname", &s) == FS_OK, "a '/' cannot be typed into a name");
    files_select(f, "badname");
    desktop_key(INPUT_KEY_DELETE);
    CHECK(fs_stat("/home/tester/badname", &s) == FS_ERR_NOT_FOUND, "Delete removes the selected folder");
    files_select(f, "sub");
    desktop_key('\n');
    CHECK(strcmp(files_folder(f), "/home/tester/sub") == 0, "Enter opens a folder");
    desktop_key('\b');
    CHECK(strcmp(files_folder(f), "/home/tester") == 0, "Backspace goes up again");

    files_select(f, "new.txt");
    desktop_key('\n');
    struct window *e = wm_focused();
    bool unsaved;
    CHECK(e != NULL && e->app == &app_editor && editor_text(e, &unsaved)[0] == '\0',
          "Enter on a file opens it in the text editor");
    type("line one\nline two");
    desktop_key(INPUT_KEY_UP);
    desktop_key(INPUT_KEY_HOME);
    type(">");
    desktop_key(INPUT_KEY_END);
    desktop_key('\b');
    editor_text(e, &unsaved);
    CHECK(strcmp(editor_text(e, NULL), "line two") != 0 && unsaved
          && strcmp(editor_text(e, NULL), ">line on\nline two") == 0,
          "typing, arrows, Home, End and Backspace edit at the cursor");
    desktop_key(19);
    char buf[64];
    size_t len = 0;
    fs_read_file("/home/tester/new.txt", buf, sizeof(buf) - 1, &len);
    buf[len] = '\0';
    editor_text(e, &unsaved);
    CHECK(strcmp(buf, ">line on\nline two") == 0 && !unsaved, "Ctrl+S saves the file");
    wm_close(e);
    CHECK(wm_open(&app_editor, "/no/such/file") == NULL && wm_open(&app_editor, "/home") == NULL,
          "the editor refuses a missing file and a folder");
    files_select(f, "sub");
    desktop_key(INPUT_KEY_DELETE);
    files_select(f, "new.txt");
    desktop_key(INPUT_KEY_DELETE);
    CHECK(fs_stat("/home/tester/new.txt", &s) == FS_ERR_NOT_FOUND && fs_stat("/home/tester/sub", &s) != FS_OK,
          "files and folders made by the test are deleted again");
    wm_close(f);
}

static void test_monitor_health(void)
{
    st_section("Monitor, Health and About");
    struct window *m = wm_show(&app_monitor);
    sched_sleep(SCHED_TIMER_HZ / 5);
    wm_tick();
    bool sane = true;
    for (unsigned c = 0; c < 8; c++)
        sane = sane && monitor_busy(m, c) <= 100;
    CHECK(m != NULL && sane, "the monitor samples every CPU (busy between 0 and 100%)");
    wm_compose();
    struct gfx_rect body = wm_body(m);
    CHECK(gfx_get(wm_back(), body.x + UI_PAD + 96, body.y + UI_BAR_H + 6) != COL_SURFACE,
          "CPU 0's bar is drawn");
    wm_close(m);

    struct window *h = wm_show(&app_health);
    desktop_key('1');
    CHECK(textbuf_has(app_text(h), "All good"), "Health runs the live checks and shows the result");
    struct window *a = wm_show(&app_about);
    CHECK(textbuf_has(app_text(a), "BharatOS") && textbuf_has(app_text(a), "Self-checks"),
          "About shows what the system is");
    wm_close(a);
    wm_close(h);
}

bool desktop_run_tests(void)
{
    st_begin();
    kprintf("\n[test] ===== Phase 11: desktop =====\n");
    test_gfx();
    test_textbuf();
    struct gfx_surface *scr = splash_screen();
    if (!CHECK(wm_init(NULL, scr != NULL ? scr->w : 1024, scr != NULL ? scr->h : 768),
               "the window manager starts (drawing into memory only)"))
        return st_end("Desktop");
    wm_set_backdrop(login_draw);
    test_login();
    test_windows();
    test_terminal();
    test_files_editor();
    test_monitor_health();
    wm_close_all();
    desktop_logout();
    CHECK(!desktop_logged_in() && desktop_user()[0] == '\0', "Log out returns to the login screen");
    return st_end("Desktop");
}
