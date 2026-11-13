# Phase 10: V1 Core (input, files, login, terminal)

**Goal:** everything the V1 desktop needs underneath, usable today from a
text terminal: keys and mouse movements reach the kernel, files live in
memory, you log in with your name and type commands.

**Status:** done. 136 new boot checks (`input_test.c`, `fs_test.c`,
`shell_test.c`) pass on `--smp 1`, `2` and `4` with every earlier suite
(516 in all). After the checks, the boot test logs in at the real prompt
and types commands, over the serial line and on the emulated PS/2
keyboard, and checks the replies. The 13 crash demos and the host unit
tests still pass.

| Step | Where |
|---|---|
| 0. Wait queues: sleep until something happens | `kernel/sched/waitq.[ch]` |
| 1. Keyboard: PS/2 on IRQ 1, scancode set 1, event queue | `kernel/input/input.[ch]`, `kernel/input/inputdev.[ch]` |
| 1b. Serial input: what you type in the terminal (IRQ 4) | `kernel/input/inputdev.c`, `kernel/drivers/serial.c` |
| 2. Mouse: PS/2 on IRQ 12, 3-byte packets | `kernel/input/input.c`, `kernel/input/inputdev.c` |
| 3. Files in memory (ramfs) and the boot bundle | `kernel/fs/fs.h`, `kernel/fs/ramfs.c`, `boot/bundle/`, `scripts/fat32.py` |
| 4. Login (name, optional password from `/etc/users`) | `kernel/apps/console.[ch]` |
| 5. Shell with a line editor and 24 commands | `kernel/apps/shell.[ch]`, `kernel/apps/lineedit.[ch]` |
| 5b. Contained crashes (`crashdemo`) | `kernel/core/crash.[ch]` |

```text
[input] PS/2 keyboard on (IRQ 1), PS/2 mouse on (IRQ 12), serial input on (IRQ 4)
[fs] Files in memory: 4 files loaded from the boot bundle (10240 bytes)
[ok] Phase 10 input verified.
[ok] Phase 10 files in memory verified.
[ok] Phase 10 login and shell verified.
[ok] 516 boot self-checks passed, 0 failed.

BharatOS login: asha
Welcome to BharatOS!
...
Hello, asha! Type 'help' to see what you can do.
asha@bharat:~$ echo hi > a.txt
asha@bharat:~$ cat a.txt
hi
asha@bharat:~$ cpus
4 CPUs online. Busy % is since boot.
CPU  busy  running            ready  switches  steals  IPIs
  0     3%  console (346)          0      11520       0     4
  1     0%  idle                   0         35       2   522
...
```

---

## 1. How V1 apps work (decision D28)

V1 apps (login and shell today, the desktop in Phase 11) are **kernel
threads that call kernel functions directly**. There is no user mode yet
(Phase 13). Each app talks to the kernel only through small APIs:

| API | What it gives |
|---|---|
| `input_*` | the next key or mouse event; sleep until one arrives |
| `fs_*` | open, read, write, create, mkdir, unlink, list, stat |
| `shell_run(line)` + an output function | run a command, collect what it prints |

Keeping the apps on these narrow APIs means the later move to user mode
is mostly replacing each call with a system call.

## 2. Wait queues

A thread that waits for a key should use no CPU. `waitq_wait(wq, cond,
arg)` puts the thread to sleep until `cond(arg)` is true;
`waitq_wake_all(wq)` (callable from an interrupt) wakes every sleeper.

- No lost wake-ups: the condition is checked with the queue's lock held,
  and the waker takes the same lock. A key that arrives between "check"
  and "sleep" either is seen by the check, or finds the thread already on
  the queue.
- The sleeper marks itself BLOCKED, lets go of the queue lock, and
  switches away **still holding its run-queue lock** (decision D23). A
  waker on another CPU waits for that lock, so it can never make the
  thread READY before its registers are saved.
- Lock order: wait queue, then run queue.
- Sleepers are remembered by **thread ID**, not pointer. A sleeper killed
  meanwhile is simply not found; a reused ID is not woken by mistake
  because the thread's `wait_on` field must point at this queue.

