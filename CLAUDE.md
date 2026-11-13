# CLAUDE.md: Working on BharatOS

Read this first. It is the short, practical guide for anyone (human or AI)
adding code to this repo.

## What this is

BharatOS is a from-scratch x86-64 operating system in C and NASM assembly,
booted by Limine, tested in QEMU. Phases 1–5 (boot, interrupts, memory,
threads, scheduler) are done and tested. The next work is Phases 6–20.

> **Always read `docs/UPDATES.md` first.** It records the newer plan:
> microkernel with async rings and handles, the serious-OS upgrades, the
> new Phase 6–20 order, the new low-level designs, the decision log and
> the interview kit. **Where it disagrees with ROADMAP, HLD or LLD, UPDATES
> wins.** Use the older docs for details UPDATES doesn't replace.

| Need to know... | Read |
|---|---|
| **The current plan, phase order, architecture and decisions (read first)** | `docs/UPDATES.md` |
| Why the project exists, the 5 USPs, talking points | `docs/PROJECT_OVERVIEW.md` |
| What to build next, step by step, with "done when" tests | `docs/ROADMAP.md` |
| How the pieces fit, main flows, key decisions | `docs/design/HLD.md` |
| Exact structs, numbers, function signatures | `docs/design/LLD.md` |
| Desktop look: colours, layout, screens | `docs/design/UI.md` |
| How a finished phase works in detail | `docs/phases/phase-N-*.md` |
| What a correct boot prints | `docs/logs/boot-output.md` |

## Commands

```bash
python scripts/fetch_limine.py      # once: download the bootloader into tools/
python build.py                     # build kernel + disk image (build/bharat.img)
python build.py run                 # boot in QEMU (BIOS), serial in terminal; quit: Ctrl+A then X
python build.py run-uefi            # boot through UEFI firmware
python build.py test                # build + boot + check all self-tests (exit 0 = pass)
python build.py test-faults         # build 8 crash-on-purpose kernels, check each report
python build.py clean
```

Run `python build.py test` after **every** change. Run `test-faults` after
touching interrupts, memory, stacks or context switching. On Windows, tool
paths come from `.env` (`BHARAT_CLANG`, `BHARAT_NASM`, `BHARAT_QEMU`, ...;
see `.env.example`).

## Layout

```text
build.py                 the only build entry point (build/run/test)
scripts/                 fat32.py (disk image), test_boot.py (QEMU test harness), fetch_limine.py
boot/limine.conf         boot menu
kernel/
  arch/x86_64/           CPU-specific: entry.asm, linker.ld, GDT/TSS, IDT, interrupts, PIT/PIC timer, context switch
  core/                  kernel_main (boot order) and panic
  mm/                    physical pages (pmm), page tables (vmm), heap, page faults, mm_init
  proc/                  processes, threads, kernel stacks
  sched/                 scheduler
  drivers/               device drivers (serial today)
  lib/                   kprintf, string, box drawing
  include/               third-party headers (limine.h)
  tests/                 boot-time self-tests and their assembly probes
user/                    (Phase 6+) user programs, user library, apps
docs/                    overview, roadmap, design/, phases/, logs/
```

New folders are listed in `docs/UPDATES.md` §8.8 (e.g. `kernel/obj`,
`kernel/ring`, `kernel/ipc`, `kernel/smp`, `user/services`, `idl/`,
`specs/`, `sim/`). Create them when the phase that needs them starts, not
before. The kernel stays in C; new user-space services may be Rust
(`no_std`), per decision D4.

**The build finds files by itself.** Any `.c` or `.asm` under `kernel/` is
compiled; every folder with a `.h` is on the include path. So:
- Add a file → it builds. No list to edit.
- Include headers by bare name: `#include "pmm.h"`, never a relative path.
- Keep file names unique across `kernel/` (objects are named by path, but
  unique names keep includes unambiguous).
- `kernel/arch/x86_64/entry.asm` is always linked first.

## How to build a phase

1. Read the phase in `docs/UPDATES.md` §7 and §8, then the matching parts
   of `docs/ROADMAP.md` and `docs/design/LLD.md` (§14 of UPDATES maps old
   phases to new ones).
2. Create the phase branch first (see Git workflow below). Do one step at
   a time. After each step: build and run `python build.py test`. Don't
   commit after every step; commit in batches (see Git workflow).
3. Add self-tests in `kernel/tests/<area>_test.c` with a
   `bool <area>_run_tests(void)` entry, called from `kernel_main` in
   `kernel/core/kernel.c` after the subsystem starts. Print `PASS`/`FAIL`
   lines and a final `[ok] Phase N ... verified.` line.
4. Add that `[ok]` line to `REQUIRED` in `scripts/test_boot.py`. Any
   `[FAIL]`, `KERNEL PANIC` or `[exception]` in output fails the test.
5. For new crash paths, add a fault demo: a `BHARAT_FAULT_DEMO_<NAME>`
   branch in `run_fault_demo()` (kernel.c), the name in `FAULT_DEMOS`
   (build.py), and expected lines in `FAULT_DEMO_EXPECT` (test_boot.py).
6. Finish: write `docs/phases/phase-N-<name>.md` (what, why, how it's
   tested, known limits), update the phase status in `docs/UPDATES.md` §7,
   the roadmap/feature table in `README.md`, and the boot banner phase line.
