#ifndef BHARAT_PROC_H
#define BHARAT_PROC_H

/* Processes and kernel threads (Phase 4).
 *
 *   Process  - an execution environment: PID, state, an address space and
 *              the threads running in it.
 *   Thread   - a schedulable execution context: TID, state, owning process,
 *              its own kernel stack, and (while suspended) a saved CPU
 *              context on that stack.
 *
 *   Process ──┬── address_space_t  (own PML4, or the shared kernel space)
 *             └── Thread(s) ── kernel stack ── struct cpu_context
 *
 * There is NO scheduler yet. Nothing runs a thread unless kernel code
 * explicitly calls thread_switch(). Phase 5 adds policy on top of these
 * primitives:
 *
 *   thread_create / thread_destroy     (create_thread / destroy_thread)
 *   thread_get_context                 (get_thread_context)
 *   thread_switch                      (switch_context)
 *   thread_set_state                   (set_thread_state)
 *   thread_next_ready                  (get_next_ready_thread, a query only)
 *
 * The boot flow (kernel_main on the boot stack) is adopted as thread 0 of
 * the kernel process (PID 0), so the kernel can switch away and back.
 *
 * Multi-core (Phase 9):
 *   - the process and thread tables, each process's thread list and its
 *     summary state are guarded by the "process table" spinlock;
 *   - a thread's state is guarded by its CPU's run-queue lock (sched.h);
 *     lock order: run queue, then process table;
 *   - lookups (process_find, thread_find, validity checks) take no lock:
 *     they read the tables as RCU readers (interrupts off), and threads
 *     and processes are freed only after rcu_synchronize(), so a reader
 *     never touches freed memory. A pointer returned by a lookup is still
 *     only safe to use while the object cannot be reaped (Phase 10 adds
 *     handles with reference counts);
 *   - a thread running on another CPU cannot be stopped at once:
 *     thread_terminate marks it `kill` and that CPU ends it at its next
 *     scheduling decision (it is told with an IPI). */

#include <stdint.h>
#include <stdbool.h>
#include "vmm.h"
#include "kstack.h"
#include "context.h"

typedef uint32_t pid_t;
typedef uint32_t tid_t;

/* Shared by processes and threads. For a process the state summarises its
 * threads (RUNNING if one of them runs, READY if one is ready, ...). */
typedef enum {
    TASK_NEW = 0,       /* created, not yet runnable */
    TASK_READY,         /* runnable, waiting to be switched to */
    TASK_RUNNING,       /* executing on a CPU */
    TASK_BLOCKED,       /* waiting for something; not runnable */
    TASK_TERMINATED,    /* finished; resources await proc_reap() */
} task_state_t;

typedef enum {
    PROC_OK = 0,
    PROC_ERR_INVALID,        /* NULL / corrupt / unknown object or argument */
    PROC_ERR_NO_MEMORY,      /* heap or PMM exhausted */
    PROC_ERR_NO_STACK,       /* no free kernel-stack slot */
    PROC_ERR_TABLE_FULL,     /* process or thread table full */
    PROC_ERR_BAD_STATE,      /* operation not allowed in the current state */
    PROC_ERR_BUSY,           /* object is in use by the running thread */
} proc_status_t;

#define PROC_MAX         64
#define THREAD_MAX       256
#define TASK_NAME_LEN    16

/* process_create flags */
#define PROC_OWN_ADDRESS_SPACE  (1u << 0)   /* new PML4 instead of the kernel's */

typedef int64_t (*thread_entry_t)(void *arg);

struct thread;

struct process {
    uint32_t magic;
    pid_t pid;
    task_state_t state;
    char name[TASK_NAME_LEN];
    pid_t parent;                 /* PID of the creating thread's process */
    address_space_t *as;          /* &own_as, or the kernel space */
    address_space_t own_as;
    bool owns_as;
    struct thread *threads;       /* singly linked through next_in_process */
    uint32_t thread_count;        /* threads not yet destroyed */
};

struct thread {
    uint32_t magic;
    tid_t tid;
    task_state_t state;
    char name[TASK_NAME_LEN];
    struct process *process;
    struct thread *next_in_process;

    /* Saved stack pointer; points at a struct cpu_context on this thread's
     * own stack. Meaningful only while the thread is not RUNNING. */
    uint64_t saved_rsp;
    struct kstack stack;

    thread_entry_t entry;
    void *arg;
    tid_t exit_to;                /* thread resumed when this one exits */
    int64_t exit_code;
    uint64_t switch_count;        /* times this thread was switched to */

    /* Scheduler bookkeeping (sched.c). A thread is on at most one list:
     * the ready queue while READY, the sleep list while sleeping. */
    struct thread *sched_prev;
    struct thread *sched_next;
    uint8_t sched_list;           /* SCHED_LIST_* */
    uint64_t wake_ns;             /* sleeping: clock time at which to wake */
    uint64_t cpu_ns;              /* time spent RUNNING */
    uint64_t cpu_ticks;           /* cpu_ns in scheduler ticks (10 ms) */
    uint64_t preempted_count;     /* times the timer took the CPU away */

