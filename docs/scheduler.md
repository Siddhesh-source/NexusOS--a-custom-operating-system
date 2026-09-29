# NEXUS OS — Phase 5: Preemptive Scheduler

Phase 5 makes NEXUS multitask on its own: kernel threads are switched
automatically by the hardware timer, without calling any switch or yield
function. It reuses the Phase 4 thread model and context switch unchanged.

Out of scope for this phase: priorities, MLFQ, SMP, CPU affinity,
user mode, syscalls, IPC and synchronisation primitives beyond disabling
interrupts.

## Architecture

| File | Role |
|---|---|
| `kernel/sched.h`, `sched.c` | ready queue, sleep list, idle thread, `sched_tick`, `schedule`, stats, tracing |
| `kernel/timer.c` | PIT IRQ0 handler: EOI, then `sched_tick()` |
| `kernel/proc.c` | reports every thread state change to the scheduler (`sched_state_changed`) |
| `kernel/cpu.h` | `IRQ_GUARD()` critical sections |
| `kernel/sched_test.c`, `sched_spin.asm` | tests, live dashboard, final status panel |

The scheduler schedules **threads**. Processes remain containers: a
process's address space is loaded when one of its threads is switched in.

Scheduler state:

| Field | Meaning |
|---|---|
| `current_thread` | `thread_current()` (proc.c); always the one RUNNING thread |
| `ready_queue` | FIFO of exactly the READY threads, excluding idle |
| sleep list | BLOCKED threads waiting for a tick |
| `stats.ticks` | timer ticks since `sched_start` |
| `stats.switches` | context switches (split into preemptive and voluntary) |
| `stats.invocations` | scheduler entries: every tick plus every yield, sleep or exit, whether or not it switched |

## Policy: round-robin

- **Queue.** A FIFO queue with one fixed quantum, `SCHED_QUANTUM_TICKS = 2`
  ticks.
- **Ticks.** The PIT runs at **100 Hz** (10 ms tick), so a thread gets a
  **20 ms** slice.
- **Order.** A thread that is preempted or yields goes to the **tail**; the
  **head** runs next. `A → B → C → A …` is verified by a test.
- **Why 20 ms.** It gives 50 preemptive switches per second at most. The
  measured cost of a switch (about 2 000 cycles in QEMU) makes that about
  0.003 % of the CPU. The quantum stays short enough for smooth interleaving.
  A faster timer would only add overhead.

The queue is **intrusive**: a thread's `sched_prev` and `sched_next` fields
link it in, so the timer interrupt never allocates. Enqueue, dequeue and
removal from the middle are all O(1).

### Keeping the queue correct by construction

All thread state changes already go through one function in proc.c
(`set_state`), which calls `sched_state_changed(t, from, to)`. That hook:

- **Leaving READY** (to RUNNING, BLOCKED or TERMINATED): remove from the
  ready queue.
- **Leaving BLOCKED**: remove from the sleep list.
- **Entering READY:** append to the queue tail (unless the thread is idle).

So the invariant **"on the ready queue ⇔ READY"** holds no matter which
path changed the state:

- creating a thread
- a preemption or voluntary switch
- sleep and wake
- `thread_terminate`
- `process_terminate` killing a sleeping thread

`sched_check()` verifies this, and the tests call it. Every non-idle READY
thread must be queued exactly once, and no TERMINATED thread may be
reachable from a queue.

## Timer integration and the preemption path

```text
Hardware Timer (PIT, 100 Hz)
      │
      ▼
Timer Interrupt (vector 32, interrupt gate → IF=0)
      │   isr32 → isr_common: push vector, error code, 15 GPRs
      ▼            (on the INTERRUPTED THREAD's kernel stack)
Save Current Context ── interrupt frame = the preempted code's full state
      │
      ▼
interrupt_dispatch → timer_irq: ticks++, send EOI
      │
      ▼
Scheduler (sched_tick)
      │   account tick, wake sleepers, quantum expired && queue non-empty?
      ├── Current Thread → READY (appended to queue tail)
      │
      └── Select READY Thread (queue head)
                    │
                    ▼
              Context Switch   thread_switch → context_switch (Phase 4)
                    │          push callee-saved + rflags, swap rsp
                    ▼
             Resume Thread
                    │  a thread that was preempted earlier: returns out of
                    │  context_switch → sched_tick → timer_irq →
                    │  interrupt_dispatch → isr_common pops its GPRs → iretq
                    ▼
            interrupted code continues exactly where it stopped
```

### Interrupt frame + thread context + kernel stack

This part is the most sensitive, so the layering is spelled out here.
Interrupts happen at CPL 0 with no IST for vector 32, so the CPU pushes
its frame onto the current thread's own kernel stack. For a thread
preempted by the timer, that stack looks like this:

```text
 higher addresses
 ┌──────────────────────────────────────────┐
 │ ... the preempted code's own frames ...  │
 ├──────────────────────────────────────────┤ ← interrupt frame
 │ SS, RSP, RFLAGS (IF=1), CS, RIP          │   pushed by the CPU
 │ error code (0), vector (32)              │   pushed by isr32
 │ rax rbx rcx rdx rsi rdi rbp r8 .. r15    │   pushed by isr_common
 ├──────────────────────────────────────────┤
 │ interrupt_dispatch / timer_irq /         │   C frames of the handler
 │ sched_tick / schedule / thread_switch    │
 ├──────────────────────────────────────────┤ ← struct cpu_context
 │ rip (return into thread_switch)          │   pushed by `call`
 │ rflags (IF=0), rbx rbp r12 r13 r14 r15   │   pushed by context_switch
 └──────────────────────────────────────────┘ ← thread->saved_rsp
 lower addresses
```

- **Two formats, two different states, nothing saved twice.**
  - The **interrupt frame** is the state of the *interrupted computation*:
    all 15 GPRs, RIP, and RFLAGS with IF=1. Only it can preserve the
    caller-saved registers (`rax rcx rdx rsi rdi r8–r11`), because the
    interrupted code never made a call.
  - The **`cpu_context`** is the state of the *interrupt handler itself*,
    at the point where it called `context_switch`. Its callee-saved
    registers hold the handler's own values, not the interrupted
    thread's, which already sit in the interrupt frame.
- **Where RSP points.** On entry to the handler, RSP points at the pushed
  GPRs. When switched away, `thread->saved_rsp` points at the
  `cpu_context` further down.
- **How RIP is restored.** `context_switch`'s `ret` resumes the handler,
  which unwinds normally. `iretq` then restores the interrupted RIP, CS,
  RFLAGS, RSP and SS.
- **How RFLAGS / IF are preserved.**
  - Inside the handler IF=0, so `context_switch` saves IF=0. On resume the
    handler still runs with interrupts off, and `iretq` restores the
    interrupted IF=1.
  - A thread that switched away voluntarily (yield, sleep) had IF=1
    stashed by `thread_switch`'s `irq_save`, and gets it back via
    `irq_restore`.
- **Why one format works for every path.** A thread resumed by the
  scheduler doesn't care how the *next* thread was suspended. Each thread
  unwinds its own stack: one through `iretq`, another through a plain
  `ret` from `sched_yield`, and a brand-new one through `thread_trampoline`.
  They never mix, because each lives on its own stack.

### EOI ordering

`timer_irq` sends EOI **before** calling `sched_tick`. After a switch, this
handler invocation might not finish until the preempted thread is picked
again, and the PIC must keep delivering IRQ0 meanwhile.

This does not cause nesting. IF stays 0 until the next thread re-enables it
(with `iretq`, `irq_restore`, or `sti` in `thread_bootstrap`), and a new
interrupt then lands on *that* thread's stack.

## Preemption safety (single CPU)

- **Critical sections.** Everything shared runs with interrupts disabled,
  via `IRQ_GUARD()` in `cpu.h`. The guard restores the previous state on
  every exit path (it uses `__attribute__((cleanup))`) and nests.
- **Scheduler.** It runs from the IRQ (IF=0 by the gate) or under a guard.
  Nested timer interrupts are impossible.
- **Allocators now guarded:** PMM, VMM, heap and kstack public entry points.
  Otherwise a thread preempted mid-`kmalloc` could let another thread
  corrupt the heap; the demo's Memory Worker exercises exactly this.
- **Console.** Each `kprintf` call, serial string and box line is written in
  one uninterrupted piece, so lines from different threads never interleave.
- **Races fixed in `thread_create` / `process_create`.** The table-capacity
  check and the target process's validity are re-checked inside the
  critical section. While one thread was allocating, another could fill the
  table or even terminate and reap the target process.
- **SMP path.** `IRQ_GUARD` is the single seam: it becomes "spinlock +
  disable interrupts" when SMP arrives.

## Current thread and bootstrap

```text
Kernel bootstrap (kernel_main on the boot stack)
      ↓
proc_init        boot flow adopted as TID 0 of PID 0 (Phase 4), current = TID 0
      ↓
sched_init       create the idle thread and take it off the ready queue
      ↓
sched_start      enable preemption; TID 0 is now an ordinary scheduled thread
```

- **Boot stack.** The boot flow is never re-created or given a new stack.
  It keeps running on the boot stack, which is never freed.
- **Current thread.** `thread_current()` is updated inside `thread_switch`,
  together with the state changes and before the stack swap, all with
  interrupts off.
- **Invariant.** `sched_check()` asserts that the current thread is always
  RUNNING and that it is the only RUNNING thread.

## Thread states under the scheduler

```text
            create                          sched_tick (quantum) / yield
   NEW ───────────► READY ◄──────────────────────────────┐
                      │  head of queue                   │
                      ▼                                  │
                   RUNNING ──────────────────────────────┘
                      │  sched_sleep              │  return / thread_exit
                      ▼                           ▼
                   BLOCKED ──(wake tick)──► READY   TERMINATED ──► reaped by idle
```

## Termination

When the running thread returns from its entry or calls `thread_exit`:

1. It becomes TERMINATED, which takes it off every list.
2. `sched_reschedule` switches to the queue head, or to idle if nothing is
   READY.
3. The terminated thread is on no list, so it can never be picked.
   `schedule()` also panics if it ever selects a non-READY thread.
4. Its stack and structure stay intact until the idle thread's
   `proc_reap()` destroys them. By then the thread's last `context_switch`
   has long completed, so nothing is using its stack.

A READY or sleeping thread killed from outside (by `thread_terminate` or
`process_terminate`) leaves its queue in the same `set_state` call.

## Idle thread

- **Creation.** `sched_init` creates it in the kernel process. It is
  **never** on the ready queue.
- **When it runs.** Only when the queue is empty and the current thread
  has blocked or exited.
- **Body.** `proc_reap(); sti; hlt`: it reclaims exited threads, then halts
  until the next interrupt.
- **Giving the CPU back.** On any tick where the queue is non-empty (for
  example after a sleeper wakes), idle is preempted immediately, without
  waiting for a quantum.

After the tests, kmain sleeps in a one-dot-per-second heartbeat loop, so
idle, not kmain, is what halts the CPU.

## Sleeping

`sched_sleep(ticks)` marks the current thread BLOCKED, puts it on the sleep
list with a wake tick, and switches away. On each tick, `sched_tick` moves
due sleepers back to READY. This is the one blocking primitive, and the
tests and demo need it: kmain must step aside so workers, and later idle,
can run.

## Statistics and diagnostics

- **`sched_get_stats()` / `sched_print_stats()`:**
  - ticks, invocations
  - switches (preemptive and voluntary)
  - runnable and sleeping counts
  - idle ticks and switches
  - the current TID
- **Per thread:** `switch_count`, `cpu_ticks` (ticks that hit while it was
  RUNNING), `preempted_count`.
- **Live dashboard:** the demo prints a boxed table of every thread.
  - Values are snapshotted in one critical section, so the table is
    consistent.
  - It refreshes every 50 ticks.
- **Tracing:** `sched_set_trace(n)` logs the next *n* switches as
  `[SCHED] tick=… current=… next=… reason=… switch_count=…`, then turns
  itself off. The demo traces 6 switches; tracing every tick would distort
  timing.

## Testing

`sched_run_tests()` runs 26 checks at boot after the Phase 3 and Phase 4
suites, and those suites still pass unchanged. It passes under BIOS and
UEFI, and in repeated boots.

| Test | How it proves it |
|---|---|
| **Preemption** | Two threads spin in pure loops that never yield, sleep or call the scheduler. Each records how often the *other's* counter advanced while it was still inside its own loop, which can only happen if it was interrupted. Also checked: the preemption counters. |
| **All-register preservation** | Three threads run `preempt_reg_spin` (asm): all 15 GPRs hold distinct values, checked in a tight loop while being preempted. The caller-saved registers survive only through the interrupt frame, so this checks the whole layered design. |
| **Stack isolation** | Three threads continuously verify and rewrite 2 KiB of stack data (no yields) while being preempted. |
| **Round-robin order** | Each thread logs itself when it first gets the CPU. The log must be a strict rotation (A B C A B C …). |
| **Fairness** | Three CPU-bound threads for 150 ticks. It reports time slices, CPU ticks and work per thread. Fails if any thread starves, or if slices differ by more than 2 or CPU ticks by more than 2 quanta. Typical result: 26 / 26 / 25 slices. |
| **Termination** | Exit by return and by `thread_exit`. A READY thread terminated before it runs is never scheduled. Killing a process removes its sleeping thread cleanly. Idle reclaims every stack. |
| **Idle and sleep** | Idle accumulates ticks while everything sleeps. `sleep(20)` lasts 20 ticks, within one quantum. |
| **Overhead** | TSC cycles per tick. Cost of a switch (two threads yielding 5 000 times each). Projected cost at 50 switches per second. |
| **Live demo** | CPU worker (primes), Memory worker (kmalloc, verify, kfree, heap_check), Counter. Four dashboard refreshes, then the final status panel. All three must progress, be preempted, and see zero memory errors. |

The fatal demos also still pass with preemption active (`build.py
test-faults`, 8 demos).

## Known limitations

- **Policy.** Single-CPU round-robin only: no priorities, no SMP, no
  per-CPU run queues.
- **Mutual exclusion is interrupt disabling.** It is correct on one CPU and
  keeps critical sections short, but it is not a lock.
- **One blocking primitive.** Tick-based sleep is the only one; there are no
  wait queues, mutexes or semaphores yet.
- **Exits.** A thread's last switch after exiting is voluntary. Exiting in
  the middle of a quantum hands the rest of the slice to the next thread.
- **Accounting.** `cpu_ticks` is sampled at tick granularity, and a thread
  that always blocks before the tick shows 0 (kmain does).
- **Timer.** The PIT stays at 100 Hz; the LAPIC timer and one-shot /
  tickless mode are future work.
- **FPU/SSE.** State is still not saved (the kernel is built without SSE).
  User threads will need FXSAVE/XSAVE.
