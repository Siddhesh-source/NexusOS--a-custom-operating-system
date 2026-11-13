#include "shell.h"
#include "heap.h"
#include "pmm.h"
#include "sched.h"
#include "proc.h"
#include "percpu.h"
#include "ipi.h"
#include "rcu.h"
#include "clock.h"
#include "spinlock.h"
#include "waitq.h"
#include "crash.h"
#include "selftest.h"
#include "input.h"
#include "cpu.h"
#include "kprintf.h"
#include "string.h"
#include <stdarg.h>

/* ---- output --------------------------------------------------------------- */

void shell_write(struct shell *sh, const char *s, size_t n)
{
    if (sh->cap != NULL) {
        size_t room = SHELL_CAPTURE_MAX - sh->cap_len;
        if (n > room) {
            n = room;
            sh->cap_full = true;
        }
        memcpy(sh->cap + sh->cap_len, s, n);
        sh->cap_len += n;
        return;
    }
    if (sh->out != NULL && n > 0)
        sh->out(sh->out_ctx, s, n);
}

void shell_printf(struct shell *sh, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    size_t n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    shell_write(sh, buf, n < sizeof(buf) ? n : sizeof(buf) - 1);
}

#define P(...) shell_printf(sh, __VA_ARGS__)

/* Turn a typed path into an absolute one; prints the problem if any. */
static bool resolve(struct shell *sh, const char *path, char *out)
{
    fs_status_t st = fs_path_resolve(sh->cwd, path, out, FS_PATH_MAX);
    if (st != FS_OK) {
        P("%s: %s\n", path, fs_status_str(st));
        return false;
    }
    return true;
}

static int fail(struct shell *sh, const char *what, fs_status_t st)
{
    P("%s: %s\n", what, fs_status_str(st));
    return 1;
}

static bool parse_uint(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (s == NULL || *s == '\0')
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > 100000000)
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

/* ---- files ---------------------------------------------------------------- */

static int cmd_pwd(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    P("%s\n", sh->cwd);
    return 0;
}

static int cmd_cd(struct shell *sh, int argc, char **argv)
{
    char path[FS_PATH_MAX];
    if (!resolve(sh, argc > 1 ? argv[1] : sh->home, path))
        return 1;
    struct fs_stat st;
    fs_status_t r = fs_stat(path, &st);
    if (r != FS_OK)
        return fail(sh, argv[argc > 1 ? 1 : 0], r);
    if (st.type != FS_DIR)
        return fail(sh, argv[1], FS_ERR_NOT_DIR);
    strlcpy(sh->cwd, path, sizeof(sh->cwd));
    return 0;
}

static int cmd_ls(struct shell *sh, int argc, char **argv)
{
    char path[FS_PATH_MAX];
    if (!resolve(sh, argc > 1 ? argv[1] : ".", path))
        return 1;
    struct fs_stat st;
    fs_status_t r = fs_stat(path, &st);
    if (r != FS_OK)
        return fail(sh, argc > 1 ? argv[1] : path, r);
    if (st.type == FS_FILE) {
        P("%-28s %lu bytes\n", argv[1], st.size);
        return 0;
    }
    struct fs_dirent e;
    unsigned i = 0;
    while (fs_list(path, i, &e) == FS_OK) {
        if (e.type == FS_DIR) {
            char name[FS_NAME_MAX + 1];
            ksnprintf(name, sizeof(name), "%s/", e.name);
            P("%-28s folder, %lu item%s\n", name, e.size, e.size == 1 ? "" : "s");
        } else
            P("%-28s %lu bytes\n", e.name, e.size);
        i++;
    }
    if (i == 0)
        P("(empty folder)\n");
    return 0;
}

static int cmd_cat(struct shell *sh, int argc, char **argv)
{
    if (argc < 2) {
        P("usage: cat <file>...\n");
        return 1;
    }
    int rc = 0;
    for (int a = 1; a < argc; a++) {
        char path[FS_PATH_MAX];
        if (!resolve(sh, argv[a], path)) {
            rc = 1;
            continue;
        }
        int fd;
        fs_status_t r = fs_open(path, FS_O_READ, &fd);
        if (r != FS_OK) {
            rc = fail(sh, argv[a], r);
            continue;
        }
        char buf[256];
        size_t n;
        char lastc = '\n';
        while (fs_read(fd, buf, sizeof(buf), &n) == FS_OK && n > 0) {
            shell_write(sh, buf, n);
            lastc = buf[n - 1];
        }
        fs_close(fd);
        if (lastc != '\n')
            shell_write(sh, "\n", 1);
    }
    return rc;
}

static int cmd_echo(struct shell *sh, int argc, char **argv)
{
    for (int a = 1; a < argc; a++) {
        if (a > 1)
            shell_write(sh, " ", 1);
        shell_write(sh, argv[a], strlen(argv[a]));
    }
    shell_write(sh, "\n", 1);
    return 0;
}

static int path_cmd(struct shell *sh, int argc, char **argv, fs_status_t (*fn)(const char *),
                    bool ok_if_exists)
{
    if (argc < 2) {
        P("usage: %s <name>...\n", argv[0]);
        return 1;
    }
    int rc = 0;
    for (int a = 1; a < argc; a++) {
        char path[FS_PATH_MAX];
        if (!resolve(sh, argv[a], path)) {
            rc = 1;
            continue;
        }
        fs_status_t r = fn(path);
        if (r != FS_OK && !(ok_if_exists && r == FS_ERR_EXISTS))
            rc = fail(sh, argv[a], r);
    }
    return rc;
}

static int cmd_mkdir(struct shell *sh, int argc, char **argv)
{
    return path_cmd(sh, argc, argv, fs_mkdir, false);
}

static int cmd_touch(struct shell *sh, int argc, char **argv)
{
    return path_cmd(sh, argc, argv, fs_create, true);
}

static int cmd_rm(struct shell *sh, int argc, char **argv)
{
    for (int a = 1; a < argc; a++) {
        char path[FS_PATH_MAX];
        if (resolve(sh, argv[a], path) && strncmp(sh->cwd, path, strlen(path)) == 0
            && (sh->cwd[strlen(path)] == '/' || sh->cwd[strlen(path)] == '\0')) {
            P("%s: you are inside it; cd out first\n", argv[a]);
            return 1;
        }
    }
    return path_cmd(sh, argc, argv, fs_unlink, false);
}

/* ---- system information --------------------------------------------------- */

static int cmd_clear(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    shell_write(sh, "\x1b[2J\x1b[H", 7);
    return 0;
}

static int cmd_whoami(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    P("%s\n", sh->user);
    return 0;
}

static void print_duration(struct shell *sh, uint64_t ns)
{
    uint64_t s = ns / 1000000000ull;
    if (s >= 3600)
        P("%lu h %lu min %lu s", s / 3600, (s / 60) % 60, s % 60);
    else if (s >= 60)
        P("%lu min %lu s", s / 60, s % 60);
    else
        P("%lu.%lu s", s, (uint64_t)((ns / 100000000ull) % 10));
}

static int cmd_uptime(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    P("up ");
    print_duration(sh, clock_now_ns());
    P(", %u CPU%s\n", smp_cpu_count(), smp_cpu_count() == 1 ? "" : "s");
    return 0;
}

static int cmd_mem(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    struct pmm_stats ps;
    struct heap_stats hs;
    struct fs_usage fu;
    pmm_get_stats(&ps);
    heap_get_stats(&hs);
    fs_get_usage(&fu);
    uint64_t mib = 1024 * 1024;
    P("Memory (RAM)    %lu MiB in the machine\n", ps.total_bytes / mib);
    P("  free          %lu MiB (%lu pages of 4 KiB)\n", ps.free_pages * 4096 / mib, ps.free_pages);
    P("  in use        %lu MiB\n", ps.allocated_pages * 4096 / mib);
    P("Kernel heap     %lu KiB used of %lu KiB mapped\n", hs.used_bytes / 1024, hs.mapped_bytes / 1024);
    P("Files           %lu KiB in %u files and folders (limit %lu MiB, %u items)\n",
      fu.bytes / 1024, fu.nodes, fu.max_bytes / mib, fu.max_nodes);
    return 0;
}

/* One CPU's numbers. With `prev` (and `dt_ticks` > 0), busy % is over
 * that interval; otherwise since boot. */
static void print_cpu_table(struct shell *sh, struct sched_stats *prev, uint64_t dt_ticks)
{
    P("CPU  busy  running            ready  switches  steals  IPIs\n");
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        if (!cpu_at(c)->online)
            continue;
        struct sched_stats s;
        sched_get_cpu_stats(c, &s);
        uint64_t idle = s.idle_ticks, total = s.ticks;
        if (prev != NULL && dt_ticks > 0) {
            idle -= prev[c].idle_ticks;
            total = dt_ticks;
        }
        unsigned busy = total == 0 ? 0 : idle >= total ? 0 : (unsigned)(100 - idle * 100 / total);
        char who[20] = "idle";
        uint64_t f = rcu_read_lock();
        struct thread *t = thread_find(s.current);
        if (t != NULL && !(t->flags & THREAD_IDLE))
            ksnprintf(who, sizeof(who), "%s (%u)", t->name, t->tid);
        rcu_read_unlock(f);
        P("%3u  %3u%%  %-18s %5u  %8lu  %6lu  %4lu\n", c, busy, who, s.runnable,
          s.switches, s.steals, s.ipis);
        if (prev != NULL)
            prev[c] = s;
    }
}

static int cmd_cpus(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    P("%u CPU%s online. Busy %% is since boot.\n", smp_cpu_count(), smp_cpu_count() == 1 ? "" : "s");
    print_cpu_table(sh, NULL, 0);
    P("steals: threads an idle CPU took from a busy one (work stealing)\n");
    return 0;
}

struct tinfo {
    tid_t tid;
    char name[TASK_NAME_LEN];
    uint8_t state;
    uint8_t flags;
    uint32_t cpu;
    uint64_t cpu_ns;
};

/* Copy the thread table (under RCU) so printing needs no lock. */
static unsigned snapshot_threads(struct tinfo *out, unsigned max)
{
    unsigned n = 0;
    uint64_t f = rcu_read_lock();
    for (unsigned slot = 0; slot < THREAD_MAX && n < max; slot++) {
        struct thread *t = thread_at(slot);
        if (t == NULL)
            continue;
        out[n].tid = t->tid;
        strlcpy(out[n].name, t->name, TASK_NAME_LEN);
        out[n].state = (uint8_t)t->state;
        out[n].flags = t->flags;
        out[n].cpu = t->cpu;
        out[n].cpu_ns = t->cpu_ns;
        n++;
    }
    rcu_read_unlock(f);
    return n;
}

static void print_threads(struct shell *sh, struct tinfo *ti, unsigned n, unsigned limit)
{
    P("  TID  name              state       CPU  CPU time\n");
    for (unsigned i = 0; i < n && i < limit; i++) {
        P("%5u  %-16s  %-10s  %3u  %lu ms\n", ti[i].tid, ti[i].name,
          task_state_str((task_state_t)ti[i].state), ti[i].cpu, ti[i].cpu_ns / 1000000);
    }
}

static int cmd_ps(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    struct tinfo *ti = kmalloc(sizeof(struct tinfo) * THREAD_MAX);
    if (ti == NULL) {
        P("ps: out of memory\n");
        return 1;
    }
    unsigned n = snapshot_threads(ti, THREAD_MAX);
    print_threads(sh, ti, n, THREAD_MAX);
    P("%u threads\n", n);
    kfree(ti);
    return 0;
}

static int cmd_top(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    struct tinfo *ti = kmalloc(sizeof(struct tinfo) * THREAD_MAX);
    struct sched_stats *prev = kzalloc(sizeof(struct sched_stats) * SMP_MAX_CPUS);
    if (ti == NULL || prev == NULL) {
        kfree(ti);
        kfree(prev);
        P("top: out of memory\n");
        return 1;
    }
    bool live = sh->key_pressed != NULL && sh->cap == NULL;
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++)
        if (cpu_at(c)->online)
            sched_get_cpu_stats(c, &prev[c]);
    uint64_t dt = 0;
    for (unsigned frame = 0; frame < 120; frame++) {
        if (live)
            shell_write(sh, "\x1b[2J\x1b[H", 7);
        P("top: CPUs and the busiest threads%s\n", live ? " (any key: stop)" : "");
        print_cpu_table(sh, prev, dt);
        unsigned n = snapshot_threads(ti, THREAD_MAX);
        /* Busiest first (simple selection; n is small). */
        for (unsigned i = 0; i < n; i++)
            for (unsigned j = i + 1; j < n; j++)
                if (ti[j].cpu_ns > ti[i].cpu_ns) {
                    struct tinfo t = ti[i];
                    ti[i] = ti[j];
                    ti[j] = t;
                }
        P("\n");
        print_threads(sh, ti, n, 8);
        if (!live)
            break;
        uint64_t t0 = sched_ticks();
        bool stop = false;
        for (unsigned i = 0; i < 10 && !stop; i++) {
            sched_sleep(SCHED_TIMER_HZ / 10);
            stop = sh->key_pressed(sh->key_ctx);
        }
        dt = sched_ticks() - t0;
        if (stop)
            break;
    }
    kfree(ti);
    kfree(prev);
    return 0;
}

/* ---- threads ---------------------------------------------------------------- */

static int cmd_kill(struct shell *sh, int argc, char **argv)
{
    uint64_t tid;
    if (argc < 2 || !parse_uint(argv[1], &tid)) {
        P("usage: kill <TID>   (see the TIDs with ps)\n");
        return 1;
    }
    struct thread *me = thread_current();
    const char *refused = NULL;
    proc_status_t st = PROC_ERR_INVALID;
    uint64_t f = rcu_read_lock();
    struct thread *t = thread_find((tid_t)tid);
    if (t == NULL)
        refused = "there is no thread with that number";
    else if (t->flags & THREAD_IDLE)
        refused = "that is a CPU's idle thread; the system needs it";
    else if (t->tid == 0)
        refused = "that is the kernel's main thread; the system needs it";
    else if (t == me)
        refused = "that is this terminal";
    else
        st = thread_terminate(t);
    rcu_read_unlock(f);
    if (refused != NULL) {
        P("kill %lu: %s\n", tid, refused);
        return 1;
    }
    if (st != PROC_OK) {
        P("kill %lu: %s\n", tid, st == PROC_ERR_BAD_STATE ? "it has already finished" : proc_status_str(st));
        return 1;
    }
    P("Thread %lu stopped.\n", tid);
    return 0;
}

static int64_t job_spin(void *arg)
{
    uint64_t until = clock_now_ns() + (uint64_t)arg * 1000000000ull;
    volatile uint64_t x = 0;
    while (clock_now_ns() < until)
        for (unsigned i = 0; i < 10000; i++)
            x += i;
    return 0;
}

static int64_t job_hello(void *arg)
{
    (void)arg;
    kprintf("\nHello from a background thread on CPU %u!\n", cpu_id());
    return 0;
}

static int cmd_run(struct shell *sh, int argc, char **argv)
{
    uint64_t secs = 5;
    if (argc < 2 || (argc > 2 && (!parse_uint(argv[2], &secs) || secs < 1 || secs > 600))) {
        P("usage: run <job> [seconds 1-600]\n");
        P("jobs:  spin   one busy thread\n");
        P("       load   one busy thread per CPU (watch them spread with cpus or top)\n");
        P("       hello  a thread that prints a greeting and ends\n");
        return 1;
    }
    unsigned count;
    thread_entry_t entry;
    if (strcmp(argv[1], "spin") == 0) {
        count = 1;
        entry = job_spin;
    } else if (strcmp(argv[1], "load") == 0) {
        count = smp_cpu_count();
        entry = job_spin;
    } else if (strcmp(argv[1], "hello") == 0) {
        count = 1;
        entry = job_hello;
    } else {
        P("run: there is no job called '%s' (try: run)\n", argv[1]);
        return 1;
    }
    for (unsigned i = 0; i < count; i++) {
        proc_status_t err;
        struct thread *t = thread_create(process_kernel(), argv[1], entry, (void *)secs, &err);
        if (t == NULL) {
            P("run: cannot start a thread: %s\n", proc_status_str(err));
            return 1;
        }
        if (entry == job_hello)
            P("Started '%s' as thread %u.\n", argv[1], t->tid);
        else
            P("Started '%s' as thread %u for %lu s.\n", argv[1], t->tid, secs);
    }
    return 0;
}

