#include "console.h"
#include "shell.h"
#include "lineedit.h"
#include "input.h"
#include "fs.h"
#include "sched.h"
#include "rcu.h"
#include "serial.h"
#include "kprintf.h"
#include "string.h"

/* ---- login rules ------------------------------------------------------------ */

bool login_name_ok(const char *name)
{
    size_t n = name != NULL ? strlen(name) : 0;
    if (n == 0 || n > LOGIN_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
              || c == '-' || c == '_'))
            return false;
    }
    return true;
}

bool login_check(const char *user, const char *password, bool *needs_password)
{
    *needs_password = false;
    if (!login_name_ok(user))
        return false;
    static char users[2048];             /* only the console thread logs in */
    size_t len = 0;
    if (fs_read_file("/etc/users", users, sizeof(users) - 1, &len) != FS_OK)
        return true;                     /* no list: everyone is welcome */
    users[len] = '\0';
    size_t ul = strlen(user);
    for (char *line = users; *line != '\0';) {
        char *end = strchr(line, '\n');
        if (end != NULL)
            *end = '\0';
        if (line[0] != '#' && strncmp(line, user, ul) == 0 && line[ul] == ':') {
            const char *want = line + ul + 1;
            if (want[0] == '\0')
                return true;             /* listed with no password */
            *needs_password = true;
            return password != NULL && strcmp(password, want) == 0;
        }
        if (end == NULL)
            break;
        line = end + 1;
    }
    return true;
}

/* ---- the console thread ------------------------------------------------------- */

static char current_user[LOGIN_NAME_MAX + 1];
static tid_t console_tid;
static unsigned restarts;

const char *session_user(void)
{
    return current_user;
}

static void console_out(void *ctx, const char *s, size_t n)
{
    (void)ctx;
    serial_write(s, n);
}

static void say(const char *s)
{
    serial_write(s, strlen(s));
}

static bool key_waiting(void *ctx)
{
    (void)ctx;
    struct input_event ev;
    while (input_poll(&ev))
        if (ev.type == INPUT_KEY)
            return true;
    return false;
}

/* Read one line at the console. False if it was cancelled (Ctrl+C). */
static bool read_line(struct lineedit *le, bool secret)
{
    le_begin(le, secret);
    for (;;) {
        enum le_result r = le_key(le, input_wait_key());
        if (r == LE_DONE)
            return true;
        if (r == LE_CANCEL)
            return false;
    }
}

static struct lineedit le;
static struct shell sh;

static void session(void)
{
    say("\nBharatOS login: ");
    if (!read_line(&le, false) || le.buf[0] == '\0')
        return;
    char name[LOGIN_NAME_MAX + 1];
    if (!login_name_ok(le.buf)) {
        say("Please use 1 to 16 letters, digits, '-' or '_' for your name.\n");
        return;
    }
    strlcpy(name, le.buf, sizeof(name));

    bool needs_password;
    if (!login_check(name, NULL, &needs_password) || needs_password) {
        say("Password: ");
        bool typed = read_line(&le, true);
        if (!typed || !login_check(name, le.buf, &needs_password)) {
            say("That password is not right.\n");
            sched_sleep(SCHED_TIMER_HZ);
            return;
        }
    }

    strlcpy(current_user, name, sizeof(current_user));
    shell_init(&sh, name, console_out, NULL);
    sh.key_pressed = key_waiting;
    say("\n");
    shell_run(&sh, "cat /etc/motd");
    char hello[96];
    ksnprintf(hello, sizeof(hello), "\nHello, %s! Type 'help' to see what you can do.\n", name);
    say(hello);

    char prompt[FS_PATH_MAX + 40];
    while (!sh.logout) {
        shell_prompt(&sh, prompt, sizeof(prompt));
        say(prompt);
        if (!read_line(&le, false))
            continue;
        le_add_history(&le, le.buf);
        char line[LE_MAX];
        strlcpy(line, le.buf, sizeof(line));
        shell_run(&sh, line);
    }
    say("Goodbye.\n");
    current_user[0] = '\0';
}

static int64_t console_main(void *arg)
{
    (void)arg;
    /* A crash in the console ends only this thread (it is restarted). */
    thread_current()->flags |= THREAD_CONTAIN;
    le_init(&le, console_out, NULL);
    say("\n\x1b[2J\x1b[H");
    say("============================================\n");
    say("   B H A R A T   O S   -   V1 core\n");
    say("============================================\n");
    say("Log in with any name (letters and digits). Example: asha\n");
    for (;;)
        session();
}

void console_start(void)
{
    proc_status_t err;
    struct thread *t = thread_create(process_kernel(), "console", console_main, NULL, &err);
    if (t == NULL) {
        kprintf("[console] cannot start the console: %s\n", proc_status_str(err));
        return;
    }
    console_tid = t->tid;
}

void console_watch(void)
{
    if (console_tid == 0)
        return;
    uint64_t f = rcu_read_lock();
    struct thread *t = thread_find(console_tid);
    bool gone = t == NULL || t->state == TASK_TERMINATED;
    rcu_read_unlock(f);
    if (!gone)
        return;
    restarts++;
    kprintf("\n[console] The terminal stopped unexpectedly; starting it again (%u).\n", restarts);
    current_user[0] = '\0';
    console_start();
}
