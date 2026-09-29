#include "proc_test.h"
#include "proc.h"
#include "pmm.h"
#include "heap.h"
#include "cpu.h"
#include "box.h"
#include "kprintf.h"

/* context_test.asm */
uint64_t ctx_test_regs(void (*fn)(void *), void *arg, uint64_t seed);

static unsigned passed, failed;

#define CHECK(cond, desc) check((cond), (desc))

static bool check(bool ok, const char *desc)
{
    if (ok) {
        passed++;
        kprintf("[test]   PASS  %s\n", desc);
    } else {
        failed++;
        kprintf("[test]   FAIL  %s\n", desc);
    }
    return ok;
}

static void section(const char *name)
{
    kprintf("\n[test] --- %s ---\n", name);
}

/* Results shown in the final summary panel. */
static struct {
    pid_t pid_a, pid_b;
    tid_t tid_a, tid_b;
    int64_t counter_a, counter_b;
    uint64_t switches;
    bool switching_ok, preservation_ok, isolation_ok, lifecycle_ok, memory_ok;
} summary;

struct usage {
    uint64_t pmm_free;
    uint64_t heap_used;
    unsigned stacks, threads, processes;
};

static void usage_snapshot(struct usage *u)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    u->pmm_free = pmm_free_page_count();
    u->heap_used = hs.used_bytes;
    u->stacks = kstack_slots_in_use();
    u->threads = thread_count();
    u->processes = process_count();
}

static bool usage_equal(const struct usage *a, const struct usage *b)
{
    return a->pmm_free == b->pmm_free && a->heap_used == b->heap_used
        && a->stacks == b->stacks && a->threads == b->threads
        && a->processes == b->processes;
}

static bool in_stack(const struct thread *t, uint64_t addr)
{
    return addr >= t->stack.base && addr < t->stack.top;
}

static int64_t trivial_entry(void *arg)
{
    return (int64_t)(uint64_t)arg;
}

/* ---- creation ----------------------------------------------------------- */