/* ---- health ----------------------------------------------------------------- */

static struct waitq check_wq = WAITQ_INIT;
static volatile bool check_flag, check_woke;

static bool check_cond(void *arg)
{
    (void)arg;
    return check_flag;
}

static int64_t check_waiter(void *arg)
{
    (void)arg;
    waitq_wait(&check_wq, check_cond, NULL);
    check_woke = true;
    return 0;
}

static volatile uint32_t answered;

static void count_answer(void *arg)
{
    (void)arg;
    __atomic_fetch_add(&answered, 1, __ATOMIC_SEQ_CST);
}

static bool live_file_check(void)
{
    const char *p = "/.checks-tmp";
    char buf[8];
    size_t n = 0;
    bool ok = fs_write_file(p, "check", 5, false) == FS_OK
              && fs_read_file(p, buf, sizeof(buf), &n) == FS_OK && n == 5 && memcmp(buf, "check", 5) == 0;
    return fs_unlink(p) == FS_OK && ok;
}

static bool live_wait_check(void)
{
    check_flag = false;
    check_woke = false;
    proc_status_t err;
    if (thread_create(process_kernel(), "checkwait", check_waiter, NULL, &err) == NULL)
        return false;
    for (unsigned i = 0; i < 50 && waitq_sleepers(&check_wq) == 0; i++)
        sched_sleep(1);
    bool slept = waitq_sleepers(&check_wq) == 1;
    check_flag = true;
    waitq_wake_all(&check_wq);
    for (unsigned i = 0; i < 100 && !check_woke; i++)
        sched_sleep(1);
    return slept && check_woke;
}

static int cmd_checks(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    const struct selftest_suite *suites;
    unsigned ns = selftest_suites(&suites), bp, bf;
    selftest_totals(&bp, &bf);
    P("Boot self-tests (run at every start):\n");
    for (unsigned i = 0; i < ns; i++)
        P("  %-24s %4u passed%s\n", suites[i].name, suites[i].passed,
          suites[i].failed ? ", SOME FAILED" : "");
    P("  %-24s %4u passed, %u failed\n", "Total", bp, bf);

    P("Live checks, now:\n");
    unsigned passed = 0, total = 0;
    struct {
        const char *what;
        bool ok;
    } live[8];
    unsigned cpus = smp_cpu_count();
    answered = 0;
    smp_call(smp_online_mask(), count_answer, NULL, false);
    live[0].what = "kernel heap is consistent";               live[0].ok = heap_check();
    live[1].what = "physical page allocator is consistent";   live[1].ok = pmm_check();
    live[2].what = "scheduler queues are consistent";         live[2].ok = sched_check();
    live[3].what = "file tree is consistent";                 live[3].ok = fs_check();
    live[4].what = "a file can be written, read and deleted"; live[4].ok = live_file_check();
    live[5].what = "a thread can sleep and be woken";          live[5].ok = live_wait_check();
    live[6].what = "the lock-order checker is learning";      live[6].ok = lockcheck_order_count() > 0;
    live[7].what = "every other CPU answers a request";       live[7].ok = answered == cpus - 1;
    for (unsigned i = 0; i < 8; i++) {
        P("  [%s] %s\n", live[i].ok ? "ok" : "!!", live[i].what);
        passed += live[i].ok;
        total++;
    }
    if (passed == total && bf == 0) {
        P("All good: %u boot checks and %u live checks passed.\n", bp, total);
        return 0;
    }
    P("Problems found: %u of %u live checks failed.\n", total - passed, total);
    return 1;
}

