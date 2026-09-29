; NEXUS OS - x86-64 entry stub. Limine enters at _start in long mode
; with paging + a usable stack. We zero BSS, set our own stack, call
; the C entry, then halt forever if it ever returns.

bits 64

section .text
global _start

extern kernel_main
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
