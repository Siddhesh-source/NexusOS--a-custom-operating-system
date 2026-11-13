# Benchmarks

Recorded by `python build.py bench`, which boots a kernel built with
`BHARAT_BENCH` and appends one row per run. Numbers are CPU time stamp
counter (TSC) cycles per operation, best of 5 rounds, under QEMU without
hardware acceleration (TCG). Use them to compare BharatOS with itself over
time; they are not real-hardware numbers. Heap numbers include the heap
poisoning and red-zone checks.

- **context_switch**: one thread switch through `sched_yield`
  (scheduler decision plus register save and restore).
- **kmalloc_kfree_64 / _4096**: one allocation and free of that size.
- **trace_event**: recording one event in the trace ring.
- **pmm_page**: one physical page allocated and freed (batches of 256).
- **kmalloc_kfree_176 / slab_176**: a thread-record-sized object from the
  general heap versus from a slab cache.
- **context_switch_par**: the same switch measured on every CPU at once
  (two threads pinned per CPU), averaged over CPUs. Close to
  context_switch means switching scales per CPU. Bench runs give QEMU one
  host thread per guest CPU (`thread=multi`), so the CPUs really run in
  parallel; on a host with fewer free cores the number grows. Empty in
  rows from before multi-core.
- **cpus**: CPUs the bench kernel ran on (`--smp`).
- **boot_s**: seconds from starting QEMU to the bench results (includes
  all boot self-tests).

**Buddy allocator vs the old bitmap (Phase 8).** In QEMU both page
allocators cost about 1,000 cycles, mostly emulation overhead, and the
difference is within run-to-run noise. Measured natively on the host
instead (same code, same workload), the old next-fit bitmap took 16
cycles per page for 256-page batches against 37 for the buddy allocator,
and 15 against 17 on nearly full, fragmented memory. The buddy allocator
is kept for contiguous blocks and per-page reference counts, not for
speed (decision D22).

**Multi-core (Phase 9).** Rows from Phase 9 on are measured with 4
guest CPUs, each on its own host thread, so every column is higher than
in older 1-CPU rows (the host shares its cores between four busy guest
CPUs and its own work). Like for like, the same kernel on 1 CPU gave
`context_switch` 2,362 and `trace_event` 229 cycles, against 2,259 and
211 before multi-core: spinlocks cost little once the switch path takes
no global lock. Three changes got there: `spin_lock_irqsave` skips a
redundant interrupt-flag check, the trace ring became lock-free, and
switches between kernel threads stopped taking the process-table lock.
With them, `context_switch_par` (all CPUs switching at once) measured
1.3-1.9x the one-pair number over several runs; it was 2.3x before.

**Noise.** Numbers are only comparable from a quiet host: a run on a busy
machine roughly doubles every column (even `trace_event`, which is a few
stores). Such runs are not kept.

| Date | Commit | Host | context_switch | kmalloc_kfree_64 | kmalloc_kfree_4096 | trace_event | pmm_page | kmalloc_kfree_176 | slab_176 | boot_s | context_switch_par | cpus |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-13 | 02957e9 | Windows | 1927 | 1982 | 34692 | 214 | – | – | – | 11.0 |
| 2026-11-13 | 1054b6b | Windows | 4506 | 1865 | 34239 | 213 | 930 | – | – | 12.2 |
| 2026-11-13 | 2fb0913 | Windows | 2537 | 2249 | 33300 | 213 | 959 | – | – | 12.2 |
| 2026-11-13 | 4b6344d | Windows | 2259 | 2136 | 33819 | 211 | 1176 | 2632 | 2284 | 12.3 |
| 2026-11-14 | 7e97735 | Windows | 3798 | 6166 | 68627 | 458 | 3684 | 7128 | 8233 | 16.7 | 7239 | 4 |
