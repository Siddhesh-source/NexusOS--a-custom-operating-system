# NEXUS OS — Phase 4: Processes, Threads & Context Switching

Phase 4 adds the execution model that the scheduler (Phase 5), userspace and
syscalls will build on: processes, kernel threads with their own kernel
stacks, a saved CPU context, and a context-switch primitive.

There is **no scheduler**. A thread runs only when kernel code explicitly
calls `thread_switch()`, and that is what makes every test deterministic.

Out of scope for this phase: scheduling policy (round-robin, priorities),
timer preemption, user mode, syscalls, fork/exec, signals, IPC, locks and
other synchronisation primitives.

## Architecture

```text
                 NEXUS Kernel
                      │
             ┌────────┴────────┐
             │                 │
        Process Manager     Memory Manager
        (proc.c)            (pmm / vmm / heap / kstack)
             │                 │
       ┌─────┴─────┐           │  PML4 per process
       │           │           │  PMM frames for stacks
    Process A   Process B      │  heap for process/thread structs
       │           │           │
    Thread A    Thread B       │
       │           │           │
    CPU Context CPU Context    │   (saved on each thread's own kernel stack)
       │           │           │
       └──────┬────┘           │
              │                │
       Context Switch ←────────┘   (context.asm + CR3 switch in thread_switch)
```

| File | Role |
|---|---|
| `kernel/proc.h`, `proc.c` | process/thread model, registry, lifecycle, `thread_switch`, diagnostics |
| `kernel/context.h`, `context.asm` | x86-64 CPU context layout, `context_switch`, `thread_trampoline` |
| `kernel/kstack.h`, `kstack.c` | kernel-stack allocator (PMM frames + VMM mappings + guard + canary) |
| `kernel/proc_test.c`, `context_test.asm` | deterministic boot-time tests and the summary panel |
| `kernel/box.c` | fixed-width box drawing for diagnostics |

## Process and thread model

**Process** (`struct process`): an execution environment.

- `pid`: never reused (monotonic counter), so a stale PID can't alias a new process
- `state`: summarises its threads (see below)
- `as`: its address space, either its own PML4 (`PROC_OWN_ADDRESS_SPACE`) or the shared kernel space
- `threads`, `thread_count`: the threads it owns
- `name`, `parent`: metadata

**Thread** (`struct thread`): an independently schedulable context.

- `tid`: never reused
- `state`, `process` (owner)
- `stack`: its own kernel stack
- `saved_rsp`: its CPU context while suspended
- `entry`, `arg`, `exit_code`, `exit_to`, `switch_count`

```text
Process ──┬── address_space_t (own PML4, or the kernel's)
          └── Thread(s) ── kernel stack ── struct cpu_context
```

At boot, `proc_init()` creates the **kernel process (PID 0)** and adopts the
running boot flow (`kernel_main` on the boot stack) as **thread 0
(`kmain`)**. This gives the kernel something to switch away *from* and back
*to*.

### States

```text
            thread_create
                 │
                NEW ──────────────┐
                 │ (automatic)    │ thread_terminate
                 ▼                ▼
   ┌────────── READY ─────────► TERMINATED ──► proc_reap() frees it
   │             │                ▲
   │ thread_switch│               │ thread_exit / return from entry
   │             ▼                │
   └──────── RUNNING ─────────────┘
    (switched   │
      away)     │ thread_set_state(self, BLOCKED) + switch
                ▼
             BLOCKED ──► READY (thread_set_state)  or  ──► TERMINATED
```

- **Transitions are validated.** Invalid ones (e.g. TERMINATED→READY or
  READY→BLOCKED) are refused with `PROC_ERR_BAD_STATE` and a diagnostic line.
- **RUNNING is entered only by `thread_switch`.** Only one thread is ever
  RUNNING, and it is always `thread_current()`.
- **Process state is derived from its threads.** It is RUNNING if a thread
  runs, READY if one is ready, then BLOCKED, then NEW. When the last live
  thread terminates, the process becomes TERMINATED.

## Kernel stacks

Each thread has its own 16 KiB kernel stack. There is no shared stack.

```text
KSTACK_REGION_BASE = 0xffffe00000000000, 256 slots × 32 KiB

slot i:  [ guard: 16 KiB, never mapped | stack: 16 KiB, mapped RW NX global ]
                                        ^ base (canary)              top ^
```

- **Allocation (`kstack_alloc`):** 4 PMM frames are mapped through the VMM
  into the shared kernel half, so the stack is valid in every address space.
  The stack is zeroed and a canary goes in its lowest qword. Allocation is
  all or nothing: on failure every frame is released.
- **Page tables:** `kstack_init` builds all page tables for the region once
  (`vmm_reserve_tables`), so creating a stack never allocates page tables.
