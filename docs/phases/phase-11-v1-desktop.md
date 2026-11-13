# Phase 11: V1 Desktop and Demo

**Goal:** the visual V1, the thing you show: a boot splash, a graphical
login with your name, then a desktop with windows for the terminal, your
files, a live view of every CPU, and the system's health.

**Status:** done. 50 new boot checks (`desktop_test.c`) pass on `--smp 1`,
`2` and `4` with every earlier suite (566 in all). After the checks, the
boot test types a name into the graphical login on the emulated PS/2
keyboard, runs the quick desktop tour (which checks its own results),
saves a QEMU screenshot of the desktop and checks it. The 13 crash demos
and the host unit tests still pass.

| Step | Where |
|---|---|
| 1. Graphics base: surfaces, clipping, text, back buffer, damage | `kernel/ui/gfx.[ch]`, `kernel/lib/font8x8.[ch]` |
| 1b. Boot splash with a progress bar fed by the real boot steps | `kernel/ui/splash.[ch]`, `kernel/drivers/fbcon.c` |
| 2. Window manager: focus, drag, close, top bar, pointer | `kernel/ui/wm.[ch]`, `kernel/ui/theme.h`, `kernel/drivers/rtc.[ch]` |
| 2b. Two input queues: desktop (PS/2) and text console (serial) | `kernel/input/input.[ch]` |
| 3. Apps: Terminal, Files, Text editor, Monitor, Health, About | `kernel/apps/*_app.c`, `kernel/apps/apps.h`, `kernel/ui/textbuf.[ch]` |
| 4. Login screen and the desktop thread | `kernel/ui/desktop.[ch]` |
| 5. Demo mode: the tour, `python build.py demo [--auto]` | `kernel/ui/tour.c`, `build.py` |

```text
[ok] Phase 11 desktop verified.
[ok] 566 boot self-checks passed, 0 failed.
[desktop] Login screen ready (type your name in the QEMU window).
[desktop] asha logged in.
[desktop] Desktop ready for asha.
[tour] Starting the tour of the desktop.
[tour] Terminal: files and CPUs
[tour] check passed: hello from the tour
...
[tour] Busiest CPU over the last second: 100%
[tour] check passed: The crash was contained
[tour] Demo finished (0 checks failed).
```

```text
┌────────────────────────────────────────────────────────────────────────────┐
│ BharatOS Terminal Files Monitor Health About  Healthy: 566 checks  asha │ 4 CPUs  20:38 │
├────────────────────────────────────────────────────────────────────────────┤
│   ┌ Monitor ─────────────────────────────────────────────── x ┐            │
│   │ [L Add load (10 s)]  Started 4 busy threads for 10 s.     │            │
│   │ CPU 0 100%  ████████████   ▁▁▁▁▃█████  steals 2  IPIs 1   │            │
│   │ CPU 1 100%  ████████████   ▁▁▁▁▃█████  steals 3  IPIs 1097│            │
│   │ Memory 10 of 507 MiB in use                               │            │
│   └───────────────────────────────────────────────────────────┘            │
│ F1-F5 open the apps   Ctrl+W closes a window   F12 shows a tour           │
└────────────────────────────────────────────────────────────────────────────┘
```

---

## 1. Graphics base

The screen is the bootloader's framebuffer: a block of memory where every
4 bytes are one pixel. `gfx` draws filled rectangles, outlines and text
into any **surface** (a block of pixels with a width, height and row
length), always inside a **clip rectangle**: nothing outside it changes.

- **Back buffer.** The desktop never draws on the screen directly. It
  draws into a copy in ordinary memory (the back buffer, 4 MB at
  1280 x 800) and then copies the changed part to the screen. Drawing in
  normal memory is fast, and the screen never shows a half-drawn window.
- **Damage rectangle.** Anything that changes (a window moves, a key
  is typed, the pointer moves, the clock ticks) adds its area to one
  "damage" rectangle. A redraw (`wm_compose`) sets the clip to that
  rectangle, redraws every layer (background, windows bottom to top, top
  bar, pointer) and copies only that rectangle to the screen. Moving the
  pointer redraws about 12 x 19 pixels twice, not the whole screen; a
  self-test checks this. Several changes are joined into the smallest
  rectangle that holds them all (decision D36).
