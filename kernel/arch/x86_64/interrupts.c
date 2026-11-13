#include "interrupts.h"
#include <stddef.h>
#include "kprintf.h"
#include "cpu.h"
#include "timer.h"
#include "pagefault.h"
#include "backtrace.h"
#include "trace.h"
#include "apic.h"
#include "panic.h"

static const char *const exception_names[32] = {
    "Divide-by-zero (#DE)",           "Debug (#DB)",
    "Non-maskable interrupt (NMI)",   "Breakpoint (#BP)",
    "Overflow (#OF)",                 "BOUND range exceeded (#BR)",
    "Invalid opcode (#UD)",           "Device not available (#NM)",
    "Double fault (#DF)",             "Coprocessor segment overrun",
    "Invalid TSS (#TS)",              "Segment not present (#NP)",
    "Stack-segment fault (#SS)",      "General protection fault (#GP)",
    "Page fault (#PF)",               "Reserved",
    "x87 floating-point error (#MF)", "Alignment check (#AC)",
    "Machine check (#MC)",            "SIMD floating-point (#XM)",
    "Virtualization exception (#VE)", "Control protection (#CP)",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection (#HV)",     "VMM communication (#VC)",
    "Security exception (#SX)",       "Reserved",
};

const char *exception_name(uint64_t vector)
{
    return vector < 32 ? exception_names[vector] : "Interrupt";
}

void interrupt_dump_frame(const struct interrupt_frame *f)
{
    kprintf("  rip=%p cs=%04lx rflags=%p\n", (void *)f->rip, f->cs, (void *)f->rflags);
    kprintf("  rsp=%p ss=%04lx\n", (void *)f->rsp, f->ss);
    kprintf("  rax=%p rbx=%p rcx=%p\n", (void *)f->rax, (void *)f->rbx, (void *)f->rcx);
    kprintf("  rdx=%p rsi=%p rdi=%p\n", (void *)f->rdx, (void *)f->rsi, (void *)f->rdi);
    kprintf("  rbp=%p r8 =%p r9 =%p\n", (void *)f->rbp, (void *)f->r8, (void *)f->r9);
    kprintf("  r10=%p r11=%p r12=%p\n", (void *)f->r10, (void *)f->r11, (void *)f->r12);
    kprintf("  r13=%p r14=%p r15=%p\n", (void *)f->r13, (void *)f->r14, (void *)f->r15);
    kprintf("  cr2=%p cr3=%p\n", (void *)read_cr2(), (void *)read_cr3());
}

void exception_fatal(const struct interrupt_frame *f, const char *reason)
{
    if (!panic_begin())
        halt_forever();         /* another CPU is already reporting a crash */
    kprintf("\n[exception] %s\n", exception_name(f->vector));
    kprintf("[exception] Exception number: %lu\n", f->vector);
    kprintf("[exception] Error code: 0x%lx\n", f->error_code);
    kprintf("[exception] Instruction pointer: %p\n", (void *)f->rip);
    if (reason != NULL)
        kprintf("[exception] %s\n", reason);
    kprintf("[exception] Saved registers:\n");
    interrupt_dump_frame(f);
    /* Start at the faulting instruction, then walk the interrupted code's
     * frames (the interrupt stubs keep RBP untouched). */
    backtrace_print(f->rip, f->rbp);
    trace_dump(TRACE_DUMP_ON_CRASH);
    kprintf("[exception] Halting...\n");
    halt_forever();
}

static interrupt_handler_t handlers[256];
static uint64_t spurious;

void interrupt_set_handler(uint8_t vector, interrupt_handler_t fn)
{
    if (vector >= 32)
        handlers[vector] = fn;
}

uint64_t interrupt_spurious_count(void)
{
    return spurious;
}

void interrupt_dispatch(struct interrupt_frame *frame)
{
    if (frame->vector >= 32 && handlers[frame->vector] != NULL) {
        handlers[frame->vector](frame);
        return;
    }
    /* A masked 8259 PIC can still raise a "spurious" IRQ 7 or 15, and the
     * local APIC its spurious vector. Neither needs an end-of-interrupt. */
    if (frame->vector == APIC_SPURIOUS_VECTOR
        || (pic_is_masked() && (frame->vector == 39 || frame->vector == 47))) {
        spurious++;
        return;
    }
    switch (frame->vector) {
    case 2: /* NMI: another CPU is reporting a crash and stops us */
        if (kernel_panic_started())
            halt_forever();
        exception_fatal(frame, "Unexpected non-maskable interrupt");
    case 14: /* Page fault */
        page_fault_handler(frame);
        break;
    case 13: /* General protection: recoverable only for registered probes */
        if (fault_try_fixup(frame)) {
            kprintf("[#GP] General protection fault at rip=%p (error 0x%lx) "
                    "- recovered by probe fixup\n",
                    (void *)frame->rip, frame->error_code);
            break;
        }
        exception_fatal(frame, NULL);
    case 32: /* IRQ0 - Timer */
        timer_irq();
        break;
    default:
        exception_fatal(frame, frame->vector < 32 ? NULL : "Unexpected interrupt vector");
    }
}
