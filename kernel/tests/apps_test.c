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
#include "crash.h"
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

/* A DHCP answer built by hand: `opts` follow the magic cookie. */
static size_t dhcp_msg(uint8_t *m, uint32_t xid, const uint8_t *opts, size_t n)
{
    memset(m, 0, 240);
    m[0] = 2;
    m[4] = (uint8_t)(xid >> 24); m[5] = (uint8_t)(xid >> 16); m[6] = (uint8_t)(xid >> 8); m[7] = (uint8_t)xid;
    m[16] = 10; m[17] = 0; m[18] = 2; m[19] = 99;                 /* your address: 10.0.2.99 */
    m[236] = 0x63; m[237] = 0x82; m[238] = 0x53; m[239] = 0x63;
    memcpy(m + 240, opts, n);
    return 240 + n;
}

static void test_dhcp_parse(void)
{
    static const uint8_t good[] = { 53, 1, 5, 1, 4, 255, 255, 255, 0, 3, 4, 10, 0, 2, 2, 6, 4, 10, 0, 2, 3,
                                    51, 4, 0, 1, 81, 128, 0, 255 };
    static const uint8_t overrun[] = { 53, 1, 5, 3, 40, 10, 0 };
    static const uint8_t no_type[] = { 1, 4, 255, 255, 255, 0, 255 };
    uint8_t m[300];
    struct net_dhcp_reply r;
    size_t n = dhcp_msg(m, 0x1234, good, sizeof(good));
    CHECK(net_dhcp_parse(m, n, 0x1234, &r) && r.type == 5 && r.ip == 0x0A000263 && r.router == 0x0A000202
          && r.dns == 0x0A000203 && r.netmask == 0xFFFFFF00 && r.lease_s == 86400,
          "a DHCP answer is read: address, mask, gateway, name server, lease");
    CHECK(!net_dhcp_parse(m, n, 0x9999, &r), "an answer meant for another request is ignored");
    CHECK(!net_dhcp_parse(m, 200, 0x1234, &r), "a DHCP answer cut short is refused");
    m[236] = 0;
    CHECK(!net_dhcp_parse(m, n, 0x1234, &r), "a message without the DHCP marker is refused");
    n = dhcp_msg(m, 0x1234, overrun, sizeof(overrun));
    CHECK(!net_dhcp_parse(m, n, 0x1234, &r), "an option running past the end is refused");
    n = dhcp_msg(m, 0x1234, no_type, sizeof(no_type));
    CHECK(!net_dhcp_parse(m, n, 0x1234, &r) && !net_dhcp_parse(NULL, 0, 0, &r), "an answer with no type is refused");
}

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
    test_dhcp_parse();
    if (!net_up()) {
        kprintf("[test] no network card: the live network checks are skipped\n");
        return;
    }
    struct net_config cfg;
    net_get_config(&cfg);
    CHECK(cfg.from_dhcp && cfg.ip == 0x0A00020F && cfg.gateway == 0x0A000202 && cfg.dns == 0x0A000203
          && cfg.netmask == 0xFFFFFF00 && cfg.lease_s > 0, "DHCP gave the address, gateway and name server");
    CHECK(net_dhcp(3000) == NET_OK && net_ip() == 0x0A00020F, "asking DHCP again gets the same address");
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