- **Text** uses the one built-in 8 x 8 font drawn at double height (8 x 16
  pixels, UI.md's "body" size), and scaled up for the logo. The font
  moved out of the screen console into `lib/font8x8.c` so both share it.
- **Colours** are the named colours of `docs/design/UI.md` (light theme),
  in `ui/theme.h`. Apps use the names, never raw values.

## 2. Boot splash

The top of the screen shows the BharatOS name and a progress bar; the
lower part keeps the boot log (the Phase 7 screen console, moved down
with `fbcon_set_top`). `kernel_main` calls `splash_step("Checking the
hardware")` and so on after each group of self-tests, so the bar moves
with the real boot, not a timer. When the desktop starts it stops the
screen console (`fbcon_stop`); if the kernel ever panics, `panic_begin`
brings the console back over the whole screen (`fbcon_resume`) so the
report can be read.

## 3. Window manager

Each window belongs to an **app**: a name, a size and a few callbacks
(`open`, `close`, `draw`, `key`, `click`, `tick`). The window manager keeps
the windows in stacking order; the top one has the focus.

| You do | What happens |
|---|---|
| click a window | it comes to the front and gets the keys |
| drag a title bar | the window moves (it cannot be moved off the screen) |
| click the `x` | the window closes; the next one gets the focus |
| click an app on the top bar, or F1..F5 | the app opens, or comes to the front if it is open |
| click the green health badge | Health opens |
| Ctrl+W | closes the focused window |
| click "Log out" | every window closes; back to the login screen |

The top bar shows the user's name, the CPU count, the boot self-check
total (green when everything passed) and the time of day from the PC's
battery-backed clock (`drivers/rtc.c`, the host's time in UTC under
QEMU). Its text is worked out once a second, not at every redraw.

The pointer is an arrow drawn last, on top of everything. Windows have no
shadows and do not resize, which keeps drawing simple (UI.md).

Only the desktop thread uses the window manager, so it has no lock. The
self-tests use it from the boot thread before that thread starts.

## 4. Input: two queues (decision D35)

Phase 10 had one input queue. With a desktop there are two readers: the
desktop on the screen and the text console on the serial line (what you
type in the terminal that ran `python build.py run`). Once the desktop
starts (`input_set_desktop(true)`), **PS/2 keyboard and mouse events go
to the desktop's queue, serial keys keep going to the console's**. So you
can use both at the same time, and the boot test can type into each one
separately. The mouse position is still kept in one place, inside the
screen, as in Phase 10.

## 5. The apps

All apps are kernel code run by the desktop thread, using only the small
V1 APIs (`fs_*`, `shell_run`, scheduler and memory statistics), so moving
them to user mode later is mechanical (decision D28).

- **Terminal** is the Phase 10 shell and line editor, unchanged. Their
  output goes into a **text buffer** (`ui/textbuf.c`): a grid of
  characters that understands newline, backspace, tab and the shell's
  clear-screen codes, and scrolls up when full. The window draws the grid.
  `logout` closes the terminal.
- **Files** lists one folder: click a row to select it, click again or
  press Enter to open it (a folder opens in place, a file in the text
  editor). Buttons (and keys) for Up (Backspace), New file (`n`), New
  folder (`d`) and Delete. New names are typed on the bottom line; a `/`
  cannot be typed. Errors are shown in plain words ("Cannot delete docs:
  the folder is not empty."). The list refreshes once a second, so files made
  in the terminal appear.
- **Text editor** holds the file in one 16 KiB block, wraps long lines,
  and supports typing, Backspace, Delete, arrows, Home and End. Save
  (button or Ctrl+S) writes the file; the title shows `*` while there
  are unsaved changes. A bigger file opens read-only, showing its start.
- **Monitor** samples every CPU's scheduler counters once a second: busy
  % over the last second as a bar (saffron above 80%), the last minute as
  a small graph, work steals and IPIs. "Add load" (or `l`) starts one busy
  thread per CPU for 10 s (the shell's `run load 10`), so you watch the
  bars fill and steals spread the work. Memory in use and the thread count
  are below.
- **Health** shows the boot self-check totals and the contained crashes,
  and runs `checks` (1), `lockdemo` (2) and `crashdemo` (3). While one
  runs, the window also **catches the kernel's own messages** by
  attaching to the serial mirror (the hook the screen console uses), so
  the lock checker's report and the crash report, both with function
  names in their call traces, appear in the window. Shell output and
  caught messages both go in under the console lock, so text from other
  CPUs cannot mix into a half-written line.
- **About** runs `about` and adds a few lines about the desktop itself.

Apps run on the desktop thread, one key or click at a time (decision
D37). A long command (`checks`, about a second under QEMU) pauses the
desktop until it ends.

## 6. Login screen and the desktop thread

The desktop thread starts after the boot self-checks, next to the text
console. It takes the screen, then loops: read the desktop's input
queue, hand keys to the login screen or the window manager, run the
tour, tick the apps once a second, redraw what changed, sleep 20 ms.
Nothing is drawn when nothing changed.

The login screen asks for a name with the same rules as the text console
(`login_name_ok`, `login_check`): any name of letters, digits, `-` or `_`;
names listed in `/etc/users` (`admin`, password `bharat`) are asked for
their password, shown as stars. Esc starts over. `login_check` used to
read `/etc/users` into a static buffer, safe only with one caller; now
the console and the desktop can log in at the same moment, so each call
uses its own buffer (bug journal 21).

The desktop thread opts in to contained crashes (Phase 10, D30): if it
faults, the report is printed and `desktop_watch()` starts it again at
the login screen (at most three times).

## 7. Demo mode and the tour

The **tour** (`ui/tour.c`) is a list of steps run by the desktop thread
between frames, so you see each one happen. It logs in as `demo` if
needed, then: types `ls /`, `cat /etc/motd`, `echo ... > tour.txt`,
`cat tour.txt` and `cpus` in the Terminal; drags the Terminal across the
screen with simulated mouse moves; in Files creates `notes.txt`, opens
`tour.txt` in the editor, adds a line and saves it, then deletes
`notes.txt`; in Monitor adds load and waits for the bars; in Health runs
the checks, the lock-order demo and the crash demo; opens About.

It types through the same key path a person uses, and checks the results
on the way (the terminal shows the echoed text, the saved file contains
the new line, the monitor saw load, Health printed "All good", the
checker caught the lock problem, the crash was contained). A failed check
prints a `[FAIL]` line.

| How | What |
|---|---|
| F12 in the QEMU window | the tour at a speed you can follow (about a minute) |
| Shift+F12 | the quick tour (waits divided by 4, typing instant); the boot test uses it |
| `python build.py demo` | builds, opens the QEMU window with the desktop, serial in the terminal |
| `python build.py demo --auto` | a kernel built with `BHARAT_DEMO_AUTO`: logs in and runs the tour by itself |

## 8. How it is tested

- **Boot self-tests (headless, 50 checks):** the window manager draws
  into memory only (`wm_init(NULL, ...)`), and the tests drive it the way
  the desktop thread does. Drawing: fills, clipping, text pixels,
  rectangle maths, present copies only the damage. Text buffer: wrapping,
  scrolling, escape codes, UTF-8 box characters. Login: a bad name, a
  password asked for and refused, a good name. Windows: open, focus,
  click a title bar, drag, kept on screen, a pointer move redraws a small
  area, no change means no redraw, the window limit, close with the `x`,
  the top bar opens an app once, F3, Ctrl+W. Terminal: a typed command's
  output and prompt, `cd`/`pwd`, `clear`, `logout`. Files and editor: new
  file, new folder, `/` refused in names, delete, open a folder, go up,
  open a file, edit with arrows/Home/End/Backspace, Ctrl+S writes the
  file, missing files and folders refused. Monitor samples every CPU and
  draws the bar; Health's checks say "All good"; About; log out.
- **Typed session** (`python build.py test`): logs in on the serial
  console and types there as before; then types `tester` and Enter on the
  emulated PS/2 keyboard into the graphical login; waits for "Desktop
  ready"; presses Shift+F12 and waits for "Demo finished (0 checks
  failed)"; saves the screen with QEMU's `screendump` and checks the
  picture: the white top bar, the desktop background, many colours. Then
  `checks` on the serial console.
- On `--smp 1`, `2` and `4`; `test-faults` (13 demos) and `unit` pass.

## 9. Try it

```bash
python build.py demo             # QEMU window: the desktop. Serial console in this terminal.
python build.py demo --auto      # same, hands-free tour
```

In the window: type your name and press Enter. Click into the window to
use the mouse (Ctrl+Alt+G lets the mouse go again). Open apps from the
top bar or with F1..F5; drag windows by their title bars; press F12 for
the tour.

## 10. Known limits (for later phases)

- Windows cannot be resized or minimised; one theme (light), one font.
- One damage rectangle: two small changes far apart redraw the area
  between them too.
- Apps run on the desktop thread: a long command pauses the desktop
  (Phase 13 moves apps to their own processes).
- The PS/2 mouse is relative, so QEMU's window must capture it; a USB
  tablet (absolute pointer) needs a USB driver.
- The clock shows UTC as QEMU's battery clock gives it; no time zones.
- Health catches every kernel message printed while its command runs,
  including unrelated ones from other threads.
- The text editor holds up to 16 KiB; no undo, no selection, no
  clipboard.
- Files, Monitor and the editor show at most 64 entries, 8 CPUs and
  24 rows at a time.
- The kernel grew to about 16,100 lines; the budget is now 17,000
  (decision D34).
