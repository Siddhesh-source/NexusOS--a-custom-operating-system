#include "proc.h"
#include "sched.h"
#include "heap.h"
#include "cpu.h"
#include "box.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"
#include "trace.h"
#include "slab.h"
#include "vm.h"
#include "percpu.h"
#include "spinlock.h"
#include "rcu.h"
#include "ipi.h"

/* Threads and processes come from their own slab caches (Phase 8):
 * fixed-size objects, no heap search, no fragmentation. */
static struct slab_cache *thread_cache, *process_cache;
_Static_assert(sizeof(struct thread) <= SLAB_MAX_OBJECT, "struct thread too big for a slab");
_Static_assert(sizeof(struct process) <= SLAB_MAX_OBJECT, "struct process too big for a slab");

#define PROCESS_MAGIC 0x434F5250u   /* "PROC" */
#define THREAD_MAGIC  0x44524854u   /* "THRD" */

extern char stack_bottom[], stack_top[];
extern char __text_start[], __text_end[];

/* Called from thread_trampoline (context.asm) on a new thread's first run. */
void thread_bootstrap(struct thread *t) __attribute__((noreturn));

/* Guarded by proc_lock. Readers that only look things up go without
 * it, as RCU readers (see proc.h). */
static struct process *proc_table[PROC_MAX];
static struct thread *thread_table[THREAD_MAX];
static unsigned nproc, nthread;
static pid_t next_pid;
static tid_t next_tid;
static struct spinlock proc_lock = SPINLOCK_INIT(LOCK_CLASS_PROC);

/* The kernel process and the adopted boot flow live in .bss, not the heap,
 * and are never destroyed. */
static struct process kernel_proc;
static struct thread boot_thread;

/* The running thread on this CPU is percpu_current() (percpu.h). */

/* ---- helpers ------------------------------------------------------------ */

