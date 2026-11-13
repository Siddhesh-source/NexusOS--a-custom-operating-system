#ifndef BHARAT_CRASH_H
#define BHARAT_CRASH_H

/* Contained crashes: a fault in a thread marked THREAD_CONTAIN ends only
 * that thread instead of halting the machine.
 *
 * The full report is still printed (what went wrong, registers, call
 * trace), with "[crash]" lines instead of "[exception]". The faulting
 * thread is then sent to thread_exit() by changing where the interrupt
 * returns to, and everything else keeps running.
 *
 * Used for the shell's `crashdemo` and for the V1 apps (kernel threads):
 * a bug in an app shows a report and the app is restarted. Containment
 * is refused (the machine halts as before) when it would not be safe:
 * the thread holds a spinlock, the fault is a double fault, NMI or
 * machine check, or a kernel panic is already under way.
 *
 * Limits: only threads that opted in; memory the thread allocated is not
 * freed (there is no owner tracking for kernel threads yet). */

#include <stdbool.h>
#include <stdint.h>
#include "interrupts.h"
#include "proc.h"

struct crash_info {
    uint64_t count;               /* contained crashes since boot */
    tid_t tid;                    /* the last one: */
    char name[TASK_NAME_LEN];
    uint64_t vector;
    uint64_t rip;
    uint64_t address;             /* page faults: the address touched */
    uint64_t time_ns;
};

/* Try to contain a fault. True: the report is printed and the frame now
 * returns into thread_exit(); the caller just returns. */
bool crash_contain(struct interrupt_frame *frame, const char *reason);

void crash_get_last(struct crash_info *out);

#endif /* BHARAT_CRASH_H */