static void test_code_tabs_find(void)
{
    struct window *w = wm_open(&app_code, "/code/hello.c");
    unsigned at = 9;
    size_t cur = 0;
    const char *msg = "";
    if (!CHECK(w != NULL && code_tabs(w, &at, NULL, NULL) == 1 && at == 0, "Code starts with one tab"))
        return;
    CHECK(code_open_tab(w, "/code/demo.sh") && code_tabs(w, &at, NULL, NULL) == 2 && at == 1
          && strstr(code_text(w, NULL, NULL), "echo") != NULL, "opening another file adds a tab and shows it");
    CHECK(code_open_tab(w, "/code/hello.c") && code_tabs(w, &at, NULL, NULL) == 2 && at == 0,
          "opening a file that is already open shows its tab");
    desktop_key(20);                                    /* Ctrl+T */
    code_tabs(w, &at, NULL, NULL);
    CHECK(at == 1, "Ctrl+T moves to the next tab");
    code_open_tab(w, "/code/fib.py");
    code_open_tab(w, "/code/README.md");
    CHECK(!code_open_tab(w, "/etc/motd") && code_tabs(w, NULL, NULL, &msg) == CODE_TABS
          && strstr(msg, "All tabs are in use") != NULL, "a fifth tab is refused with a reason");
    desktop_key('x');                                   /* README.md is now unsaved */
    desktop_key(17);                                    /* Ctrl+Q */
    CHECK(code_tabs(w, NULL, NULL, &msg) == CODE_TABS && strstr(msg, "Not saved") != NULL,
          "closing an unsaved tab asks first");
    desktop_key(17);
    CHECK(code_tabs(w, NULL, NULL, NULL) == CODE_TABS - 1, "Ctrl+Q twice closes it without saving");
    CHECK(!code_open_tab(w, "/no/such/file.c"), "a missing file is not opened in a new tab");
    code_open_tab(w, "/code/hello.c");
    desktop_key(6);                                     /* Ctrl+F */
    type("MAIN");
    desktop_key('\n');
    code_tabs(w, NULL, &cur, &msg);
    const char *text = code_text(w, NULL, NULL);
    CHECK(strncmp(text + cur, "main", 4) == 0 && strstr(msg, "Found on line 11") != NULL,
          "Ctrl+F finds text, ignoring upper/lower case");
    type("zzz");
    desktop_key('\n');
    code_tabs(w, NULL, NULL, &msg);
    CHECK(strstr(msg, "is not in this file") != NULL, "text that is not there is reported");
    desktop_key(0x1B);
    bool unsaved = false;
    desktop_key('q');
    code_text(w, &unsaved, NULL);
    CHECK(unsaved, "Esc closes the find bar and typing edits the file again");
    wm_compose();
    wm_close(w);
}

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
    test_code_tabs_find();
    CHECK(app_for_file("/a/b.c") == &app_code && app_for_file("x.bvid") == &app_player
          && app_for_file("/www/home.html") == &app_browser && app_for_file("notes.txt") == &app_editor,
          "each kind of file opens in the app that suits it");
}

static void test_player_playlist(struct window *w)
{
    unsigned at = 9;
    const char *name = "", *msg = "";
    CHECK(player_playlist(w, &at, &name, NULL) == 2 && at == 0 && strcmp(name, "bunny.bvid") == 0,
          "the playlist holds every video in /media, in name order");
    desktop_key('n');
    player_playlist(w, NULL, &name, NULL);
    CHECK(strcmp(name, "waves.bvid") == 0 && strstr(w->title, "waves") != NULL && player_frame(w, NULL, NULL) == 0,
          "N plays the next video from its start");
    desktop_key('n');
    player_playlist(w, NULL, &name, NULL);
    CHECK(strcmp(name, "bunny.bvid") == 0, "after the last video comes the first");
    desktop_key('p');
    desktop_key(INPUT_KEY_END);
    sched_sleep(SCHED_TIMER_HZ / 4);
    wm_animate();
    player_playlist(w, NULL, &name, NULL);
    CHECK(strcmp(name, "bunny.bvid") == 0 && player_frame(w, NULL, NULL) < 3,
          "when a video ends the next one starts");
    struct gfx_rect before = w->r;
    desktop_key('f');
    wm_compose();
    CHECK(w->full && w->r.x == 0 && w->r.w == wm_back()->w && gfx_get(wm_back(), 2, 2) == 0,
          "F shows the video full screen, over the top bar");
    desktop_key(0x1B);
    CHECK(!w->full && w->r.x == before.x && w->r.y == before.y && w->r.w == before.w,
          "Esc leaves full screen and the window goes back");
    wm_compose();
    fs_write_file("/media/broken.bvid", "BVID not really", 15, false);
    struct window *v = wm_open(&app_player, "/media/bunny.bvid");
    desktop_key('p');
    player_playlist(v, NULL, &name, &msg);
    CHECK(strcmp(name, "waves.bvid") == 0 && strstr(msg, "Skipped broken.bvid") != NULL,
          "a broken video in the playlist is skipped with a message");
    wm_close(v);
    char path[FS_PATH_MAX];
    for (unsigned i = 0; i < PLAYER_LIST_MAX; i++) {
        ksnprintf(path, sizeof(path), "/media/zz%u.bvid", i);
        fs_write_file(path, "x", 1, false);
    }
    v = wm_open(&app_player, "/media/bunny.bvid");
    CHECK(v != NULL && player_playlist(v, NULL, NULL, NULL) == PLAYER_LIST_MAX,
          "the playlist stops at its limit");
    wm_close(v);
    for (unsigned i = 0; i < PLAYER_LIST_MAX; i++) {
        ksnprintf(path, sizeof(path), "/media/zz%u.bvid", i);
        fs_unlink(path);
    }
    fs_unlink("/media/broken.bvid");
    wm_focus(w);
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
    test_player_playlist(w);
    wm_close(w);
}

