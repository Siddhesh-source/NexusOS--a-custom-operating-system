#ifndef NEXUS_INTERRUPTS_H
#define NEXUS_INTERRUPTS_H

#include <stdint.h>

/* Stack layout built by isr_common in interrupt_asm.asm. Field order MUST
 * match the push order there (lowest address first). Handlers may modify
 * the frame; iretq resumes from the (possibly updated) rip/rsp. */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    /* Pushed by the CPU */
    uint64_t rip, cs, rflags, rsp, ss;
};

#define ISR_STUB_COUNT 33   /* vectors 0-31 (exceptions) + 32 (timer) */
extern const uint64_t isr_stub_table[ISR_STUB_COUNT];

/* Called from isr_common for every vector. */
void interrupt_dispatch(struct interrupt_frame *frame);

/* Print the saved general-purpose registers and CPU frame. */
void interrupt_dump_frame(const struct interrupt_frame *frame);

/* Report an unrecoverable exception and halt the CPU. */
void exception_fatal(const struct interrupt_frame *frame, const char *reason)
    __attribute__((noreturn));

const char *exception_name(uint64_t vector);

#endif /* NEXUS_INTERRUPTS_H */