static void test_creation(void)
{
    section("Process and thread creation");
    proc_status_t err;
    address_space_t *ks = vmm_kernel_space();

    struct process *p = process_create("proc-a", PROC_OWN_ADDRESS_SPACE, &err);
    CHECK(p != NULL && err == PROC_OK && p->pid > 0, "process receives a valid PID");
    if (p == NULL)
        return;
    CHECK(process_find(p->pid) == p, "process is registered and found by PID");
    CHECK(p->state == TASK_NEW && p->thread_count == 0, "new process is NEW with no threads");
    CHECK(p->owns_as && p->as->pml4_phys != ks->pml4_phys, "process has its own address space");
    uint64_t pa1 = 0, pa2 = 1;
    vmm_translate(ks, (uint64_t)&passed, &pa1);
    vmm_translate(p->as, (uint64_t)&passed, &pa2);
    CHECK(pa1 == pa2, "kernel half is shared into the process address space");

    struct process *q = process_create("proc-b", 0, &err);
    CHECK(q != NULL && q->pid != p->pid && q->as == ks,
          "second process: distinct PID, shares the kernel address space");
    CHECK(process_create("bad", 0x80, &err) == NULL && err == PROC_ERR_INVALID,
          "unknown process flags rejected");

    struct thread *t1 = thread_create(p, "t1", trivial_entry, NULL, &err);
    struct thread *t2 = thread_create(p, "t2", trivial_entry, NULL, &err);
    CHECK(t1 != NULL && t2 != NULL && t1->tid > 0 && t2->tid > 0 && t1->tid != t2->tid,
          "threads receive valid, distinct TIDs");
    if (t1 == NULL || t2 == NULL)
        return;
    CHECK(thread_find(t1->tid) == t1 && thread_find(t2->tid) == t2, "threads found by TID");
    CHECK(t1->state == TASK_READY && t2->state == TASK_READY, "new threads start READY");
    CHECK(t1->process == p && p->thread_count == 2 && p->threads == t1 && t1->next_in_process == t2,
          "threads are linked to their owning process");
    CHECK(p->state == TASK_READY, "process state follows its threads (READY)");

    CHECK(in_stack(t1, t1->stack.base) && t1->stack.top - t1->stack.base == KSTACK_SIZE
          && (t1->stack.top & 15) == 0 && t1->stack.base >= KSTACK_REGION_BASE,
          "kernel stack: 16 KiB, 16-byte aligned top, in the stack region");
    CHECK(t1->stack.top <= t2->stack.base || t2->stack.top <= t1->stack.base,
          "each thread has an independent, non-overlapping stack");
    CHECK(vmm_translate(ks, t1->stack.base, NULL) == VMM_OK
          && vmm_translate(ks, t1->stack.top - 8, NULL) == VMM_OK
          && vmm_translate(ks, t1->stack.base - PAGE_SIZE, NULL) == VMM_ERR_NOT_MAPPED,
          "stack pages are mapped; the page below the stack is an unmapped guard");
    CHECK(kstack_canary_intact(&t1->stack), "stack canary installed");

    const struct cpu_context *ctx = thread_get_context(t1);
    CHECK(ctx != NULL && ctx->rip == (uint64_t)thread_trampoline && ctx->r12 == (uint64_t)t1
          && ctx->rflags == RFLAGS_RESERVED_1
          && (uint64_t)ctx + sizeof(*ctx) + 16 == t1->stack.top,
          "initial context: rip=trampoline, r12=thread, IF off, at stack top");
    CHECK(thread_get_context(thread_current()) == NULL, "running thread has no saved context");
    CHECK(thread_next_ready(t1) == t2 && thread_next_ready(t2) == t1,
          "READY-thread lookup walks the table and wraps");

    CHECK(thread_create(NULL, "x", trivial_entry, NULL, &err) == NULL && err == PROC_ERR_INVALID,
          "thread in NULL process rejected");
    CHECK(thread_create(p, "x", NULL, NULL, &err) == NULL && err == PROC_ERR_INVALID,
          "thread without entry point rejected");

    struct thread *tq = thread_create(q, "tq", trivial_entry, NULL, &err);
    CHECK(tq != NULL && process_terminate(q) == PROC_OK && tq->state == TASK_TERMINATED
          && q->state == TASK_TERMINATED,
          "process_terminate terminates the process and its threads");
    CHECK(thread_create(q, "late", trivial_entry, NULL, &err) == NULL && err == PROC_ERR_BAD_STATE,
          "no new threads in a terminated process");

    kprintf("[test]   diagnostics for a freshly created thread:\n");
    thread_dump(t1);

    CHECK(process_terminate(p) == PROC_OK, "terminate process with two unstarted threads");
    pid_t pid_p = p->pid, pid_q = q->pid;
    tid_t tid1 = t1->tid;
    CHECK(proc_reap() == 3 && process_find(pid_p) == NULL && process_find(pid_q) == NULL
          && thread_find(tid1) == NULL,
          "reaping removes the terminated threads and processes");
}

/* ---- deterministic ping-pong ------------------------------------------- */

#define PP_ROUNDS 10

struct pingpong {
    const char *label;
    int64_t start;
    struct thread *self;
    struct thread *peer;
    uint64_t cr3_mismatches;
    uint64_t switch_errors;
};

static struct {
    char who;
    int64_t value;
} trace[2 * PP_ROUNDS + 4];
static unsigned trace_len;

static int64_t pingpong_entry(void *arg)
{
    struct pingpong *pp = arg;
    int64_t counter = pp->start;     /* lives on this thread's own stack */

    for (int i = 0; i < PP_ROUNDS; i++) {
        counter++;
        kprintf("[%s] counter = %ld  (TID %u, rsp in own stack)\n",
                pp->label, counter, pp->self->tid);
        if (trace_len < sizeof(trace) / sizeof(trace[0])) {
            trace[trace_len].who = pp->label[0];
            trace[trace_len].value = counter;
            trace_len++;
        }
        if ((read_cr3() & ~PAGE_MASK) != pp->self->process->as->pml4_phys)
            pp->cr3_mismatches++;
        if (thread_switch(pp->peer) != PROC_OK)
            pp->switch_errors++;
    }
    return counter;
}

