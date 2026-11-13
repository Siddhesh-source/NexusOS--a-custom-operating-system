# BharatOS Roadmap: Phases 6–12

This is the build plan from where the code is today (Phase 5 done) to a
finished, demo-ready OS with a desktop. Read
[PROJECT_OVERVIEW.md](PROJECT_OVERVIEW.md) first for the *why*, and
[design/LLD.md](design/LLD.md) for the exact structures each step uses.

## How every phase works

Every phase follows the same five rules. A phase is **not done** until all
five are true.

1. **Small steps.** Each step builds and boots on its own. Commit after
   each working step.
2. **Tests at boot.** New behaviour gets self-tests that run inside the
   kernel (or a user test program) and print `PASS`/`FAIL`.
   `python build.py test` must pass.
3. **Bad paths tested.** Anything that rejects input (bad pointers, denied
   permissions, corrupt disk data) gets a test that feeds it bad input.
4. **A one-minute demo.** Each phase ends with a short scripted demo
   (listed below) you can show someone.
5. **Docs updated.** A `docs/phases/phase-N-*.md` file describing what was
   built, why, and its known limits. Update README and this file's status.

## Status

| Phase | Name | USP | Status |
|---|---|---|---|
| 1–5 | Boot, interrupts, memory, threads, scheduler | foundation | ✅ done |
| 6 | Apps run walled off (user mode + system calls) | 4 (groundwork) | ⏳ next |
| 7 | Programs, keyboard and the shell | — | planned |
| 8 | Explain-it layer + visible self-check | 1, 2 | planned |
| 9 | Files with undo, safe from power cuts | 3 | planned |
| 10 | Apps ask first (permissions) | 4 | planned |
| 11 | Desktop: a clean visual interface | all | planned |
| 12 | Made for Bharat: languages, low-end, real hardware | 5 | planned |
| later | Multi-core, networking, sound | — | optional |

---

## Phase 6: Apps run walled off (user mode + system calls)

**Goal:** run a program at the CPU's lowest privilege level (ring 3) so it
cannot touch the kernel or other programs, and give it a small, carefully
checked set of system calls. A crashing program is stopped; the OS keeps
running.

**Steps**

0. **CI first.** Add a GitHub Actions workflow that installs clang, lld,
   nasm and qemu on Ubuntu, fetches Limine and runs `python build.py test`.
   Every later phase is protected by it.
1. **User segments in the GDT.** Add user code and data descriptors.
   Reorder to `null, kernel code, kernel data, user data, user code, TSS`
   (this order also works with the faster `syscall` instruction later).
2. **Kernel stack on entry.** Set `TSS.rsp0` to the current thread's kernel
   stack top on every switch, so an interrupt from ring 3 lands on that
   thread's own kernel stack.
3. **Save floating-point state.** Give each thread a 512-byte, 16-aligned
   area and save/restore it with `fxsave`/`fxrstor` on switch. User
   programs may use SSE; the kernel still doesn't.
4. **User memory.** Map a program's code, data and stack into the lower
   half of its own address space with the `VMM_USER` flag. Layout in LLD.
5. **Enter user mode.** A small assembly routine builds an interrupt-return
   frame (`iretq`) to jump to the program's entry point in ring 3.
6. **System calls via `int 0x80`.** One IDT entry callable from ring 3,
   one table of handlers indexed by number. Simplest option: it reuses the
   interrupt code that already works. (Switch to `syscall`/`sysret` later
   only if profiling says it matters.)
7. **Safe copying.** `copy_from_user` / `copy_to_user` check that the whole
   range is in the user half and mapped with user access *before* copying.
   Every system call uses them; none touches user pointers directly.
8. **First system calls:** `exit`, `write` (console), `getpid`, `yield`,
   `sleep`, `uptime`.