static struct spinlock demo_a = SPINLOCK_INIT(LOCK_CLASS_DEMO_A);
static struct spinlock demo_b = SPINLOCK_INIT(LOCK_CLASS_DEMO_B);

__attribute__((noinline)) static void demo_lock_pair(struct spinlock *first, struct spinlock *second)
{
    uint64_t f;
    spin_lock_irqsave(first, &f);
    spin_lock(second);
    spin_unlock(second);
    spin_unlock_irqrestore(first, f);
}

static int cmd_lockdemo(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    P("Every lock has a class. The checker records the order classes are taken in.\n");
    lockcheck_forget(LOCK_CLASS_DEMO_A);
    lockcheck_forget(LOCK_CLASS_DEMO_B);
    bool was_fatal = lockcheck_set_fatal(false);
    uint64_t v0 = lockcheck_violations();
    P("1. Take lock A, then lock B: recorded as \"A before B\".\n");
    demo_lock_pair(&demo_a, &demo_b);
    P("2. Take lock B, then lock A: the opposite order. If two CPUs did 1 and 2\n"
      "   at the same moment, each would wait for the other forever (deadlock).\n");
    demo_lock_pair(&demo_b, &demo_a);
    uint64_t caught = lockcheck_violations() - v0;
    lockcheck_set_fatal(was_fatal);
    lockcheck_forget(LOCK_CLASS_DEMO_A);
    lockcheck_forget(LOCK_CLASS_DEMO_B);
    if (caught == 1) {
        P("The checker caught it (report above), although no deadlock happened.\n"
          "Normally this stops the system at once; the demo only reports it.\n");
        return 0;
    }
    P("The checker did not report the mistake: that is a bug.\n");
    return 1;
}

static int64_t crash_thread(void *arg)
{
    thread_current()->flags |= THREAD_CONTAIN;
    switch ((uintptr_t)arg) {
    case 1:
        __asm__ volatile("xor %%ecx, %%ecx; xor %%edx, %%edx; mov $1, %%eax; div %%ecx"
                         : : : "rax", "rcx", "rdx");
        break;
    case 2:
        __asm__ volatile("ud2");
        break;
    default:
        __asm__ volatile("movq $0, %%rax; movq (%%rax), %%rax" : : : "rax", "memory");
        break;
    }
    return 0;                                  /* never reached */
}

static int cmd_crashdemo(struct shell *sh, int argc, char **argv)
{
    uintptr_t kind = 0;
    const char *what = "read memory at address 0 (a null pointer)";
    if (argc > 1 && strcmp(argv[1], "divide") == 0) {
        kind = 1;
        what = "divide by zero";
    } else if (argc > 1 && strcmp(argv[1], "opcode") == 0) {
        kind = 2;
        what = "run an invalid instruction";
    } else if (argc > 1 && strcmp(argv[1], "null") != 0) {
        P("usage: crashdemo [null|divide|opcode]\n");
        return 1;
    }
    struct crash_info before, after;
    crash_get_last(&before);
    proc_status_t err;
    struct thread *t = thread_create(process_kernel(), "crashdemo", crash_thread, (void *)kind, &err);
    if (t == NULL) {
        P("crashdemo: cannot start a thread: %s\n", proc_status_str(err));
        return 1;
    }
    tid_t tid = t->tid;
    P("Started thread %u. It will %s.\n", tid, what);
    for (unsigned i = 0; i < 200; i++) {
        crash_get_last(&after);
        if (after.count > before.count)
            break;
        sched_sleep(1);
    }
    if (after.count > before.count && after.tid == tid) {
        P("The crash was contained: thread %u was stopped and its report printed;\n"
          "everything else is still running (try: uptime, cpus).\n", tid);
        return 0;
    }
    P("crashdemo: the thread did not crash as expected.\n");
    return 1;
}

/* ---- the rest --------------------------------------------------------------- */