static void test_pingpong(void)
{
    section("Context switching: A <-> B ping-pong");
    proc_status_t err;
    struct usage before, after;
    usage_snapshot(&before);

    struct process *pa = process_create("demo-a", PROC_OWN_ADDRESS_SPACE, &err);
    struct process *pb = process_create("demo-b", PROC_OWN_ADDRESS_SPACE, &err);
    static struct pingpong a = { .label = "A", .start = 0 };
    static struct pingpong b = { .label = "B", .start = 100 };
    kprintf("[NEXUS] Creating thread A\n");
    a.self = thread_create(pa, "thread-A", pingpong_entry, &a, &err);
    kprintf("[NEXUS] Creating thread B\n");
    b.self = thread_create(pb, "thread-B", pingpong_entry, &b, &err);
    if (!CHECK(pa && pb && a.self && b.self, "create two processes, one thread each"))
        return;
    a.peer = b.self;
    b.peer = a.self;

    summary.pid_a = pa->pid;
    summary.pid_b = pb->pid;
    summary.tid_a = a.self->tid;
    summary.tid_b = b.self->tid;

    kprintf("\n[NEXUS] Process table before the first switch:\n");
    proc_dump_table();
    kprintf("\n");

    /* kmain -> A; A and B alternate; A's final return exits back to kmain,
     * then kmain resumes B for its final return. */
    uint64_t cr3_kernel = read_cr3();
    CHECK(thread_switch(a.self) == PROC_OK, "kmain switches to thread A and is resumed");
    CHECK(read_cr3() == cr3_kernel, "kernel address space restored on return to kmain");
    CHECK(a.self->state == TASK_TERMINATED && a.self->exit_code == PP_ROUNDS
          && b.self->state == TASK_READY,
          "A finished all rounds and exited; B is suspended READY");
    CHECK(thread_switch(b.self) == PROC_OK && b.self->state == TASK_TERMINATED
          && b.self->exit_code == 100 + PP_ROUNDS,
          "B resumes where it stopped and finishes");

    bool exact = trace_len == 2 * PP_ROUNDS;
    for (unsigned i = 0; exact && i < trace_len; i++) {
        char who = (i % 2 == 0) ? 'A' : 'B';
        int64_t want = (i % 2 == 0 ? 0 : 100) + (int64_t)(i / 2) + 1;
        exact = trace[i].who == who && trace[i].value == want;
    }
    CHECK(exact, "trace is exactly A1 B101 A2 B102 ... A10 B110");
    CHECK(a.cr3_mismatches == 0 && b.cr3_mismatches == 0,
          "each thread always ran in its own process's address space");
    CHECK(a.switch_errors == 0 && b.switch_errors == 0, "every switch succeeded");

    summary.counter_a = a.self->exit_code;
    summary.counter_b = b.self->exit_code;
    summary.switches = a.self->switch_count + b.self->switch_count;
    summary.switching_ok = exact && a.cr3_mismatches == 0 && b.cr3_mismatches == 0
                        && a.switch_errors == 0 && b.switch_errors == 0;

    proc_reap();
    usage_snapshot(&after);
    CHECK(usage_equal(&before, &after), "reaping returns all stacks, structures and PML4s");
}

/* ---- register preservation --------------------------------------------- */

#define REG_ROUNDS 25

struct regtest {
    struct thread *peer;
    uint64_t seed;
    bool interrupts_off;     /* keep IF clear across switches */
    uint64_t bad_mask;
    unsigned if_errors;
    unsigned rounds;
};

static void switch_to(void *arg)
{
    thread_switch((struct thread *)arg);
}

static int64_t regtest_entry(void *arg)
{
    struct regtest *r = arg;
    for (int i = 0; i < REG_ROUNDS; i++) {
        if (r->interrupts_off)
            __asm__ volatile("cli");
        else
            __asm__ volatile("sti");

        r->bad_mask |= ctx_test_regs(switch_to, r->peer, r->seed + (uint64_t)i * 0x9E3779B97F4A7C15ULL);

        uint64_t rflags;
        __asm__ volatile("pushfq; popq %0" : "=r"(rflags));
        if (((rflags & RFLAGS_IF) != 0) == r->interrupts_off)
            r->if_errors++;
        r->rounds++;
    }
    __asm__ volatile("sti");
    return 0;
}

