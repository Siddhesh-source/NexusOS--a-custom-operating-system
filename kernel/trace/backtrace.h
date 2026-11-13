#ifndef BHARAT_BACKTRACE_H
#define BHARAT_BACKTRACE_H

/* Call traces with function names.
 *
 * The kernel is built with frame pointers: every function saves the
 * caller's RBP and sets RBP to its own frame, so [rbp] is the previous
 * frame and [rbp+8] is the return address. Walking that chain gives the
 * list of calls that led here.
 *
 * Names come from `ksym_table`, a sorted list of every function's start
 * address generated from the kernel ELF at build time (scripts/ksyms.py).
 *
 * Limits: assembly routines without a frame (interrupt stubs,
 * context_switch) do not appear; inlined functions show as their caller.
 * The walk stops at the first frame that is not readable kernel memory or
 * whose return address is not kernel code, so a corrupt stack never
 * causes a second fault. */

#include <stdint.h>
#include <stdbool.h>

struct ksym {
    uint64_t addr;
    uint64_t size;          /* 0 for assembly labels */
    const char *name;
};

/* Generated table, sorted by address, plus an end marker. */
extern const struct ksym ksym_table[];
extern const uint64_t ksym_count;

#define BACKTRACE_MAX_DEPTH 32

/* The function containing `addr`, or NULL if it is not kernel code.
 * `*offset` (if not NULL) receives addr - function start. */
const struct ksym *ksym_lookup(uint64_t addr, uint64_t *offset);

/* Fill `out` with return addresses, starting from the frame `rbp`.
 * Returns how many were found (at most `max`). */
unsigned backtrace_collect(uint64_t rbp, uint64_t *out, unsigned max);

/* Print "Call trace" with one symbolised line per frame. If `rip` is not
 * 0 it is printed first as the exact faulting instruction. */
void backtrace_print(uint64_t rip, uint64_t rbp);

/* Print the call trace of the caller of this function. */
void backtrace_print_here(void);

#endif /* BHARAT_BACKTRACE_H */
