# V1 extras, round two: quick wins 11–21

A second round of quick wins from [QUICK_WINS.md](../../QUICK_WINS.md),
built after the network, Browser, Player and Code round
([phase-11-extras-apps-network.md](phase-11-extras-apps-network.md)) and
before V1 hardening (Phase 12). Each one is small, shows up in a demo, and
has boot self-tests.

## What was built

| # | What | How | Files |
|---|---|---|---|
| 11 | **DHCP** | At start-up the stack broadcasts a DHCP "discover", takes the "offer", asks for it ("request") and keeps the "ack": address, netmask, gateway, name server, lease time. If nobody answers in 3 s, QEMU's fixed settings stay. Answers may come to the broadcast address, so UDP to 255.255.255.255 is now accepted. `net` shows where the settings came from. | `kernel/net/net.c`, `net.h`, `shell.c` |
| 12 | **`why slow`, `why crash`** | `why slow` takes two snapshots of every thread's CPU time and every CPU's idle time half a second apart, then names the busiest thread (and `kill <id>` to stop it), the CPU load, memory and file space, and gives a plain answer. `why crash` reads the last contained crash: which thread, what it did wrong in plain words (null pointer, divide by zero, ...), and that only that thread stopped. Rules over real data, no AI (D15). | `kernel/apps/why.c` |
| 13 | **Hindi and Marathi on the login screen** | नमस्ते · नमस्कार is always shown under the clock. F1 switches the labels ("Your name", "Password", "Press Enter to sign in") between English, हिन्दी and मराठी. Devanagari letters join and change shape, so whole phrases are shaped at build time by `scripts/make_fonts.py` (Pillow with libraqm, Noto Sans Devanagari, OFL) into `kernel/ui/phrases.bin`; the kernel just blends the finished pictures (`gfx_phrase`). No shaping engine in the kernel (D16). | `scripts/make_fonts.py`, `kernel/ui/font.c`, `gfx.c`, `desktop.c` |
| 14 | **Screenshots** | Print Screen (scancode E0 37) saves the screen as `/home/<you>/screenshot-N.ppm` (a plain picture format: a short header, then red, green, blue bytes) and a notice above the dock says where (new `wm_toast`). | `kernel/input/input.c`, `kernel/ui/desktop.c`, `wm.c` |
| 15 | **Code: tabs and find** | Each open file is a tab with its own text, cursor and scroll. The explorer opens files in a new tab (up to 4); Ctrl+T moves to the next tab, Ctrl+Q closes one (twice if not saved). Ctrl+F opens a find bar: matches are highlighted, Enter jumps to the next one (case ignored, wrapping round), Esc closes. | `kernel/apps/code_app.c` |
| 16 | **Player: playlist and full screen** | The playlist is every `.bvid` in the folder of the video opened, in name order. N/P and a Next button move along it; at the end of a video the next one starts; a video that cannot be read is skipped with a message. F shows the video full screen: the picture as big as fits in whole pixels, the top bar and dock hidden (new `wm_fullscreen`); F or Esc goes back. A second clip, `waves.bvid`, is drawn by `scripts/make_video.py --waves` (no outside footage). | `kernel/apps/player_app.c`, `kernel/ui/wm.c`, `scripts/make_video.py` |
| 17 | **Browser: bookmarks and history** | Ctrl+D adds the page to `/home/<you>/bookmarks.txt` (one "address, Tab, title" line each). The Bookmarks and History buttons open `about:bookmarks` and `about:history`, pages of links made on the spot, so the normal link code shows them. Titles are written with `&lt;` etc., so a page title cannot add tags. | `kernel/apps/browser_app.c` |
| 18 | **Files: copy, paste, rename** | Copy (Ctrl+C) remembers the selected file; Paste (Ctrl+V) copies it into the folder shown, as `name-copy.ext`, `name-copy2.ext`, ... if the name is taken. Rename (`r`) works on files and folders through a new `fs_rename`, which moves a node to a new name or folder in one step. | `kernel/fs/ramfs.c`, `kernel/apps/files_app.c` |
| 19 | **Terminal: scrollback and colours** | The text buffer keeps the last 200 lines that scroll off the top; Page Up / Page Down look through them. It also keeps a colour per cell and understands the standard colour codes (ESC [30–37 m, 90–97 m, 0 m). `ls` colours folders, scripts, videos and web pages, on screen and on serial, but never inside a file (`ls > list.txt`). | `kernel/ui/textbuf.c`, `terminal_app.c`, `shell.c` |
| 20 | **A Rust service** | `user/services/hello-rs` is a `no_std` Rust library with one C-ABI entry (greet, CRC-32) that checks every pointer and length. `build.py` compiles it with `rustc --target x86_64-unknown-none` and links it in when rustc and that target exist (PATH, `~/.cargo/bin`, or `BHARAT_RUSTC`); otherwise, or with `BHARAT_NO_RUST=1`, it says so and builds without it. In the kernel it runs on its own contained thread, `rust-hello`, behind a one-request mailbox (`rust_svc_call`): the caller's bytes are copied in and the answer copied out, the shape a ring will have once user mode exists (decision D43). | `user/services/hello-rs/`, `build.py`, `kernel/core/rust_svc.c` |
| 21 | **Boot time on the splash** | `clock_boot_ns` reads the CPU's time-stamp counter, which starts at 0 at power-on, so the number includes firmware and bootloader. The splash's last step reads "Ready: booted in 22.5 s, 707 self-checks passed"; the login screen shows "Started in 22.5 s". | `kernel/arch/x86_64/clock.c`, `kernel/ui/splash.c`, `kernel/core/kernel.c` |

## Why these designs

- **DHCP with a fallback:** the stack must keep working under any QEMU
  setup and in tests, so a missing DHCP server is not an error.
- **Phrases, not a shaping engine:** a few fixed labels need correct
  Devanagari now; shaping arbitrary typed text can wait (D16).
- **Generated pages for bookmarks and history:** zero new drawing code;
  the escaping keeps them safe.
- **A real `fs_rename`:** copy-then-delete (what `mv` does) cannot rename
  a folder and doubles the memory for big files.
- **Rust behind a mailbox:** the service never sees kernel pointers, so
  moving it to user mode later changes the transport only (D43).

## How it is tested

85 new boot self-checks in `kernel/tests/apps_test.c` ("Network and
apps" now has 138):

- **DHCP:** a hand-built answer is read (address, mask, gateway, name
  server, lease); a wrong transaction ID, a short message, a missing
  marker, an option running past the end and a missing type are refused;
  the live lease from QEMU is checked and asked for again.
- **why:** usage, an unknown question, `why crash` after a contained
  crash names the thread and the cause, `why slow` finds a busy `spin`
  thread from live numbers and says how to stop it.
- **Login languages:** every phrase is present, unknown phrases draw
  nothing, the greeting has ink on the screen, F1 changes the label and
  hint pixels to Hindi, then Marathi, then back; an unknown language falls
  back to English; signing in works with Hindi labels.
- **Screenshots:** the Print Screen scancode; the saved file has the
  right size, header and pixels; the next name; "too many"; refused
  before sign-in.
- **Code:** tabs (open, reopen, Ctrl+T, the limit, closing an unsaved tab,
  a missing file) and find (found with case ignored, not found, Esc).
- **Player:** order, N/P, wrap-around, playing on at the end, full screen
  on and off (the top bar is hidden), a broken video skipped, the limit.
- **Browser:** the empty bookmarks page, Ctrl+D, duplicates, the lists
  and their links, a hostile title, about: pages refused, the limit.
- **Files:** `fs_rename` and its refusals (taken name, folder into
  itself, missing, root); copy and paste in the same and another folder,
  folder rename, empty name.
- **Terminal:** colour codes, resets and `ESC [1;32m`; scrollback limits
  and clamping; `ls` in colour in a Terminal window; Page Up; `ls > file`
  without colour codes.
- **Rust:** greeting from another thread, CRC-32 of "123456789" =
  0xCBF43926, unknown request, answer too small, bad and oversized
  requests, 100 calls in a row; built without Rust: "not built".
- **Boot time:** power-on time is longer than the kernel's own clock; the
  boot test reads the "[boot] Booted in" line and checks it is above zero
  and no more than the time the harness measured.

Totals: **707 boot self-checks** pass with 4 CPUs (706 with 1 CPU; one
check needs a second CPU), 701 when built without Rust; zero compiler
warnings. Screens checked by eye from QEMU screenshots: the Hindi login,
Terminal colours, Code find, Player full screen, Files buttons, Browser.

## Numbers

- Kernel size: about 21,700 lines; the budget was raised to 22,500
  (decision D44).
- Boot with every check: about 21–23 s under QEMU without hardware
  acceleration (one run took 30 s on a busy host); budget 90 s.

## Known limits

- The DHCP lease is never renewed (leases under QEMU last a day).
- `why` knows two questions; `why slow` pauses the terminal for half a
  second while it watches.
- Devanagari only for the fixed login phrases; typed text is ASCII.
- Screenshots are PPM (no compression, about 3 MB at 1280 x 800), at most
  20 per user; the Player and Browser cannot show them yet.
- Code: 4 tabs, find only (no replace). Files: folders cannot be copied.
- History is forgotten at restart; bookmarks can only be removed by
  editing `bookmarks.txt`.
- The Rust service runs inside the kernel until user mode (Phase 13); one
  request at a time.
- HTTPS (item 22) was not started; see QUICK_WINS.md.
