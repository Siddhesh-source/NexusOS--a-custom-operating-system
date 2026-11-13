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
- **boot_s**: seconds from starting QEMU to the bench results (includes
  all boot self-tests).

| Date | Commit | Host | context_switch | kmalloc_kfree_64 | kmalloc_kfree_4096 | trace_event | boot_s |
|---|---|---|---|---|---|---|---|
| 2026-11-13 | 02957e9 | Windows | 1927 | 1982 | 34692 | 214 | 11.0 |
