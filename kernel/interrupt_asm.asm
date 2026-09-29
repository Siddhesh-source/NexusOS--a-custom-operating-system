; NEXUS OS - Assembly interrupt stubs for 64-bit mode
;
; Every stub leaves the stack in the same shape before jumping to isr_common:
;
;     [rsp+ 0]  vector number        (pushed by the stub)
;     [rsp+ 8]  error code           (pushed by the CPU, or a dummy 0)
;     [rsp+16]  RIP, CS, RFLAGS, RSP, SS   (pushed by the CPU)
;
; isr_common then pushes the 15 GPRs and passes a pointer to the whole block
; (struct interrupt_frame in interrupts.h) to interrupt_dispatch(). The C side
; may modify the frame (e.g. RIP for exception fixups); iretq resumes from it.

bits 64
section .text

extern interrupt_dispatch

; Exception without a CPU-pushed error code: push a dummy so frames match.
%macro ISR_NOERR 1
isr%1:
    push    qword 0
    push    qword %1
    jmp     isr_common
%endmacro

; Exception where the CPU already pushed an error code.
%macro ISR_ERR 1
isr%1:
    push    qword %1
    jmp     isr_common
%endmacro

isr_common:
    cld
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    ; The CPU aligns RSP to 16 before pushing its 5-qword frame; together
    ; with vector + error code + 15 GPRs the stack is 16-byte aligned here,
    ; as the SysV ABI requires at the call instruction.
    mov     rdi, rsp
    call    interrupt_dispatch

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax

    add     rsp, 16             ; drop vector + error code
    iretq

; CPU exceptions 0-31. Vectors 8, 10-14, 17, 21, 29, 30 carry an error code.
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; Timer interrupt (IRQ0 -> vector 32 after PIC remap)
ISR_NOERR 32

section .rodata
align 8

; Table of stub addresses indexed by vector, consumed by idt_init().
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 33
    dq isr %+ i
%assign i i+1
%endrep