static void test_registers(void)
{
    section("Context preservation: callee-saved registers and RFLAGS.IF");
    proc_status_t err;
    struct process *p = process_create("regtest", 0, &err);
    static struct regtest x = { .seed = 0x1111222233334444ULL, .interrupts_off = true };
    static struct regtest y = { .seed = 0xAAAABBBBCCCCDDDDULL, .interrupts_off = false };
    struct thread *tx = thread_create(p, "reg-x", regtest_entry, &x, &err);
    struct thread *ty = thread_create(p, "reg-y", regtest_entry, &y, &err);
    if (!CHECK(tx && ty, "create two register-test threads"))
        return;
    x.peer = ty;
    y.peer = tx;

    thread_switch(tx);
    thread_switch(ty);

    CHECK(x.rounds == REG_ROUNDS && y.rounds == REG_ROUNDS,
          "both threads completed every round (execution resumed correctly)");
    CHECK(x.bad_mask == 0 && y.bad_mask == 0,
          "rbx, rbp, r12-r15 preserved across 50 switches with different values per thread");
    CHECK(x.if_errors == 0 && y.if_errors == 0,
          "each thread keeps its own interrupt-enable flag across switches");
    if (x.bad_mask || y.bad_mask)
        kprintf("[test]   corrupted register mask: x=%lx y=%lx\n", x.bad_mask, y.bad_mask);

    summary.preservation_ok = x.bad_mask == 0 && y.bad_mask == 0
                           && x.if_errors == 0 && y.if_errors == 0
                           && x.rounds == REG_ROUNDS && y.rounds == REG_ROUNDS;
    process_terminate(p);
    proc_reap();
}

/* ---- stress: a ring of threads with interrupts enabled ----------------- */

#define RING_SIZE  3
#define RING_LAPS  20000

struct ring_member {
    struct thread *next;
    uint64_t seed;
    uint64_t bad_mask;
    uint64_t laps;
    uint64_t order_errors;
};

static struct ring_member ring[RING_SIZE];
static unsigned ring_last;          /* index of the member that ran last */

static int64_t ring_entry(void *arg)
{
    struct ring_member *m = arg;
    unsigned me = (unsigned)(m - ring);
    uint64_t local_laps = 0;         /* must survive every switch */

    for (uint64_t lap = 0; lap < RING_LAPS; lap++) {
        /* Strict ring order: the member before us must have run last. */
        if (lap > 0 && ring_last != (me + RING_SIZE - 1) % RING_SIZE)
            m->order_errors++;
        ring_last = me;
        local_laps++;
        m->bad_mask |= ctx_test_regs(switch_to, m->next, m->seed ^ lap);
    }
    m->laps = local_laps;
    return 0;
}

static void test_stress(void)
{
    section("Stress: 3-thread ring, 60000 switches, interrupts enabled");
    proc_status_t err;
    struct process *p = process_create("ring", 0, &err);
    struct thread *t[RING_SIZE];
    for (unsigned i = 0; i < RING_SIZE; i++) {
        ring[i] = (struct ring_member){ .seed = 0xC0FFEE00ULL * (i + 1) };
        t[i] = thread_create(p, "ring", ring_entry, &ring[i], &err);
    }
    if (!CHECK(t[0] && t[1] && t[2], "create a ring of three threads"))
        return;
    for (unsigned i = 0; i < RING_SIZE; i++)
        ring[i].next = t[(i + 1) % RING_SIZE];
    ring_last = RING_SIZE - 1;

    extern volatile uint64_t timer_ticks;
    uint64_t ticks0 = timer_ticks;

    /* kmain -> t0; the ring runs; each member's final return exits to
     * kmain, which resumes whichever members are still suspended. */
    for (unsigned i = 0; i < RING_SIZE; i++) {
        if (t[i]->state == TASK_READY)
            thread_switch(t[i]);
    }
    uint64_t ticks = timer_ticks - ticks0;

    bool done = true, regs = true, order = true;
    uint64_t total = 0;
    for (unsigned i = 0; i < RING_SIZE; i++) {
        done &= t[i]->state == TASK_TERMINATED && ring[i].laps == RING_LAPS;
        regs &= ring[i].bad_mask == 0;
        order &= ring[i].order_errors == 0;
        total += t[i]->switch_count;
    }
    kprintf("[test]   %lu switches into ring threads; %lu timer interrupts arrived meanwhile\n",
            total, ticks);
    CHECK(done, "every member completed all laps with its local counter intact");
    CHECK(order, "execution followed the exact ring order A -> B -> C -> A");
    CHECK(regs, "callee-saved registers intact on every one of the switches");
    CHECK(total >= RING_SIZE * RING_LAPS, "thousands of switches performed");

    summary.preservation_ok &= done && regs && order;
    process_terminate(p);
    proc_reap();
}