static int cmd_about(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    unsigned bp, bf;
    selftest_totals(&bp, &bf);
    struct pmm_stats ps;
    pmm_get_stats(&ps);
    P("BharatOS V1 core: keyboard, mouse, files in memory, login and terminal\n");
    P("An operating system written from scratch for x86-64 PCs, in C and assembly.\n");
    P("  CPUs:        %u, each with its own run queue; idle CPUs steal work\n", smp_cpu_count());
    P("  Memory:      %lu MiB; buddy page allocator, slab caches, demand paging\n",
      ps.total_bytes / (1024 * 1024));
    P("  Scheduler:   preemptive, tickless (no timer interrupt when idle)\n");
    P("  Safety:      lock-order checker, heap poisoning, stack canaries,\n"
      "               crash reports with function names\n");
    P("  Self-checks: %u passed at this boot\n", bp);
    return 0;
}

static int cmd_reboot(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    P("Restarting...\n");
    sched_sleep(SCHED_TIMER_HZ / 10);         /* let the text reach the screen */
    outb(0x64, 0xFE);                           /* keyboard controller: reset line */
    clock_delay_ns(100000000);
    /* Still here: an empty interrupt table makes the next interrupt reset
     * the CPU (triple fault). */
    struct __attribute__((packed)) { uint16_t limit; uint64_t base; } none = { 0, 0 };
    __asm__ volatile("cli; lidt %0; int3" : : "m"(none));
    return 1;
}

static int cmd_logout(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    sh->logout = true;
    return 0;
}

static int cmd_help(struct shell *sh, int argc, char **argv);

struct command {
    const char *name;
    int (*fn)(struct shell *sh, int argc, char **argv);
    const char *help;
};

static const struct command commands[] = {
    { "help",      cmd_help,      "this list" },
    { "ls",        cmd_ls,        "list a folder (ls, ls /etc)" },
    { "cd",        cmd_cd,        "go to a folder (cd .., cd /home; cd alone: home)" },
    { "pwd",       cmd_pwd,       "show the folder you are in" },
    { "cat",       cmd_cat,       "show a file (cat /etc/motd)" },
    { "echo",      cmd_echo,      "print text; echo hi > a.txt writes it to a file" },
    { "mkdir",     cmd_mkdir,     "make a folder" },
    { "rm",        cmd_rm,        "delete a file or an empty folder" },
    { "touch",     cmd_touch,     "make an empty file" },
    { "clear",     cmd_clear,     "clear the screen" },
    { "whoami",    cmd_whoami,    "show your name" },
    { "uptime",    cmd_uptime,    "time since the system started" },
    { "mem",       cmd_mem,       "memory and file space in use" },
    { "cpus",      cmd_cpus,      "what each CPU is doing" },
    { "ps",        cmd_ps,        "list the running threads" },
    { "top",       cmd_top,       "live view of CPUs and busy threads" },
    { "kill",      cmd_kill,      "stop a thread (kill <TID>)" },
    { "run",       cmd_run,       "start a background job (run spin, run load)" },
    { "checks",    cmd_checks,    "run the system's self-checks" },
    { "lockdemo",  cmd_lockdemo,  "show the lock-order checker catching a mistake" },
    { "crashdemo", cmd_crashdemo, "crash a thread on purpose and show the report" },
    { "about",     cmd_about,     "about BharatOS" },
    { "reboot",    cmd_reboot,    "restart the machine" },
    { "logout",    cmd_logout,    "log out (back to the login prompt)" },
};

#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

static int cmd_help(struct shell *sh, int argc, char **argv)
{
    (void)argc; (void)argv;
    P("Commands:\n");
    for (unsigned i = 0; i < NCOMMANDS; i++)
        P("  %-10s %s\n", commands[i].name, commands[i].help);
    P("Send any command's output to a file with > (replace) or >> (add).\n");
    return 0;
}

/* ---- running a line ----------------------------------------------------------- */

/* Split into words. Double quotes keep spaces; '>' and '>>' are words of
 * their own. Returns the count, or -1 if there are too many words. */
