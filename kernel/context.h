#ifndef NEXUS_CONTEXT_H
#define NEXUS_CONTEXT_H

/* x86-64 kernel-thread CPU context (see context.asm).
 *
 * A context switch is always a *voluntary* C function call
 * (context_switch()), so the SysV AMD64 ABI that clang uses for this kernel
 * decides what must survive it:
 *
 *   - callee-saved: rbx, rbp, r12, r13, r14, r15, rsp  -> saved/restored
 *   - rflags: not callee-saved by the ABI, but it holds IF (interrupt
 *     enable). Each thread keeps its own IF across a switch  -> saved
 *   - rip: the return address pushed by the `call`       -> saved (implicitly)
 *   - rax, rcx, rdx, rsi, rdi, r8-r11: caller-saved. The compiler assumes a
 *     call destroys them, so they are dead here           -> not saved
 *   - x87/SSE/AVX state: the kernel is built with -mno-sse -mno-mmx and
 *     never touches the FPU, so there is no FP state      -> not saved
 *   - segment registers: flat and identical for every kernel thread
 *   - cr3: switched separately in C (thread_switch) when the next thread
 *     belongs to a process with a different address space
 *
 * The context lives ON THE THREAD'S OWN KERNEL STACK. A suspended thread is
 * fully described by one value: its saved stack pointer, which points at a
 * struct cpu_context. Resuming = load that rsp, pop the callee-saved
 * registers and rflags, and `ret` to the saved rip. */

#include <stdint.h>

struct cpu_context {
    /* Lowest address = the saved rsp. Order MUST match context.asm. */
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rflags;
    uint64_t rip;       /* return address; `ret` resumes here */
};

_Static_assert(sizeof(struct cpu_context) == 8 * 8, "cpu_context layout");

#define RFLAGS_RESERVED_1   (1ULL << 1)   /* bit 1 is always set */
#define RFLAGS_IF           (1ULL << 9)

/* Save the current context on the current stack, store the resulting rsp in
 * *save_rsp, then switch to the stack `load_rsp` and resume the context found
 * there. Returns (in the original thread) only when something later switches
 * back to it. Interrupts should be disabled by the caller. */
void context_switch(uint64_t *save_rsp, uint64_t load_rsp);

/* First "return address" of every new thread (context.asm). Expects the
 * struct thread pointer in r12 and calls thread_bootstrap(thread). */
extern char thread_trampoline[];

#endif /* NEXUS_CONTEXT_H */