9. **Crash isolation.** A fault in ring 3 terminates only that process and
   prints a plain report ("program X tried to read address Y, which it
   doesn't own; it was stopped"). Faults in ring 0 still panic.
10. **Permission field from day one.** Each process gets a `caps` field
    (empty list). System calls check it from the start, even though only
    "console" exists now. Cheap to add now, painful to add later.
11. **Embedded test programs.** Until Phase 7's loader exists, the build
    assembles small user programs and links their bytes into the kernel.

**Done when (tests)**
- A user program prints through `write` and exits with a code the kernel sees.
- Reading kernel memory, writing to its own code, or running its own data
  from ring 3 each kills only that program (one test each).
- Passing a kernel address, an unmapped address, or a range that crosses
  into the kernel half to `write` returns an error and does not crash.
- Two user programs run at the same time; neither can see the other's memory.
- A user program that loops forever is still preempted.
- Floating-point registers survive preemption (two threads using SSE).
- All 223 existing checks still pass.

**Demo:** boot; two user programs print interleaved lines; a third crashes
on purpose; the kernel prints a plain report and keeps going.

---

## Phase 7: Programs, keyboard and the shell

**Goal:** load real programs from a file bundle, type commands, and run
them. The first interactive BharatOS.

**Steps**
1. **Startup file bundle (initrd).** The build packs everything in
   `user/` into a plain `.tar` file. Limine loads it as a module; the
   kernel reads it in place (read-only). Tar is chosen because the format is
   trivial: a 512-byte header then the file bytes.
2. **Program loader.** Load static 64-bit ELF files: read the program
   headers, map each segment with the right permissions, set up the stack
   with `argc/argv`, jump in. No dynamic linking.
3. **User library (`user/lib`).** System-call wrappers, `printf`, string
   functions, a simple `malloc` on top of a `mem_grow` system call. One
   header: `bharat.h`.
4. **Console input.** A PS/2 keyboard driver (IRQ1) and serial input both
   feed one input buffer. `read` blocks the thread until a line is ready.
   This is the first real use of the BLOCKED state. Serial input lets the
   test harness type commands automatically.
5. **New system calls:** `read`, `spawn(path, args)`, `wait(pid)`,
   `mem_grow`, `list_dir`, `kill`.
6. **Shell `bsh`.** Built-in commands: `help`, `ls`, `run`/bare program name,
   `ps`, `mem`, `uptime`, `kill`, `clear`, `exit`. Clear error messages
   ("no program called 'xyz'; type help").
7. **Starter programs:** `hello`, `echo`, `cat`, `spin` (CPU hog, for
   demos), `crash` (faults on purpose).

**Done when (tests)**
- The test harness types `hello` over serial and sees its output.
- `ps` lists the shell and a running `spin` with CPU use.
- `crash` is reported and the shell keeps working.
- Malformed ELF files (bad magic, segment into kernel half, huge size) are
  rejected with an error, never a crash.
- `read` blocks without using CPU (idle thread runs while waiting).

**Demo:** boot to `bsh>`; `ls`, `hello`, `spin &`, `ps`, `kill`, `crash`, `ps`.

---

## Phase 8: Explain-it layer + visible self-check (USP 1 and 2)

**Goal:** the OS tells you what's happening and why, in plain sentences,
and shows its own health.

**Steps**
1. **Per-app accounting.** Each process keeps: CPU ticks (sum of its
   threads), memory pages in use, system calls made, last crash reason.
2. **Event diary.** A fixed-size ring buffer in the kernel (e.g. 1024
   entries). Events: app started/exited/crashed, low memory, permission
   used/denied (Phase 10), file restored (Phase 9). Old entries are
   overwritten; no allocation in the hot path.
3. **CPU usage over time.** Every second, record each process's CPU share
   into a 60-entry history ring. Enough for "last 30 seconds".
4. **`why` command.** A small table of rules in the user library turns
   stats and events into sentences. Each rule is checked in order and the
   first match answers:
   - `why slow`: top CPU users in the last 30 s, and memory pressure if any.
   - `why crash [app]`: last crash of that app, translated from the fault
     report.
   - `why memory`: biggest memory users.
   - `why` alone: summary of the last few notable events.
5. **Self-check made visible.** Boot tests already run; now their results
   are stored. `health` prints "Self-check: 223/223 passed" and lists any
   failure. If a core check fails, boot into **safe mode** (shell only,
   banner explains why).
6. **Auto-restart.** A tiny supervisor restarts background services that
   crash, at most 3 times a minute, and logs each restart.
7. **Keep what you're using fast.** Add two priority levels: the program
   you're interacting with (foreground) is picked before background ones,
   with an anti-starvation rule (background still runs at least every N
   slices). Simple, and fixes the most common "it feels slow" case.

**Done when (tests)**
- With `spin` running, `why slow` names `spin` and a percentage within
  ±10% of measured.
- After `crash`, `why crash crash` gives the right fault in plain words.
- Diary wraps around correctly after 10,000 events with no memory growth.
- A build with one test forced to fail boots into safe mode and `health`
  shows the failure.
- Foreground echo stays responsive (key to screen under 50 ms) with 4 CPU
  hogs running.

**Demo:** start 3 `spin`s, type `why slow`; run `crash`, type `why`;
type `health`.

---

## Phase 9: Files with undo, safe from power cuts (USP 3)

**Goal:** a real writable file system on a second virtual disk, where a
save can never be half-done and every change can be undone.

**Steps**
1. **Disk driver.** ATA PIO on the IDE controller QEMU already provides.
   Slow but tiny and well documented. Second disk image `data.img`.
   (Switch to virtio-blk later for speed.)
2. **One file interface (VFS).** `open/read/write/close/list/stat/remove`
   with three backends: the read-only startup bundle (`/sys`), the
   BharatFS data disk (`/home`), and devices (`/dev/console`).
3. **BharatFS: write new, then switch.** (Layout in LLD.)
   - Data blocks are never overwritten while in use.
   - A save writes new data blocks, then new metadata blocks, then a new
     small **root record** (sequence number + checksum) into one of two
     fixed slots, alternating.
   - At mount, read both slots and use the newest one whose checksum is
     valid. A power cut at any moment leaves the previous root valid.
   - Every block has a checksum, so silent corruption is detected and
     reported, not passed on.
4. **Versions.** Old roots are kept in a version list. `history <file>`,
   `undo <file>`, `rewind <folder> <time>`. Space rule: keep every version
   for 1 day, then one per day for 7 days; a background cleaner frees the
   rest.
5. **Shell commands:** `write`, `cp`, `mv`, `rm` (goes to versions, so
   it's undoable), `history`, `undo`, `rewind`, `df`.
6. **`mkfs` tool in Python** (`scripts/mkfs_bharatfs.py`) to create the
   empty data disk at build time, and a host-side checker used by tests.

**Done when (tests)**
- Power-cut test: the harness kills QEMU at random points during a long
  write loop (20 runs); after each reboot the disk mounts, the checker
  finds no errors, and every file equals some previously saved version.
- `undo` and `rewind` restore exact bytes (compared by checksum).
- A flipped bit on disk is detected and reported by name.
- Disk full gives a clean error and no damage.

**Demo:** write a file, kill the VM mid-save, reboot, `cat` shows the last
good version; `rm` a file, `undo` it.

---

## Phase 10: Apps ask first (USP 4)

**Goal:** apps get nothing by default; access is granted by the user,
checked at one place, and logged.

**Steps**
1. **Permission list per process.** Each entry: kind (files, device,
   network later), a path prefix or device name, and rights (read, write).
   The `caps` field from Phase 6 becomes this list.
2. **App manifest.** Each app ships a short text file
   (`notes.app`: name, needs `files:/home/notes rw`). Requests outside the
   manifest are denied without asking.
3. **One check point.** Every system call that touches a resource calls
   `cap_check(process, kind, target, rights)`. No other path exists.
4. **Asking.** First use of a manifest-listed permission pauses the app
   and asks the user (shell now, desktop in Phase 11):
   Allow / Allow once / Deny. The answer is saved in `/home/.permissions`.
5. **Children get less, never more.** `spawn` passes only a subset of
   the parent's list.
6. **Access log.** Every allow/deny is a diary event.
   `why access <app>` lists what it touched. `perms` lists and revokes
   grants.

**Done when (tests)**
- An app without a grant gets a clear "not allowed" error, never data.
- A child process cannot gain a permission its parent lacks.
- Revoking a grant takes effect on the next call.
- Path tricks (`/home/notes/../secret`) are normalised and denied.

**Demo:** run `notes` and get asked; deny, then allow; show
`why access notes`; revoke with `perms`.

---

## Phase 11: Desktop, a clean visual interface

**Goal:** a simple, good-looking desktop that makes every USP visible
without the command line. Look and feel: [design/UI.md](design/UI.md).

**Steps**
1. **Screen.** Limine already gives the kernel a framebuffer (the request
   exists in `kernel/core/kernel.c`). Map it into one trusted user
   process, the **desktop**, guarded by a `screen` permission.
2. **Mouse.** PS/2 mouse driver (IRQ12), events into an input queue.
3. **Messages between programs (IPC).** Channels with fixed-size
   messages (256 bytes) and a shared-memory call for pixel buffers. Simple,
   copy-free for pictures, easy to check.
4. **Windows, the simple way.** Each app draws into its own shared pixel
   buffer and tells the desktop "frame ready". The desktop copies dirty
   rectangles to the screen, draws title bars, and routes mouse/keyboard
   to the focused window. One process owns the screen; no complex
   compositing.
5. **Text.** Bitmap fonts (PSF format) for Latin. Devanagari via fonts
   prepared at build time by a Python script (see Phase 12).
6. **Built-in apps (all small):**
   - **Home:** app launcher + clock + health badge ("✓ Self-check passed").
   - **Terminal:** `bsh` in a window.
   - **Files:** browse `/home`, with a **History** button per file
     (undo/rewind from Phase 9).
   - **System ("Why?")**: live CPU/memory bars per app and the plain
     sentences from Phase 8.
   - **Permissions:** who can access what, with a toggle per grant.
   - **Settings:** language, theme (light/dark), text size.
7. **Permission prompts** become a centred dialog.

**Done when (tests)**
- Screenshot tests: the harness takes QEMU screenshots (`screendump`) of
  each app and compares them with saved reference images (pixel diff under
  a small threshold).
- A crashing app's window closes with a notice; the desktop stays up.
- Desktop idles under 2% CPU with nothing changing.

**Demo:** the full story in two minutes: boot to desktop → health badge →
open Files, edit, undo → run a CPU hog, open "Why?" → app asks for
permission → switch to Hindi.

---

## Phase 12: Made for Bharat (USP 5)

**Goal:** comfortable on cheap hardware, fully offline, and in Indian
languages.

**Steps**
1. **Message IDs everywhere.** Every user-visible string goes through
   `msg(ID)`; language packs are files of `ID = text`. Tests fail if a
   string is shown without an ID.
2. **Hindi and Marathi first.** Fixed UI text is shaped and rendered into
   glyph strips at **build time** by a Python script using a proper
   open-licence font (Noto Sans Devanagari). Correct output, no complex
   code in the OS. Typed text uses a small runtime shaper for common cases
   (letter + vowel sign + halant joins), documented with its limits.
3. **Memory and speed budgets as tests.** Boot with `-m 128M` in CI;
   fail if boot-to-desktop is over the time budget or free memory at idle
   is under the floor. Report the numbers in `health`.
4. **Offline help.** `help <topic>` and the "Why?" app read local text
   files in the current language.
5. **Real hardware.** Boot the image from a USB stick on at least one
   real BIOS PC and one UEFI PC; document what works.

**Done when (tests):** language switch screenshot tests pass for English,
Hindi and Marathi; 128 MB boot test passes in CI; real-hardware boot
documented with photos.

**Demo:** same two-minute desktop demo, in Hindi, on a 128 MB virtual
machine.

---

## Later (only after Phase 12)

- **Multi-core (SMP):** replace `IRQ_GUARD()` with real spinlocks (it was
  designed as the single place to do this), per-CPU run queues.
- **Networking:** virtio-net driver, a small UDP/TCP stack, a `network`
  permission (Phase 10 model already supports new kinds).
- **Faster disk:** virtio-blk driver behind the same disk interface.
- **Sound, USB storage.**

## What we are deliberately *not* doing

- No POSIX compatibility layer or Linux binary support: it would swallow
  the project.
- No dynamic linking, no fork(): `spawn` is simpler and enough.
- No AI features in the core: "why" is rules over real data, so it is
  explainable and works offline.
- No feature that can't be shown in a one-minute demo and tested
  automatically.
