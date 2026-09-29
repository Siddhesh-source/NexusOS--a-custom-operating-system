; NEXUS OS - register-preservation probe for context-switch tests.
;
; uint64_t ctx_test_regs(void (*fn)(void *), void *arg, uint64_t seed);
;
; Loads seed-derived values into every callee-saved register (rbx, rbp,
; r12-r15), calls fn(arg) (which context-switches away and back), then
; checks that each register still holds its value. Returns a bitmask of the
; registers that came back wrong (0 = all preserved):
;   bit 0 rbx, 1 rbp, 2 r12, 3 r13, 4 r14, 5 r15

bits 64
section .text

global ctx_test_regs

%macro CHECK 3          ; register, bit, expected-value computation into rcx
    mov     rcx, rdx
    %3
    cmp     %1, rcx
    je      %%ok
    or      eax, 1 << %2
%%ok:
%endmacro

ctx_test_regs:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    push    rdx                 ; seed; 7 pushes keep rsp 16-byte aligned

    mov     rax, rdi            ; fn
    mov     rdi, rsi            ; arg

    lea     rbx, [rdx + 0x1111]
    lea     rbp, [rdx + 0x2222]
    mov     r12, rdx
    rol     r12, 17
    mov     r13, rdx
    not     r13
    mov     r14, rdx
    bswap   r14
    mov     r15, rdx
    neg     r15

    call    rax

    mov     rdx, [rsp]          ; seed
    xor     eax, eax
    CHECK   rbx, 0, {add rcx, 0x1111}
    CHECK   rbp, 1, {add rcx, 0x2222}
    CHECK   r12, 2, {rol rcx, 17}
    CHECK   r13, 3, {not rcx}
    CHECK   r14, 4, {bswap rcx}
    CHECK   r15, 5, {neg rcx}

    pop     rdx
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret
