# Bug Journal

Hard bugs found while building BharatOS, one entry each:
**symptom → how it was found → root cause → fix → test that now guards it.**
New entries go at the end. Entries 1–7 come from Phases 2–5 (see
`phases/phase-2-interrupts.md` and `phases/phase-5-scheduler.md`).

---

### 1. TSS layout: reserved field 4 bytes too small (Phase 2)
- **Symptom:** any exception using the emergency stack (IST1), such as a
  double fault, triple-faulted (the machine reset) instead of printing a
  report.
- **How found:** not recorded.
- **Root cause:** the reserved field at offset 0x1C was 4 bytes instead of
  8, so the CPU read IST1 from the wrong offset and switched to a garbage
  stack.
- **Fix:** corrected the TSS layout.
- **Guarding test:** fault demos `stack`, `df` and `tstack` (all delivered
  on IST1).

### 2. Stale segment registers after loading the GDT (Phase 2)
- **Symptom:** the first `iretq` would raise `#GP`.
- **How found:** not recorded.
- **Root cause:** after `lgdt`, CS and SS still held Limine's selectors
  (0x28/0x30), which lie outside the new GDT.
- **Fix:** `gdt_flush` far-returns into 0x08 and reloads the data segments
  with 0x10.
- **Guarding test:** every boot; all interrupt tests return through `iretq`.

### 3. Interrupt entry stubs used `call` (Phase 2)
- **Symptom:** the error code was read from the CS slot, and `iretq`
  returned to the wrong place.
- **How found:** not recorded.
- **Root cause:** `call` pushed an extra return address on top of the
  CPU's interrupt frame.
- **Fix:** the stubs reach the common code without `call`; the frame now
  matches `struct interrupt_frame`.
- **Guarding test:** the `#GP` probe fixup test and every fault demo, which
  check the reported error code and instruction pointer.

### 4. Invalid IDT gate type (Phase 2)
- **Symptom:** the `#GP`, `#PF` and IRQ0 gates could not be delivered.
- **How found:** not recorded.
- **Root cause:** those gates used type 0x6, which is invalid in long mode.
- **Fix:** all gates use type 0xE, the 64-bit interrupt gate.
- **Guarding test:** fault demos `pf`, `null`; the timer-driven scheduler
  tests.

### 5. Timer would have arrived as a double fault (Phase 2)
- **Symptom:** IRQ0 would have hit vector 8 (`#DF`).
- **How found:** not recorded.
- **Root cause:** the PIC was never remapped, and `timer_init` overwrote
  IDT[32] with a plain C function.
- **Fix:** the PIC is remapped to vectors 32–47 with only IRQ0 unmasked;
  the timer goes through a proper stub; the PIT mode bits are encoded
  correctly.
- **Guarding test:** every boot (scheduler tests depend on the timer);
  fault demo `df` checks that a real `#DF` is still reported.

### 6. Races in `thread_create` / `process_create` (Phase 5)
- **Symptom:** with preemption, the thread table could overflow, or a
  thread could be added to a process that was already terminated and
  reaped.
- **How found:** not recorded.
- **Root cause:** the table-capacity check and the process-validity check
  happened before the insert, outside the critical section, so another
  thread could run in between.
- **Fix:** both are checked again inside the critical section.
- **Guarding test:** Phase 4 and 5 lifecycle and stress tests.

### 7. Allocators not safe under preemption (Phase 5)
- **Symptom:** a thread preempted in the middle of `kmalloc` could let
  another thread corrupt the heap.
- **How found:** not recorded; the demo's Memory Worker exercises exactly
  this case.
- **Root cause:** PMM, VMM, heap and kernel-stack code changed shared
  lists with interrupts on.
- **Fix:** `IRQ_GUARD()` at every public entry point of those allocators.
- **Guarding test:** the live demo's "Memory worker errors: 0". Since
  Phase 6, heap poisoning also catches such corruption when memory is
  reused.