/* ---- stack isolation --------------------------------------------------- */

#define ISO_WORDS  1024      /* 8 KiB of locals on a 16 KiB stack */
#define ISO_ROUNDS 20
#define ISO_DEPTH  6

struct isotest {
    struct thread *self;
    struct thread *peer;
    uint64_t seed;
    uint64_t buf_addr;
    uint64_t errors;
    uint64_t words_checked;
    bool buf_in_own_stack;
};

static inline uint64_t mix(uint64_t seed, uint64_t i, uint64_t round)
{
    uint64_t x = seed ^ (i * 0x9E3779B97F4A7C15ULL) ^ (round << 40);
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ULL;
    return x ^ (x >> 32);
}

/* Each recursion level keeps its own frame data; the deepest level
 * switches away, and every level verifies its frame on the way back up. */
static __attribute__((noinline)) uint64_t iso_recurse(struct isotest *it, int depth)
{
    volatile uint64_t frame[32];
    for (int j = 0; j < 32; j++)
        frame[j] = mix(it->seed, (uint64_t)depth, 1000 + (uint64_t)j);

    uint64_t errs = 0;
    if (depth > 0)
        errs += iso_recurse(it, depth - 1);
    else
        thread_switch(it->peer);

    for (int j = 0; j < 32; j++) {
        if (frame[j] != mix(it->seed, (uint64_t)depth, 1000 + (uint64_t)j))
            errs++;
    }
    it->words_checked += 32;
    return errs;
}

static int64_t iso_entry(void *arg)
{
    struct isotest *it = arg;
    volatile uint64_t buf[ISO_WORDS];

    it->buf_addr = (uint64_t)buf;
    it->buf_in_own_stack = in_stack(it->self, (uint64_t)buf)
                        && in_stack(it->self, (uint64_t)&buf[ISO_WORDS - 1]);

    for (uint64_t i = 0; i < ISO_WORDS; i++)
        buf[i] = mix(it->seed, i, 0);

    for (uint64_t round = 1; round <= ISO_ROUNDS; round++) {
        thread_switch(it->peer);
        for (uint64_t i = 0; i < ISO_WORDS; i++) {
            if (buf[i] != mix(it->seed, i, round - 1))
                it->errors++;
            buf[i] = mix(it->seed, i, round);
        }
        it->words_checked += ISO_WORDS;
    }

    it->errors += iso_recurse(it, ISO_DEPTH);
    return (int64_t)it->errors;
}

static void test_isolation(void)
{
    section("Stack isolation: 8 KiB of locals + recursion per thread");
    proc_status_t err;
    struct process *p = process_create("isotest", 0, &err);
    static struct isotest s1 = { .seed = 0x5EED0001ULL };
    static struct isotest s2 = { .seed = 0x5EED0002ULL };
    s1.self = thread_create(p, "iso-1", iso_entry, &s1, &err);
    s2.self = thread_create(p, "iso-2", iso_entry, &s2, &err);
    if (!CHECK(s1.self && s2.self, "create two stack-isolation threads"))
        return;
    s1.peer = s2.self;
    s2.peer = s1.self;

    thread_switch(s1.self);
    thread_switch(s2.self);

    kprintf("[test]   iso-1 locals at %p, iso-2 locals at %p\n",
            (void *)s1.buf_addr, (void *)s2.buf_addr);
    CHECK(s1.buf_in_own_stack && s2.buf_in_own_stack && s1.buf_addr != s2.buf_addr,
          "each thread's locals live inside its own kernel stack");
    CHECK(s1.self->state == TASK_TERMINATED && s2.self->state == TASK_TERMINATED,
          "both threads ran to completion");
    CHECK(s1.words_checked == s2.words_checked
          && s1.words_checked == ISO_ROUNDS * ISO_WORDS + (ISO_DEPTH + 1) * 32,
          "every word was verified after every switch");
    CHECK(s1.errors == 0 && s2.errors == 0,
          "A's stack data == expected A state, B's == expected B state");
    CHECK(kstack_canary_intact(&s1.self->stack) && kstack_canary_intact(&s2.self->stack),
          "stack canaries intact after heavy stack use");
    kprintf("[test]   peak stack use: iso-1 %lu B, iso-2 %lu B of %d B\n",
            kstack_high_water(&s1.self->stack), kstack_high_water(&s2.self->stack), KSTACK_SIZE);

    summary.isolation_ok = s1.errors == 0 && s2.errors == 0 && s1.buf_in_own_stack
                        && s2.buf_in_own_stack && s1.words_checked == s2.words_checked;
    process_terminate(p);
    proc_reap();
}

