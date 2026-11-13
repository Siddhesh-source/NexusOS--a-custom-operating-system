# Phase 9: Multi-Core

**Goal:** use every CPU in the machine. Wake the other cores, give each
one its own data and run queue, protect shared data with named
spinlocks, catch lock-order mistakes before they deadlock, keep every
CPU's page-table cache correct, and let lookups run without locks.

**Status:** done. 30 boot checks (`kernel/tests/smp_test.c`) pass on
`--smp 1`, `2` and `4`, together with every earlier suite. The
lock-inversion fault demo (`lockorder`) panics with both call traces. The
host unit tests still pass, and the bench gives a per-CPU switch number.

| Step | Where |
|---|---|
| 1. Wake the other cores (Limine's MP feature) | `kernel/smp/smp.c`, `kernel/smp/ap_entry.asm` |
| 2. Per-CPU data through the GS base; per-CPU GDT and TSS | `kernel/smp/percpu.[ch]`, `kernel/arch/x86_64/gdt.c` |
| 3. `IRQ_GUARD` → named spinlocks (`SPIN_GUARD`) | `kernel/smp/spinlock.[ch]`, every subsystem |
| 4. Lock-order checker | `kernel/smp/spinlock.c` |
| 5. Per-CPU run queues, work stealing, load balancing | `kernel/sched/sched.c` |
| 6. TLB shootdown through IPIs | `kernel/smp/ipi.c`, `kernel/mm/vmm.c` |
| 7. RCU for the process and thread tables | `kernel/smp/rcu.[ch]`, `kernel/proc/proc.c` |

```text
[smp] 4 CPUs online (boot CPU APIC id 0, CPU 1 APIC id 1, CPU 2 APIC id 2, CPU 3 APIC id 3)
[test]   1000000 increments on 4 CPU(s) in 267 ms; ...
[test]   PASS  no lost updates: the counter is exactly 1,000,000
[test]   8 threads finished in 186 ms (one CPU: about 304 ms), speed-up 1.6x; 7 thread(s) stolen; ran on CPUs 0xf
╔══════════════════════════════════════════════════════════════════════════╗
║ BHARAT OS - 4 CPU DASHBOARD  (all CPUs busy)                             ║
╠══════════════════════════════════════════════════════════════════════════╣
║ CPU  RUNNING     SWITCHES   STOLEN    IPIS  QUEUED  IDLE%                ║
║ 0    kmain          10416        0       0       2    23%                ║
║ 1    worker            25        3     521       1    90%                ║
║ 2    worker            22        2     520       1    90%                ║
║ 3    worker            22        2     520       1    90%                ║
╚══════════════════════════════════════════════════════════════════════════╝
```

---

## 1. Waking the other cores

Limine has already sent the start-up messages (INIT/SIPI) and parked
every extra CPU (an "application processor", AP) in 64-bit mode, each one
spinning on a `goto_address`. For each AP, `smp_init`:
1. creates an idle thread for it (`thread_create_idle`); the AP will
   start on that thread's stack, so the thread is RUNNING from the start;
2. puts the AP's `struct percpu` in `extra_argument`;
3. writes the address of `ap_entry` and waits up to one second for the AP
   to say it is online.

`ap_entry.asm` turns on what our page tables need (no-execute, write
protect, global pages), loads our page tables and the idle stack, then
calls `ap_main`. That loads the AP's own GDT and TSS, the shared IDT, its
local APIC and its timer, marks it online, and enters the idle loop.
CPUs start one at a time.

## 2. Per-CPU data

Each CPU has a `struct percpu` (`id`, `current` thread, `idle` thread,
loaded address space, held locks, counters). The CPU's GS base register
points at it, so `this_cpu()` is one memory read with no lookup or lock.
`thread_current()` is a single `mov %gs:24`: whichever CPU runs it, the
answer is the caller, even if the thread moves CPUs right afterwards.

Loading a GDT reloads the GS selector, which clears the GS base, so
`gdt_init_cpu` sets it again. Each CPU has its own GDT and TSS: the TSS
holds the CPU's own emergency stack for double faults, and `ltr` marks a
TSS descriptor busy, so two CPUs could not share one.

Run queues live in `sched.c` (one per CPU, cache-line aligned), not in
`struct percpu`, so the scheduler keeps its data to itself.

## 3. Spinlocks

`IRQ_GUARD()` (interrupts off) only stops *this* CPU from being
preempted. Data shared between CPUs now uses a named spinlock:

```c
static struct spinlock heap_lock = SPINLOCK_INIT(LOCK_CLASS_HEAP);
void *kmalloc_aligned(size_t size, size_t align)
{
    SPIN_GUARD(&heap_lock);       /* interrupts off + lock, until return */
    ...
```

| Lock | Guards |
|---|---|
| run queue (one per CPU) | that CPU's ready and sleep lists, and the state of its threads |
| process table | process and thread tables, each process's thread list and summary state |
| memory regions | memory objects and regions (`vm.c`) |
| kernel stacks, heap, slab (one per cache), page tables, physical pages | their allocators |
| I/O APIC, PCI config | redirection entries, the shared PCI port pair |

The event trace ring has no lock at all: every CPU records an event on
every switch, so a shared lock there made all CPUs wait for each other
(per-CPU switch cost was 2.3× the one-CPU cost with it). A writer claims
a slot with one atomic add and writes the sequence number last; readers
skip a slot whose number is not the one they expect. Likewise, switches
between kernel threads no longer take the process-table lock to update
the kernel process's summary state, which never changes.

The console has its own small lock (`console_lock`): the CPU holding it
may take it again (kprintf calls serial_write), and nothing else is
locked under it, so it stays outside the order checker.

Rules, all checked at run time: `spin_lock` needs interrupts off; a CPU
may not take a lock it already holds or release one it does not hold;
and a thread may not yield or sleep while holding one.

`IRQ_GUARD` stays for data only one CPU touches (its APIC timer, its
per-CPU record) and in the single-CPU self-tests.

## 4. Lock-order checker

Every lock has a class. The first time class B is taken while class A is
held, the checker records "A before B" together with a call trace. If B
is later held while A is taken, two CPUs doing those at once would each
wait for the other forever. The checker reports it **the first time the
opposite order happens, even if no deadlock happened in that run**:

```text
[lockcheck] Lock order problem on CPU 0:
[lockcheck]   taking "test A" while holding "test B"
[lockcheck]   but earlier "test A" was held while taking "test B".
[lockcheck] Earlier order ("test A" then "test B", CPU 0):
Call trace (most recent call first):
  #0 0xffffffff8021bbaf  take_a_then_b+0x2f
  ...
[lockcheck] Now:
Call trace (most recent call first):
  #0 0xffffffff8021005f  spin_lock+0x9f
  #1 0xffffffff8021bc1f  take_b_then_a+0x2f
  ...
PANIC: lock order problem: two locks are taken in opposite orders
```

It also reports two locks of the same class held at once. `spin_trylock`
never waits, so it cannot deadlock and is not checked (work stealing
uses it to take a second run queue). Orders are kept as one 32-bit mask
per class, plus a 64-entry table of first-seen call traces. A boot learns
about 14 real orders, for example run queue → process table and memory
regions → physical pages.

The trace walk itself must not take the page-table lock (the checker may
run while that lock is held), so call traces now use
`vmm_translate_kernel`, a lock-free walk of the kernel half: those page
tables are never freed.

## 5. Per-CPU run queues and work stealing

Each CPU has its own ready list, sleep list, idle thread, alarm and
statistics. A thread belongs to one CPU (`thread->cpu`); its state and
list links change only under that CPU's run-queue lock
(`sched_lock_thread` relocks if the thread moved meanwhile).

**The switch holds the lock across itself.** `schedule()` takes this
CPU's run-queue lock, picks the next thread, and keeps the lock through
`context_switch`. The thread that runs next releases it
(`sched_finish_switch`, also called first thing by a new thread). So no
other CPU can wake, steal or free the outgoing thread until its
registers are saved. This rules out the classic SMP bug of one CPU
running a thread whose context another CPU is still saving.

Moving work between CPUs:
- **Placement:** a new thread that may run anywhere goes to the CPU with
  the fewest READY threads (`sched_pick_cpu`).
- **Stealing:** a CPU with nothing to run takes the oldest unpinned
  READY thread from another CPU's queue. It only *try*-locks the other
  queue, so two CPUs stealing from each other never wait for each other.
- **IPIs:** queuing a thread for another CPU sends it a "reschedule"
  IPI. When a busy CPU gets more work, an idle CPU is told to come and
  steal (CPUs in the idle loop are halted and have no alarm: tickless).

Pinned threads never move: the idle threads, kmain, and threads made
with `thread_create_on(..., cpu)`.

**Killing a thread on another CPU.** `thread_terminate` cannot stop a
thread that is running elsewhere right now. It sets `thread->kill` and
sends that CPU a reschedule IPI; that CPU ends the thread at its next
scheduling decision. `thread_set_state` on such a thread returns BUSY.

**Reaping.** A thread is freed only once it is TERMINATED *and* not
running on any CPU, checked under its run-queue lock (an exiting thread
holds that lock from marking itself TERMINATED until its registers are
saved). Only the boot CPU's idle thread reaps.

**Stealing is off during the Phase 5–8 tests.** Those tests check
single-CPU behaviour on purpose: exact round-robin order and fairness on
one queue. Their threads stay on CPU 0 while the other CPUs are online
and idle. The multi-core tests turn stealing on, and so does normal
running after boot (`sched_set_stealing`).

## 6. TLB shootdown

Each CPU caches page translations in its TLB. When a mapping is removed
or loses a permission, every CPU that may have cached it must drop it,
or it could keep writing to a page that now belongs to someone else.
`vmm_unmap_page` and `vmm_protect_page` invalidate locally (`invlpg`),
then ask the others with an IPI and **wait** until all have done it:
- kernel addresses: every other CPU;
- user addresses: only CPUs that have that address space loaded
  (`address_space.cpus`, kept by `vmm_switch`). The new CPU is added to
  the mask *before* CR3 is loaded, and the changing CPU writes the PTE
  *before* reading the mask, with a full barrier on both sides, so
  neither can miss the other.

Adding a mapping needs no shootdown: CPUs never cache "not present".

**`smp_call` and why it cannot deadlock.** Shootdowns may be sent while
holding locks (the heap holds its lock when it unmaps). One mailbox per
sending CPU holds the function and a bit per target. Every CPU that waits
with interrupts off, whether spinning for a lock or waiting for its own
`smp_call`, keeps answering other CPUs' requests from inside the wait
loop (`cpu_relax`). So a CPU that waits never blocks a CPU that waits for
it.

## 7. RCU

The process and thread tables are read far more often than they change.
Lookups (`process_find`, `thread_find`, the validity checks of every
process API) now take no lock: they read with interrupts off
(`rcu_read_lock`) and load the pointers atomically. Writers still use
the process-table lock, and objects are freed only after
`rcu_synchronize()`:

```text
proc_reap: 1. under the locks, take finished threads out of the tables
           2. rcu_synchronize(): wait until every CPU has passed a point
              where it cannot be reading them
           3. free stacks and structures
```

`rcu_synchronize` sends every other CPU an IPI and waits for the answer.
An interrupt only arrives while interrupts are on, and readers keep them
off, so an answer proves that CPU finished any read that might still
hold an old pointer. These requests are marked `quiescent`: they are
answered only by the interrupt itself, never from a spin loop (a CPU
spinning with interrupts off might be in the middle of a read).

Handle lookup (the plan's other RCU user) arrives with handles in
Phase 10.

## How it is tested

`kernel/tests/smp_test.c`, after the earlier suites:

| Test | What it proves |
|---|---|
| CPUs and per-CPU data | each CPU has its own APIC id and idle thread; a thread pinned to each CPU finds its own record through GS |
| 1,000,000 increments | a plain read-modify-write under a spinlock from every CPU loses nothing |
| allocators from every CPU | kmalloc, slab and pages used at once on all CPUs; no overwritten data; `heap_check`, `pmm_check`, `slab_check` pass; every page returns |
| lock-order checker | it learned real kernel orders; A→B is recorded; B→A is reported (non-fatal test mode) without a deadlock happening |
| TLB shootdown | every other CPU reads a page (caching it), the address is pointed at a new page, and every CPU sees the new page |
| RCU | readers on every other CPU check an object while the writer replaces and frees it 300 times; freed memory is poisoned, so a late reader would notice; none does |
| work stealing | threads queued on one CPU spread over all CPUs; steals counted; a thread running on another CPU is killed |
| dashboard | three frames of per-CPU state under load; queues consistent |

- **Fault demo `lockorder`:** opposite lock orders panic with both
  call traces (`python build.py test-faults`).
- **`python build.py test --smp N`** boots with N CPUs (default 4);
  `--smp 1` must also pass.
- **Unit tests:** the scheduler's host tests run the per-CPU scheduler on
  one fake CPU (fake spinlocks and per-CPU data in `tests/unit/fakes`).
- **Bench:** `context_switch_par` is the switch cost on every CPU at
  once, next to the one-CPU `context_switch`.

**How QEMU runs the CPUs.** Boot tests use `-accel tcg,thread=single`:
one host thread runs every guest CPU in turn, finely interleaved. The
results then do not depend on the host. With `thread=multi` (one host
thread per guest CPU, truly parallel) every multi-core check passed, with
a real 1.6x speed-up on 4 CPUs. But the Phase 5 fairness check failed
now and then: host timer jitter reached the 20 ms slices of threads that
all run on CPU 0 (see bug journal 15; the check now holds slice counts to
2 and CPU time to 20%). The bench uses
`thread=multi`, because parallel numbers are its point. For a manual
parallel stress run:

```bash
qemu-system-x86_64 -accel tcg,thread=multi -smp 4 -m 512M \
  -drive file=build/bharat.img,format=raw,if=ide -nographic
```

## Bugs found and lessons

Each is written up in `docs/BUG_JOURNAL.md`:
- **15:** one CPU never got work (idle CPUs were always woken in the same
  order), and the old fairness check flaked on 4 CPUs because the host
  delivers some alarms late. Those flaky boots first looked like crashes;
  the harness had just stopped two seconds after a `[FAIL]` line. Lesson:
  read the whole log before blaming the hardware.
- **16:** RCU grace periods took a third of a second because the test's
  readers almost never had interrupts on. Lesson: keep interrupts-off
  sections short; they delay every grace period and shootdown.
- **17:** spinlocks made a switch nearly 4x slower until global locks
  left the switch path. Lesson: nothing shared by all CPUs belongs on the
  switch path.

## Known limits

- At most 16 CPUs; CPUs cannot be taken offline.
- Only the boot CPU's idle thread reaps exited threads.
- Device interrupts all go to the boot CPU.
- One shootdown IPI round per page (no batching yet). Freeing a 16 KiB
  thread stack costs four rounds.
- Stealing takes one thread at a time, and only when a CPU is idle; a
  busy CPU never pushes work away.
- No priorities, and no preemption counter: "not preemptible" means
  "interrupts off".
- Every APIC timer is assumed to run at the boot CPU's measured rate, and
  the TSC to be in step on all CPUs (true in QEMU and on modern CPUs).
- A pointer returned by `process_find` / `thread_find` is only safe while
  the object cannot be reaped; reference-counted handles come in
  Phase 10.