static void test_browser_bookmarks(struct window *w)
{
    const char *bm = "/home/tester/bookmarks.txt";
    char buf[512];
    size_t n = 0;
    unsigned links = 0;
    fs_unlink(bm);
    browser_go(w, "about:bookmarks");
    CHECK(browser_page_has(w, "Ctrl+D") && browser_page_has(w, "bookmarks"), "with no bookmarks the page says how to add one");
    browser_go(w, "/home/tester/t.html");
    desktop_key(4);                                     /* Ctrl+D */
    buf[0] = '\0';
    if (fs_read_file(bm, buf, sizeof(buf) - 1, &n) == FS_OK)
        buf[n] = '\0';
    CHECK(strstr(browser_message(w), "Bookmarked") != NULL && strstr(buf, "/home/tester/t.html\tTest page\n") != NULL,
          "Ctrl+D bookmarks the page, kept in the user's bookmarks file");
    desktop_key(4);
    CHECK(strstr(browser_message(w), "Already") != NULL, "a page is bookmarked only once");
    static const char tricky[] = "<title>a&lt;b&gt;c</title><p>tricky</p>";
    fs_write_file("/home/tester/x.html", tricky, sizeof(tricky) - 1, false);
    browser_go(w, "/home/tester/x.html");
    desktop_key(4);
    browser_go(w, "about:bookmarks");
    browser_url(w, &links);
    CHECK(browser_page_has(w, "Test") && browser_page_has(w, "a<b>c") && links == 2,
          "the Bookmarks page lists them as links; a title cannot add tags");
    browser_follow(w, 0);
    CHECK(strcmp(browser_url(w, NULL), "/home/tester/t.html") == 0, "a bookmark opens its page");
    browser_go(w, "about:history");
    browser_url(w, &links);
    CHECK(browser_page_has(w, "History") && browser_page_has(w, "Test") && browser_page_has(w, "second.html")
          && links >= 3, "the History page lists the pages visited");
    desktop_key(4);
    CHECK(strstr(browser_message(w), "cannot be bookmarked") != NULL, "the History page itself is not bookmarked");
    fs_unlink(bm);
    for (unsigned i = 0; i < BOOKMARK_MAX; i++) {
        ksnprintf(buf, sizeof(buf), "http://10.0.2.2/%u\tpage %u\n", i, i);
        fs_write_file(bm, buf, strlen(buf), true);
    }
    browser_go(w, "/home/tester/t.html");
    desktop_key(4);
    CHECK(strstr(browser_message(w), "full") != NULL, "bookmarks stop at their limit, with a reason");
    fs_unlink(bm);
    fs_unlink("/home/tester/x.html");
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
    test_browser_bookmarks(w);
    wm_compose();
    wm_close(w);
    fs_unlink("/home/tester/t.html");
    fs_unlink("/home/tester/second.html");
}