/* ---- lifecycle and failure handling ------------------------------------ */

static int64_t explicit_exit_entry(void *arg)
{
    (void)arg;
    thread_exit(7);
}

static int64_t blocking_entry(void *arg)
{
    struct thread *resume = arg;
    thread_set_state(thread_current(), TASK_BLOCKED);
    thread_switch(resume);           /* suspended while BLOCKED */
    return 99;                       /* runs only after being made READY */
}

static void test_lifecycle(void)
{
    section("Lifecycle: termination, cleanup, invalid transitions");
    proc_status_t err;
    address_space_t *ks = vmm_kernel_space();
    struct thread *self = thread_current();
    bool ok = true;

    struct process *p = process_create("lifecycle", PROC_OWN_ADDRESS_SPACE, &err);
    struct thread *t = thread_create(p, "exit42", trivial_entry, (void *)42, &err);
    ok &= CHECK(thread_set_state(t, TASK_RUNNING) == PROC_ERR_BAD_STATE,
                "only thread_switch can make a thread RUNNING");
    ok &= CHECK(thread_set_state(t, TASK_BLOCKED) == PROC_ERR_BAD_STATE,
                "READY -> BLOCKED is not a valid transition");
    ok &= CHECK(thread_switch(t) == PROC_OK && t->state == TASK_TERMINATED && t->exit_code == 42,
                "thread returns from its entry: TERMINATED with exit code 42");
    ok &= CHECK(p->state == TASK_TERMINATED, "process terminates with its last thread");

    uint64_t stack_base = t->stack.base;
    tid_t tid = t->tid;
    pid_t pid = p->pid;
    ok &= CHECK(vmm_translate(ks, stack_base, NULL) == VMM_OK && kstack_canary_intact(&t->stack),
                "terminated thread keeps its stack until reaped (no premature free)");
    ok &= CHECK(thread_set_state(t, TASK_READY) == PROC_ERR_BAD_STATE,
                "a TERMINATED thread cannot be revived");
    ok &= CHECK(thread_switch(t) == PROC_ERR_BAD_STATE, "switching to a TERMINATED thread is refused");
    ok &= CHECK(proc_reap() == 1 && thread_find(tid) == NULL && process_find(pid) == NULL,
                "reap destroys the thread and its empty process");
    ok &= CHECK(vmm_translate(ks, stack_base, NULL) == VMM_ERR_NOT_MAPPED,
                "reaped thread's stack is unmapped and its frames freed");

    /* All of q's threads exist up front: a process terminates as soon as its
     * last live thread does, so it could not gain threads afterwards. */
    struct process *q = process_create("lifecycle2", 0, &err);
    struct thread *te = thread_create(q, "exit7", explicit_exit_entry, NULL, &err);
    struct thread *never = thread_create(q, "never", trivial_entry, NULL, &err);
    struct thread *tb = thread_create(q, "blocker", blocking_entry, self, &err);
    ok &= CHECK(te && never && tb, "create three threads in one process");
    if (!te || !never || !tb)
        return;

    ok &= CHECK(thread_switch(te) == PROC_OK && te->state == TASK_TERMINATED && te->exit_code == 7,
                "thread_exit(7) from inside a thread");
    ok &= CHECK(q->state == TASK_READY, "process stays alive while it has live threads");

    ok &= CHECK(thread_destroy(never) == PROC_ERR_BAD_STATE, "destroying a live thread is refused");
    ok &= CHECK(thread_terminate(never) == PROC_OK && never->state == TASK_TERMINATED
                && thread_destroy(never) == PROC_OK,
                "a never-run thread can be terminated and destroyed");

    ok &= CHECK(thread_switch(tb) == PROC_OK && tb->state == TASK_BLOCKED,
                "thread blocks itself and switches away (BLOCKED)");
    ok &= CHECK(thread_switch(tb) == PROC_ERR_BAD_STATE, "a BLOCKED thread cannot be switched to");
    ok &= CHECK(thread_set_state(tb, TASK_READY) == PROC_OK && thread_switch(tb) == PROC_OK
                && tb->state == TASK_TERMINATED && tb->exit_code == 99,
                "BLOCKED -> READY, then it resumes and finishes");
    ok &= CHECK(q->state == TASK_TERMINATED, "process terminates when its last thread exits");

    struct thread fake = { 0 };
    ok &= CHECK(thread_switch(NULL) == PROC_ERR_INVALID, "switch to NULL rejected");
    ok &= CHECK(thread_switch(&fake) == PROC_ERR_INVALID, "switch to an unregistered thread rejected");
    ok &= CHECK(thread_switch(self) == PROC_ERR_BAD_STATE, "switch to the running thread rejected");
    ok &= CHECK(thread_destroy(self) == PROC_ERR_BUSY && thread_terminate(self) == PROC_ERR_BUSY
                && thread_set_state(self, TASK_TERMINATED) == PROC_ERR_BUSY,
                "the running thread cannot be destroyed or terminated from outside");
    ok &= CHECK(process_terminate(process_kernel()) == PROC_ERR_BUSY,
                "the running thread's process cannot be terminated");

    process_terminate(q);
    proc_reap();
    summary.lifecycle_ok = ok;
}

