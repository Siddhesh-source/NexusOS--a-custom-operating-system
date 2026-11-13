#include "crash.h"
#include "percpu.h"
#include "spinlock.h"
#include "panic.h"
#include "backtrace.h"
#include "clock.h"
#include "cpu.h"
#include "kprintf.h"
#include "serial.h"
#include "string.h"

static struct crash_info last;

/* Where a contained thread "returns" to from the fault. */
static void __attribute__((noreturn, used)) crash_exit_thread(int64_t code)
{
    thread_exit(code);
}

static bool containable_vector(uint64_t v)
{
    /* #DE, #UD, #GP, #PF, #BR, #OF, #AC: the thread's own fault, taken on
     * its own stack. Never #DF (it runs on the emergency stack), NMI or
     * machine check. */
    return v == 0 || v == 4 || v == 5 || v == 6 || v == 13 || v == 14 || v == 17;
}

bool crash_contain(struct interrupt_frame *f, const char *reason)
{
    struct thread *t = percpu_current();
    if (t == NULL || !(t->flags & THREAD_CONTAIN) || (t->flags & THREAD_IDLE)
        || kernel_panicking() || !containable_vector(f->vector) || spin_held_count() != 0)
        return false;

    uint64_t addr = f->vector == 14 ? read_cr2() : 0;
    uint64_t flags = console_lock();
    kprintf("\n[crash] Thread \"%s\" (TID %u) crashed: %s\n", t->name, t->tid,
            exception_name(f->vector));
    if (reason != NULL && f->vector != 14)   /* the #PF reason says "kernel halted" */
        kprintf("[crash] %s\n", reason);
    if (f->vector == 14)
        kprintf("[crash] It touched address %p, which is not usable memory.\n", (void *)addr);
    kprintf("[crash] Registers:\n");
    interrupt_dump_frame(f);
    backtrace_print(f->rip, f->rbp);
    kprintf("[crash] Only this thread was stopped; the rest of the system keeps running.\n");

    /* Recorded under the console lock, so two CPUs crashing at once do
     * not mix their records. */
    last.count++;
    last.tid = t->tid;
    strlcpy(last.name, t->name, sizeof(last.name));
    last.vector = f->vector;
    last.rip = f->rip;
    last.address = addr;
    last.time_ns = clock_now_ns();
    console_unlock(flags);

    /* Resume in crash_exit_thread(-1 - vector) on the thread's own stack,
     * aligned as if it had just been called. */
    f->rip = (uint64_t)crash_exit_thread;
    f->rdi = (uint64_t)(-1 - (int64_t)f->vector);
    f->rsp = (f->rsp & ~0xFull) - 8;
    f->rbp = 0;                     /* call traces stop here */
    return true;
}

void crash_get_last(struct crash_info *out)
{
    uint64_t flags = console_lock();
    *out = last;
    console_unlock(flags);
}