static int split(char *line, char *buf, size_t bufsize, char **argv)
{
    int argc = 0;
    size_t o = 0;
    char *p = line;
    for (;;) {
        while (*p == ' ')
            p++;
        if (*p == '\0')
            break;
        if (argc == SHELL_MAX_ARGS)
            return -1;
        argv[argc++] = buf + o;
        if (*p == '>') {
            buf[o++] = *p++;
            if (*p == '>')
                buf[o++] = *p++;
            buf[o++] = '\0';
            continue;
        }
        bool quoted = false;
        while (*p != '\0' && (quoted || (*p != ' ' && *p != '>')) && o + 1 < bufsize) {
            if (*p == '"')
                quoted = !quoted;
            else
                buf[o++] = *p;
            p++;
        }
        buf[o++] = '\0';
    }
    return argc;
}

int shell_run(struct shell *sh, const char *line)
{
    char copy[LE_MAX], words[LE_MAX * 2];
    char *argv[SHELL_MAX_ARGS + 1];
    strlcpy(copy, line, sizeof(copy));
    int argc = split(copy, words, sizeof(words), argv);
    if (argc < 0) {
        P("Too many words (at most %d).\n", SHELL_MAX_ARGS);
        return sh->status = 1;
    }
    if (argc == 0)
        return sh->status = 0;

    /* Redirection: "... > file" or "... >> file", at the end. */
    const char *target = NULL;
    bool append = false;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] != '>')
            continue;
        if (i + 2 != argc || i == 0) {
            P("Use > or >> once, at the end: echo hi > file.txt\n");
            return sh->status = 1;
        }
        append = argv[i][1] == '>';
        target = argv[i + 1];
        argc = i;
    }
    argv[argc] = NULL;

    const struct command *cmd = NULL;
    for (unsigned i = 0; i < NCOMMANDS && cmd == NULL; i++)
        if (strcmp(commands[i].name, argv[0]) == 0)
            cmd = &commands[i];
    if (cmd == NULL) {
        P("%s: unknown command (type 'help' to see the commands)\n", argv[0]);
        return sh->status = 127;
    }

    char path[FS_PATH_MAX];
    if (target != NULL) {
        if (!resolve(sh, target, path))
            return sh->status = 1;
        sh->cap = kmalloc(SHELL_CAPTURE_MAX);
        if (sh->cap == NULL) {
            P("Not enough memory to redirect the output.\n");
            return sh->status = 1;
        }
        sh->cap_len = 0;
        sh->cap_full = false;
    }
    int rc = cmd->fn(sh, argc, argv);
    if (target != NULL) {
        char *data = sh->cap;
        size_t len = sh->cap_len;
        bool full = sh->cap_full;
        sh->cap = NULL;
        fs_status_t st = fs_write_file(path, data, len, append);
        kfree(data);
        if (st != FS_OK)
            rc = fail(sh, target, st);
        else if (full)
            P("(output cut at %u KiB)\n", SHELL_CAPTURE_MAX / 1024);
    }
    return sh->status = rc;
}

void shell_init(struct shell *sh, const char *user, le_output_t out, void *ctx)
{
    memset(sh, 0, sizeof(*sh));
    strlcpy(sh->user, user, sizeof(sh->user));
    sh->out = out;
    sh->out_ctx = ctx;
    fs_mkdir("/home");
    ksnprintf(sh->home, sizeof(sh->home), "/home/%s", sh->user);
    fs_status_t st = fs_mkdir(sh->home);
    if (st != FS_OK && st != FS_ERR_EXISTS)
        strlcpy(sh->home, "/", sizeof(sh->home));
    strlcpy(sh->cwd, sh->home, sizeof(sh->cwd));
}

void shell_prompt(const struct shell *sh, char *buf, size_t size)
{
    size_t hl = strlen(sh->home);
    if (hl > 1 && strncmp(sh->cwd, sh->home, hl) == 0 && (sh->cwd[hl] == '\0' || sh->cwd[hl] == '/'))
        ksnprintf(buf, size, "%s@bharat:~%s$ ", sh->user, sh->cwd + hl);
    else
        ksnprintf(buf, size, "%s@bharat:%s$ ", sh->user, sh->cwd);
}