    /* Multi-core (sched.c). */
    uint32_t cpu;                 /* CPU whose run queue owns this thread */
    bool pinned;                  /* never moved to another CPU */
    volatile bool kill;           /* terminate at the next scheduling point */
    uint8_t flags;                /* THREAD_* */
    uint32_t ran_on;              /* bit mask of CPUs it has run on */

    /* Wait queue this thread sleeps on (waitq.c), guarded by its run-queue
     * lock; NULL when not waiting. */
    const void *wait_on;
};

#define THREAD_IDLE       (1u << 0)  /* a CPU's idle thread */
#define THREAD_CONTAIN    (1u << 1)  /* a crash in it ends only the thread (crash demo) */
#define THREAD_ANY_CPU    (~0u)      /* thread_create_on: no pinning */

/* Which scheduler list a thread is on (struct thread.sched_list). */
#define SCHED_LIST_NONE   0
#define SCHED_LIST_READY  1
#define SCHED_LIST_SLEEP  2

/* ---- lifecycle ---------------------------------------------------------- */

/* Create the kernel process (PID 0) and adopt the calling boot flow as its
 * thread 0. Requires the heap and kernel stacks to be initialised. */
void proc_init(void);

struct process *process_create(const char *name, uint32_t flags, proc_status_t *err);

/* Terminate every thread of a process. Refused (BUSY) if the calling thread
 * belongs to it; that thread must use thread_exit instead. */
proc_status_t process_terminate(struct process *p);

/* Create a READY kernel thread in process p that will run entry(arg) when
 * first switched to. Its return value becomes the exit code. It may run
 * on any CPU (the scheduler places it and may move it). */
struct thread *thread_create(struct process *p, const char *name,
                             thread_entry_t entry, void *arg, proc_status_t *err);

/* Same, but pinned to CPU `cpu` (or THREAD_ANY_CPU for no pinning). */
struct thread *thread_create_on(struct process *p, const char *name, thread_entry_t entry,
                                void *arg, unsigned cpu, proc_status_t *err);

/* An idle thread for an application processor that is about to start:
 * its stack is where the CPU starts, so it is RUNNING from the outset. */
struct thread *thread_create_idle(unsigned cpu);

/* Mark a thread TERMINATED. One running on another CPU is marked `kill`
 * and ends at that CPU's next scheduling point. */
proc_status_t thread_terminate(struct thread *t);

/* Release a TERMINATED thread's stack and structure. */
proc_status_t thread_destroy(struct thread *t);

/* End the calling thread. Its resources stay allocated (so nothing frees
 * the stack we are standing on) until proc_reap() runs on another thread. */
void thread_exit(int64_t code) __attribute__((noreturn));

/* Destroy every TERMINATED thread and every TERMINATED process without
 * threads. Returns the number of threads reclaimed. */
unsigned proc_reap(void);

/* Give empty slab pages of the thread/process caches back to the page
 * allocator (they are otherwise kept for reuse). */
void proc_trim_caches(void);

/* Pages currently held by the thread and process slab caches. */
uint64_t proc_cache_pages(void);

/* ---- switching and state ---------------------------------------------- */

/* Suspend the calling thread (RUNNING -> READY, unless it already set
 * itself BLOCKED/TERMINATED) and resume `next`, which must be READY.
 * Returns when some thread switches back to the caller. Caller errors are
 * returned; detected CPU-state corruption panics. */
proc_status_t thread_switch(struct thread *next);

/* Validated state change for use by the scheduler. Entering RUNNING is
 * reserved to thread_switch, and a running thread ends via thread_exit.
 * A thread running on another CPU cannot be changed (BUSY). */
proc_status_t thread_set_state(struct thread *t, task_state_t state);

/* For sched.c: the same, with t's run-queue lock already held. */
proc_status_t thread_set_state_locked(struct thread *t, task_state_t state);

/* For sched.c: switch from the calling thread to `next` (READY, on this
 * CPU's queue). This CPU's run-queue lock must be held; the thread that
 * runs next releases it (sched_finish_switch). */
void thread_switch_to(struct thread *next);

/* Saved context of a suspended thread, or NULL while it runs. */
const struct cpu_context *thread_get_context(const struct thread *t);

/* First READY thread after `after` in TID-table order (wrapping), or NULL.
 * A lookup helper only; choosing what runs is Phase 5's job. */
struct thread *thread_next_ready(const struct thread *after);

/* ---- registry --------------------------------------------------------- */

struct thread *thread_current(void);
struct process *process_current(void);
struct process *process_kernel(void);
struct process *process_find(pid_t pid);
struct thread *thread_find(tid_t tid);

unsigned process_count(void);
unsigned thread_count(void);
/* Table slot enumeration: returns NULL for empty slots. */
struct process *process_at(unsigned slot);
struct thread *thread_at(unsigned slot);

/* ---- diagnostics ------------------------------------------------------ */

void thread_dump(const struct thread *t);
void proc_dump_table(void);
const char *task_state_str(task_state_t s);
const char *proc_status_str(proc_status_t s);

#endif /* BHARAT_PROC_H */
