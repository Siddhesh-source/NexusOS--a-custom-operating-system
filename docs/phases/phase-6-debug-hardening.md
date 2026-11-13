# Phase 6: Debug and Hardening Foundation

**Goal:** make every later phase easier to debug and harder to break.
Nothing here is a user-facing feature. The point is that when something
goes wrong in Phases 7–20, the kernel says *where* and *why*, and many
mistakes are caught the moment they happen instead of much later.

**Status:** done. `python build.py test` (now 252 checks),
`test-faults` (12 crash demos), `unit` (about 2.8 million host-side
checks) and `bench` all pass.

| Step | What it gives you | Where |
|---|---|---|
| 1. CI | every push to a PR is built and boot-tested on Ubuntu | `.github/workflows/ci.yml` |
| 2. Call traces | a crash prints the chain of function calls by name | `kernel/trace/backtrace.c`, `scripts/ksyms.py` |
| 3. Debug mode | QEMU waits for GDB/LLDB before running the kernel | `build.py debug`, `scripts/bharat.gdb` |
| 4. Stack protector | writing past a local array is caught before the function returns | `kernel/core/stack_protector.c`, `entry.asm` |
| 5. Heap poisoning | use after free and heap overflows are caught | `kernel/mm/heap.c` |
| 6. Event trace | the last 256 kernel events, timestamped, printed on every crash | `kernel/trace/trace.c` |
| 7. Host unit tests | heap and scheduler code tested as normal programs, millions of cases | `tests/unit/`, `build.py unit` |
| 8. Benchmarks | cost of a thread switch, an allocation and an event, kept over time | `kernel/tests/bench.c`, `docs/benchmarks.md` |
| 9. Budgets | kernel size and boot time checked on every test run | `scripts/budgets.py` |

---

## 1. CI

One GitHub Actions job on Ubuntu 24.04. The apt packages (clang, lld,
nasm, QEMU) and the Limine download are cached, so a run costs about a
minute plus the boot. It runs `build.py unit` and then `build.py test`,
one BIOS boot that also checks the budgets. It has a 15-minute cap.

Fault demos, benchmarks and UEFI boots are left out on purpose: CI must
stay quick (see CLAUDE.md). Run those locally when a change touches
interrupts, memory, stacks or performance.

## 2. Call traces with function names

A crash used to print only raw addresses. It now prints:

```text
PANIC: forced panic to show a call trace (fault demo)

Call trace (most recent call first):
  #0 0xffffffff802080e0  demo_panic_inner+0x10
  #1 0xffffffff802080c7  debug_demo_panic+0x17
  #2 0xffffffff80200b72  kernel_main+0x392
  #3 0xffffffff8020018a  _start+0x4a

Recent events (16 of 10955, oldest first):
  [10940] +0 cycles  test a=0x4e2 b=0x0
  ...
```

(`run_fault_demo` sits between `debug_demo_panic` and `kernel_main` in
the source, but its last statement is the call, which the compiler turned
into a jump, so it has no frame of its own. See "Limits" below.)

**How the walk works.** The kernel is built with frame pointers
(`-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer`). Every function
starts with `push rbp; mov rbp, rsp`, so `[rbp]` holds the caller's frame
pointer and `[rbp+8]` holds the return address. Following that chain gives
every call that led here. `entry.asm` sets RBP to 0 before `kernel_main`,
which marks the end of the chain.

A crash must never cause a second crash, so each step is checked before
anything is read:
- the frame address must be 8-byte aligned and mapped (checked with
  `vmm_translate`; before paging is set up, only the boot stack is
  trusted);
- the return address must be inside kernel code;
- the next frame must be higher in memory (stacks grow down);
- the walk stops after 32 frames.

For fatal exceptions, the walk starts at the exact faulting instruction
(`frame->rip`) and then follows the interrupted code's RBP. The interrupt
entry stubs don't change RBP.

**Where the names come from.** `build.py` links the kernel twice:

1. Link with an empty symbol table.
2. `scripts/ksyms.py` reads the ELF's symbol table (plain Python, no
   tools) and writes `build/ksyms_table.c`: a sorted list of
   `{address, size, name}` for every function.
3. Link again with that table.

The table is read-only data placed after all code, so it cannot move any
function. The build still checks this: it re-reads the symbols after the
second link and stops if any address changed. Lookup is a binary search.
A return address points just *after* its call, so the lookup uses
`address - 1`. Without that, a call at the very end of a function (common
before `panic`, which never returns) would be blamed on the next function.

**Limits.** Assembly routines without a frame (interrupt stubs,
`context_switch`) don't appear. Inlined functions show up as their caller,
and tail calls (a final call turned into a jump) drop a frame (see the bug
journal).

