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