## 3. Input

Three sources feed one queue (`INPUT_QUEUE_SIZE` = 256 events):

| Source | Line | Decoder |
|---|---|---|
| PS/2 keyboard (QEMU window, real PCs) | IRQ 1 → vector 0x41 | `kbd_decode`: scancode set 1, Shift, Ctrl, Alt, Caps Lock, E0 keys (arrows, Home, End, Delete, ...), F1–F12 |
| PS/2 mouse | IRQ 12 → 0x42 | `mouse_decode`: 3-byte packets; bytes that cannot start a packet are skipped so a lost byte loses one packet, not all later ones; overflow packets dropped |
| Serial port (terminal, `-nographic`) | IRQ 4 → 0x43 | `serial_decode`: characters, Enter (`\r`, `\n` or `\r\n`), Backspace (DEL or BS), arrows and Home/End/Delete escape sequences |

The decoders are pure functions (no hardware, no locks), so Phase 12 can
run them in host unit tests. Every key press becomes one event with the
character it types (or an `INPUT_KEY_*` code); mouse events carry the
movement, the new position (kept on the screen) and the buttons.

**Why serial input too?** `python build.py run` puts QEMU in the
terminal; the keys you type there reach the guest as serial bytes, not
PS/2 scancodes. Decoding both to the same events means the shell works
the same in a QEMU window and in a terminal.

The i8042 controller is set up with its interrupts off, both devices are
told to start sending, then the interrupts are turned on and routed
through the I/O APIC to the boot CPU. If no controller answers (some
UEFI-only PCs), only serial input is used.

## 4. Files in memory (ramfs)

A tree of folders and files in kernel memory:

- each node (file or folder) comes from a slab cache; a file's bytes are
  one heap block that doubles in size as the file grows;
- folder entries are kept sorted by name, so `ls` needs no sorting;
- open files are numbered 0–31 like Unix file descriptors; an open file
  cannot be deleted;
- **one spinlock** guards the whole tree (simplest design that works;
  files are small, so copying under the lock is cheap).

| Limit | Value | When hit |
|---|---|---|
| name | 31 characters, no `/`, no control characters, not `.`/`..` | `FS_ERR_NAME_TOO_LONG` / `FS_ERR_INVALID` |
| path | 255 characters | `FS_ERR_NAME_TOO_LONG` |
| one file | 256 KiB | `FS_ERR_TOO_BIG` |
| all files | 8 MiB | `FS_ERR_NO_SPACE` |
| files and folders | 1024 | `FS_ERR_NO_SPACE` |
| open files | 32 | `FS_ERR_NO_SPACE` |

A refused call changes nothing (tested by filling each limit and running
`fs_check()` afterwards). `fs_path_resolve(cwd, path)` turns what you
type (relative, `.`, `..`, repeated `/`) into a clean absolute path; the
file system itself only accepts clean absolute paths.

**The boot bundle (decision D29).** Every file under `boot/bundle/` is
packed by `build.py` into a tar archive (plain ustar, fixed timestamps so
the same files give the same bytes) and placed on the boot disk. Limine
loads it next to the kernel as a module; `fs_load_tar` checks each
header's checksum and creates the folders and files. Module memory is
deliberately outside the kernel's direct map, so the bundle is mapped
read-only first (see bug journal entry 18).

Files are not saved: everything starts fresh at the next boot. BharatFS
(Phase 17) replaces ramfs with files that survive power cuts.

## 5. Login and the shell

The console app (`console.c`) is one kernel thread: it shows the login
prompt, then runs the shell until `logout`.

- **Login:** any name of 1–16 letters, digits, `-` or `_`. A name listed
  in `/etc/users` as `name:password` must give that password (typed as
  `*`). This is a greeting, not a security boundary; permissions come in
  Phase 18. Your home folder `/home/<name>` is created at login.
