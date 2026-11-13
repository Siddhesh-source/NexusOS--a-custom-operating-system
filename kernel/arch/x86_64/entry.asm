; BharatOS - x86-64 entry stub. Limine enters at _start in long mode
; with paging + a usable stack. We zero BSS, set our own stack, call
; the C entry, then halt forever if it ever returns.

bits 64

section .text
global _start

extern kernel_main
extern __stack_chk_guard
extern __bss_start
extern __bss_end

align 16

_start:
    lea     rdi, [rel __bss_start]
    lea     rcx, [rel __bss_end]
    sub     rcx, rdi
    xor     eax, eax
    rep     stosb

    lea     rsp, [rel stack_top]

    ; Stack protector canary: mix the time stamp counter into the default
    ; value so it differs on every boot. Done here, before any C function
    ; with a canary has started, so no saved copy can go stale. The low
    ; byte stays 0 (see stack_protector.c).
    rdtsc
    shl     rdx, 32
    or      rax, rdx
    mov     rdx, 0x9e3779b97f4a7c15     ; spreads TSC bits over all 64
    imul    rax, rdx
    xor     rax, [rel __stack_chk_guard]
    xor     al, al
    mov     [rel __stack_chk_guard], rax

    ; RBP = 0 marks the outermost frame for call-trace walks.
    xor     ebp, ebp
    call    kernel_main

.halt:
    cli
    hlt
    jmp     .halt

section .bss
align 4096

global stack_top
global stack_bottom
stack_bottom:
    resb 65536
stack_top:
