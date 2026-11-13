# Way Ahead

Where BharatOS is, what comes next, and why. The detailed plan is
`docs/UPDATES.md` §7; this is the short version.

## Where we are (Phases 1–11 done)

A from-scratch x86-64 kernel that boots on BIOS and UEFI and runs on every
CPU. It has:
- memory management (buddy, slab, heap, demand paging, copy-on-write);
- threads and a tickless scheduler with per-CPU queues and work stealing;
- spinlocks with a lock-order checker, TLB shootdown and RCU;
- 566 self-checks on every boot, 13 crash demos with symbolised reports,
  host unit tests, benchmarks and CI;
- **V1 core (Phase 10):** keyboard, mouse and terminal input; files in
  memory loaded from a boot bundle; login with your name; a shell with
  24 commands (`ls cat echo > cpus top checks lockdemo crashdemo` ...);
  crashes of a demo thread reported and contained.

- **V1 desktop (Phase 11):** a boot splash, a graphical login, windows
  you can drag and close, a top bar, and the apps Terminal, Files, Text
  editor, Monitor (every CPU live), Health (checks, lock-order and crash
  demos) and About; a scripted tour that checks itself.

`python build.py demo`, type your name in the QEMU window, press F12 for
the tour. Next is Phase 12: hardening V1 before recording the demo.

## What comes next: V1, a demo-ready OS

**Goal:** a demo you can give in about 3 minutes, end of day:

```text
boot splash  →  login: type your name  →  desktop with your name on the taskbar
  → Terminal: ls, cat, echo > file, cpus, checks
  → Files: browse, open, create, delete
  → System Monitor: 4 CPUs working live, work being stolen between them
  → Health: 516 boot checks passed, lock-order checker catching a mistake,
            a crash report with a call trace
```

| Phase | What | Result you can show |
|---|---|---|
| 10 ✅ | V1 core: keyboard, mouse, files in RAM (ramfs), login, terminal | type commands on the boot screen and work with files |
| 11 ✅ | V1 desktop: windows, taskbar, Terminal, Files, System Monitor, Health, About; `python build.py demo` | the full visual demo, hands-free with `--auto` |
| **12** | V1 hardening: unit tests, fuzzing, screenshot tests, leftovers, real-hardware boot | V1 you can trust and record on video |

**How V1 stays quick (decision D28):** apps run as kernel threads behind
small APIs (`input_*`, `fs_*`, `ui_*`), and files live in RAM, seeded
from a small boot bundle. That skips months of user-mode and storage work
now. The narrow APIs keep the later move mechanical.

## After V1 (kept, just moved later)

Nothing from the earlier plan was deleted. In order:

| Phase | Was | What |
|---|---|---|
| 13 | 10 | user mode, handles (capabilities), async system-call rings |
| 14 | 11 | microkernel IPC and restartable services |
| 15 | 12 | self-check and explain (`why`); the shell moves to user mode |
| 16 | 13 | drivers as services (virtio-blk, ...) |
| 17 | 14 | BharatFS: crash-safe files with undo; replaces ramfs |
| 18 | 15 | permissions ("apps ask first") and verified boot |
| 19 | 16 | networking |
| 20 | 17 | resume, replay and live migration |
| 21 | 18 | desktop polish and WebAssembly apps |
| 22 | 19 | made for Bharat: Hindi/Marathi, 128 MB, real hardware |
| 23 | 20 | extras: Raft folders, safe tracing programs |

## Rules that do not change

- Every feature gets self-tests; `python build.py test` after every step;
  1, 2 and 4 CPUs must pass.
- Plain-language docs, bug journal, decision log, benchmarks.
- Git: one branch per phase, batched conventional commits, PR, merge,
  never delete branches, no AI attribution.

## Starting the next phase

Paste the "Next phase prompt (V1)" from `PROMPT.md` into a new session.
