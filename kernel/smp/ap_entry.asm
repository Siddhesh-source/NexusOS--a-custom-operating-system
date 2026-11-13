; BharatOS - where an application processor (AP) starts.
;
; Limine wakes each extra CPU in 64-bit mode on Limine's own page tables
; and stack, and jumps here with RDI = its struct limine_mp_info. Its
; `extra_argument` (offset 24) is the struct percpu the boot CPU prepared.
; We turn on the CPU features our page tables rely on (NX, write-protect,
; global pages), load our page tables, move onto the idle thread's stack
; (percpu.boot_stack, offset 16) and continue in C.

bits 64
section .text

global ap_entry
extern ap_main
extern smp_ap_cr3

ap_entry:
    cli
    mov     rbx, [rdi + 24]             ; struct percpu *

    mov     ecx, 0xC0000080             ; EFER.NXE: our tables use NX bits
    rdmsr
    or      eax, 1 << 11
    wrmsr

    mov     rax, cr0                    ; CR0.WP: honour read-only pages
    or      rax, 1 << 16
    mov     cr0, rax

    mov     rax, cr4                    ; CR4.PGE: global kernel pages
    or      rax, 1 << 7
    mov     cr4, rax

    mov     rax, [rel smp_ap_cr3]
    mov     cr3, rax

    mov     rax, cr4                    ; toggling PGE flushes any global
    and     rax, ~(1 << 7)              ; entries left from Limine's tables
    mov     cr4, rax
    or      rax, 1 << 7
    mov     cr4, rax

    mov     rsp, [rbx + 16]             ; percpu.boot_stack (16-byte aligned)
    xor     ebp, ebp                    ; outermost frame for call traces
    mov     rdi, rbx
    call    ap_main                     ; never returns

.halt:
    cli
    hlt
    jmp     .halt
