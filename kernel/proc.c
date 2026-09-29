#include "proc.h"
#include "heap.h"
#include "cpu.h"
#include "box.h"
#include "kprintf.h"
#include "panic.h"
#include "string.h"

#define PROCESS_MAGIC 0x434F5250u   /* "PROC" */
#define THREAD_MAGIC  0x44524854u   /* "THRD" */

extern char stack_bottom[], stack_top[];
extern char __text_start[], __text_end[];

/* Called from thread_trampoline (context.asm) on a new thread's first run. */
void thread_bootstrap(struct thread *t) __attribute__((noreturn));

static struct process *proc_table[PROC_MAX];
static struct thread *thread_table[THREAD_MAX];
static unsigned nproc, nthread;
static pid_t next_pid;
static tid_t next_tid;

/* The kernel process and the adopted boot flow live in .bss, not the heap,
 * and are never destroyed. */
static struct process kernel_proc;
static struct thread boot_thread;

static struct thread *current;

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
    for (unsigned i = 0; i < PROC_MAX; i++) {
        if (proc_table[i] == p)
            return p->magic == PROCESS_MAGIC;
    }
    return false;
}

static bool thread_valid(const struct thread *t)
{
    if (t == NULL)
        return false;
    for (unsigned i = 0; i < THREAD_MAX; i++) {
        if (thread_table[i] == t)
            return t->magic == THREAD_MAGIC;
    }
    return false;
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
 * terminated, the process is TERMINATED for good. */
static void process_update_state(struct process *p)
{
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

static proc_status_t set_state(struct thread *t, task_state_t to)
{
    if (!transition_allowed(t->state, to)) {
        kprintf("[proc] invalid state transition for TID %u: %s -> %s\n",
                t->tid, task_state_str(t->state), task_state_str(to));
        return PROC_ERR_BAD_STATE;
    }
    t->state = to;
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

static bool table_insert(void **table, unsigned size, void *obj)
{
    for (unsigned i = 0; i < size; i++) {
        if (table[i] == NULL) {
            table[i] = obj;
            return true;
        }
    }
    return false;
}

static void table_remove(void **table, unsigned size, void *obj)
{
    for (unsigned i = 0; i < size; i++) {
        if (table[i] == obj)
            table[i] = NULL;
    }
}

/* ---- initialisation ----------------------------------------------------- */

void proc_init(void)
{
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

    kernel_proc.threads = &boot_thread;
    kernel_proc.thread_count = 1;
    kernel_proc.state = TASK_RUNNING;

    proc_table[0] = &kernel_proc;
    thread_table[0] = &boot_thread;
    nproc = nthread = 1;
    next_pid = next_tid = 1;
    current = &boot_thread;

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

    struct process *p = kzalloc(sizeof(*p));
    if (p == NULL)
        return fail(err, PROC_ERR_NO_MEMORY, "process_create");

    if (flags & PROC_OWN_ADDRESS_SPACE) {
        if (vmm_create_address_space(&p->own_as) != VMM_OK) {
            kfree(p);
            return fail(err, PROC_ERR_NO_MEMORY, "process_create (address space)");
        }
        p->as = &p->own_as;
        p->owns_as = true;
    } else {
        p->as = vmm_kernel_space();
    }

    uint64_t flags_irq = irq_save();
    p->magic = PROCESS_MAGIC;
    p->pid = next_pid++;
    p->state = TASK_NEW;
    copy_name(p->name, name);
    p->parent = current->process->pid;
    table_insert((void **)proc_table, PROC_MAX, p);
    nproc++;
    irq_restore(flags_irq);

    if (err != NULL)
        *err = PROC_OK;
    return p;
}

proc_status_t process_terminate(struct process *p)
{
    if (!process_valid(p))
        return PROC_ERR_INVALID;
    if (p == current->process)
        return PROC_ERR_BUSY;
    if (p->state == TASK_TERMINATED)
        return PROC_ERR_BAD_STATE;

    uint64_t flags = irq_save();
    for (struct thread *t = p->threads; t != NULL; t = t->next_in_process) {
        if (t->state != TASK_TERMINATED) {
            t->state = TASK_TERMINATED;
            t->exit_code = -1;
        }
    }
    p->state = TASK_TERMINATED;
    irq_restore(flags);
    return PROC_OK;
}

static void process_destroy(struct process *p)
{
    if (p->owns_as)
        vmm_destroy_address_space(&p->own_as);
    table_remove((void **)proc_table, PROC_MAX, p);
    nproc--;
    p->magic = 0;
    kfree(p);
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
    if (!process_valid(p) || entry == NULL)
        return fail(err, PROC_ERR_INVALID, "thread_create");
    if (p->state == TASK_TERMINATED)
        return fail(err, PROC_ERR_BAD_STATE, "thread_create (process terminated)");
    if (nthread == THREAD_MAX)
        return fail(err, PROC_ERR_TABLE_FULL, "thread_create");

    struct thread *t = kzalloc(sizeof(*t));
    if (t == NULL)
        return fail(err, PROC_ERR_NO_MEMORY, "thread_create");

    kstack_status_t ks = kstack_alloc(&t->stack);
    if (ks != KSTACK_OK) {
        kfree(t);
        return fail(err, ks == KSTACK_ERR_NO_SLOT ? PROC_ERR_NO_STACK : PROC_ERR_NO_MEMORY,
                    "thread_create (kernel stack)");
    }

    t->magic = THREAD_MAGIC;
    t->state = TASK_NEW;
    copy_name(t->name, name);
    t->process = p;
    t->entry = entry;
    t->arg = arg;
    t->exit_to = current->tid;
    init_context(t);

    uint64_t flags = irq_save();
    t->tid = next_tid++;
    struct thread **link = &p->threads;
    while (*link != NULL)
        link = &(*link)->next_in_process;
    *link = t;
    p->thread_count++;
    table_insert((void **)thread_table, THREAD_MAX, t);
    nthread++;
    set_state(t, TASK_READY);
    irq_restore(flags);

    if (err != NULL)
        *err = PROC_OK;
    return t;
}

void thread_bootstrap(struct thread *t)
{
    if (t != current || t->state != TASK_RUNNING)
        context_corrupt(t, "new thread started while not the RUNNING current thread");
    __asm__ volatile("sti");
    thread_exit(t->entry(t->arg));
}

proc_status_t thread_terminate(struct thread *t)
{
    if (!thread_valid(t))
        return PROC_ERR_INVALID;
    if (t == current)
        return PROC_ERR_BUSY;

    uint64_t flags = irq_save();
    proc_status_t st = set_state(t, TASK_TERMINATED);
    if (st == PROC_OK)
        t->exit_code = -1;
    irq_restore(flags);
    return st;
}

proc_status_t thread_destroy(struct thread *t)
{
    if (!thread_valid(t))
        return PROC_ERR_INVALID;
    if (t == current || t == &boot_thread)
        return PROC_ERR_BUSY;
    if (t->state != TASK_TERMINATED)
        return PROC_ERR_BAD_STATE;
    if (!kstack_canary_intact(&t->stack))
        context_corrupt(t, "kernel stack canary overwritten before destroy");

    uint64_t flags = irq_save();
    kstack_free(&t->stack);

    struct process *p = t->process;
    for (struct thread **link = &p->threads; *link != NULL; link = &(*link)->next_in_process) {
        if (*link == t) {
            *link = t->next_in_process;
            break;
        }
    }
    p->thread_count--;
    table_remove((void **)thread_table, THREAD_MAX, t);
    nthread--;
    t->magic = 0;
    kfree(t);
    irq_restore(flags);
    return PROC_OK;
}

void thread_exit(int64_t code)
{
    irq_save();   /* stays off; the next thread restores its own IF */
    struct thread *t = current;
    if (t == &boot_thread)
        panic("thread_exit: the boot thread cannot exit");

    t->exit_code = code;
    set_state(t, TASK_TERMINATED);

    /* No scheduler yet: hand the CPU back to the thread that created us,
     * falling back to the boot thread, then to any READY thread. */
    struct thread *next = thread_find(t->exit_to);
    if (next == NULL || next->state != TASK_READY)
        next = boot_thread.state == TASK_READY ? &boot_thread : thread_next_ready(t);
    if (next == NULL)
        panic("thread_exit: no READY thread to resume");

    thread_switch(next);
    panic("thread_exit: a TERMINATED thread was resumed");
}

unsigned proc_reap(void)
{
    unsigned reaped = 0;
    uint64_t flags = irq_save();

    for (unsigned i = 0; i < THREAD_MAX; i++) {
        struct thread *t = thread_table[i];
        if (t != NULL && t != current && t->state == TASK_TERMINATED
            && thread_destroy(t) == PROC_OK)
            reaped++;
    }
    for (unsigned i = 0; i < PROC_MAX; i++) {
        struct process *p = proc_table[i];
        if (p != NULL && p != &kernel_proc && p->state == TASK_TERMINATED
            && p->thread_count == 0)
            process_destroy(p);
    }

    irq_restore(flags);
    return reaped;
}

/* ---- switching ---------------------------------------------------------- */

proc_status_t thread_switch(struct thread *next)
{
    struct thread *prev = current;

    if (!thread_valid(next)) {
        kprintf("[proc] thread_switch: invalid thread %p\n", (void *)next);
        return PROC_ERR_INVALID;
    }
    if (next == prev || next->state != TASK_READY) {
        kprintf("[proc] thread_switch: TID %u is %s, not a READY thread\n",
                next->tid, next == prev ? "the current thread" : task_state_str(next->state));
        return PROC_ERR_BAD_STATE;
    }

    uint64_t flags = irq_save();

    if (!kstack_canary_intact(&prev->stack))
        context_corrupt(prev, "outgoing thread's stack canary overwritten (stack overflow?)");
    validate_suspended(next);

    if (prev->state == TASK_RUNNING)
        set_state(prev, TASK_READY);   /* else it blocked or exited itself */
    set_state(next, TASK_RUNNING);
    next->switch_count++;

    if (next->process->as != prev->process->as)
        vmm_switch(next->process->as);
    current = next;

    context_switch(&prev->saved_rsp, next->saved_rsp);

    /* Some later thread_switch resumed us; `current` is prev again. */
    irq_restore(flags);
    return PROC_OK;
}

proc_status_t thread_set_state(struct thread *t, task_state_t state)
{
    if (!thread_valid(t))
        return PROC_ERR_INVALID;
    if (state == TASK_RUNNING) {
        kprintf("[proc] set_state: only thread_switch may make TID %u RUNNING\n", t->tid);
        return PROC_ERR_BAD_STATE;
    }
    if (t == current && state == TASK_TERMINATED)
        return PROC_ERR_BUSY;   /* use thread_exit */

    uint64_t flags = irq_save();
    proc_status_t st = set_state(t, state);
    irq_restore(flags);
    return st;
}

const struct cpu_context *thread_get_context(const struct thread *t)
{
    if (!thread_valid(t) || t == current || !saved_rsp_in_bounds(t))
        return NULL;
    return (const struct cpu_context *)t->saved_rsp;
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
        struct thread *t = thread_table[(start + n) % THREAD_MAX];
        if (t != NULL && t->state == TASK_READY)
            return t;
    }
    return NULL;
}

/* ---- registry ----------------------------------------------------------- */

struct thread *thread_current(void)   { return current; }
struct process *process_current(void) { return current->process; }
struct process *process_kernel(void)  { return &kernel_proc; }
unsigned process_count(void)          { return nproc; }
unsigned thread_count(void)           { return nthread; }

struct process *process_at(unsigned slot)
{
    return slot < PROC_MAX ? proc_table[slot] : NULL;
}

struct thread *thread_at(unsigned slot)
{
    return slot < THREAD_MAX ? thread_table[slot] : NULL;
}

struct process *process_find(pid_t pid)
{
    for (unsigned i = 0; i < PROC_MAX; i++) {
        if (proc_table[i] != NULL && proc_table[i]->pid == pid)
            return proc_table[i];
    }
    return NULL;
}

struct thread *thread_find(tid_t tid)
{
    for (unsigned i = 0; i < THREAD_MAX; i++) {
        if (thread_table[i] != NULL && thread_table[i]->tid == tid)
            return thread_table[i];
    }
    return NULL;
}

/* ---- diagnostics -------------------------------------------------------- */

/* Live rsp/rip for the running thread; saved values for a suspended one. */
static void thread_regs(const struct thread *t, uint64_t *rsp, uint64_t *rip)
{
    if (t == current) {
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
    kprintf("[NEXUS PROC] ------------------------------------------\n");
    kprintf("  PID      : %u (%s)\n", t->process->pid, t->process->name);
    kprintf("  TID      : %u (%s)\n", t->tid, t->name);
    kprintf("  STATE    : %s\n", task_state_str(t->state));
    kprintf("  KSTACK   : %p - %p (%lu KiB, peak use %lu B, canary %s)\n",
            (void *)t->stack.base, (void *)t->stack.top,
            (t->stack.top - t->stack.base) / 1024, kstack_high_water(&t->stack),
            kstack_canary_intact(&t->stack) ? "intact" : "SMASHED");
    kprintf("  RSP      : %p%s\n", (void *)rsp, t == current ? " (live)" : " (saved)");
    kprintf("  RIP      : %p%s\n", (void *)rip,
            t == current ? " (live)" : rip ? " (saved return address)" : " (unreadable)");
    kprintf("  ADDR SPC : PML4 %p (%s)\n", (void *)t->process->as->pml4_phys,
            t->process->owns_as ? "own address space" : "kernel address space");
    kprintf("  SWITCHES : %lu\n", t->switch_count);
}

void proc_dump_table(void)
{
    box_top();
    box_line("%s", "NEXUS PROCESS MANAGER");
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
