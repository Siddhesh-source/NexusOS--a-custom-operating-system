; NEXUS OS - Memory probes for testing invalid accesses.
;
; Each probe performs exactly one memory access at a labelled instruction.
; If that instruction faults, fault_try_fixup() (pagefault.c) recognises the
; RIP and resumes at mm_probe_fixup, which makes the probe return 1. This is
; the only way a kernel page fault is survivable at this stage.

bits 64
section .text

global mm_probe_read
global mm_probe_write
global mm_probe_exec
global mm_probe_read_insn
global mm_probe_write_insn
global mm_probe_exec_ret
global mm_probe_fixup

; int mm_probe_read(uint64_t addr /* rdi */, uint64_t *out /* rsi */)
mm_probe_read:
    xor     eax, eax
mm_probe_read_insn:
    mov     rcx, [rdi]
    mov     [rsi], rcx
    ret

; int mm_probe_write(uint64_t addr /* rdi */, uint64_t value /* rsi */)
mm_probe_write:
    xor     eax, eax
mm_probe_write_insn:
    mov     [rdi], rsi
    ret

; int mm_probe_exec(uint64_t addr /* rdi */)
; Calls into addr. An instruction-fetch fault there has RIP == addr, so the
; fixup identifies it by the return address on top of the faulting stack.
mm_probe_exec:
    xor     eax, eax
    call    rdi
mm_probe_exec_ret:
    ret

mm_probe_fixup:
    mov     eax, 1
    ret