7. Also, from Phase 6 on: add numbers to `docs/benchmarks.md` when
   performance is touched, add hard bugs to `docs/BUG_JOURNAL.md`
   (symptom → how found → root cause → fix → guarding test), and add big
   choices to the decision log (UPDATES §10).

## Git workflow (strict)

**Branches**
- Every new phase starts on a new branch from an up-to-date `main`. The
  branch name is the conventional type plus a short description of the
  work, e.g. `feat/debug-hardening`, `feat/modern-hardware`,
  `refactor/memory-allocators`.
- Never commit phase work directly to `main`.
- When the phase is complete and its tests pass: push the branch, open a
  PR to `main` with `gh pr create` (summary of what changed + test
  results), then merge it (`gh pr merge --squash --delete-branch`, or
  `--merge` if the commits are worth keeping separately), and pull `main`.

**Commits**
- Commit in **batches**: one commit per logical group of finished,
  tested work (for example, all the debugging tools together, then all
  the docs). Never one commit per tiny step, and never broken/untested
  work.
- **Conventional Commits, always:** `type(scope): short summary in
  lowercase imperative`, max ~72 characters, optional body explaining
  *what and why*.
  - Types: `feat`, `fix`, `refactor`, `perf`, `test`, `docs`, `build`,
    `ci`, `chore`.
  - Scope = area changed: `mm`, `sched`, `proc`, `arch`, `drivers`,
    `build`, `tests`, `docs`, ...
  - Breaking changes: `type(scope)!:` plus a `BREAKING CHANGE:` line in
    the body.
- **Never mention phase numbers** in commit messages or PR titles.
  Describe what actually changed: `feat(arch): print symbolised call
  traces on panic`, not `Phase 6 step 2`.

**Keep checks fast (no expensive CI)**
- Locally, run `python build.py test` once per batch before committing.
  Run `python build.py test-faults` only when the batch touched
  interrupts, faults, stacks, memory or context switching.
- Don't run long jobs (big simulation/fuzz runs, benchmarks, TLC model
  checks, multi-config sweeps) unless the batch is about that area or
  the user asks. Use small run counts by default (e.g. `--runs 1000`).
- Don't re-run checks that already passed on the same code, and don't
  run checks for things the change can't affect (docs-only changes need
  no boot test).
- GitHub CI must stay **swift**: one job, cached toolchain, build + one
  BIOS boot test with a short timeout. No nightly jobs, no matrix of
  configurations, no long sim/fuzz/bench in CI unless the user asks.
  This overrides any longer CI plans in `docs/UPDATES.md`.

## Code rules (match the existing code)

- C11 freestanding (`-std=gnu11`), no libc. Use `kernel/lib` (`kprintf`,
  `memset`, `memcpy`, `strlen`, ...).
- `snake_case` everywhere. Functions are prefixed by their area:
  `pmm_`, `vmm_`, `proc_`/`thread_`/`process_`, `sched_`, `kstack_`.
- Header guards: `BHARAT_<NAME>_H`.
- Errors are returned as an area status enum (`vmm_status_t`,
  `proc_status_t`) or, for system calls, negative `E_*` codes. Validate
  every argument. Never ignore a returned error.
- `panic("plain description")` only for kernel bugs/corrupted state, never
  for bad input from a user program.
- Shared kernel data (heap, PMM, VMM, scheduler lists, process tables) is
  changed only inside `IRQ_GUARD()`. Never allocate memory or sleep inside
  the timer interrupt path.
- The kernel is built with `-mno-sse`: no floating point in kernel code.
- No new external dependencies. The build must keep working with just
  Python, Clang/LLD, NASM and QEMU on Windows, Linux and WSL.
- Comments explain *why*, in plain words. Each header starts with a short
  block saying what the module does and its limits.
- Zero compiler warnings (`-Wall -Wextra`).

## Safety rules for the next phases

- The kernel touches user memory **only** through `copy_from_user`,
  `copy_to_user`, `copy_string_from_user` (with SMAP on). Never dereference
  a user pointer directly. Copy ring entries before validating them.
- Every resource is a kernel object reached only through a handle, checked
  with `handle_get()` for type and rights. There is no other path, and
  rights can only shrink when handles are duplicated or passed on.
- Until Phase 9, shared data uses `IRQ_GUARD()`; after it, named spinlocks
  with a lock class (checked by the lock-order checker).
- A fault from ring 3 kills only that process. A fault in ring 0 panics.
- Fixed-size tables and rings; check every limit and test hitting it.

## Writing docs and messages

- Plain words, short sentences. Say "the list of things an app may use",
  then name it (`caps`) once. Avoid jargon; when a technical term is
  needed, explain it the first time.
- User-facing text (shell, desktop, crash reports) must make sense to a
  non-programmer. From Phase 12, every such string has a message ID.

## Do not

- Don't add features outside the plan in `docs/UPDATES.md` without
  updating it first.
- Don't skip or weaken a test to make a build pass. Find the bug.
- Don't commit `build/`, `tools/limine/`, `.env`, or disk images.
- Don't use complicated designs where a simple one works. The
  "simplest design that works" choices are deliberate (UPDATES §10, HLD §6).
