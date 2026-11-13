/* Phase 10 self-tests: line editor, login rules, and the shell running a
 * scripted list of commands whose output is collected and checked. */

#include "shell_test.h"
#include "selftest.h"
#include "shell.h"
#include "lineedit.h"
#include "console.h"
#include "crash.h"
#include "input.h"
#include "fs.h"
#include "heap.h"
#include "sched.h"
#include "kprintf.h"
#include "string.h"

#define CHECK(c, d) st_check((c), (d))

/* ---- collected output ------------------------------------------------------- */

#define OUT_MAX 8192
static char out_buf[OUT_MAX];
static size_t out_len;

static void collect(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    for (size_t i = 0; i < n && out_len + 1 < OUT_MAX; i++)
        out_buf[out_len++] = s[i];
    out_buf[out_len] = '\0';
}

static void reset_out(void)
{
    out_len = 0;
    out_buf[0] = '\0';
}

static bool has(const char *text)
{
    size_t n = strlen(text);
    for (size_t i = 0; i + n <= out_len; i++)
        if (memcmp(out_buf + i, text, n) == 0)
            return true;
    return false;
}

/* ---- line editor ------------------------------------------------------------ */

static enum le_result feed(struct lineedit *le, const uint16_t *keys, unsigned n)
{
    enum le_result r = LE_MORE;
    for (unsigned i = 0; i < n && r == LE_MORE; i++)
        r = le_key(le, keys[i]);
    return r;
}

static void test_lineedit(void)
{
    st_section("Line editor");
    static struct lineedit le;
    le_init(&le, collect, NULL);

    reset_out();
    le_begin(&le, false);
    const uint16_t k1[] = { 'l', 's', 'x', '\b', ' ', '/', '\n' };
    CHECK(feed(&le, k1, 7) == LE_DONE && strcmp(le.buf, "ls /") == 0, "typing with a backspace gives \"ls /\"");
    CHECK(has("lsx\b \b /\n"), "the echo shows each key and erases the deleted one");
    le_add_history(&le, le.buf);

    le_begin(&le, false);
    const uint16_t k2[] = { 'p', 'w', 'd', '\n' };
    feed(&le, k2, 4);
    le_add_history(&le, le.buf);
    le_add_history(&le, "pwd");                     /* a repeat is not stored twice */

    le_begin(&le, false);
    const uint16_t k3[] = { 'x', INPUT_KEY_UP, INPUT_KEY_UP, '\n' };
    CHECK(feed(&le, k3, 4) == LE_DONE && strcmp(le.buf, "ls /") == 0, "Up twice recalls the line before last");
    le_begin(&le, false);
    const uint16_t k4[] = { 'x', INPUT_KEY_UP, INPUT_KEY_DOWN, '\n' };
    CHECK(feed(&le, k4, 4) == LE_DONE && strcmp(le.buf, "x") == 0, "Down returns to what was being typed");

    le_begin(&le, false);
    const uint16_t k5[] = { 'a', 'b', 3 };
    CHECK(feed(&le, k5, 3) == LE_CANCEL && le.buf[0] == '\0', "Ctrl+C abandons the line");

    reset_out();
    le_begin(&le, true);
    const uint16_t k6[] = { 's', 'e', 'c', '\n' };
    CHECK(feed(&le, k6, 4) == LE_DONE && strcmp(le.buf, "sec") == 0 && has("***") && !has("sec"),
          "secret mode echoes stars, never the text");

    le_begin(&le, false);
    unsigned extra = 0;
    for (unsigned i = 0; i < LE_MAX + 20; i++)
        extra += le_key(&le, 'z') != LE_MORE;
    CHECK(extra == 0 && le.len == LE_MAX - 1, "a line longer than 159 characters is cut, not overrun");
    const uint16_t k7[] = { INPUT_KEY_LEFT, INPUT_KEY_F1, 0x1B, '\n' };
    size_t before = le.len;
    CHECK(feed(&le, k7, 4) == LE_DONE && le.len == before, "keys that type nothing are ignored");
}

