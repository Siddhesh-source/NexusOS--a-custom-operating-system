/* Unit tests for kernel/sched/sched.c (ready queue, sleep list, quantum,
 * idle fallback), run on the build machine.
 *
 * proc.c is replaced by a small fake that keeps the same contract: every
 * state change goes through sched_state_changed(), and thread_switch()
 * makes the old thread READY (if it was RUNNING) and the new one RUNNING.
 * There is no real stack switch: after schedule() returns, the test simply
 * acts as whichever thread is now current.
 *
 * The clock is fake too: time only moves when the test calls advance(),
 * which fires the scheduler's one-shot alarm whenever its deadline is
 * passed, like the APIC timer would. So the tests can also check that the
 * scheduler is tickless: no alarm when there is nothing to do. */
#include "unit.h"
#include "sched.h"
#include "clock.h"
#include "proc.h"
#include "kprintf.h"
#include "string.h"

#define NTHREADS 12           /* test threads besides boot and idle */
#define RANDOM_OPS 50000

static struct thread threads[NTHREADS + 2];   /* [0] boot, [1] idle */
static struct thread *current;
static unsigned created;
static struct process kernel_proc;

/* ---- fake clock.c ------------------------------------------------------- */

static uint64_t fake_now;
static bool alarm_armed;
static uint64_t alarm_at;
static uint64_t alarms_fired;

bool clock_ready(void) { return true; }
uint64_t clock_now_ns(void) { return fake_now; }
void clock_cancel_alarm(void) { alarm_armed = false; }

void clock_set_alarm(uint64_t deadline_ns)
{
    alarm_armed = true;
    alarm_at = deadline_ns;
}

/* Let `ns` of time pass, firing the alarm each time its deadline comes. */
static void advance(uint64_t ns)
{
    uint64_t target = fake_now + ns;
    while (alarm_armed && alarm_at <= target) {
        if (alarm_at > fake_now)
            fake_now = alarm_at;
        alarm_armed = false;          /* one-shot */
        alarms_fired++;
        sched_timer_event();
    }
    fake_now = target;
}

/* One old-style "tick" worth of time. */
static void tick(void)
{
    advance(SCHED_TICK_NS);
}

/* ---- fake proc.c -------------------------------------------------------- */

struct thread *thread_current(void) { return current; }
struct process *process_kernel(void) { return &kernel_proc; }
unsigned proc_reap(void) { return 0; }

struct thread *thread_at(unsigned slot)
{
    return slot < created ? &threads[slot] : NULL;
}

const char *task_state_str(task_state_t s)
{
    static const char *const names[] = { "NEW", "READY", "RUNNING", "BLOCKED", "TERMINATED" };
    return (unsigned)s < 5 ? names[s] : "?";
}

proc_status_t thread_set_state(struct thread *t, task_state_t state)
{
    task_state_t from = t->state;
    t->state = state;
    sched_state_changed(t, from, state);
    return PROC_OK;
}

proc_status_t thread_switch(struct thread *next)
{
    if (next->state != TASK_READY) {
        UCHECK(0, "thread_switch to TID %u in state %s", next->tid, task_state_str(next->state));
        return PROC_ERR_BAD_STATE;
    }
    if (current->state == TASK_RUNNING)
        thread_set_state(current, TASK_READY);
    thread_set_state(next, TASK_RUNNING);
    next->switch_count++;
    current = next;
    return PROC_OK;
}

static void init_thread(struct thread *t, unsigned slot, const char *name)
{
    memset(t, 0, sizeof(*t));
    t->tid = slot;
    t->state = TASK_NEW;
    for (unsigned i = 0; name[i] && i < TASK_NAME_LEN - 1; i++)
        t->name[i] = name[i];
}

static struct thread *new_thread(const char *name)
{
    struct thread *t = &threads[created];
    init_thread(t, created, name);
    created++;
    return t;
}

struct thread *thread_create(struct process *p, const char *name, thread_entry_t entry,
                             void *arg, proc_status_t *err)
{
    (void)p; (void)entry; (void)arg; (void)err;
    struct thread *t = new_thread(name);
    thread_set_state(t, TASK_READY);
    return t;
}

/* ---- helpers ------------------------------------------------------------ */

/* A new READY thread, reusing the slot of an exited one when the table
 * is full. Returns NULL if every slot is in use. */
static struct thread *spawn(void)
{
    struct thread *t = NULL;
    if (created < NTHREADS + 2) {
        t = new_thread("worker");
    } else {
        for (unsigned i = 2; i < created && t == NULL; i++)
            if (threads[i].state == TASK_TERMINATED) {
                t = &threads[i];
                init_thread(t, i, "worker");
            }
    }
    if (t != NULL)
        thread_set_state(t, TASK_READY);
    return t;
}

/* The thread that is current exits (as thread_exit would). */
static void exit_current(void)
{
    thread_set_state(current, TASK_TERMINATED);
    sched_reschedule();
}

/* TID 0 is the running boot flow; sched_init creates idle as TID 1. */
static void boot(void)
{
    current = new_thread("kmain");
    current->state = TASK_RUNNING;
    sched_init();
    sched_start();
}

/* ---- tests -------------------------------------------------------------- */