static void copy_name(char *dst, const char *src)
{
    if (src == NULL)
        src = "unnamed";
    size_t i = 0;
    for (; i < TASK_NAME_LEN - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void *fail(proc_status_t *err, proc_status_t st, const char *what)
{
    kprintf("[proc] %s failed: %s\n", what, proc_status_str(st));
    if (err != NULL)
        *err = st;
    return NULL;
}

/* Registry membership is checked before touching the object, so a stale or
 * garbage pointer is rejected without being dereferenced. */
static bool process_valid(const struct process *p)
{
    if (p == NULL)
        return false;
    bool ok = false;
    uint64_t f = rcu_read_lock();
    for (unsigned i = 0; i < PROC_MAX; i++) {
        if (rcu_dereference(proc_table[i]) == p) {
            ok = p->magic == PROCESS_MAGIC;
            break;
        }
    }
    rcu_read_unlock(f);
    return ok;
}

static bool thread_valid(const struct thread *t)
{
    if (t == NULL)
        return false;
    bool ok = false;
    uint64_t f = rcu_read_lock();
    for (unsigned i = 0; i < THREAD_MAX; i++) {
        if (rcu_dereference(thread_table[i]) == t) {
            ok = t->magic == THREAD_MAGIC;
            break;
        }
    }
    rcu_read_unlock(f);
    return ok;
}

/* Is `t` the thread running on its CPU right now? (t's run-queue lock
 * held, so the answer cannot change underneath.) */
static bool running_somewhere(const struct thread *t)
{
    return cpu_at(t->cpu)->current == t;
}

static bool transition_allowed(task_state_t from, task_state_t to)
{
    switch (from) {
    case TASK_NEW:        return to == TASK_READY || to == TASK_TERMINATED;
    case TASK_READY:      return to == TASK_RUNNING || to == TASK_TERMINATED;
    case TASK_RUNNING:    return to == TASK_READY || to == TASK_BLOCKED || to == TASK_TERMINATED;
    case TASK_BLOCKED:    return to == TASK_READY || to == TASK_TERMINATED;
    case TASK_TERMINATED: return false;
    }
    return false;
}

/* A process's state summarises its threads. Once every thread has
 * terminated, the process is TERMINATED for good. Called with a thread's
 * run-queue lock held; takes proc_lock (order: run queue, then process
 * table). The other threads' states are read without their locks: every
 * change runs this again afterwards, so the last update sees the final
 * states. */
static void process_update_state(struct process *p)
{
    /* The kernel process always has kmain RUNNING; its summary never
     * changes, and every switch between kernel threads comes here, so
     * skip it rather than take a global lock on each switch. */
    if (p == &kernel_proc)
        return;
    SPIN_GUARD(&proc_lock);
    if (p->state == TASK_TERMINATED || p->threads == NULL)
        return;
    bool running = false, ready = false, blocked = false, fresh = false;
    for (struct thread *t = p->threads; t != NULL; t = t->next_in_process) {
        running |= t->state == TASK_RUNNING;
        ready   |= t->state == TASK_READY;
        blocked |= t->state == TASK_BLOCKED;
        fresh   |= t->state == TASK_NEW;
    }
    p->state = running ? TASK_RUNNING : ready ? TASK_READY : blocked ? TASK_BLOCKED
             : fresh ? TASK_NEW : TASK_TERMINATED;
}

/* Every thread state change goes through here, so the scheduler's queues
 * (ready queues = exactly the READY threads) can never drift out of sync.
 * Callers hold t's run-queue lock. */
static proc_status_t set_state(struct thread *t, task_state_t to)
{
    if (!transition_allowed(t->state, to)) {
        kprintf("[proc] invalid state transition for TID %u: %s -> %s\n",
                t->tid, task_state_str(t->state), task_state_str(to));
        return PROC_ERR_BAD_STATE;
    }
    task_state_t from = t->state;
    t->state = to;
    sched_state_changed(t, from, to);
    process_update_state(t->process);
    return PROC_OK;
}

static bool saved_rsp_in_bounds(const struct thread *t)
{
    uint64_t rsp = t->saved_rsp;
    return (rsp & 7) == 0 && rsp >= t->stack.base + 8
        && rsp <= t->stack.top - sizeof(struct cpu_context);
}

static void __attribute__((noreturn)) context_corrupt(const struct thread *t, const char *why)
{
    kprintf("\n[proc] FATAL context-switch error: %s\n", why);
    thread_dump(t);
    panic("corrupted thread context");
}

/* Sanity-check a suspended thread before resuming it. Any failure means the
 * CPU state we are about to load is garbage, so we stop here. */
static void validate_suspended(const struct thread *t)
{
    if (!kstack_canary_intact(&t->stack))
        context_corrupt(t, "kernel stack canary overwritten (stack overflow?)");
    if (!saved_rsp_in_bounds(t))
        context_corrupt(t, "saved stack pointer lies outside the thread's kernel stack");
    const struct cpu_context *ctx = (const struct cpu_context *)t->saved_rsp;
    if (ctx->rip < (uint64_t)__text_start || ctx->rip >= (uint64_t)__text_end)
        context_corrupt(t, "saved instruction pointer lies outside kernel text");
    if (!(ctx->rflags & RFLAGS_RESERVED_1))
        context_corrupt(t, "saved RFLAGS is malformed");
}

/* Table changes need proc_lock. The stores are atomic so lock-free
 * readers see either the old or the new pointer. */
static bool table_insert(void **table, unsigned size, void *obj)
{
    for (unsigned i = 0; i < size; i++) {
        if (table[i] == NULL) {
            rcu_assign_pointer(table[i], obj);
            return true;
        }
    }
    return false;
}

static bool table_remove(void **table, unsigned size, void *obj)
{
    for (unsigned i = 0; i < size; i++) {
        if (table[i] == obj) {
            rcu_assign_pointer(table[i], NULL);
            return true;
        }
    }
    return false;
}

/* ---- initialisation ----------------------------------------------------- */

void proc_init(void)
{
    thread_cache = slab_create("thread", sizeof(struct thread), 16, THREAD_MAX);
    process_cache = slab_create("process", sizeof(struct process), 16, PROC_MAX);
    if (thread_cache == NULL || process_cache == NULL)
        panic("proc: cannot create the thread/process slab caches");

    kernel_proc.magic = PROCESS_MAGIC;
    kernel_proc.pid = 0;
    copy_name(kernel_proc.name, "kernel");
    kernel_proc.parent = 0;
    kernel_proc.as = vmm_kernel_space();
    kernel_proc.owns_as = false;

    /* The boot stack from entry.asm; its lowest page is the guard page, so
     * the usable base (and the canary) start one page above it. */
    boot_thread.magic = THREAD_MAGIC;
    boot_thread.tid = 0;
    copy_name(boot_thread.name, "kmain");
    boot_thread.process = &kernel_proc;
    boot_thread.stack.base = (uint64_t)stack_bottom + PAGE_SIZE;
    boot_thread.stack.top = (uint64_t)stack_top;
    boot_thread.stack.slot = -1;
    *(uint64_t *)boot_thread.stack.base = KSTACK_CANARY;
    boot_thread.state = TASK_RUNNING;
    boot_thread.cpu = 0;
    boot_thread.pinned = true;           /* kmain stays on the boot CPU */
    boot_thread.ran_on = 1;

    kernel_proc.threads = &boot_thread;
    kernel_proc.thread_count = 1;
    kernel_proc.state = TASK_RUNNING;

    proc_table[0] = &kernel_proc;
    thread_table[0] = &boot_thread;
    nproc = nthread = 1;
    next_pid = next_tid = 1;
    this_cpu()->current = &boot_thread;

    kprintf("[proc] Kernel process PID 0 created; boot flow adopted as TID 0 "
            "(stack %p - %p)\n", (void *)boot_thread.stack.base, (void *)boot_thread.stack.top);
}

/* ---- processes ---------------------------------------------------------- */

struct process *process_create(const char *name, uint32_t flags, proc_status_t *err)
{
    if (flags & ~PROC_OWN_ADDRESS_SPACE)
        return fail(err, PROC_ERR_INVALID, "process_create");
    if (nproc == PROC_MAX)
        return fail(err, PROC_ERR_TABLE_FULL, "process_create");

    struct process *p = slab_zalloc(process_cache);
    if (p == NULL)
        return fail(err, PROC_ERR_NO_MEMORY, "process_create");

    if (flags & PROC_OWN_ADDRESS_SPACE) {
        if (vmm_create_address_space(&p->own_as) != VMM_OK) {
            slab_free(process_cache, p);
            return fail(err, PROC_ERR_NO_MEMORY, "process_create (address space)");
        }
        p->as = &p->own_as;
        p->owns_as = true;
    } else {
        p->as = vmm_kernel_space();
    }

    uint64_t flags_irq;
    spin_lock_irqsave(&proc_lock, &flags_irq);
    /* Re-check: another thread (or CPU) may have filled the table
     * between the early check and now. */
    if (!table_insert((void **)proc_table, PROC_MAX, p)) {
        spin_unlock_irqrestore(&proc_lock, flags_irq);
        if (p->owns_as)
            vmm_destroy_address_space(&p->own_as);
        slab_free(process_cache, p);
        return fail(err, PROC_ERR_TABLE_FULL, "process_create");
    }
    p->magic = PROCESS_MAGIC;
    p->pid = next_pid++;
    p->state = TASK_NEW;
    copy_name(p->name, name);
    p->parent = percpu_current()->process->pid;
    nproc++;
    spin_unlock_irqrestore(&proc_lock, flags_irq);

    if (err != NULL)
        *err = PROC_OK;
    return p;
}

/* Terminate `t` (interrupts off). One running on another CPU is marked
 * `kill` and that CPU is told to reschedule. */
static void terminate_thread(struct thread *t)
{
    struct runqueue *rq = sched_lock_thread(t);
    unsigned cpu = t->cpu;
    bool remote = false;
    if (t->state != TASK_TERMINATED) {
        if (running_somewhere(t) && t != percpu_current()) {
            t->kill = true;
            remote = true;
        } else if (set_state(t, TASK_TERMINATED) == PROC_OK) {   /* also leaves the lists */
            t->exit_code = -1;
        }
    }
    sched_unlock(rq);
    if (remote)
        ipi_reschedule(cpu);
}

proc_status_t process_terminate(struct process *p)
{
    if (!process_valid(p))
        return PROC_ERR_INVALID;
    if (p == percpu_current()->process)
        return PROC_ERR_BUSY;
    if (p->state == TASK_TERMINATED)
        return PROC_ERR_BAD_STATE;

    /* Interrupts off: an RCU read of the thread list, so no thread of it
     * can be freed while we walk it. */
    uint64_t flags = rcu_read_lock();
    for (struct thread *t = rcu_dereference(p->threads); t != NULL;
         t = rcu_dereference(t->next_in_process))
        terminate_thread(t);
    spin_lock(&proc_lock);
    p->state = TASK_TERMINATED;
    spin_unlock(&proc_lock);
    rcu_read_unlock(flags);
    return PROC_OK;
}

/* Free a process already removed from the table, after a grace period. */
static void process_free(struct process *p)
{
    if (p->owns_as) {
        vm_space_release(&p->own_as);     /* regions and their memory */
        vmm_destroy_address_space(&p->own_as);
    }
    p->magic = 0;
    slab_free(process_cache, p);
}

/* ---- threads ------------------------------------------------------------ */

/* Build the first context of a new thread so that the first context_switch
 * to it "returns" into thread_trampoline:
 *
 *   stack.top      ->  (16-byte aligned)
 *   top - 8         :  0                 \ padding; after the `ret` rsp =
 *   top - 16        :  0                 / top - 16, 16-byte aligned
 *   top - 16 - 64   :  struct cpu_context { r15..rbx = 0 except r12 = thread,
 *                                           rflags = 0x2 (IF off),
 *                                           rip = thread_trampoline }
 *   saved_rsp       -> the cpu_context
 *
 * Interrupts start disabled because we arrive from inside thread_switch;
 * thread_bootstrap enables them. */
static void init_context(struct thread *t)
{
    uint64_t top = t->stack.top;
    ((uint64_t *)top)[-1] = 0;
    ((uint64_t *)top)[-2] = 0;

    struct cpu_context *ctx = (struct cpu_context *)(top - 16 - sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));
    ctx->r12 = (uint64_t)t;
    ctx->rflags = RFLAGS_RESERVED_1;
    ctx->rip = (uint64_t)thread_trampoline;
    t->saved_rsp = (uint64_t)ctx;
}

struct thread *thread_create(struct process *p, const char *name,
                             thread_entry_t entry, void *arg, proc_status_t *err)
{
    return thread_create_on(p, name, entry, arg, THREAD_ANY_CPU, err);
}

/* Publish a new thread: table, its process's list, a TID. */
static proc_status_t register_thread(struct process *p, struct thread *t)
{
    SPIN_GUARD(&proc_lock);
    /* Re-validate atomically: while we were allocating (preemptible), the
     * process may have been terminated or reaped, or the table filled. */
    proc_status_t late = !process_valid(p) ? PROC_ERR_INVALID
                       : p->state == TASK_TERMINATED ? PROC_ERR_BAD_STATE
                       : !table_insert((void **)thread_table, THREAD_MAX, t) ? PROC_ERR_TABLE_FULL
                       : PROC_OK;
    if (late != PROC_OK)
        return late;
    t->tid = next_tid++;
    struct thread **link = &p->threads;
    while (*link != NULL)
        link = &(*link)->next_in_process;
    rcu_assign_pointer(*link, t);
    p->thread_count++;
    nthread++;
    return PROC_OK;
}

struct thread *thread_create_on(struct process *p, const char *name, thread_entry_t entry,
                                void *arg, unsigned cpu, proc_status_t *err)
{
    if (!process_valid(p) || entry == NULL
        || (cpu != THREAD_ANY_CPU && (cpu_at(cpu) == NULL || !cpu_at(cpu)->online)))
        return fail(err, PROC_ERR_INVALID, "thread_create");
    if (p->state == TASK_TERMINATED)
        return fail(err, PROC_ERR_BAD_STATE, "thread_create (process terminated)");
    if (nthread == THREAD_MAX)
        return fail(err, PROC_ERR_TABLE_FULL, "thread_create");

    struct thread *t = slab_zalloc(thread_cache);
    if (t == NULL)
        return fail(err, PROC_ERR_NO_MEMORY, "thread_create");

    kstack_status_t ks = kstack_alloc(&t->stack);
    if (ks != KSTACK_OK) {
        slab_free(thread_cache, t);
        return fail(err, ks == KSTACK_ERR_NO_SLOT ? PROC_ERR_NO_STACK : PROC_ERR_NO_MEMORY,
                    "thread_create (kernel stack)");
    }

    t->magic = THREAD_MAGIC;
    t->state = TASK_NEW;
    copy_name(t->name, name);
    t->process = p;
    t->entry = entry;
    t->arg = arg;
    t->exit_to = percpu_current()->tid;
    t->pinned = cpu != THREAD_ANY_CPU;
    t->cpu = t->pinned ? cpu : sched_pick_cpu();
    init_context(t);

    /* Interrupts stay off from publishing the thread until it is READY:
     * that makes us an RCU reader, so even if its process is terminated
     * and reaped meanwhile, `t` is not freed under us. */
    uint64_t flags = irq_save();
    proc_status_t late = register_thread(p, t);
    if (late != PROC_OK) {
        irq_restore(flags);
        kstack_free(&t->stack);
        slab_free(thread_cache, t);
        return fail(err, late, "thread_create");
    }
    tid_t tid = t->tid;
    struct runqueue *rq = sched_lock_thread(t);
    bool started = t->state == TASK_NEW && set_state(t, TASK_READY) == PROC_OK;
    sched_unlock(rq);
    irq_restore(flags);
    trace_event(TRACE_THREAD_CREATE, tid, p->pid);
    if (!started)                         /* terminated before it could start */
        return fail(err, PROC_ERR_BAD_STATE, "thread_create (process terminated)");

    if (err != NULL)
        *err = PROC_OK;
    return t;
}

struct thread *thread_create_idle(unsigned cpu)
{
    struct thread *t = slab_zalloc(thread_cache);
    if (t == NULL)
        return NULL;
    if (kstack_alloc(&t->stack) != KSTACK_OK) {
        slab_free(thread_cache, t);
        return NULL;
    }
    t->magic = THREAD_MAGIC;
    t->state = TASK_RUNNING;        /* the CPU starts on this stack */
    char name[TASK_NAME_LEN] = "idle";
    name[4] = (char)('0' + cpu / 10);
    name[5] = (char)('0' + cpu % 10);
    copy_name(t->name, name);
    t->process = &kernel_proc;
    t->cpu = cpu;
    t->pinned = true;
    t->flags = THREAD_IDLE;
    t->ran_on = 1u << cpu;
    uint64_t flags = irq_save();
    proc_status_t st = register_thread(&kernel_proc, t);
    irq_restore(flags);
    if (st != PROC_OK) {
        kstack_free(&t->stack);
        slab_free(thread_cache, t);
        return NULL;
    }
    return t;
}

void thread_bootstrap(struct thread *t)
{
    sched_finish_switch();     /* second half of the switch that started us */
    if (t != percpu_current() || t->state != TASK_RUNNING)
        context_corrupt(t, "new thread started while not the RUNNING current thread");
    __asm__ volatile("sti");
    thread_exit(t->entry(t->arg));
}

proc_status_t thread_terminate(struct thread *t)
{
    if (!thread_valid(t))
        return PROC_ERR_INVALID;
    if (t == percpu_current())
        return PROC_ERR_BUSY;

    uint64_t flags = irq_save();
    proc_status_t st = t->state == TASK_TERMINATED ? PROC_ERR_BAD_STATE : PROC_OK;
    if (st == PROC_OK)
        terminate_thread(t);
    irq_restore(flags);
    return st;
}

/* Can `t` be freed? TERMINATED and no longer running anywhere. Its
 * run-queue lock makes the answer stable: a thread that exits holds that
 * lock from marking itself TERMINATED until its registers are saved. */
static bool finished(struct thread *t)
{
    struct runqueue *rq = sched_lock_thread(t);
    bool done = t->state == TASK_TERMINATED && !running_somewhere(t);
    sched_unlock(rq);
    return done;
}

/* Remove a finished thread from the table and its process (proc_lock).
 * Returns false if someone else already did. */
static bool unregister_thread(struct thread *t)
{
    SPIN_GUARD(&proc_lock);
    if (!table_remove((void **)thread_table, THREAD_MAX, t))
        return false;
    struct process *p = t->process;
    for (struct thread **link = &p->threads; *link != NULL; link = &(*link)->next_in_process) {
        if (*link == t) {
            rcu_assign_pointer(*link, t->next_in_process);
            break;
        }
    }
    p->thread_count--;
    nthread--;
    return true;
}

/* Free an unregistered thread. Call after a grace period. */
static void thread_free(struct thread *t)
{
    if (!kstack_canary_intact(&t->stack))
        context_corrupt(t, "kernel stack canary overwritten before destroy");
    kstack_free(&t->stack);
    t->magic = 0;
    slab_free(thread_cache, t);
}

proc_status_t thread_destroy(struct thread *t)
{
    if (!thread_valid(t))
        return PROC_ERR_INVALID;
    if (t == percpu_current() || t == &boot_thread || (t->flags & THREAD_IDLE))
        return PROC_ERR_BUSY;
    uint64_t flags = rcu_read_lock();     /* t stays allocated while we look */
    proc_status_t st = !finished(t) ? PROC_ERR_BAD_STATE
                     : !unregister_thread(t) ? PROC_ERR_INVALID : PROC_OK;
    rcu_read_unlock(flags);
    if (st != PROC_OK)
        return st;
    rcu_synchronize();                    /* no lookup can still be reading it */
    thread_free(t);
    return PROC_OK;
}

void thread_exit(int64_t code)
{
    irq_save();   /* stays off; the next thread restores its own IF */
    struct thread *t = percpu_current();
    if (t == &boot_thread)
        panic("thread_exit: the boot thread cannot exit");

    t->exit_code = code;
    trace_event(TRACE_THREAD_EXIT, t->tid, (uint64_t)code);
    struct runqueue *rq = sched_lock_this_cpu();
    set_state(t, TASK_TERMINATED);

    /* With the scheduler running, it picks the next READY thread (or idle).
     * The TERMINATED thread is on no queue, so it can never be resumed;
     * its stack stays intact until proc_reap runs on another thread. The
     * run-queue lock is held until the switch away is complete. */
    if (sched_active())
        sched_reschedule_locked(rq);

    /* Before the scheduler starts (Phase 4 tests): hand the CPU back to the
     * thread that created us, falling back to the boot thread, then to any
     * READY thread. */
    struct thread *next = thread_find(t->exit_to);
    if (next == NULL || next->state != TASK_READY)
        next = boot_thread.state == TASK_READY ? &boot_thread : thread_next_ready(t);
    if (next == NULL)
        panic("thread_exit: no READY thread to resume");

    thread_switch_to(next);
    panic("thread_exit: a TERMINATED thread was resumed");
}

void proc_trim_caches(void)
{
    slab_shrink(thread_cache);
    slab_shrink(process_cache);
}

uint64_t proc_cache_pages(void)
{
    struct slab_stats t, p;
    slab_get_stats(thread_cache, &t);
    slab_get_stats(process_cache, &p);
    return t.slabs + p.slabs;
}

#define REAP_BATCH 32

/* Reclaim finished threads and empty TERMINATED processes:
 *   1. find candidates and unregister them (locks; interrupts off),
 *   2. wait for a grace period, so no lock-free lookup still sees them,
 *   3. free their memory. */
unsigned proc_reap(void)
{
    unsigned reaped = 0;
    for (;;) {
        struct thread *dead[REAP_BATCH];
        struct process *gone[PROC_MAX];
        unsigned nd = 0, ng = 0;

        uint64_t flags = rcu_read_lock();
        for (unsigned i = 0; i < THREAD_MAX && nd < REAP_BATCH; i++) {
            struct thread *t = rcu_dereference(thread_table[i]);
            if (t != NULL && t != &boot_thread && !(t->flags & THREAD_IDLE)
                && t->state == TASK_TERMINATED && finished(t) && unregister_thread(t))
                dead[nd++] = t;
        }
        spin_lock(&proc_lock);
        for (unsigned i = 0; i < PROC_MAX; i++) {
            struct process *p = proc_table[i];
            if (p != NULL && p != &kernel_proc && p->state == TASK_TERMINATED
                && p->thread_count == 0 && table_remove((void **)proc_table, PROC_MAX, p)) {
                nproc--;
                gone[ng++] = p;
            }
        }
        spin_unlock(&proc_lock);
        rcu_read_unlock(flags);

        if (nd == 0 && ng == 0)
            break;
        rcu_synchronize();
        for (unsigned i = 0; i < nd; i++)
            thread_free(dead[i]);
        for (unsigned i = 0; i < ng; i++)
            process_free(gone[i]);
        reaped += nd;
        if (nd < REAP_BATCH)
            break;
    }
    return reaped;
}

/* ---- switching ---------------------------------------------------------- */

void thread_switch_to(struct thread *next)
{
    struct thread *prev = percpu_current();
    struct percpu *c = this_cpu();

    if (!kstack_canary_intact(&prev->stack))
        context_corrupt(prev, "outgoing thread's stack canary overwritten (stack overflow?)");
    validate_suspended(next);

    /* `next` becomes this CPU's current first, so the state hook queues
     * `prev` (no longer running here) when it turns READY. */
    c->current = next;
    if (prev->state == TASK_RUNNING)
        set_state(prev, TASK_READY);   /* else it blocked or exited itself */
    else if (prev->state == TASK_READY)
        sched_state_changed(prev, TASK_READY, TASK_READY);   /* made itself READY: queue it */
    set_state(next, TASK_RUNNING);
    next->switch_count++;
    next->ran_on |= 1u << c->id;

    if (next->process->as != c->space)
        vmm_switch(next->process->as);

    context_switch(&prev->saved_rsp, next->saved_rsp);

    /* Some later switch resumed us (perhaps on another CPU); release the
     * run-queue lock that switch took. */
    sched_finish_switch();
}

proc_status_t thread_switch(struct thread *next)
{
    struct thread *prev = percpu_current();

    if (!thread_valid(next)) {
        kprintf("[proc] thread_switch: invalid thread %p\n", (void *)next);
        return PROC_ERR_INVALID;
    }
    if (next == prev || next->state != TASK_READY || (next->flags & THREAD_IDLE)) {
        kprintf("[proc] thread_switch: TID %u is %s, not a READY thread\n",
                next->tid, next == prev ? "the current thread" : task_state_str(next->state));
        return PROC_ERR_BAD_STATE;
    }

    uint64_t flags = irq_save();
    struct runqueue *rq = sched_lock_this_cpu();
    if (next->state != TASK_READY || next->cpu != cpu_id()) {
        sched_unlock(rq);
        irq_restore(flags);
        kprintf("[proc] thread_switch: TID %u is not READY on this CPU\n", next->tid);
        return PROC_ERR_BAD_STATE;
    }
    thread_switch_to(next);

    /* Some later thread_switch resumed us; `current` is prev again. */
    irq_restore(flags);
    return PROC_OK;
}

proc_status_t thread_set_state_locked(struct thread *t, task_state_t state)
{
    return set_state(t, state);
}

proc_status_t thread_set_state(struct thread *t, task_state_t state)
{
    if (!thread_valid(t))
        return PROC_ERR_INVALID;
    if (state == TASK_RUNNING) {
        kprintf("[proc] set_state: only thread_switch may make TID %u RUNNING\n", t->tid);
        return PROC_ERR_BAD_STATE;
    }
    if (t == percpu_current() && state == TASK_TERMINATED)
        return PROC_ERR_BUSY;   /* use thread_exit */

    uint64_t flags = irq_save();
    struct runqueue *rq = sched_lock_thread(t);
    proc_status_t st;
    if (t != percpu_current() && running_somewhere(t))
        st = PROC_ERR_BUSY;     /* running on another CPU right now */
    else if (t == percpu_current() && state == TASK_READY && t->state == TASK_RUNNING)
        st = PROC_OK;           /* already runnable */
    else
        st = set_state(t, state);
    sched_unlock(rq);
    irq_restore(flags);
    return st;
}

const struct cpu_context *thread_get_context(const struct thread *t)
{
    if (!thread_valid(t) || t == percpu_current())
        return NULL;
    uint64_t flags = irq_save();
    struct runqueue *rq = sched_lock_thread((struct thread *)t);
    bool usable = !running_somewhere(t) && saved_rsp_in_bounds(t);
    sched_unlock(rq);
    irq_restore(flags);
    return usable ? (const struct cpu_context *)t->saved_rsp : NULL;
}

struct thread *thread_next_ready(const struct thread *after)
{
    unsigned start = 0;
    for (unsigned i = 0; after != NULL && i < THREAD_MAX; i++) {
        if (thread_table[i] == after) {
            start = i + 1;
            break;
        }
    }
    for (unsigned n = 0; n < THREAD_MAX; n++) {
        struct thread *t = rcu_dereference(thread_table[(start + n) % THREAD_MAX]);
        if (t != NULL && t->state == TASK_READY)
            return t;
    }
    return NULL;
}

/* ---- registry ----------------------------------------------------------- */

struct thread *thread_current(void)   { return percpu_current(); }
struct process *process_current(void) { return percpu_current()->process; }
struct process *process_kernel(void)  { return &kernel_proc; }
unsigned process_count(void)          { return nproc; }
unsigned thread_count(void)           { return nthread; }

struct process *process_at(unsigned slot)
{
    return slot < PROC_MAX ? rcu_dereference(proc_table[slot]) : NULL;
}

struct thread *thread_at(unsigned slot)
{
    return slot < THREAD_MAX ? rcu_dereference(thread_table[slot]) : NULL;
}

struct process *process_find(pid_t pid)
{
    struct process *found = NULL;
    uint64_t f = rcu_read_lock();
    for (unsigned i = 0; i < PROC_MAX && found == NULL; i++) {
        struct process *p = rcu_dereference(proc_table[i]);
        if (p != NULL && p->pid == pid)
            found = p;
    }
    rcu_read_unlock(f);
    return found;
}

struct thread *thread_find(tid_t tid)
{
    struct thread *found = NULL;
    uint64_t f = rcu_read_lock();
    for (unsigned i = 0; i < THREAD_MAX && found == NULL; i++) {
        struct thread *t = rcu_dereference(thread_table[i]);
        if (t != NULL && t->tid == tid)
            found = t;
    }
    rcu_read_unlock(f);
    return found;
}

/* ---- diagnostics -------------------------------------------------------- */

/* Live rsp/rip for the running thread; saved values for a suspended one. */
static void thread_regs(const struct thread *t, uint64_t *rsp, uint64_t *rip)
{
    if (t == percpu_current()) {
        __asm__ volatile("mov %%rsp, %0" : "=r"(*rsp));
        __asm__ volatile("lea 0(%%rip), %0" : "=r"(*rip));
    } else if (saved_rsp_in_bounds(t)) {
        *rsp = t->saved_rsp;
        *rip = ((const struct cpu_context *)t->saved_rsp)->rip;
    } else {
        *rsp = t->saved_rsp;
        *rip = 0;
    }
}

void thread_dump(const struct thread *t)
{
    uint64_t rsp, rip;
    thread_regs(t, &rsp, &rip);
    kprintf("[BHARAT PROC] ------------------------------------------\n");
    kprintf("  PID      : %u (%s)\n", t->process->pid, t->process->name);
    kprintf("  TID      : %u (%s)\n", t->tid, t->name);
    kprintf("  STATE    : %s\n", task_state_str(t->state));
    kprintf("  KSTACK   : %p - %p (%lu KiB, peak use %lu B, canary %s)\n",
            (void *)t->stack.base, (void *)t->stack.top,
            (t->stack.top - t->stack.base) / 1024, kstack_high_water(&t->stack),
            kstack_canary_intact(&t->stack) ? "intact" : "SMASHED");
    kprintf("  RSP      : %p%s\n", (void *)rsp, t == percpu_current() ? " (live)" : " (saved)");
    kprintf("  RIP      : %p%s\n", (void *)rip,
            t == percpu_current() ? " (live)" : rip ? " (saved return address)" : " (unreadable)");
    kprintf("  ADDR SPC : PML4 %p (%s)\n", (void *)t->process->as->pml4_phys,
            t->process->owns_as ? "own address space" : "kernel address space");
    kprintf("  SWITCHES : %lu\n", t->switch_count);
}

void proc_dump_table(void)
{
    box_top();
    box_line("%s", "BHARAT PROCESS MANAGER");
    box_separator();
    box_line("%-4s %-4s %-11s %-10s %-18s %-18s", "PID", "TID", "STATE", "NAME",
             "KSTACK TOP", "RSP");
    for (unsigned i = 0; i < THREAD_MAX; i++) {
        const struct thread *t = thread_table[i];
        if (t == NULL)
            continue;
        uint64_t rsp, rip;
        thread_regs(t, &rsp, &rip);
        box_line("%-4u %-4u %-11s %-10s %p %p", t->process->pid, t->tid,
                 task_state_str(t->state), t->name, (void *)t->stack.top, (void *)rsp);
    }
    box_separator();
    for (unsigned i = 0; i < PROC_MAX; i++) {
        const struct process *p = proc_table[i];
        if (p == NULL)
            continue;
        box_line("PID %-3u %-10s %-11s thr=%-3u PML4=%p %s", p->pid, p->name,
                 task_state_str(p->state), p->thread_count, (void *)p->as->pml4_phys,
                 p->owns_as ? "own" : "kernel");
    }
    box_bottom();
}

const char *task_state_str(task_state_t s)
{
    switch (s) {
    case TASK_NEW:        return "NEW";
    case TASK_READY:      return "READY";
    case TASK_RUNNING:    return "RUNNING";
    case TASK_BLOCKED:    return "BLOCKED";
    case TASK_TERMINATED: return "TERMINATED";
    }
    return "?";
}

const char *proc_status_str(proc_status_t s)
{
    switch (s) {
    case PROC_OK:             return "ok";
    case PROC_ERR_INVALID:    return "invalid object or argument";
    case PROC_ERR_NO_MEMORY:  return "out of memory";
    case PROC_ERR_NO_STACK:   return "no free kernel stack slot";
    case PROC_ERR_TABLE_FULL: return "table full";
    case PROC_ERR_BAD_STATE:  return "operation not allowed in current state";
    case PROC_ERR_BUSY:       return "in use by the running thread";
    }
    return "unknown";
}