/* ---- login ------------------------------------------------------------------ */

static void test_login(void)
{
    st_section("Login rules");
    CHECK(login_name_ok("asha") && login_name_ok("Ravi_K-2") && login_name_ok("abcdefghijklmnop"),
          "letters, digits, '-' and '_' up to 16 characters are valid names");
    CHECK(!login_name_ok("") && !login_name_ok("a b") && !login_name_ok("../x")
          && !login_name_ok("abcdefghijklmnopq") && !login_name_ok(NULL),
          "empty, spaces, slashes and 17 characters are refused");
    bool need;
    CHECK(login_check("asha", NULL, &need) && !need, "a name not in /etc/users logs in without a password");
    CHECK(!login_check("admin", NULL, &need) && need && !login_check("admin", "wrong", &need)
          && login_check("admin", "bharat", &need), "admin needs its password from /etc/users");
}

/* ---- the shell ---------------------------------------------------------------- */

static struct shell sh;

static int run(const char *line)
{
    reset_out();
    return shell_run(&sh, line);
}

static void test_files_commands(void)
{
    st_section("Shell: files");
    shell_init(&sh, "tester", collect, NULL);
    CHECK(run("pwd") == 0 && has("/home/tester\n"), "starts in /home/tester (created at login)");
    char prompt[64];
    shell_prompt(&sh, prompt, sizeof(prompt));
    CHECK(strcmp(prompt, "tester@bharat:~$ ") == 0, "prompt shows the name and ~ for home");
    CHECK(run("whoami") == 0 && has("tester\n"), "whoami");
    CHECK(run("echo hi there > a.txt") == 0 && out_len == 0, "echo hi there > a.txt prints nothing");
    CHECK(run("cat a.txt") == 0 && has("hi there\n"), "cat a.txt shows \"hi there\"");
    CHECK(run("echo more >> a.txt") == 0 && run("cat a.txt") == 0 && has("hi there\nmore\n"),
          ">> adds to the end of the file");
    CHECK(run("mkdir notes") == 0 && run("touch notes/n1.txt") == 0 && run("ls") == 0
          && has("a.txt") && has("notes/") && has("1 item"), "mkdir, touch and ls (folders end with /)");
    CHECK(run("cd notes") == 0 && run("pwd") == 0 && has("/home/tester/notes"), "cd into a folder");
    shell_prompt(&sh, prompt, sizeof(prompt));
    CHECK(strcmp(prompt, "tester@bharat:~/notes$ ") == 0, "prompt shows ~/notes");
    CHECK(run("cd ..") == 0 && run("cd /etc") == 0 && run("cat motd") == 0 && has("Welcome to BharatOS"),
          "cd .. and cd /etc; cat motd");
    CHECK(run("cd") == 0 && run("pwd") == 0 && has("/home/tester\n"), "cd alone goes home");
    CHECK(run("ls / > list.txt") == 0 && run("cat list.txt") == 0 && has("etc/") && has("home/"),
          "any command's output can go to a file");
    CHECK(run("rm notes") != 0 && has("not empty"), "rm of a folder with things in it explains why");
    CHECK(run("rm notes/n1.txt") == 0 && run("rm notes") == 0 && run("rm a.txt list.txt") == 0
          && run("ls") == 0 && has("(empty folder)"), "rm files and the empty folder");
    CHECK(run("cat a.txt") != 0 && has("no such file"), "cat of a deleted file says there is no such file");
    CHECK(run("cd /etc/motd") != 0 && has("not a folder"), "cd into a file is refused");
    CHECK(run("mkdir /etc") != 0 && has("already exists"), "mkdir of an existing folder is refused");
    CHECK(run("echo x > /no/such/dir/f") != 0 && has("no such file"), "redirecting into a missing folder fails");
    CHECK(run("echo x >") != 0 && has("at the end"), "a > with no file name is explained");
    CHECK(run("echo \"a > b\"") == 0 && has("a > b\n"), "quotes keep spaces and > as text");
    CHECK(fs_check(), "the file tree is consistent after all that");
}