static void test_files_copy_rename(void)
{
    st_section("Files: copy, paste and rename");
    struct fs_stat st;
    char buf[32];
    size_t n = 0;
    fs_mkdir("/home/tester/cp");
    fs_write_file("/home/tester/cp/notes.txt", "hello notes", 11, false);
    CHECK(fs_rename("/home/tester/cp/notes.txt", "/home/tester/cp/n2.txt") == FS_OK
          && fs_stat("/home/tester/cp/n2.txt", &st) == FS_OK && st.size == 11
          && fs_stat("/home/tester/cp/notes.txt", &st) == FS_ERR_NOT_FOUND, "fs_rename renames a file");
    fs_write_file("/home/tester/cp/other.txt", "x", 1, false);
    CHECK(fs_rename("/home/tester/cp/n2.txt", "/home/tester/cp/other.txt") == FS_ERR_EXISTS
          && fs_rename("/home/tester/cp", "/home/tester/cp/inside") == FS_ERR_INVALID
          && fs_rename("/home/tester/nothing", "/home/tester/x") == FS_ERR_NOT_FOUND && fs_rename("/", "/x") == FS_ERR_BUSY,
          "fs_rename refuses a taken name, a folder into itself, a missing file and the root");
    fs_rename("/home/tester/cp/n2.txt", "/home/tester/cp/notes.txt");
    fs_unlink("/home/tester/cp/other.txt");
    struct window *f = wm_open(&app_files, "/home/tester/cp");
    if (!CHECK(f != NULL, "Files opens a folder"))
        return;
    desktop_key(22);                                    /* Ctrl+V with nothing copied */
    CHECK(strstr(files_message(f), "Nothing to paste") != NULL,
          "Paste with nothing copied says so");
    fs_unlink("/home/tester/cp/notes-copy.txt");
    files_select(f, "notes.txt");
    desktop_key(3);                                     /* Ctrl+C */
    desktop_key(22);                                    /* Ctrl+V */
    buf[0] = '\0';
    if (fs_read_file("/home/tester/cp/notes-copy.txt", buf, sizeof(buf) - 1, &n) == FS_OK)
        buf[n] = '\0';
    CHECK(strcmp(buf, "hello notes") == 0 && strstr(files_message(f), "Pasted notes-copy.txt") != NULL,
          "Copy and Paste in the same folder makes notes-copy.txt");
    desktop_key(22);
    CHECK(fs_stat("/home/tester/cp/notes-copy2.txt", &st) == FS_OK, "pasting again makes notes-copy2.txt");
    desktop_key('\b');                                  /* up to /home/tester */
    desktop_key(22);
    CHECK(fs_stat("/home/tester/notes.txt", &st) == FS_OK && st.size == 11, "Paste in another folder keeps the name");
    fs_unlink("/home/tester/notes.txt");
    files_select(f, "cp");
    desktop_key(3);
    CHECK(strstr(files_message(f), "folders cannot be copied") != NULL, "copying a folder is refused with a reason");
    desktop_key('r');
    for (unsigned i = 0; i < 2; i++)
        desktop_key('\b');
    type("docs2");                                      /* "cp" -> "docs2" */
    desktop_key('\n');
    CHECK(fs_stat("/home/tester/docs2/notes.txt", &st) == FS_OK && fs_stat("/home/tester/cp", &st) != FS_OK
          && strstr(files_message(f), "Renamed cp to docs2") != NULL, "Rename renames a folder with its files");
    files_select(f, "docs2");
    desktop_key('r');
    for (unsigned i = 0; i < 5; i++)
        desktop_key('\b');
    desktop_key('\n');
    CHECK(strstr(files_message(f), "Cannot rename") != NULL && fs_stat("/home/tester/docs2", &st) == FS_OK,
          "an empty new name is refused and nothing changes");
    wm_compose();
    wm_close(f);
    const char *left[] = { "notes.txt", "notes-copy.txt", "notes-copy2.txt" };
    char p[FS_PATH_MAX];
    for (unsigned i = 0; i < 3; i++) {
        ksnprintf(p, sizeof(p), "/home/tester/docs2/%s", left[i]);
        fs_unlink(p);
    }
    fs_unlink("/home/tester/docs2");
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
    test_files_copy_rename();
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
    CHECK(run(sh, "why", "why slow") && sh->status != 0, "why alone lists the questions it can answer");
    CHECK(run(sh, "why rain", "cannot explain 'rain'"), "why says plainly what it cannot explain");
    struct crash_info ci;
    crash_get_last(&ci);
    if (ci.count == 0)
        CHECK(run(sh, "why crash", "No program has crashed"), "why crash with no crash says so");
    run(sh, "crashdemo", NULL);
    CHECK(run(sh, "why crash", "'crashdemo'") && strstr(out_buf, "null") != NULL
          && strstr(out_buf, "only that thread was stopped") != NULL,
          "why crash names the program and says what it did wrong");
    run(sh, "run spin 2", NULL);
    CHECK(run(sh, "why slow", "busiest thread spin") && strstr(out_buf, "kill") != NULL,
          "why slow finds a busy thread from live numbers and says how to stop it");
    CHECK(strstr(out_buf, "memory") != NULL && strstr(out_buf, "CPUs") != NULL, "why slow reports CPU load and memory");
    run(sh, "rm d/c.txt", NULL);
    run(sh, "rm d", NULL);
    run(sh, "rm w.txt", NULL);
    kfree(sh);
}

