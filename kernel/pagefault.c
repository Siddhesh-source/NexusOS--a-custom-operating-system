#include "pagefault.h"
#include "vmm.h"
#include "cpu.h"
#include "kprintf.h"

extern char mm_probe_read_insn[], mm_probe_write_insn[];
extern char mm_probe_exec_ret[], mm_probe_fixup[];

struct page_fault_info pf_last;
volatile uint64_t pf_count;

bool fault_try_fixup(struct interrupt_frame *frame)
{
    if (frame->rip == (uint64_t)mm_probe_read_insn
        || frame->rip == (uint64_t)mm_probe_write_insn) {
        frame->rip = (uint64_t)mm_probe_fixup;
        return true;
    }

    /* Faulting fetch inside mm_probe_exec's callee: the call's return
     * address is on top of the stack. Pop it and resume at the fixup. */
    if (frame->vector == 14 && (frame->error_code & PF_ERR_IFETCH)
        && *(const uint64_t *)frame->rsp == (uint64_t)mm_probe_exec_ret) {
        frame->rsp += 8;
        frame->rip = (uint64_t)mm_probe_fixup;
        return true;
    }
    return false;
}

void page_fault_handler(struct interrupt_frame *frame)
{
    struct page_fault_info info = {
        .address = read_cr2(),
        .rip = frame->rip,
        .error_code = frame->error_code,
        .present = frame->error_code & PF_ERR_PRESENT,
        .write = frame->error_code & PF_ERR_WRITE,
        .user = frame->error_code & PF_ERR_USER,
        .instruction_fetch = frame->error_code & PF_ERR_IFETCH,
        .reserved_bit = frame->error_code & PF_ERR_RSVD,
    };

    const char *access = info.instruction_fetch ? "instruction fetch"
                       : info.write ? "write" : "read";

    kprintf("\n[#PF] Page fault\n");
    kprintf("[#PF]   faulting address : %p (%s)\n",
            (void *)info.address, vmm_describe_address(info.address));
    kprintf("[#PF]   instruction ptr  : %p\n", (void *)info.rip);
    kprintf("[#PF]   error code       : 0x%lx [P=%d W=%d U=%d RSVD=%d I/D=%d]\n",
            info.error_code, info.present, info.write, info.user,
            info.reserved_bit, info.instruction_fetch);
    kprintf("[#PF]   access           : %s\n", access);
    kprintf("[#PF]   mode             : %s\n", info.user ? "user" : "kernel");
    kprintf("[#PF]   cause            : %s\n",
            info.reserved_bit ? "reserved bit set in paging structure"
            : info.present ? "protection violation (page present)"
            : "page not present");

    info.recovered = fault_try_fixup(frame);
    pf_last = info;
    pf_count++;

    if (info.recovered) {
        kprintf("[#PF]   outcome          : recovered (memory probe fixup)\n");
        return;
    }

    /* No demand paging, swap or copy-on-write yet: any other fault is a
     * kernel bug. */
    exception_fatal(frame, "Unhandled page fault - kernel halted");
}