### 8. Call trace missed a frame: the compiler turned a call into a jump (Phase 6)
- **Symptom:** the self-test expecting the trace
  `bt_level2 ← bt_level1 ← test_backtrace` failed. The live trace skipped
  `bt_level1` and `test_backtrace`.
- **How found:** disassembled with `llvm-objdump --disassemble-symbols=bt_level1`.
  `bt_level1` was `push rbp; pop rbp; jmp bt_level2`.
- **Root cause:** two separate compiler optimisations.
  (a) The caller ignored the return value, so clang dropped the `+ 1` and
  made the call the last instruction, then replaced `call` with `jmp` (a
  *tail call*), so no frame was left behind.
  (b) `test_backtrace`, a static function called once, was inlined into
  its caller.
- **Fix:** `__attribute__((noinline, disable_tail_calls))` on the test
  chain and `noinline` on `test_backtrace`. This is a real limit of
  frame-pointer traces, now written up in the phase doc.
- **Guarding test:** "frames are bt_level2 <- bt_level1 <- test_backtrace".

### 9. Stack-smash demo did not reach the canary (Phase 6)
- **Symptom:** writing 24 bytes into a 16-byte stack buffer was not
  detected.
- **How found:** disassembled the victim function. The buffer was at
  `rbp-0x20` and the canary at `rbp-0x8`, with 8 bytes of padding between
  them, so 24 bytes stopped short of the canary.
- **Root cause:** the compiler is free to pad between locals and the
  canary; the demo assumed they were adjacent.
- **Fix:** the demo now writes from the buffer up to (not including) the
  saved frame pointer. That always covers the canary, whatever the
  padding, and leaves the saved RBP and return address intact so the call
  trace still works.
- **Guarding test:** fault demo `smash`.

### 10. Going tickless doubled the cost of a thread switch (Phase 7)
- **Symptom:** the context-switch benchmark went from 1,927 to 4,506
  cycles after the scheduler moved from a periodic tick to one-shot alarms.
- **How found:** `python build.py bench`, compared with the previous row
  in `docs/benchmarks.md`.
- **Root cause:** almost every switch moved the slice end, so the
  scheduler rewrote the APIC timer registers each time. Device-register
  writes are slow (much slower under emulation).
- **Fix:** lazy re-arming: reprogram only when a deadline moves earlier;
  an early alarm just fires and the scheduler looks again (decision D20).
  Back to about 2,300 cycles.
- **Guarding test:** the benchmark table; unit test "a sleep with no
  stale alarm takes exactly one alarm".

### 11. QEMU's PIT gives two interrupts per period through the I/O APIC (Phase 7)
- **Symptom:** routing the 100 Hz PIT through the I/O APIC delivered 20–21
  interrupts in 100 ms instead of about 10.
- **How found:** the I/O APIC routing self-test; the window was confirmed
  to be 100 ms by both the TSC and the HPET, ruling out a clock error.
- **Root cause:** in square-wave mode (3) QEMU raises and lowers the line
  every half period, and its I/O APIC turns that into two deliveries per
  period. In rate-generator mode (2) there is one short pulse per period.
- **Fix:** the test programs the PIT in mode 2 while it is routed (and
  restores mode 3 after).
- **Guarding test:** "about 10 interrupts in 100 ms at 100 Hz".

### 12. Waking a sleeper armed an extra "run now" alarm (Phase 7)
- **Symptom:** a 5-tick sleep took 3 alarm interrupts in the host unit
  test, where 1 (plus at most one stale one) was expected.
- **How found:** `python build.py unit` (fake clock), then printing each
  fired deadline: the same deadline fired twice.
- **Root cause:** waking the sleeper inside the alarm handler reported
  "a thread became READY while idle runs", which armed an alarm for
  *now*; the handler then switched to that thread anyway, and with lazy
  re-arming the pointless alarm stayed armed and fired again.
- **Fix:** no arming while the alarm handler wakes sleepers; the
  handler's own decision sets the alarm.
- **Guarding test:** the same unit test (at most 2 alarms with a stale
  one; exactly 1 without).