- **Protection:**
  - Overflowing a stack hits the unmapped guard, raising #PF → #DF on the
    IST1 stack (see the `tstack` demo).
  - A smashed canary is caught on every switch and on destroy.
  - Freeing the stack you are running on panics.
- **Alignment:** `top` is 16-byte aligned. The initial frame is built so the
  first function a new thread calls sees a SysV-conformant stack.
- **Ownership:** the thread owns its stack. It is freed only by
  `thread_destroy`, which runs on a different thread.
- **Boot thread:** thread 0 uses the boot stack from `entry.asm`. Its lowest
  page is the Phase 3 guard page, so its canary sits one page above.

## CPU context

A switch is always a **voluntary C call** (`context_switch`), so the SysV
AMD64 ABI that clang uses for this kernel decides what must be kept:

| State | Saved? | Why |
|---|---|---|
| `rbx, rbp, r12–r15` | yes | callee-saved: the caller expects them unchanged |
| `rsp` | yes, as `thread->saved_rsp` | identifies the whole suspended context |
| `rip` | yes, implicitly | the return address pushed by `call context_switch` |
| `rflags` | yes | not callee-saved, but holds IF: each thread keeps its own interrupt-enable state |
| `rax rcx rdx rsi rdi r8–r11` | no | caller-saved: already dead at the call site |
| x87/SSE/AVX | no | the kernel is built with `-mno-sse -mno-mmx` and never uses the FPU |
| segment registers | no | flat and identical for all kernel threads |
| `cr3` | switched in C | `thread_switch` loads the next process's PML4 if it differs |

```c
struct cpu_context {          /* lowest address = saved rsp */
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t rflags;
    uint64_t rip;             /* `ret` resumes here */
};
```

**The context lives on the thread's own stack.** `saved_rsp` points at it.
It is valid only while the thread is not RUNNING.

## Context-switch mechanism

`context_switch(uint64_t *save_rsp, uint64_t load_rsp)` in `context.asm` does
the following:

1. `pushfq`, then push `rbx rbp r12 r13 r14 r15` onto the **outgoing** stack.
2. `mov [save_rsp], rsp`: the outgoing thread is now fully described by that
   value.
3. `mov rsp, load_rsp`: from here on, execution is on the **incoming** stack.
4. Pop `r15 … rbx`, then `popfq`.
5. `ret`: resume at the incoming thread's saved return address.

`thread_switch(next)` wraps it with policy-free bookkeeping:

1. Validate `next`: it must be registered, READY, and not the caller.
   Caller errors are returned.
2. Disable interrupts.
3. Check the outgoing thread's canary, and the incoming thread's canary,
   saved-RSP bounds, saved RIP (must be in kernel `.text`) and RFLAGS.
   **Any corruption panics**, printing a full thread dump (see the `ctx`
   demo). Corrupted CPU state is never loaded.
4. Update states: the outgoing thread goes RUNNING→READY unless it already
   set itself BLOCKED or TERMINATED; the incoming thread becomes RUNNING.
5. Switch CR3 if the processes' address spaces differ. This is safe mid-switch
   because the kernel code, the stacks and the heap all live in the shared
   kernel half.
6. Call `context_switch`. When some later switch returns here, restore this
   thread's interrupt flag.

**A new thread's first run.** `thread_create` builds an initial
`cpu_context` so the first `ret` lands in `thread_trampoline`:

- `r12` holds the thread pointer, `rbp` is 0 (ends backtraces),
  `rflags = 0x2` (IF off), and rsp is 16-byte aligned.
- The trampoline calls `thread_bootstrap(t)`, which enables interrupts, runs
  `entry(arg)`, and passes the return value to `thread_exit`.

**Exit (Phase 5 update).** Once the scheduler is running, `thread_exit` hands the CPU to the scheduler (see [scheduler.md](scheduler.md)). Before that, during the Phase 4 boot tests, `thread_exit` marks the thread TERMINATED and switches to its
`exit_to` thread (the creator). If that thread isn't READY, it falls back to
kmain, then to any READY thread. This is the one spot where Phase 5's
scheduler will take over.

## Process ↔ address space

- Each process holds an `address_space_t *`. A thread reaches it through
  `thread->process->as`.
- `PROC_OWN_ADDRESS_SPACE` creates a fresh PML4 with a private user half and
  the **shared kernel half** (Phase 3 design), so kernel code, stacks and
  heap stay reachable after any CR3 switch.
- `thread_switch` changes CR3 only when crossing between processes.
- User mappings, user mode and fork/exec are not implemented yet; the
  structure is ready for them.

## Lifecycle and reclamation

```text
create → READY → RUNNING ⇄ READY/BLOCKED → TERMINATED → (proc_reap) → freed
```

- A thread cannot free the stack it is running on, so `thread_exit` only
  marks the thread TERMINATED and switches away. The stack and structure
  stay intact, which keeps post-mortem inspection safe.
