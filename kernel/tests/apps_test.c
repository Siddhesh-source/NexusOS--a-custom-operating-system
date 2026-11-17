/* Self-tests for the network stack and the newer desktop apps (Code,
 * Player, Browser, Files' places), the new shell commands and Tab
 * completion. Everything runs headless at boot, like the desktop tests.
 *
 * Network checks talk only to QEMU's built-in gateway (10.0.2.2), which
 * always answers, so they need no internet. Without a network card they
 * are skipped (and say so). Fetching a real page over TCP is checked by
 * the boot test harness, which serves one from the PC (test_boot.py). */

#include "apps_test.h"
#include "selftest.h"
#include "apps.h"
#include "desktop.h"
#include "wm.h"
#include "net.h"
#include "shell.h"
#include "lineedit.h"
#include "fs.h"
#include "input.h"
#include "sched.h"
#include "clock.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

#define CHECK(c, d) st_check((c), (d))

static void type(const char *s)
{
    while (*s != '\0')
        desktop_key((uint8_t)*s++);
}

/* ---- network -------------------------------------------------------------------- */

static void test_net(void)
{
    st_section("Network");
    uint32_t ip = 0;
    char buf[16];
    CHECK(net_parse_ip("10.0.2.15", &ip) && ip == 0x0A00020F, "an address is read from dotted numbers");
    CHECK(!net_parse_ip("10.0.2", &ip) && !net_parse_ip("10.0.2.256", &ip) && !net_parse_ip("a.b.c.d", &ip)
          && !net_parse_ip("1.2.3.4.5", &ip), "bad addresses are refused");
    net_format_ip(0x0A000202, buf);
    CHECK(strcmp(buf, "10.0.2.2") == 0, "an address is written back as dotted numbers");
    char page[128];
    size_t len;
    int status;
    CHECK(http_get("https://example.com/", page, sizeof(page), &len, &status, NULL) == NET_ERR_UNSUPPORTED,
          "a secure (https) address is refused with a clear reason");
    CHECK(http_get("http://:80/", page, sizeof(page), &len, &status, NULL) == NET_ERR_INVALID
          && http_get("http://host:99999/", page, sizeof(page), &len, &status, NULL) == NET_ERR_INVALID,
          "a broken web address is refused");
    if (!net_up()) {
        kprintf("[test] no network card: the live network checks are skipped\n");
        return;
    }
    uint8_t mac[6], zero[6] = { 0 };
    net_mac(mac);
    CHECK(memcmp(mac, zero, 6) != 0, "the network card has an address (MAC)");
    uint8_t gw[6];
    CHECK(net_arp_resolve(net_gateway(), gw, 2000) == NET_OK, "ARP finds the gateway's card address");
    uint64_t us = 0;
    net_status_t st = net_ping(net_gateway(), 1, 2000, &us);
    CHECK(st == NET_OK, "the gateway answers a ping");
    kprintf("[test]   ping 10.0.2.2: %lu us\n", us);
    CHECK(net_resolve("10.0.2.2", &ip) == NET_OK && ip == 0x0A000202 && net_resolve("", &ip) == NET_ERR_INVALID,
          "names that are already addresses need no lookup; empty names are refused");
    int conns[NET_MAX_TCP + 1];
    CHECK(tcp_send(-1, "x", 1, 10) == NET_ERR_INVALID && tcp_receive_all(NET_MAX_TCP, page, 1, &len, 10)
          == NET_ERR_INVALID, "bad connection numbers are refused");
    (void)conns;
    struct net_stats s;
    net_get_stats(&s);
    CHECK(s.frames_in > 0 && s.frames_out > 0, "packets went out and came back");
}

/* ---- apps ------------------------------------------------------------------------- */

static void test_code(void)
{
    st_section("Code");
    struct window *w = wm_open(&app_code, "/code/hello.c");
    bool unsaved = true;
    const char *text = code_text(w, &unsaved, NULL);
    CHECK(w != NULL && text != NULL && strstr(text, "#include") != NULL && !unsaved,
          "Code opens a C file from the bundle");
    wm_close(w);
    fs_unlink("/home/tester/t.sh");
    w = wm_open(&app_code, "/home/tester/t.sh");
    CHECK(w != NULL && code_text(w, NULL, NULL)[0] == '\0', "a new file starts empty");
    type("echo from code\n    echo indented");
    struct textbuf *out = NULL;
    text = code_text(w, &unsaved, &out);
    CHECK(strcmp(text, "echo from code\n    echo indented") == 0 && unsaved, "typing goes into the file");
    desktop_key('\n');
    text = code_text(w, NULL, NULL);
    CHECK(strcmp(text, "echo from code\n    echo indented\n    ") == 0, "Enter keeps the indent of the line above");
    desktop_key(19);                                    /* Ctrl+S */
    struct fs_stat st;
    CHECK(fs_stat("/home/tester/t.sh", &st) == FS_OK && st.size == strlen(text), "Ctrl+S saves the file");
    desktop_key(18);                                    /* Ctrl+R: Run */
    code_text(w, NULL, &out);
    CHECK(out != NULL && textbuf_has(out, "from code") && textbuf_has(out, "indented"),
          "Run runs a shell script and shows its output");
    wm_compose();
    wm_close(w);
    fs_unlink("/home/tester/t.sh");
    CHECK(app_for_file("/a/b.c") == &app_code && app_for_file("x.bvid") == &app_player
          && app_for_file("/www/home.html") == &app_browser && app_for_file("notes.txt") == &app_editor,
          "each kind of file opens in the app that suits it");
}

