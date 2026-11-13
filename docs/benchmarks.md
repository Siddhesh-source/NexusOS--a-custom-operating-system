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

**Noise.** Numbers are only comparable from a quiet host: a run on a busy
machine roughly doubles every column (even `trace_event`, which is a few
stores). Such runs are not kept.

| Date | Commit | Host | context_switch | kmalloc_kfree_64 | kmalloc_kfree_4096 | trace_event | pmm_page | kmalloc_kfree_176 | slab_176 | boot_s |
|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-13 | 02957e9 | Windows | 1927 | 1982 | 34692 | 214 | – | – | – | 11.0 |
| 2026-11-13 | 1054b6b | Windows | 4506 | 1865 | 34239 | 213 | 930 | – | – | 12.2 |
| 2026-11-13 | 2fb0913 | Windows | 2537 | 2249 | 33300 | 213 | 959 | – | – | 12.2 |
| 2026-11-13 | 4b6344d | Windows | 2259 | 2136 | 33819 | 211 | 1176 | 2632 | 2284 | 12.3 |