/* ---- memory integration ------------------------------------------------ */

static void test_memory(void)
{
    section("Memory integration: stack sourcing, exhaustion, table limits");
    proc_status_t err;
    bool ok = true;
    struct usage before, after;
    usage_snapshot(&before);

    struct process *p = process_create("memtest", 0, &err);
    uint64_t free0 = pmm_free_page_count();
    struct thread *t = thread_create(p, "m", trivial_entry, NULL, &err);
    ok &= CHECK(t != NULL && free0 - pmm_free_page_count() == KSTACK_SIZE / PAGE_SIZE,
                "a thread's stack is exactly 4 PMM frames");
    pid_t memtest_pid = p->pid;
    thread_terminate(t);
    proc_reap();
    ok &= CHECK(pmm_free_page_count() == free0, "destroying the thread returns them");
    ok &= CHECK(process_find(memtest_pid) == NULL,
                "the process was reaped together with its only thread");
    /* That reap destroyed p along with its only thread; use a fresh process.
     * A stale pointer to it would be rejected by the registry check. */
    ok &= CHECK(thread_create(p, "stale", trivial_entry, NULL, &err) == NULL
                && err == PROC_ERR_INVALID,
                "a reaped (dangling) process pointer is rejected, not dereferenced");
    p = process_create("memtest2", 0, &err);

    /* Exhaust physical memory, then try to create a thread. */
    uint64_t head = 0, pg;
    while ((pg = pmm_alloc_page()) != 0) {
        *(uint64_t *)phys_to_virt(pg) = head;
        head = pg;
    }
    struct usage exhausted;
    usage_snapshot(&exhausted);
    ok &= CHECK(thread_create(p, "oom", trivial_entry, NULL, &err) == NULL && err == PROC_ERR_NO_MEMORY,
                "thread creation fails cleanly when no stack frames are left");
    ok &= CHECK(process_create("oom", PROC_OWN_ADDRESS_SPACE, &err) == NULL && err == PROC_ERR_NO_MEMORY,
                "process creation fails cleanly when no PML4 frame is left");
    struct usage after_fail;
    usage_snapshot(&after_fail);
    ok &= CHECK(usage_equal(&exhausted, &after_fail), "failed creations leak nothing");
    while ((pg = head) != 0) {
        head = *(uint64_t *)phys_to_virt(pg);
        pmm_free_page(pg);
    }

    /* Fill the thread table; every thread gets its own stack. */
    unsigned created = 0;
    uint64_t prev_top = 0;
    bool distinct = true;
    struct thread *tt;
    while ((tt = thread_create(p, "fill", trivial_entry, NULL, &err)) != NULL) {
        distinct &= tt->stack.top != prev_top;
        prev_top = tt->stack.top;
        created++;
    }
    kprintf("[test]   created %u threads before the table filled (%u live, %u stacks)\n",
            created, thread_count(), kstack_slots_in_use());
    ok &= CHECK(err == PROC_ERR_TABLE_FULL && thread_count() == THREAD_MAX,
                "thread table fills up at THREAD_MAX and reports it");
    ok &= CHECK(distinct && kstack_slots_in_use() == before.stacks + created,
                "every one of them has its own kernel stack");
    process_terminate(p);
    proc_reap();

    unsigned procs = 0;
    struct process *pp;
    while ((pp = process_create("fill", 0, &err)) != NULL) {
        process_terminate(pp);
        procs++;
    }
    ok &= CHECK(err == PROC_ERR_TABLE_FULL && process_count() == PROC_MAX,
                "process table fills up at PROC_MAX and reports it");
    proc_reap();

    usage_snapshot(&after);
    ok &= CHECK(usage_equal(&before, &after),
                "no leaks: PMM frames, heap bytes, stacks, table entries all restored");
    summary.memory_ok = ok;
    (void)procs;
}