static void test_player(void)
{
    st_section("Player");
    uint8_t bad[900];
    uint16_t vw, vh, fps, frames;
    memset(bad, 0, sizeof(bad));
    CHECK(!player_parse(bad, sizeof(bad), &vw, &vh, &fps, &frames), "a file that is not a video is refused");
    memcpy(bad, "BVID", 4);
    bad[4] = 192; bad[6] = 108; bad[8] = 12; bad[10] = 120;
    CHECK(!player_parse(bad, sizeof(bad), &vw, &vh, &fps, &frames), "a video cut short is refused");
    CHECK(wm_open(&app_player, "/etc/motd") == NULL, "the Player refuses a file that is not a video");
    struct window *w = wm_open(&app_player, NULL);
    unsigned shown = 0;
    bool playing = false;
    CHECK(w != NULL, "the Player opens the bundled video");
    if (w == NULL)
        return;
    wm_compose();
    unsigned f0 = player_frame(w, &shown, &playing);
    CHECK(playing && shown > 0, "it starts playing and draws a frame");
    sched_sleep(SCHED_TIMER_HZ / 2);
    wm_animate();
    wm_compose();
    unsigned f1 = player_frame(w, &shown, NULL);
    CHECK(f1 > f0, "frames move on with the clock");
    desktop_key(' ');
    player_frame(w, NULL, &playing);
    unsigned f2 = player_frame(w, NULL, NULL);
    sched_sleep(SCHED_TIMER_HZ / 4);
    wm_animate();
    CHECK(!playing && player_frame(w, NULL, NULL) == f2, "Space pauses: the picture stays");
    desktop_key(INPUT_KEY_RIGHT);
    CHECK(player_frame(w, NULL, NULL) > f2, "Right jumps forward");
    desktop_key(INPUT_KEY_HOME);
    CHECK(player_frame(w, NULL, NULL) == 0, "Home goes back to the start");
    wm_close(w);
}

static void test_browser(void)
{
    st_section("Browser");
    static const char page[] =
        "<html><head><title>Test page</title><style>p{color:red}</style></head><body>"
        "<h1>Hello &amp; welcome</h1><!-- hidden --><p>First <b>para</b>graph.</p>"
        "<script>do_not_show();</script><ul><li>one</li><li>two</li></ul>"
        "<a href=\"/www/home.html\">home</a> <a href=\"second.html\">next</a></body></html>";
    static const char second[] = "<h1>Second</h1><pre>a  b\nc</pre>";
    fs_write_file("/home/tester/t.html", page, sizeof(page) - 1, false);
    fs_write_file("/home/tester/second.html", second, sizeof(second) - 1, false);
    struct window *w = wm_open(&app_browser, "/home/tester/t.html");
    unsigned links = 0;
    CHECK(w != NULL && browser_page_has(w, "Hello") && browser_page_has(w, "&") && browser_page_has(w, "welcome"),
          "a page's words are shown and &amp; is decoded");
    CHECK(!browser_page_has(w, "do_not_show") && !browser_page_has(w, "hidden") && !browser_page_has(w, "color"),
          "scripts, styles and comments are not shown");
    CHECK(browser_page_has(w, "Test page"), "the title is read");
    browser_url(w, &links);
    CHECK(links == 2, "both links are found");
    browser_follow(w, 1);
    CHECK(strcmp(browser_url(w, NULL), "/home/tester/second.html") == 0 && browser_page_has(w, "Second"),
          "a relative link opens the page next to it");
    browser_back(w);
    CHECK(strcmp(browser_url(w, NULL), "/home/tester/t.html") == 0, "Back returns to the page before");
    browser_follow(w, 0);
    CHECK(strcmp(browser_url(w, NULL), "/www/home.html") == 0 && browser_page_has(w, "BharatOS"),
          "the start page opens from a link");
    browser_go(w, "https://example.com/");
    CHECK(browser_page_has(w, "Secure") && browser_page_has(w, "TLS"), "a secure address explains why it cannot open");
    wm_compose();
    wm_close(w);
    fs_unlink("/home/tester/t.html");
    fs_unlink("/home/tester/second.html");
}

