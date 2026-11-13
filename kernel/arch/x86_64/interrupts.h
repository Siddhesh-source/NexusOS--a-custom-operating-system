#ifndef BHARAT_INTERRUPTS_H
#define BHARAT_INTERRUPTS_H

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

#define ISR_STUB_COUNT 256  /* every vector has a stub */
extern const uint64_t isr_stub_table[ISR_STUB_COUNT];

/* Handler for a vector >= 32 (device or timer interrupt). It runs with
 * interrupts off and must send its own end-of-interrupt. */
typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);
void interrupt_set_handler(uint8_t vector, interrupt_handler_t fn);

/* Interrupts that arrived and were ignored: legacy PIC spurious IRQs and
 * the APIC spurious vector. */
uint64_t interrupt_spurious_count(void);

/* Called from isr_common for every vector. */
void interrupt_dispatch(struct interrupt_frame *frame);

/* Print the saved general-purpose registers and CPU frame. */
void interrupt_dump_frame(const struct interrupt_frame *frame);

/* Report an unrecoverable exception and halt the CPU. */
void exception_fatal(const struct interrupt_frame *frame, const char *reason)
    __attribute__((noreturn));

/* An exception nobody handled: ends only the current thread if it asked
 * for that (crash.h), otherwise exception_fatal. Returns only in the
 * first case. */
void exception_unhandled(struct interrupt_frame *frame, const char *reason);

const char *exception_name(uint64_t vector);

#endif /* BHARAT_INTERRUPTS_H */
