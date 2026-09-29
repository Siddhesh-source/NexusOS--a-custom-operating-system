; NEXUS OS - full-register preemption probe for scheduler tests.
;
; uint64_t preempt_reg_spin(uint64_t seed, volatile uint64_t *stop,
;                           volatile uint64_t *iterations);
;
; Loads a distinct seed-derived value into ALL 15 general-purpose registers
; (rax rbx rcx rdx rsi rdi rbp r8-r15), then spins comparing every register
; against its expected value until *stop != 0. It never yields and never
; calls anything, so the only way it loses the CPU is timer preemption, and
; the only thing that preserves the caller-saved registers across that is
; the interrupt frame. Returns the number of mismatches observed (0 = every
; register survived every preemption).

bits 64
section .text

global preempt_reg_spin

%define SLOT_STOP   0
%define SLOT_ITERS  8
%define SLOT_BAD    16
%define SLOT_REGS   24          ; 15 expected values
%define FRAME       (SLOT_REGS + 15 * 8)

%macro LOADREG 2                ; register, index
    mov     %1, [rsp + SLOT_REGS + 8 * %2]
%endmacro

%macro CHECKREG 2               ; register, index
    cmp     %1, [rsp + SLOT_REGS + 8 * %2]
    jne     .bad
%endmacro

%macro ALLREGS 1                ; apply macro to every register with its index
    %1 rax, 0
    %1 rbx, 1
    %1 rcx, 2
    %1 rdx, 3
    %1 rsi, 4
    %1 rdi, 5
    %1 rbp, 6
    %1 r8,  7
    %1 r9,  8
    %1 r10, 9
    %1 r11, 10
    %1 r12, 11
    %1 r13, 12
    %1 r14, 13
    %1 r15, 14
%endmacro

preempt_reg_spin:
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15
    sub     rsp, FRAME

    mov     [rsp + SLOT_STOP], rsi
    mov     [rsp + SLOT_ITERS], rdx
    mov     qword [rsp + SLOT_BAD], 0

    ; expected[k] = seed ^ ((k + 1) * 0x0F0F0F0F0F0F0F0F)
%assign k 0
%rep 15
    mov     rax, (k + 1) * 0x0F0F0F0F0F0F0F0F
    xor     rax, rdi
    mov     [rsp + SLOT_REGS + 8 * k], rax
%assign k k+1
%endrep

.reload:
    ALLREGS LOADREG

.loop:
    ALLREGS CHECKREG

    ; ++*iterations and test *stop without disturbing any checked register
    ; (push/pop keep rax; pop does not change flags).
    push    rax
    mov     rax, [rsp + 8 + SLOT_ITERS]
    inc     qword [rax]
    mov     rax, [rsp + 8 + SLOT_STOP]
    cmp     qword [rax], 0
    pop     rax
    je      .loop
    jmp     .done

.bad:
    inc     qword [rsp + SLOT_BAD]
    jmp     .reload

.done:
    mov     rax, [rsp + SLOT_BAD]
    add     rsp, FRAME
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    ret
