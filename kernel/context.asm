; NEXUS OS - x86-64 kernel-thread context switch.
;
; See context.h for which registers are preserved and why. The saved context
; is a struct cpu_context built on the outgoing thread's own stack:
;
;   high  [rip   ]  <- pushed by the caller's `call context_switch`
;         [rflags]
;         [rbx   ]
;         [rbp   ]
;         [r12   ]
;         [r13   ]
;         [r14   ]
;   low   [r15   ]  <- saved rsp (stored into *save_rsp)

bits 64
section .text

global context_switch
global thread_trampoline
extern thread_bootstrap

; void context_switch(uint64_t *save_rsp /* rdi */, uint64_t load_rsp /* rsi */)
context_switch:
    pushfq
    push    rbx
    push    rbp
    push    r12
    push    r13
    push    r14
    push    r15

    mov     [rdi], rsp          ; outgoing thread is now fully suspended
    mov     rsp, rsi            ; from here on we run on the incoming stack

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rbp
    pop     rbx
    popfq                       ; restores the incoming thread's IF
    ret                         ; resumes at the incoming thread's saved rip

; Entered via the `ret` above the first time a new thread runs. thread_create
; builds the initial context so that r12 = struct thread *, rbp = 0 (ends
; stack traces) and rsp is 16-byte aligned here, as the ABI requires just
; before a `call`.
thread_trampoline:
    mov     rdi, r12
    call    thread_bootstrap    ; never returns
    ud2
