# BharatOS: Project Overview

**A small, from-scratch operating system that is safe, explains itself
and is made for the people who use it.**

| | |
|---|---|
| **Status** | Phases 1–5 done: the kernel core works and is tested (see [ROADMAP.md](ROADMAP.md) for Phases 6–12) |
| **Runs on** | x86-64 PCs (tested in QEMU), BIOS and UEFI |
| **Written in** | C and x86-64 assembly, built with a single Python script |
| **Proof** | 223 automatic checks run *inside the kernel on every boot*, plus 8 deliberate-crash tests |

---

## 1. The idea in one paragraph

Most people never think about their operating system until it hurts them:
a power cut corrupts a file, a cheap laptop crawls, an app quietly reads
their photos, or the screen freezes and no one can say why. BharatOS is
built around fixing those everyday problems with simple, solid engineering
instead of piling on features. It checks itself every time it starts, tells
you in plain words what is going on, lets you undo any change to your
files, makes apps ask before they touch your things, and runs well on
low-cost hardware in Indian languages.

## 2. The everyday problems we are solving

| # | Problem people actually have | Who feels it most |
|---|---|---|
| 1 | "My computer is slow / froze / crashed and I have no idea why." | everyone, especially non-technical users |
| 2 | "The power went out and my file is gone or broken." | homes, schools and small shops with unreliable power |
| 3 | "I deleted / overwrote something by mistake." | students, office workers |
| 4 | "Some app is reading my contacts, photos or files without asking." | anyone with private data |
| 5 | "My old laptop is too slow for modern systems, and the system doesn't speak my language." | first-time computer users, budget hardware owners, schools |
| 6 | "An update or a bad driver broke my machine and I can't tell what changed." | IT staff, schools, cyber cafés |

## 3. The five things that make BharatOS different (USPs)

Each one is a real problem (above), a simple design that solves it, and a
demo you can show in under a minute.

### USP 1: Checks itself on every boot

- **Problem:** systems fail silently. You find out something is broken
  only when you lose work.
- **What BharatOS does:** every boot runs the OS's own health checks
  (today: 223 of them) for memory, threads and the scheduler. You get a
  one-line report ("Self-check: 223/223 passed"). If a core check fails,
  the system starts in a safe mode and tells you what failed.
- **Why it's simple:** the checks already exist and already run. We only
  keep their results and show them.
- **Demo:** boot, type `health`, see the report. Boot a deliberately
  broken build and watch it refuse to pretend everything is fine.

### USP 2: Explains itself in plain words

- **Problem:** "Not responding" and error codes help no one.
- **What BharatOS does:** the system keeps a short diary of what happened
  (apps started, stopped, crashed, used a permission, ran out of memory).
  A `why` command (and a "Why?" panel in the desktop) turns it into
  sentences:
  - "It's slow because **Video Editor** used 85% of the processor for the
    last 30 seconds."
  - "**Notes** closed because it tried to use memory that wasn't its own.
    Your other apps were not affected. It has been restarted."
- **Why it's simple:** the kernel already counts CPU time per thread and
  already produces detailed crash reports. We add a fixed-size event log
  and a short table of rules. No AI needed, works offline.
- **Demo:** run a program that hogs the CPU, type `why slow`.

### USP 3: Undo for your files, safe from power cuts

- **Problem:** power cuts corrupt files, mistakes overwrite work,
  ransomware encrypts everything.
- **What BharatOS does:** the file system never overwrites data in place.
  Every save writes new blocks, then flips one small "current version"
  record. So:
  - A power cut in the middle of a save leaves the *previous* version
    intact. There is never a half-written file.
  - Old versions are kept, so you can `undo` a change, see a file's
    `history`, or `rewind` a folder to how it was yesterday.
- **Why it's simple:** "write new, then switch one pointer" is one rule
  that gives both crash safety and undo. No separate backup tool.
- **Demo:** start saving a big file, kill the virtual machine mid-save,
  reboot, show the file is the last good version. Then `undo` a change.

### USP 4: Apps ask first

- **Problem:** apps get broad access by default and users can't see what
  they did.
- **What BharatOS does:** every app starts with **no access** to your
  files, camera or network. It must ask ("**Notes** wants to read
  **Documents/notes**: Allow / Allow once / Deny"). Every use is written to
  the diary, so `why access notes` shows exactly what Notes touched.
- **Why it's simple:** each process carries a short list of things it may
  use. Every system call that touches a resource checks that list. Child
  programs can only get a subset of their parent's list. That's the whole
  model.
- **Demo:** run an app without permission, watch it get a clear "not
  allowed" and a prompt; open the access log.

### USP 5: Made for Bharat: light, offline and in your language

- **Problem:** modern systems need expensive hardware, constant internet,
  and speak English first.
- **What BharatOS does:**
  - Targets **128 MB of RAM** and a boot of a couple of seconds, checked by
    automatic tests so it stays that way.
  - Everything (help, `why`, settings) works **fully offline**.
  - System messages come from language packs. **Hindi and Marathi
    (Devanagari)** ship first, more scripts later. Language is a setting,
    not an afterthought.
- **Why it's simple:** every message the OS shows has an ID, and each
  language is just a table of ID to text. A memory budget is a test that
  fails the build, not a promise.
- **Demo:** switch language to Hindi; the shell, the "Why?" panel and
  permission prompts all change.

## 4. What is already built (Phases 1–5)

| Phase | What works today |
|---|---|
| 1. Boot | Starts from BIOS or UEFI via the Limine bootloader, 64-bit mode, kernel in high memory, serial console output |
| 2. CPU & interrupts | Crash handlers for all 32 CPU errors with detailed reports, emergency stack for double faults, 100 Hz timer |
| 3. Memory | Tracks every 4 KiB page of RAM, builds its own page tables, code is read-only, data can't run as code, kernel heap, separate address space per process |
| 4. Processes & threads | Processes and kernel threads, own stack per thread with overflow guards, assembly context switch, corrupted-state detection |
| 5. Scheduler | Time-sharing (round-robin, 20 ms slices), a looping thread can't hog the CPU, idle thread, sleep, live statistics |

**Proof, not promises:** 114 memory checks, 83 process checks, 26 scheduler
checks and 8 crash demos, all passing under BIOS and UEFI with zero compiler
warnings. Full boot log: [logs/boot-output.md](logs/boot-output.md).

## 5. Where it's going

```text
 Done                    Next                                     Goal
 ──────────────────────  ───────────────────────────────────────  ─────────────────
 1 Boot                  6  Apps run safely, walled off (user mode)
 2 Interrupts            7  Programs + keyboard + shell (bsh)
 3 Memory                8  Explain-it layer + visible self-check   USP 1, 2
 4 Threads               9  Files with undo, power-cut safe          USP 3
 5 Scheduler             10 Apps ask first (permissions)             USP 4
                         11 Desktop: clean visual interface          all USPs visible
                         12 Made for Bharat: languages, low-end      USP 5
```

Details, steps and "done when" checks for each phase: [ROADMAP.md](ROADMAP.md).
How the pieces fit: [design/HLD.md](design/HLD.md). Exact data structures
and interfaces: [design/LLD.md](design/LLD.md). The desktop's look:
[design/UI.md](design/UI.md).

## 6. Talking points (for reviews, interviews, demos)

**On engineering quality**
- "Every boot proves the OS works. 223 checks run inside the real kernel,
  not in a simulator."
- "We test the bad paths too: 8 builds crash on purpose (null pointer,
  stack overflow, corrupted thread state) and the tests confirm each crash
  is caught and reported correctly."
- "Preemption is *proven*: threads that never give up the CPU still take
  fair turns (26/26/25 time slices), and all 15 registers survive being
  interrupted."
- "Real hardware-level bugs were caught by these tests: a wrong TSS layout
  that rebooted the machine, interrupt handlers returning to the wrong
  place, races that only appear with preemption."

**On product thinking**
- "We started from user problems (power cuts, 'why is it slow', apps
  snooping, cheap hardware), not from a feature list."
- "One simple rule, *write new data then switch one pointer*, gives both
  power-cut safety and undo."
- "Permissions are checked in one place, at the system-call boundary, so
  there's no way around them."
- "The 'why' feature is a table of rules over data the kernel already
  collects. It's explainable and works offline."
- "Low memory use is enforced by a test that fails the build, so it can't
  slowly get worse."

**On scope discipline**
- "Every phase ends with automatic tests and a 1-minute demo, or it isn't
  done."
- "We pick the simplest design that works (e.g. `int 0x80` system calls,
  a tar archive for the first file system, one desktop process that draws
  everything) and record when we'd upgrade."

## 7. Honest limits today

- One CPU core only (multi-core is a later, optional phase).
- Everything still runs inside the kernel; user mode arrives in Phase 6.
- No disk, keyboard, network or graphics drivers yet (serial and timer only).
- Scheduling is round-robin without priorities (Phase 8 adds a simple
  foreground boost).
- Floating-point registers are not saved yet (Phase 6).

## 8. Tech stack

| Area | Choice | Why |
|---|---|---|
| Kernel | C11 (freestanding) | direct hardware control, no runtime needed |
| Low-level | x86-64 assembly (NASM) | entry, interrupt stubs, context switch |
| Compiler / linker | Clang + LLD | cross-compiles from Windows, Linux or WSL |
| Bootloader | Limine | BIOS and UEFI with one setup; gives us memory map, framebuffer and modules |
| Emulator | QEMU (+ OVMF for UEFI) | fast, scriptable, screenshots for UI tests |
| Build & tests | Python 3 | one cross-platform script; writes the disk image itself |