### 13. A fault demo raced with the faster scheduler (Phase 8)
- **Symptom:** `test-faults` failed for `tstack`: the stack overflow was
  caught correctly, but the demo's own "[demo] Thread TID ..." line was
  missing. It passed when run alone.
- **How found:** reading the failed boot's output: the overflow report
  came where the message should have been.
- **Root cause:** creating the new thread makes it READY; the tickless
  scheduler preempts the long-running creator within microseconds,
  before it printed. The old 10 ms tick had hidden the race.
- **Fix:** create the thread and print with interrupts off.
- **Guarding test:** fault demo `tstack`.

### 14. The slab cache was slower than the heap it replaces (Phase 8)
- **Symptom:** the first benchmark gave 4,625 cycles for a slab object
  against 2,606 for the same size from the heap.
- **How found:** `python build.py bench` (the new `slab_176` column).
- **Root cause:** the use-after-free check compared a freed slot one byte
  at a time; the heap compares 8 bytes at a time.
- **Fix:** compare 8-byte words. Now 2,284 cycles, about 14% faster than
  the heap.
- **Guarding test:** the benchmark table; `slab_check` and the slab
  use-after-free tests still catch single-byte changes.

### 15. One CPU never got work, and a fairness check flaked (Phase 9)
- **Symptom:** with 4 CPUs, the work-stealing test said the threads ran
  on CPUs 0x7: CPU 3 never stole anything, every run. Separately, the old
  fairness check failed about one boot in ten with `--smp 4`, never with
  `--smp 1`. Those boots looked like crashes at first (QEMU's output
  stopped mid-line), but the test harness had simply stopped watching
  two seconds after the `[FAIL]` line.
- **How found:** temporary per-CPU counters (steal attempts, IPIs
  received, which branch the reschedule handler took). CPU 3 took plenty
  of IPIs but its scheduler never tried to steal.
- **Root cause:** two separate things. (1) A busy CPU woke "the first
  idle CPU after me" to steal. CPUs 1 and 2, idle again after each short
  thread, always came first, so CPU 3, tickless and halted, was never
  asked. (2) With several guest CPUs on one QEMU host thread, the
  alarm that ends a time slice sometimes arrives several ms late, so
  single slices vary in length; the check allowed 4 ticks of CPU-time
  spread, assuming every slice was exactly 2 ticks. The slice counts
  themselves never differed by more than 1.
- **Fix:** (1) wake one idle CPU per waiting thread, starting the search
  at a rotating position; an idle CPU whose try-lock steal fails retries
  after 1 ms. (2) The kernel's own decisions, slice counts, must still be
  within 2; CPU time must be within 20% (D21: host lateness is not the
  kernel's share). A thread getting far more time still fails.
- **Guarding test:** "the work spread over every CPU" in `smp_test.c`;
  the fairness check in `sched_test.c` on `--smp 4`.

### 16. RCU grace periods took a third of a second each (Phase 9)
- **Symptom:** the RCU self-test (300 updates, each waiting for a grace
  period) took 92 seconds; the whole boot took 110 s instead of 15.
- **How found:** timestamping each test section of a boot from the host.
- **Root cause:** the test's reader threads spent almost all their time
  inside a read section (interrupts off) and turned interrupts on for
  only a few instructions between reads. A grace period needs every
  reader CPU to take an IPI, and QEMU only checks for interrupts between
  blocks of instructions, so that tiny window was usually missed.
- **Fix:** the readers do a little work outside the read section, as
  real readers do. Lesson: a CPU that keeps interrupts off nearly all the
  time delays every grace period and every TLB shootdown; keep read
  sections and spinlock sections short.
- **Guarding test:** the RCU test in `smp_test.c` (and the boot-time
  budget, which caught the slowdown).

### 17. Spinlocks made a thread switch nearly 4x slower (Phase 9)
- **Symptom:** after IRQ_GUARD became spinlocks, the bench showed
  `context_switch` at 8,590 cycles (was 2,259) and `trace_event` at
  1,708 (was 211), even on one CPU. With 4 CPUs switching at once, each
  switch cost 2.3x the one-CPU number.
- **How found:** `python build.py bench`, then the same bench kernel on
  1 and 4 CPUs to separate kernel cost from QEMU's mode.
- **Root cause:** every lock re-read the interrupt flag (a slow `pushfq`
  under QEMU) right after turning interrupts off, and called a function
  to ask "is this CPU panicking?". Worse, every switch took two locks
  shared by all CPUs: the trace ring's (each switch records an event) and
  the process table's (to update the kernel process's summary state,
  which never changes), so CPUs queued behind each other.