## 3. `python build.py debug`

```bash
python build.py debug          # terminal 1: QEMU paused, serial output here
gdb -x scripts/bharat.gdb      # terminal 2: connects and stops at kernel_main
```

QEMU starts with `-s -S`: a GDB server on TCP port 1234, with the CPU
paused. The script loads `build/bharat.elf` for symbols and connects. It
uses a *hardware* breakpoint at `kernel_main`, because the kernel's pages
are not loaded yet when QEMU starts. The comments in the script show how
to stop at `context_switch` and step through a thread switch. LLDB works
too; the command prints the exact line.

## 4. Stack protector

Built with `-fstack-protector-strong -mstack-protector-guard=global`. Every
function with a local array (or a local whose address is taken) stores a
secret value, the *canary*, between its locals and the saved return
address. It checks the canary just before returning, and if it changed, it
calls `__stack_chk_fail`, which panics with a call trace.

The canary is `__stack_chk_guard`. `entry.asm` mixes the time stamp
counter into it before any C code runs, so it differs on every boot. Doing
it this early matters: if a function had already saved the old value,
changing it later would make that function's check fail. The lowest byte
is forced to 0, so string copies, which stop at a 0 byte, cannot write the
canary back.

## 5. Heap poisoning and red zones (a light KASAN)

Two fill patterns turn silent memory corruption into an immediate report:

| Memory | Filled with | Checked when | Caught |
|---|---|---|---|
| free heap memory (including fresh pages and merged headers) | `0xDE` | the memory is handed out again | **use after free** (a write into freed memory) |
| 16+ bytes after each allocation (the *red zone*) | `0xFB` | `kfree` | **heap overflow** (a write past the end) |

The block header's spare 32-bit field now records the requested size, so
the red zone starts exactly where the caller's bytes end. A 20-byte
allocation has its red zone at byte 20, not at the 16-byte-rounded size.

Real corruption is a kernel bug, so it panics with the address and a call
trace. `heap_check()` checks both patterns *without* panicking. The boot
self-tests use it to prove detection works: they make a deliberate bad
write, check that it is reported, and repair it.

New allocations now contain `0xDE` rather than old data, which also makes
"used before being set" bugs easy to spot. `kzalloc` still returns zeroes.

**Cost:** every free fills the block, and every allocation scans the bytes
it hands out. A 64-byte alloc+free costs about 2,000 cycles and a 4 KiB one
about 35,000 (`docs/benchmarks.md`). That is fine for now. The slab
allocator in Phase 8 will revisit it (decision D19).

**Limits:** stray *reads* are not caught, and neither is a write that
jumps over the red zone into the next block.

## 6. Event trace

One ring of 256 fixed-size records (`kernel/trace/trace.c`), each holding
`{tsc, seq, type, a, b}`. Recording an event is a few stores with
interrupts briefly off, about 200 cycles, with no allocation and no
printing. That makes it safe in the timer interrupt and cheap enough to
leave on all the time.

Events recorded today: thread switches (preempted or voluntary), thread
create and exit, page faults, and heap growth. Every panic and fatal
exception prints the last 16, so a crash report shows what the kernel was
doing just before.

This replaces the scheduler's old `sched_set_trace(n)`, which printed
lines directly. The live demo now prints the last 6 events from the ring:

```text
Recent events (6 of 10594, oldest first):
  [10589] +0 cycles  sched.preempt a=0x126 b=0x127
  [10590] +60855604 cycles  sched.preempt a=0x127 b=0x128
```

`a` and `b` depend on the event type: for switches they are the old and
new thread IDs (in hex).

## 7. Host-side unit tests: `python build.py unit`

Real kernel files (`heap.c`, `sched.c`, `trace.c`, `kprintf.c`,
`string.c`) are compiled into small programs for the build machine and
run there. That is thousands of times faster than booting, and failures
are reproducible from a seed.

**No C library.** The programs are freestanding, like the kernel. A tiny
host layer gives them output and exit:
- **Linux:** two raw system calls (`write`, `exit_group`) and a `_start`
  in assembly.
- **Windows:** three `kernel32.dll` calls. The import library is made from
  a 3-line `kernel32.def` by `llvm-dlltool`, which ships with LLVM.

So the same clang + lld that build the kernel are all that is needed,
with no MSVC, MinGW or glibc headers. This matters on Windows, where the
usual hosted toolchains are often missing.

