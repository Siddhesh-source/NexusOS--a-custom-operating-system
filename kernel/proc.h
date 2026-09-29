#ifndef NEXUS_PROC_H
#define NEXUS_PROC_H

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
 * Single CPU, not SMP-safe. Table updates and switches run with interrupts
 * disabled. */

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
    TASK_RUNNING,       /* executing on the CPU (exactly one thread) */
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
    uint64_t wake_tick;           /* sleeping: tick at which to wake */
    uint64_t cpu_ticks;           /* timer ticks that hit while RUNNING */
    uint64_t preempted_count;     /* times the timer took the CPU away */
};

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
 * first switched to. Its return value becomes the exit code. */
struct thread *thread_create(struct process *p, const char *name,
                             thread_entry_t entry, void *arg, proc_status_t *err);

/* Mark a thread that is not running as TERMINATED. */
proc_status_t thread_terminate(struct thread *t);

/* Release a TERMINATED thread's stack and structure. */
proc_status_t thread_destroy(struct thread *t);

/* End the calling thread. Its resources stay allocated (so nothing frees
 * the stack we are standing on) until proc_reap() runs on another thread. */
void thread_exit(int64_t code) __attribute__((noreturn));

/* Destroy every TERMINATED thread and every TERMINATED process without
 * threads. Returns the number of threads reclaimed. */
unsigned proc_reap(void);

/* ---- switching and state ---------------------------------------------- */

/* Suspend the calling thread (RUNNING -> READY, unless it already set
 * itself BLOCKED/TERMINATED) and resume `next`, which must be READY.
 * Returns when some thread switches back to the caller. Caller errors are
 * returned; detected CPU-state corruption panics. */
proc_status_t thread_switch(struct thread *next);

/* Validated state change for use by the scheduler. Entering RUNNING is
 * reserved to thread_switch, and a running thread ends via thread_exit. */
proc_status_t thread_set_state(struct thread *t, task_state_t state);

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

#endif /* NEXUS_PROC_H */