- **Fix:** skip the redundant flag check in `spin_lock_irqsave`; make the
  panic test one inline load; a lock-free trace ring (atomic slot claim,
  sequence number written last); no summary update for the kernel
  process. Now 2,362 and 229 cycles on one CPU; 1.3-1.9x when all CPUs
  switch at once. Lesson: nothing shared by all CPUs belongs on the
  switch path.
- **Guarding test:** the `context_switch`, `trace_event` and
  `context_switch_par` columns of `docs/benchmarks.md`.

### 18. Reading the boot bundle crashed the kernel (Phase 10)
- **Symptom:** the first boot with the file bundle stopped with a page
  fault in `fs_load_tar`, reading the bundle's first byte. The address
  was a correct direct-map address for the module Limine had loaded.
- **How found:** the crash report: `[#PF] faulting address :
  0xffff80001fe29000 (HHDM (direct physical map))`, call trace
  `fs_load_tar <- kernel_main`.
- **Root cause:** Limine puts modules in "kernel and modules" memory.
  Our own page tables (Phase 3) deliberately leave that type out of the
  direct map, so the kernel image is reachable only through its
  permission-checked mapping. Limine's address was valid in Limine's page
  tables, not in ours.
- **Fix:** map just the bundle's pages, read-only, with `vmm_map_phys`
  before reading it (the way ACPI tables are mapped).
- **Guarding test:** `fs_test.c` "the boot bundle was loaded" and
  "/etc/motd ... came from it", on every boot.

### 19. Enter typed by the test harness never arrived (Phase 10)
- **Symptom:** the typed-session boot test sent `tester\r` at the login
  prompt. The name was echoed, then nothing: the shell never saw Enter.
  Typing by hand in a terminal worked.
- **How found:** the session log showed `BharatOS login: tester` and no
  newline; sending `\n` instead worked at once.
- **Root cause:** QEMU on Windows reads a piped stdin with its own reader
  that skips `\r` bytes (it expects `\r\n` from some terminals), so a
  lone `\r` from a pipe is lost. A real terminal sends `\r` and reaches
  the guest through a different path.
- **Fix:** the harness sends `\n`. The kernel's serial decoder accepts
  `\r`, `\n` and `\r\n` as one Enter, so both work.
- **Guarding test:** the typed session in `python build.py test`.

### 20. "The work spread over every CPU" failed once in CI (Phase 9 test)
- **Symptom:** in one CI run the work-stealing test reported the busy
  threads ran on CPUs 0x7 of 4, so that check failed. The same code
  passed locally and on the next CI run. The shell's `checks` test failed
  in the same run only because it reports any failed boot suite.
- **How found:** the CI log: "8 threads finished in 245 ms (one CPU:
  about 240 ms) ... ran on CPUs 0x7".
- **Root cause:** a race in the test, not the scheduler. CI's QEMU runs
  every guest CPU on one host thread, taking turns, so here there was no
  speed-up at all: the eight threads finished before CPU 3 got a turn to
  steal one.
- **Fix:** while the spread is being tested, threads that finish their
  work keep spinning (up to 3 s) until every CPU has run some of it, so
  an idle CPU always finds something to steal. `checks` live waits are
  longer, and its test prints the command's output if it fails.
- **Guarding test:** the same check, now on 1, 2 and 4 CPUs every boot.