static void test_system_commands(void)
{
    st_section("Shell: system commands");
    CHECK(run("help") == 0 && has("crashdemo") && has("lockdemo") && has("checks"), "help lists the commands");
    CHECK(run("frobnicate") == 127 && has("unknown command"), "an unknown command is explained");
    CHECK(run("") == 0 && out_len == 0 && run("   ") == 0, "empty lines do nothing");
    CHECK(run("uptime") == 0 && has("up ") && has("CPU"), "uptime");
    CHECK(run("mem") == 0 && has("free") && has("Files"), "mem shows RAM, heap and file space");
    CHECK(run("cpus") == 0 && has("CPU  busy") && has("  0  "), "cpus shows a row per CPU");
    CHECK(run("ps") == 0 && has("kmain") && has("idle") && has("threads"), "ps lists the threads");
    CHECK(run("top") == 0 && has("busiest threads") && has("TID"), "top shows one screen when not interactive");
    CHECK(run("about") == 0 && has("BharatOS") && has("Self-checks"), "about");
    CHECK(run("clear") == 0 && has("\x1b[2J"), "clear sends the clear-screen code");
    CHECK(run("echo a b c d e f g h i j k l m n o p q") != 0 && has("Too many words"),
          "more than 16 words are refused");
}

static void test_threads_commands(void)
{
    st_section("Shell: threads, checks and demos");
    CHECK(run("run") != 0 && has("spin") && run("run fly") != 0 && has("no job"),
          "run lists its jobs and refuses unknown ones");
    CHECK(run("run spin 999") != 0 && run("run spin 0") != 0, "run refuses durations outside 1-600 s");
    CHECK(run("run spin 30") == 0 && has("Started 'spin' as thread"), "run spin starts a background thread");
    /* Its TID is the number after "thread ". */
    unsigned tid = 0;
    for (size_t i = 0; i + 7 < out_len; i++)
        if (memcmp(out_buf + i, "thread ", 7) == 0)
            for (size_t j = i + 7; out_buf[j] >= '0' && out_buf[j] <= '9'; j++)
                tid = tid * 10 + (unsigned)(out_buf[j] - '0');
    sched_sleep(2);
    char cmd[32];
    ksnprintf(cmd, sizeof(cmd), "kill %u", tid);
    CHECK(tid > 0 && run(cmd) == 0 && has("stopped"), "kill stops it");
    CHECK(run(cmd) != 0, "killing it again is refused (already finished, or gone)");
    CHECK(run("kill 0") != 0 && has("main thread") && run("kill 99999") != 0 && run("kill abc") != 0,
          "kill refuses the main thread, unknown numbers and words");

    CHECK(run("checks") == 0 && has("All good") && has("Total"), "checks: boot results plus live checks all pass");
    CHECK(run("lockdemo") == 0 && has("caught it"), "lockdemo: the checker catches the inversion, system keeps going");

    struct crash_info c0, c1;
    crash_get_last(&c0);
    CHECK(run("crashdemo") == 0 && has("contained"), "crashdemo: a null-pointer read is contained");
    CHECK(run("crashdemo divide") == 0 && run("crashdemo opcode") == 0, "divide by zero and a bad instruction too");
    crash_get_last(&c1);
    CHECK(c1.count == c0.count + 3 && strcmp(c1.name, "crashdemo") == 0 && c1.vector == 6,
          "three crashes recorded; the last was an invalid opcode in thread crashdemo");
    CHECK(run("crashdemo boom") != 0, "unknown crash kinds are refused");
    CHECK(heap_check() && sched_check(), "heap and scheduler are consistent after the crashes");
    CHECK(run("logout") == 0 && sh.logout, "logout ends the session");

    /* Clean up the test user's home. */
    fs_unlink("/home/tester");
}

bool shell_run_tests(void)
{
    st_begin();
    kprintf("\n[test] ===== Phase 10: login and shell =====\n");
    test_lineedit();
    test_login();
    test_files_commands();
    test_system_commands();
    test_threads_commands();
    return st_end("Login and shell");
}