static void test_files_places(void)
{
    st_section("Files: places and disk");
    struct window *f = wm_open(&app_files, NULL);
    if (f == NULL) {
        CHECK(false, "Files opens");
        return;
    }
    struct gfx_rect b = wm_body(f);
    /* Click "Videos" (the third place). */
    wm_mouse(b.x + 40, b.y + UI_BAR_H + 30 + 2 * 30 + 10, INPUT_BUTTON_LEFT);
    wm_mouse(b.x + 40, b.y + UI_BAR_H + 30 + 2 * 30 + 10, 0);
    CHECK(strcmp(files_folder(f), "/media") == 0, "clicking a place opens that folder");
    CHECK(files_select(f, "bunny.bvid"), "the bundled video is in /media");
    desktop_key('\n');
    struct window *p = wm_focused();
    CHECK(p != NULL && p->app == &app_player, "opening a video starts the Player");
    wm_close(p);
    desktop_key('\b');
    CHECK(strcmp(files_folder(f), "/") == 0, "Up closes the folder and goes to the one holding it");
    wm_compose();
    wm_close(f);
}

/* ---- shell additions ------------------------------------------------------------ */

#define OUT_MAX 4096
static char out_buf[OUT_MAX];
static size_t out_len;

static void collect(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n && out_len + 1 < OUT_MAX; i++)
        out_buf[out_len++] = s[i];
    out_buf[out_len] = '\0';
}

static bool run(struct shell *sh, const char *line, const char *expect)
{
    out_len = 0;
    out_buf[0] = '\0';
    shell_run(sh, line);
    return expect == NULL || strstr(out_buf, expect) != NULL;
}

static void test_shell_more(void)
{
    st_section("Shell: new commands and Tab");
    struct shell *sh = kzalloc(sizeof(*sh));
    if (sh == NULL) {
        CHECK(false, "memory for a shell");
        return;
    }
    shell_init(sh, "tester", collect, NULL);
    run(sh, "echo alpha beta > w.txt", NULL);
    run(sh, "echo gamma >> w.txt", NULL);
    CHECK(run(sh, "wc w.txt", "2 lines") && strstr(out_buf, "3 words") != NULL, "wc counts lines and words");
    CHECK(run(sh, "grep GAMMA w.txt", "2: gamma"), "grep finds a line, ignoring upper/lower case");
    CHECK(run(sh, "head -n 1 w.txt", "alpha beta") && strstr(out_buf, "gamma") == NULL, "head shows the first lines");
    run(sh, "cp w.txt c.txt", NULL);
    CHECK(run(sh, "cat c.txt", "gamma"), "cp copies a file");
    run(sh, "mkdir d", NULL);
    run(sh, "mv c.txt d", NULL);
    struct fs_stat st;
    CHECK(fs_stat("/home/tester/d/c.txt", &st) == FS_OK && fs_stat("/home/tester/c.txt", &st) != FS_OK,
          "mv moves a file into a folder");
    CHECK(run(sh, "tree", "+-- d/") && strstr(out_buf, "|   +-- c.txt") != NULL, "tree shows folders inside folders");
    CHECK(run(sh, "cp nothing.txt x", "no such file or folder"), "cp of a missing file says so");
    CHECK(run(sh, "df", "BharatOS files"), "df shows how full the disk is");
    char done[LE_MAX];
    CHECK(shell_complete(sh, "tre", done, sizeof(done)) && strcmp(done, "tree ") == 0,
          "Tab completes a command name");
    CHECK(shell_complete(sh, "cat w.", done, sizeof(done)) && strcmp(done, "cat w.txt ") == 0,
          "Tab completes a file name");
    CHECK(shell_complete(sh, "ls /med", done, sizeof(done)) && strcmp(done, "ls /media/") == 0,
          "Tab completes a folder with a /");
    CHECK(!shell_complete(sh, "zzz", done, sizeof(done)), "Tab with no match changes nothing");
    if (net_up())
        CHECK(run(sh, "ping 10.0.2.2 1", "1 of 1 answered"), "ping gets an answer from the gateway");
    run(sh, "rm d/c.txt", NULL);
    run(sh, "rm d", NULL);
    run(sh, "rm w.txt", NULL);
    kfree(sh);
}

bool apps_run_tests(void)
{
    st_begin();
    kprintf("\n[test] ===== Network and apps =====\n");
    test_net();
    test_shell_more();
    type("tester\n");
    if (CHECK(desktop_logged_in(), "logged in for the app checks")) {
        test_code();
        test_player();
        test_browser();
        test_files_places();
    }
    wm_close_all();
    desktop_logout();
    return st_end("Network and apps");
}