/* ---- Hindi and Marathi on the login screen ----------------------------------- */

static uint64_t area_sum(struct gfx_rect r)
{
    uint64_t sum = 0;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++)
            sum = sum * 31 + gfx_get(wm_back(), x, y);
    return sum;
}

static void test_login_languages(void)
{
    st_section("Login in Hindi and Marathi");
    bool all = true;
    for (unsigned i = 0; i < PHRASE_COUNT; i++)
        all &= gfx_phrase_width((enum phrase_id)i) > 0;
    uint16_t w, h;
    CHECK(all, "every Hindi and Marathi phrase is in the kernel");
    CHECK(font_phrase(PHRASE_COUNT, &w, &h) == NULL && gfx_phrase_width((enum phrase_id)99) == 0,
          "an unknown phrase draws nothing");
    login_reset();
    wm_set_backdrop(login_draw);
    char shot[FS_PATH_MAX];
    CHECK(desktop_screenshot(shot, sizeof(shot)) == FS_ERR_INVALID, "no screenshot is saved before anyone signs in");
    struct gfx_surface *b = wm_back();
    int32_t cx = b->w / 2, cy = b->h / 2;
    struct gfx_rect label = gfx_rect(cx - 150, cy - 2, 300, 26), hint = gfx_rect(cx - 150, cy + 80, 300, 26);
    struct gfx_rect greet = gfx_rect(cx - 120, cy - 160, 240, 40);
    login_set_language(LOGIN_ENGLISH);
    wm_compose();
    uint64_t en = area_sum(label), en_hint = area_sum(hint);
    unsigned ink = 0;
    for (int32_t y = greet.y; y < greet.y + greet.h; y++)
        for (int32_t x = greet.x; x < greet.x + greet.w; x++)
            ink += (gfx_get(b, x, y) & 0xFF) > 0xC0;
    CHECK(ink > 100, "the login screen greets in Hindi and Marathi");
    desktop_key(INPUT_KEY_F1);
    wm_compose();
    uint64_t hi = area_sum(label);
    CHECK(login_language() == LOGIN_HINDI && hi != en && area_sum(hint) != en_hint,
          "F1 switches the labels to Hindi");
    desktop_key(INPUT_KEY_F1);
    wm_compose();
    uint64_t mr = area_sum(label);
    CHECK(login_language() == LOGIN_MARATHI && mr != en && mr != hi, "F1 again switches them to Marathi");
    desktop_key(INPUT_KEY_F1);
    CHECK(login_language() == LOGIN_ENGLISH, "and F1 once more goes back to English");
    login_set_language((enum login_lang)7);
    CHECK(login_language() == LOGIN_ENGLISH, "an unknown language falls back to English");
    login_set_language(LOGIN_HINDI);
}

