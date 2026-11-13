#ifndef BHARAT_TRACE_H
#define BHARAT_TRACE_H

/* Event trace: one fixed-size ring of small timestamped records that any
 * subsystem can add to, and that is printed when the kernel crashes.
 *
 * Recording is cheap (a few stores, no allocation, no printing), so it is
 * safe from the timer interrupt and always on. When the ring is full the
 * oldest events are overwritten; `trace_dump()` prints the newest ones.
 *
 * Limits: one ring for all CPUs, written without a lock (each event
 * records its CPU; an event still being written when the ring is read is
 * skipped); two 64-bit arguments per event; the meaning of the arguments
 * depends on the event type. */

#include <stdint.h>
#include <stdbool.h>

#define TRACE_CAPACITY        256   /* events kept; must be a power of two */
#define TRACE_DUMP_ON_CRASH   16    /* events printed by panic/exceptions */

typedef enum {
    TRACE_NONE = 0,
    TRACE_SCHED_SWITCH,     /* a = from TID, b = to TID */
    TRACE_SCHED_PREEMPT,    /* a = from TID, b = to TID (quantum expired) */
    TRACE_THREAD_CREATE,    /* a = TID, b = PID */
    TRACE_THREAD_EXIT,      /* a = TID, b = exit code */
    TRACE_PAGE_FAULT,       /* a = faulting address, b = instruction */
    TRACE_HEAP_GROW,        /* a = new heap end, b = pages added */
    TRACE_TEST,             /* a, b = free for self-tests */
    TRACE_ALARM_SET,        /* a = deadline (ns), b = now (ns) */
    TRACE_ALARM_FIRE,       /* a = deadline (ns), b = now (ns) */
    TRACE_SCHED_STEAL,      /* a = TID taken, b = CPU it was taken from */
    TRACE_TYPE_COUNT
} trace_type_t;

struct trace_event {
    uint64_t tsc;           /* CPU time stamp counter when recorded */
    uint64_t seq;           /* 1, 2, 3, ... in recording order */
    uint32_t type;          /* trace_type_t */
    uint32_t cpu;           /* CPU that recorded it */
    uint64_t a, b;
};

void trace_event(trace_type_t type, uint64_t a, uint64_t b);

/* Print the newest `count` events, oldest first. */
void trace_dump(unsigned count);

/* Copy the newest `max` events (oldest first) into `out`; returns how
 * many were copied. */
unsigned trace_snapshot(struct trace_event *out, unsigned max);

/* Total events ever recorded (including overwritten ones). */
uint64_t trace_total(void);

const char *trace_type_name(trace_type_t type);

/* Turn recording off or on (for example to freeze the ring while
 * inspecting it). Returns the previous setting. */
bool trace_set_enabled(bool on);

#endif /* BHARAT_TRACE_H */
