/* `why`: plain-English answers built from live numbers, not guesses.
 *
 *   why slow    watches the system for half a second: how busy the CPUs
 *               are, which thread used the most time, how full memory and
 *               the file space are; then says what (if anything) is the
 *               cause and what to do about it.
 *   why crash   the last contained crash: which program, what it did
 *               wrong in plain words, and what happened next.
 *
 * Rules over real data, no AI (decision D15). This is the first version;
 * the full `why` service comes with Phase 15. */

#include "why.h"
#include "shell.h"
#include "crash.h"
#include "proc.h"
#include "sched.h"
#include "percpu.h"
#include "rcu.h"
#include "pmm.h"
#include "fs.h"
#include "clock.h"
#include "heap.h"
#include "string.h"

#define P(...) shell_printf(sh, __VA_ARGS__)

struct sample {
    tid_t tid;
    char name[TASK_NAME_LEN];
    uint64_t cpu_ns;
    bool idle;
};

static unsigned take_sample(struct sample *out)
{
    unsigned n = 0;
    uint64_t f = rcu_read_lock();
    for (unsigned slot = 0; slot < THREAD_MAX; slot++) {
        struct thread *t = thread_at(slot);
        if (t == NULL)
            continue;
        out[n].tid = t->tid;
        strlcpy(out[n].name, t->name, TASK_NAME_LEN);
        out[n].cpu_ns = t->cpu_ns;
        out[n].idle = (t->flags & THREAD_IDLE) != 0;
        n++;
    }
    rcu_read_unlock(f);
    return n;
}

static int why_slow(struct shell *sh)
{
    struct sample *a = kmalloc(sizeof(struct sample) * THREAD_MAX * 2);
    if (a == NULL) {
        P("why: not enough memory to look\n");
        return 1;
    }
    struct sample *b = a + THREAD_MAX;
    uint64_t idle0[SMP_MAX_CPUS] = { 0 };
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        struct sched_stats s;
        if (cpu_at(c)->online) {
            sched_get_cpu_stats(c, &s);
            idle0[c] = s.idle_ticks;
        }
    }
    unsigned na = take_sample(a);
    uint64_t t0 = clock_now_ns(), k0 = sched_ticks();
    P("Watching the system for half a second...\n");
    sched_sleep(SCHED_TIMER_HZ / 2);
    unsigned nb = take_sample(b);
    uint64_t dt_ns = clock_now_ns() - t0, dt = sched_ticks() - k0;

    /* CPU load: the share of time each CPU was not idle. */
    unsigned cpus = 0, busy_sum = 0, busiest_cpu = 0;
    for (unsigned c = 0; c < SMP_MAX_CPUS; c++) {
        if (!cpu_at(c)->online)
            continue;
        struct sched_stats s;
        sched_get_cpu_stats(c, &s);
        uint64_t idle = s.idle_ticks - idle0[c];
        unsigned busy = dt == 0 || idle >= dt ? 0 : (unsigned)(100 - idle * 100 / dt);
        busy_sum += busy;
        busiest_cpu = busy > busiest_cpu ? busy : busiest_cpu;
        cpus++;
    }
    unsigned load = cpus ? busy_sum / cpus : 0;

    /* The thread that used the most CPU time while we watched (not idle
     * threads, not this command). */
    const struct sample *top = NULL;
    uint64_t top_ns = 0;
    tid_t me = thread_current()->tid;
    for (unsigned i = 0; i < nb; i++) {
        if (b[i].idle || b[i].tid == me)
            continue;
        uint64_t before = 0;
        for (unsigned j = 0; j < na; j++)
            if (a[j].tid == b[i].tid)
                before = a[j].cpu_ns;
        uint64_t used = b[i].cpu_ns - before;
        if (used > top_ns) {
            top_ns = used;
            top = &b[i];
        }
    }
    unsigned top_pct = dt_ns ? (unsigned)(top_ns * 100 / dt_ns) : 0;

    struct pmm_stats ps;
    struct fs_usage fu;
    pmm_get_stats(&ps);
    fs_get_usage(&fu);
    uint64_t managed = ps.managed_pages ? ps.managed_pages : 1;
    unsigned mem_used = (unsigned)(ps.allocated_pages * 100 / managed);
    unsigned files_used = fu.max_bytes ? (unsigned)(fu.bytes * 100 / fu.max_bytes) : 0;

    P("  CPUs           %u%% busy on average over %u CPU%s (busiest %u%%)\n", load, cpus,
      cpus == 1 ? "" : "s", busiest_cpu);
    if (top != NULL)
        P("  busiest thread %s (thread %u), %u%% of one CPU\n", top->name, top->tid, top_pct);
    else
        P("  busiest thread none: every thread was waiting\n");
    P("  memory         %u%% in use (%lu MiB free)\n", mem_used, ps.free_pages * 4096 / (1024 * 1024));
    P("  file space     %u%% in use\n", files_used);

    P("Answer: ");
    bool found = false;
    if (top != NULL && top_pct >= 50) {
        P("'%s' (thread %u) is keeping a CPU busy. If it should not be running,\n"
          "        stop it with: kill %u\n", top->name, top->tid, top->tid);
        found = true;
    }
    if (load >= 80) {
        P("%sall the CPUs are busy, so new work has to wait its turn.\n", found ? "        Also, " : "");
        found = true;
    }
    if (mem_used >= 90) {
        P("%smemory is nearly full; close windows or delete big files.\n", found ? "        Also, " : "");
        found = true;
    }
    if (files_used >= 90) {
        P("%sthe file space is nearly full; delete files you do not need.\n", found ? "        Also, " : "");
        found = true;
    }
    if (!found)
        P("nothing is slowing the system down right now.\n");
    kfree(a);
    return 0;
}

/* What a fault means, for someone who is not a programmer. */
static const char *fault_words(uint64_t vector, uint64_t address)
{
    switch (vector) {
    case 0:  return "it divided a number by zero";
    case 6:  return "it tried to run something that is not a valid instruction";
    case 13: return "it broke a protection rule of the CPU";
    case 14: return address < 4096 ? "it used an empty (null) pointer: it read memory at address 0"
                                   : "it touched memory that does not belong to it";
    default: return "the CPU stopped it for breaking a rule";
    }
}

static int why_crash(struct shell *sh)
{
    struct crash_info ci;
    crash_get_last(&ci);
    if (ci.count == 0) {
        P("No program has crashed since the system started.\n");
        return 0;
    }
    uint64_t ago = (clock_now_ns() - ci.time_ns) / 1000000000ull;
    P("The last crash: '%s' (thread %u), %lu s ago.\n", ci.name, ci.tid, ago);
    P("What went wrong: %s.\n", fault_words(ci.vector, ci.address));
    P("What happened next: only that thread was stopped; everything else kept\n"
      "running. The full report (registers, call trace) is in the Health app\n"
      "and the serial log. Crashes since start-up: %lu.\n", ci.count);
    return 0;
}

int why_command(struct shell *sh, int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "slow") == 0)
        return why_slow(sh);
    if (argc == 2 && strcmp(argv[1], "crash") == 0)
        return why_crash(sh);
    if (argc >= 2)
        P("why: I cannot explain '%s' yet.\n", argv[1]);
    P("usage: why slow    what is making the system slow, from live numbers\n"
      "       why crash   what the last crash was, in plain words\n");
    return 1;
}