- **`proc_reap()`**, run on another thread (today kmain; later the
  scheduler or an idle/reaper thread), destroys every TERMINATED thread
  (unmapping its stack and freeing its frames and struct). It then destroys
  every TERMINATED process with no threads left, freeing its PML4.
- **Registry-checked pointers.** Every API validates an object by looking it
  up in the registry *before* dereferencing it. A dangling pointer to a
  reaped thread or process is rejected with `PROC_ERR_INVALID` and is never
  read (this is tested).

## Registry

The registry is two fixed tables: `PROC_MAX = 64` processes and
`THREAD_MAX = 256` threads. Structs come from the kernel heap. The kernel
process and boot thread are static.

| Operation | API |
|---|---|
| create | `process_create`, `thread_create` |
| find | `process_find(pid)`, `thread_find(tid)` |
| enumerate | `process_at(slot)`, `thread_at(slot)`, `process_count`, `thread_count` |
| terminate | `thread_terminate`, `thread_exit`, `process_terminate` |
| reclaim | `thread_destroy`, `proc_reap` |

## Primitives for the Phase 5 scheduler

| Spec name | NEXUS function |
|---|---|
| `create_thread()` | `thread_create()` |
| `destroy_thread()` | `thread_destroy()` (after `thread_terminate` / exit) |
| `get_thread_context()` | `thread_get_context()` |
| `switch_context()` | `thread_switch()` → `context_switch()` |
| `set_thread_state()` | `thread_set_state()` |
| `get_next_ready_thread()` | `thread_next_ready(after)`: a table-order lookup, not a policy |

A scheduler's core becomes:

```c
set current thread's state (READY or BLOCKED);
next = <policy picks a READY thread>;
thread_switch(next);
```

## Diagnostics

- **`thread_dump(t)`** prints PID, TID, state, stack range with peak usage
  and canary status, RSP/RIP (live for the running thread, saved otherwise),
  the address space, and the switch count.
- **`proc_dump_table()`** prints the boxed process manager table: every
  thread with its stack top and RSP, then every process with its state,
  thread count and PML4.
- Every value shown is read from the live kernel structures.

## Testing methodology

`proc_run_tests()` runs at every boot (83 checks). `python build.py test`
passes only if every Phase 3 and Phase 4 check passes and no
`[FAIL]`/panic/exception appears. The same suite passes under BIOS and UEFI.

| Test | What it proves |
|---|---|
| Creation | valid unique PIDs/TIDs, registry lookups, READY start state, own address space with shared kernel half, independent 16 KiB stacks with guard page and canary, exact initial context, argument validation |
| Ping-pong | kmain→A→B→A…: trace must be exactly `A1 B101 A2 B102 … A10 B110`; each thread runs under its own process's CR3; kernel CR3 is restored |
| Registers | asm probe loads distinct values into `rbx rbp r12–r15` in each thread, switches, and checks them: 50 switches, and per-thread RFLAGS.IF (one thread runs with IF=0, the other with IF=1) |
| Stress | 3-thread ring, 60 000 switches with interrupts enabled (timer IRQs land mid-run), checks registers on every hop, strict ring order, local counters |
| Stack isolation | each thread keeps 8 KiB of seeded locals, verified after each of 20 switches, plus 7 recursion frames verified after switching at the deepest level. A shared or clobbered stack fails immediately |
| Lifecycle | exit codes (return and `thread_exit`), stack retained until reap then unmapped, BLOCKED→READY, all invalid transitions and invalid switch targets rejected, running thread and its process protected |
| Memory | a stack is exactly 4 PMM frames and is returned; OOM thread/process creation fails cleanly without leaking; thread table fills at `THREAD_MAX` with a distinct stack per thread; process table fills at `PROC_MAX`; PMM, heap, stack slots and tables all return to baseline |

**Fatal-path demos** (`python build.py test-faults`):

- `--fault-demo ctx`: corrupts a suspended thread's saved RIP. Detected
  before load, followed by a thread dump and a panic.
- `--fault-demo tstack`: a thread recurses into its guard page, raising #DF
  on the IST stack.

## Known limitations

- **(Resolved in Phase 5.)** At first there was no scheduler: `thread_exit` handed off to its creator as
  a placeholder.
- **Single CPU.** Consistency relies on disabling interrupts; there are no
  locks or per-CPU `current` yet.
- **Fixed limits.** 16 KiB stacks, 256 threads, 64 processes.
- **Linear table scans** for validation and lookup, O(table size), which is
  fine at these sizes.
- **Unsaved FPU/SSE state.** Adding SSE to the kernel or to user threads
  will require saving it (FXSAVE/XSAVE).
- **Unused TSS.RSP0.** It will need updating per thread once user mode
  exists.
- **Manual reaping.** Reclamation happens only when someone calls
  `proc_reap()`.