/* ---- screenshots ------------------------------------------------------------- */

static void test_screenshot(void)
{
    st_section("Screenshots (Print Screen)");
    struct kbd_decoder d;
    memset(&d, 0, sizeof(d));
    struct input_event ev;
    bool got = false;
    const uint8_t seq[] = { 0xE0, 0x2A, 0xE0, 0x37 };
    for (unsigned i = 0; i < sizeof(seq); i++)
        if (kbd_decode(&d, seq[i], &ev))
            got = ev.key == INPUT_KEY_PRINT;
    CHECK(got, "the Print Screen key is read from the keyboard");
    char path[FS_PATH_MAX];
    for (unsigned i = 1; i <= DESKTOP_SHOTS_MAX; i++) {
        ksnprintf(path, sizeof(path), "/home/tester/screenshot-%u.ppm", i);
        fs_unlink(path);
    }
    wm_compose();
    desktop_key(INPUT_KEY_PRINT);
    struct gfx_surface *b = wm_back();
    struct fs_stat st;
    char head[32];
    int hl = ksnprintf(head, sizeof(head), "P6\n%d %d\n255\n", b->w, b->h);
    size_t want = (size_t)hl + (size_t)b->w * (size_t)b->h * 3, n = 0;
    CHECK(strstr(wm_toast_text(), "Screenshot saved: /home/tester/screenshot-1.ppm") != NULL,
          "Print Screen saves the screen and says where");
    CHECK(fs_stat("/home/tester/screenshot-1.ppm", &st) == FS_OK && st.size == want,
          "the picture has every pixel of the screen");
    uint8_t *buf = kmalloc(want);
    if (buf != NULL && fs_read_file("/home/tester/screenshot-1.ppm", buf, want, &n) == FS_OK) {
        int32_t x = b->w / 2, y = b->h - 1;
        const uint8_t *px = buf + hl + ((size_t)y * (size_t)b->w + (size_t)x) * 3;
        uint32_t c = gfx_get(b, x, y);
        CHECK(memcmp(buf, head, (size_t)hl) == 0 && px[0] == (uint8_t)(c >> 16) && px[1] == (uint8_t)(c >> 8)
              && px[2] == (uint8_t)c, "it is a PPM picture whose pixels match the screen");
    } else {
        CHECK(false, "the screenshot can be read back");
    }
    kfree(buf);
    desktop_key(INPUT_KEY_PRINT);
    CHECK(fs_stat("/home/tester/screenshot-2.ppm", &st) == FS_OK, "a second screenshot gets the next name");
    for (unsigned i = 3; i <= DESKTOP_SHOTS_MAX; i++) {
        ksnprintf(path, sizeof(path), "/home/tester/screenshot-%u.ppm", i);
        fs_write_file(path, "x", 1, false);
    }
    desktop_key(INPUT_KEY_PRINT);
    CHECK(strstr(wm_toast_text(), "Too many screenshots") != NULL, "when every name is taken it says so");
    for (unsigned i = 1; i <= DESKTOP_SHOTS_MAX; i++) {
        ksnprintf(path, sizeof(path), "/home/tester/screenshot-%u.ppm", i);
        fs_unlink(path);
    }
    wm_toast("");
}

bool apps_run_tests(void)
{
    st_begin();
    kprintf("\n[test] ===== Network and apps =====\n");
    test_net();
    test_shell_more();
    test_login_languages();
    type("tester\n");
    login_set_language(LOGIN_ENGLISH);
    if (CHECK(desktop_logged_in(), "signing in works with Hindi labels")) {
        test_code();
        test_player();
        test_browser();
        test_files_places();
        test_screenshot();
    }
    wm_close_all();
    desktop_logout();
    return st_end("Network and apps");
}