**Fakes** (`tests/unit/fakes.c`, `fakes/cpu.h`). The page allocator and
page tables are faked (the fake allocator can be told to fail the Nth
call), the serial port writes to standard output, and the interrupt guards
do nothing. `panic()` can be *expected*: the test jumps back with
`__builtin_setjmp`/`__builtin_longjmp`, so it can check that a corruption
really panics. `fakes/` is passed with `-iquote`, so it wins over the
kernel's `cpu.h` for quoted includes. The heap runs over an 8 MiB array
(`KERNEL_HEAP_BASE`/`MAX` can now be overridden; the kernel never does).

**What is tested.**
- **Heap:** 5 seeds × 20,000 random allocations and frees (sizes 1 B–64
  KiB, alignments 16–4096) against a model. Each live block keeps its fill
  byte, alignment and red zone. `heap_check` passes throughout, used-block
  counts match, and freeing everything leaves one free block. Bad frees,
  limits, all-or-nothing growth when the 4th frame allocation fails, and
  use-after-free/overflow panics are also checked. About 254,000 checks.
- **Scheduler:** a fake `proc.c` with the same contract (every state
  change goes through `sched_state_changed`; `thread_switch` turns the old
  thread READY and the new one RUNNING). It checks exact round-robin order
  with the 2-tick quantum, sleep for exactly N ticks, the idle fallback,
  and that yield with no other READY thread keeps the CPU. Then 3 seeds ×
  50,000 random ticks, yields, sleeps, spawns and exits check after every
  step that the queues are consistent, exactly one thread runs, idle never
  holds the CPU over a tick while work is READY, and no READY thread
  starves. About 2.55 million checks.

To check that the tests can actually fail, we planted two bugs (quantum
off by one; free poisoning only half the block) and both suites failed.

## 8. Benchmarks: `python build.py bench`

Builds the kernel with `BHARAT_BENCH`, boots it, and reads lines like
`[bench] context_switch: 1927 cycles/op`. It then appends a row (date,
commit, host, results, boot seconds) to `docs/benchmarks.md`. Each result
is the best of 5 rounds, which filters out rounds hit by a timer
interrupt. The numbers are TSC cycles under QEMU without hardware
acceleration, so they are for comparing BharatOS with itself over time.

| Benchmark | First result |
|---|---|
| thread switch via `sched_yield` | 1,927 cycles |
| `kmalloc`+`kfree`, 64 B | 1,982 cycles |
| `kmalloc`+`kfree`, 4 KiB | 34,692 cycles (poisoning scan) |
| `trace_event` | 214 cycles |

## 9. Budgets

`python build.py test` (and so CI) prints and checks:

```text
[budget] kernel_lines: 5058 (limit 8000) ok
[budget] boot_seconds: 10.6 (limit 90) ok
```

- **kernel_lines:** C, headers and assembly under `kernel/`, not counting
  self-tests or third-party headers. Going over means: simplify, or raise
  the limit on purpose in the same change.
- **boot_seconds:** from starting QEMU until every boot self-test has
  passed. The limit is generous because CI machines are slow; it exists
  to catch big regressions.

Limits live in `scripts/budgets.py`.

---

## How it is tested

| Command | What it proves |
|---|---|
| `python build.py test` | 29 new boot checks (`kernel/tests/debug_test.c`): the symbol table is complete and sorted; name+offset lookup; non-code addresses rejected; a 3-deep call chain is walked in the right order; null and unmapped frame pointers stop the walk; the canary is randomised with a 0 low byte; red zone and use-after-free writes are reported by `heap_check` and pass again once repaired; freed memory is poisoned; `kzalloc` zeroes only what was asked; trace order, timestamps, wrap-around, disable, names. Plus budgets. |
| `python build.py test-faults` | 12 demos, 4 new: **`panic`** (forced panic: trace shows `demo_panic_inner ← debug_demo_panic ← kernel_main`, plus recent events); **`smash`** (stack overflow caught by the canary, trace names `stack_smash_victim`); **`uaf`** (write after free caught when the memory is reused); **`overflow`** (write past a 20-byte block caught by `kfree`). |
| `python build.py unit` | the host tests above |
| `python build.py bench` | runs and records the benchmarks |

Debug mode was checked by connecting to the GDB server and reading the
paused CPU's state.

## Known limits

- Call traces skip assembly routines and inlined functions; no line
  numbers (they would need DWARF parsing).
- Heap checks catch writes, not reads, and not writes that skip the red
  zone. Page-level guards (as in KFENCE) could come with the Phase 8
  allocators.
- The event trace is for one CPU; Phase 9 needs per-CPU rings.
- `build.py sim` (simulation with injected failures) is not built yet; it
  comes with BharatFS in Phase 14.
- Host unit tests cover the heap and scheduler; the PMM and VMM still
  rely on the boot tests.