- **Line editor:** typing, Backspace, Enter, Up/Down through the last 16
  lines, Ctrl+C to abandon a line, Ctrl+U to clear it. Echo goes through
  an output function, so it works on the console, in a window (Phase 11)
  and in tests.
- **Output** goes to the serial terminal and the screen. The screen
  console now understands Backspace, clear-screen and clear-line codes
  and shows a cursor.

| Commands | |
|---|---|
| Files | `ls cd pwd cat echo mkdir rm touch`, and `> file` / `>> file` after any command |
| System | `whoami uptime mem cpus ps top kill run about clear reboot logout help` |
| Health | `checks`: the boot self-test results by suite plus 8 live checks (heap, page allocator, scheduler queues, file tree, a file round trip, a sleep/wake, the lock checker, every CPU answering an IPI) |
| Demos | `lockdemo`: two locks taken in opposite orders; the checker reports it without stopping the system. `crashdemo [null\|divide\|opcode]`: a thread crashes on purpose; the report is printed and only that thread ends |
| Jobs | `run spin N`, `run load N` (one busy thread per CPU: watch them spread with `cpus` or `top`), `run hello` |

## 6. Contained crashes

Before V1, any fault in kernel code halted the machine. A demo should be
able to show a crash report and carry on, so a thread can now opt in
(`THREAD_CONTAIN`): when it faults, `crash_contain()` prints the full
report (what happened, registers, call trace with function names), with
`[crash]` lines instead of `[exception]`, records it, and changes where
the interrupt returns to so the thread runs `thread_exit()`. Only that
thread ends.

Containment is refused, and the machine halts as before, when it would
not be safe: the thread holds a spinlock, the fault is a double fault,
NMI or machine check, or a panic is already running. The console thread
opts in too: if it crashes, the kernel's main thread notices within a
second and starts it again. Kernel threads that did not opt in (the
scheduler, the self-tests) still halt the machine on any fault, so real
kernel bugs are never hidden.

## 7. How it is tested

- **Boot self-tests (headless):** decoders with every key class; the
  queue (order, full queue drops and counts, unknown events refused);
  bytes injected at the PS/2 controller (commands 0xD2/0xD3) arriving
  through the **real IRQ 1 and IRQ 12 paths**; a thread sleeping on input
  using no CPU and woken by a key; three sleepers woken at once. Files:
  paths, create/write/read/seek/list/stat/delete, bad paths and wrong
  types, every size limit, a tar made in the test, a damaged and a cut
  archive, the real bundle. Shell: the line editor, login rules, and a
  scripted list of commands whose output is checked, including three
  contained crashes and the lock demo.
- **Typed session:** `python build.py test` then logs in at the real
  prompt as `tester`, types `echo ... > t.txt` and `cat t.txt` over the
  serial line, types `about` on the emulated PS/2 keyboard (QEMU monitor
  `sendkey`), runs `checks`, and checks every reply.
- `test-faults` (13 demos) and `unit` still pass.

## 8. Try it

```bash
python build.py run              # terminal only: type at the prompt
python build.py run --window     # QEMU window shows the screen; type there or in the terminal
```

Log in with any name, then try: `ls`, `cat /etc/motd`, `echo hi > a.txt`,
`cat a.txt`, `cpus`, `run load 10`, `top` (any key stops it), `checks`,
`lockdemo`, `crashdemo`. `admin` needs the password `bharat`.

## 9. Known limits (for later phases)

- Files live in memory only; nothing survives a reboot (Phase 17).
- One input queue for the whole system; the desktop (Phase 11) will be
  its single reader and pass events to windows.
- US keyboard layout only; no key repeat control; mouse has no wheel.
- Line editing only at the end of the line (no cursor movement inside).
- Login is not a security boundary; `/etc/users` is plain text (Phase 18).
- Apps are kernel threads: a bug in one can still corrupt kernel memory
  before it faults. User mode (Phase 13) fixes that properly.
- A contained crash does not free what the thread allocated.
- Wait queues hold at most 8 sleepers (extra ones check every tick) and
  have no timeouts.
- The screen console wraps to the top instead of scrolling.