/* ---- summary panel ------------------------------------------------------ */

static void print_summary(void)
{
    kprintf("\n");
    box_top();
    box_line("%s", "NEXUS PROCESS MANAGER - Phase 4 results");
    box_separator();
    box_line("%s", "Context Switch Test");
    box_line("%s", "");
    box_line("kmain → Thread A (PID %u, TID %u) → Thread B (PID %u, TID %u) → A ...",
             summary.pid_a, summary.tid_a, summary.pid_b, summary.tid_b);
    box_line("%s", "");
    box_line("A counter: %ld   (started at 0, %d rounds)", summary.counter_a, PP_ROUNDS);
    box_line("B counter: %ld  (started at 100, %d rounds)", summary.counter_b, PP_ROUNDS);
    box_line("switches into A and B: %lu", summary.switches);
    box_line("%s", "");
    box_line("[%s] Thread switching (exact A/B interleaving)", summary.switching_ok ? "PASS" : "FAIL");
    box_line("[%s] Context preservation (registers, RFLAGS.IF)", summary.preservation_ok ? "PASS" : "FAIL");
    box_line("[%s] Stack isolation", summary.isolation_ok ? "PASS" : "FAIL");
    box_line("[%s] Lifecycle and cleanup", summary.lifecycle_ok ? "PASS" : "FAIL");
    box_line("[%s] Memory integration, no leaks", summary.memory_ok ? "PASS" : "FAIL");
    box_separator();
    box_line("checks: %u passed, %u failed", passed, failed);
    box_bottom();
}

/* ---- fatal demos -------------------------------------------------------- */

void proc_demo_corrupt_context(void)
{
    proc_status_t err;
    struct thread *t = thread_create(process_kernel(), "victim", trivial_entry, NULL, &err);
    struct cpu_context *ctx = (struct cpu_context *)t->saved_rsp;
    kprintf("\n[demo] Corrupting TID %u's saved RIP (%p -> 0xdeadbeef) and switching to it...\n",
            t->tid, (void *)ctx->rip);
    ctx->rip = 0xdeadbeef;
    thread_switch(t);
    kprintf("[demo] ERROR: corrupted context was not detected\n");
}

static __attribute__((noinline)) uint64_t overflow(uint64_t depth)
{
    volatile uint8_t pad[512];
    pad[0] = (uint8_t)depth;
    if (depth == UINT64_MAX)   /* never true; keeps the recursion non-tail */
        return 0;
    return overflow(depth + 1) + pad[0];
}

static int64_t overflow_entry(void *arg)
{
    (void)arg;
    return (int64_t)overflow(0);
}

void proc_demo_thread_stack_overflow(void)
{
    proc_status_t err;
    struct thread *t = thread_create(process_kernel(), "overflow", overflow_entry, NULL, &err);
    kprintf("\n[demo] Thread TID %u will recurse until it hits the guard below %p...\n",
            t->tid, (void *)t->stack.base);
    thread_switch(t);
}

bool proc_run_tests(void)
{
    passed = failed = 0;
    kprintf("\n[test] ===== Phase 4 process / thread tests =====\n");

    test_creation();
    test_pingpong();
    test_registers();
    test_stress();
    test_isolation();
    test_lifecycle();
    test_memory();

    kprintf("\n[test] ===== %u passed, %u failed =====\n", passed, failed);
    kprintf("\n[NEXUS] Process table after all tests (only the kernel remains):\n");
    proc_dump_table();
    print_summary();
    return failed == 0;
}