static void test_round_robin(void)
{
    /* Three threads that never block: each must run exactly one quantum
     * in turn, in FIFO order, forever. */
    struct thread *a = spawn(), *b = spawn(), *c = spawn();
    struct thread *order[4] = { current, a, b, c };
    unsigned next = 1;
    for (unsigned n = 1; n <= 400; n++) {
        struct thread *before = current;
        tick();
        if (n % SCHED_QUANTUM_TICKS == 0) {
            UCHECK(current == order[next % 4], "tick %u: expected TID %u, got TID %u",
                   n, order[next % 4]->tid, current->tid);
            next++;
        } else {
            UCHECK(current == before, "tick %u: switched before the quantum ended", n);
        }
        UCHECK(sched_check(), "tick %u: queue invariants", n);
    }
    /* Each worker exits when it next runs; then only boot is left. */
    while (current != order[0])
        exit_current();
    for (unsigned i = 1; i < 4; i++) {
        while (order[i]->state != TASK_TERMINATED) {
            sched_yield();
            if (current != order[0])
                exit_current();
        }
    }
    struct sched_stats s;
    sched_get_stats(&s);
    UCHECK(current == order[0] && s.runnable == 0 && sched_check(),
           "queue empty again after the workers exit");
}

static void test_sleep_and_idle(void)
{
    struct thread *boot_thread = current;
    struct sched_stats s0;
    sched_get_stats(&s0);

    uint64_t fired0 = alarms_fired;
    sched_sleep(5);               /* only thread: CPU goes to idle */
    UCHECK(current == sched_idle_thread(), "sleeping alone switches to idle");
    for (unsigned i = 0; i < 4; i++) {
        tick();
        UCHECK(current == sched_idle_thread(), "still idle at tick %u of the sleep", i + 1);
    }
    tick();                 /* 5th tick: woken and run at once */
    UCHECK(current == boot_thread, "sleeper runs again after exactly 5 ticks");
    /* Lazy re-arming: an alarm left from before the sleep (an old slice
     * end) may fire once; then only the wake-up. */
    UCHECK(alarms_fired - fired0 <= 2, "tickless: the 5-tick sleep took at most 2 alarms (got %lu)",
           alarms_fired - fired0);
    UCHECK(!alarm_armed, "tickless: alone and with no sleepers, no alarm is armed");
    uint64_t fired_again = alarms_fired;
    sched_sleep(3);               /* nothing stale left now */
    for (unsigned i = 0; i < 3; i++)
        tick();
    UCHECK(current == boot_thread && alarms_fired - fired_again == 1,
           "tickless: a sleep with no stale alarm takes exactly one alarm (got %lu)",
           alarms_fired - fired_again);
    uint64_t fired1 = alarms_fired;
    for (unsigned i = 0; i < 100; i++)
        tick();
    UCHECK(alarms_fired == fired1 && current == boot_thread,
           "tickless: a second of running alone causes no timer interrupts");

    struct sched_stats s1;
    sched_get_stats(&s1);
    UCHECK(s1.idle_ticks - s0.idle_ticks == 8, "8 idle ticks counted, 5 + 3 (got %lu)",
           s1.idle_ticks - s0.idle_ticks);
    UCHECK(sched_check(), "invariants after sleep");

    sched_yield();                /* nobody else READY: keeps the CPU */
    UCHECK(current == boot_thread, "yield with nothing else READY keeps running");
}

/* Random mix of ticks, yields, sleeps, spawns and exits. After every step
 * the scheduler's own checks must pass, exactly one thread runs, and no
 * READY thread waits longer than (number READY + 1) quanta while nothing
 * blocks (checked by watching each thread's wait). */
static void test_random(uint64_t seed)
{
    unit_seed(seed);
    uint64_t waited[NTHREADS + 2] = { 0 };

    for (unsigned op = 0; op < RANDOM_OPS; op++) {
        unsigned r = (unsigned)unit_rand_below(100);
        if (r < 60) {
            tick();
        } else if (r < 70) {
            sched_yield();
        } else if (r < 78 && current != sched_idle_thread()) {
            sched_sleep(1 + unit_rand_below(6));
        } else if (r < 86) {
            spawn();
        } else if (r < 90 && current != sched_idle_thread() && current->tid != 0) {
            exit_current();
        }

        UCHECK(sched_check(), "seed %lu op %u: queue invariants", seed, op);
        UCHECK(current->state == TASK_RUNNING, "seed %lu op %u: current not RUNNING", seed, op);

        unsigned ready = 0, running = 0;
        for (unsigned i = 0; i < created; i++) {
            ready += threads[i].state == TASK_READY && &threads[i] != sched_idle_thread();
            running += threads[i].state == TASK_RUNNING;
        }
        UCHECK(running == 1, "seed %lu op %u: %u threads RUNNING", seed, op, running);
        UCHECK(ready == 0 || current != sched_idle_thread() || r >= 60,
               "seed %lu op %u: idle kept the CPU across a tick with work READY", seed, op);

        /* Starvation check: count ticks each READY thread waits. Idle is
         * READY whenever it is not running and is never queued: skip it. */
        for (unsigned i = 0; i < created; i++) {
            if (&threads[i] == sched_idle_thread())
                continue;
            if (threads[i].state == TASK_READY && r < 60)
                waited[i]++;
            else if (threads[i].state != TASK_READY)
                waited[i] = 0;
            UCHECK(waited[i] <= (uint64_t)(NTHREADS + 2) * SCHED_QUANTUM_TICKS + 1,
                   "seed %lu op %u: TID %u starved for %lu ticks", seed, op, i, waited[i]);
        }
    }
}

int unit_main(void)
{
    kprintf("[unit] sched: quantum %u ticks, %u random operations per seed\n",
            SCHED_QUANTUM_TICKS, RANDOM_OPS);
    boot();
    test_round_robin();
    test_sleep_and_idle();
    static const uint64_t seeds[] = { 7, 42, 0xc0ffee };
    for (unsigned i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
        test_random(seeds[i]);
    return unit_report("sched");
}
