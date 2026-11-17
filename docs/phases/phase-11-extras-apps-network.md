# V1 extras: network, Browser, Player, Code

After the V1 desktop (Phase 11) and before V1 hardening (Phase 12), a
round of quick wins made the desktop feel like a complete OS: a network
stack written from scratch, three new apps, a better Files and Terminal,
and more memory. The list and the reasons are in
[QUICK_WINS.md](../../QUICK_WINS.md); the plan after this is in
[FINAL.md](../../FINAL.md).

## What was built

| Area | What | Files |
|---|---|---|
| Memory | QEMU gets 2 GB (was 512 MB); ramfs files up to 4 MiB, 32 MiB in all | `build.py`, `scripts/test_boot.py`, `kernel/fs/fs.h` |
| Network card | Intel e1000 driver: two DMA rings of 32 descriptors, 2 KiB buffers, polled | `kernel/net/e1000.c` |
| Network stack | Ethernet, ARP (8-entry cache), IPv4 (checksums, no fragments), ICMP echo (answer and ask), UDP (one waiting reply), DNS (A records), TCP (connect, stop-and-wait send with resends, in-order receive, FIN/RST), HTTP/1.0 GET with redirects | `kernel/net/net.c` |
| Browser | address bar, Back, Home, links, headings, lists, `<pre>`, entities; scripts, styles and comments hidden; https explained | `kernel/apps/browser_app.c` |
| Player | `.bvid` video: 256-colour palette + raw frames; frame chosen by the clock; play/pause, seek, progress bar | `kernel/apps/player_app.c`, `scripts/make_video.py`, `boot/bundle/media/` |
| Code | explorer, line numbers, colours (C, Python, shell, Markdown), current-line highlight, status bar, Run for `.sh`, auto-indent | `kernel/apps/code_app.c` |
| Files | Places column, disk bar, files open in the app that suits them | `kernel/apps/files_app.c`, `kernel/apps/icons.c` |
| Terminal | Tab completion (commands and paths); `tree cp mv head wc grep df net ping host fetch` | `kernel/apps/shell.c`, `kernel/apps/lineedit.c` |
| Window manager | an `anim` callback every frame (video); 10 windows; F1..F8 | `kernel/ui/wm.c`, `kernel/ui/desktop.c` |
| Graphics | `gfx_image8`: palette image drawn scaled, clipped | `kernel/ui/gfx.c` |

## How the network works

Nothing runs in the background. Whoever waits for the network (ping, the
browser, `fetch`) calls `net_poll()` in a loop: it takes the frames the
card has received, answers ARP requests and pings, and files TCP data
into the right connection; then the caller sleeps one tick and tries
again. One spinlock ("network", a new lock class) guards all stack state;
nobody sleeps while holding it.

TCP keeps it simple: one segment in flight at a time when sending (the
HTTP request is one segment), resent after 300 ms, doubling up to six
times; received data is accepted only in order (anything else is
acknowledged again so the sender resends). The window we advertise is the
free room in a 64 KiB receive buffer.

QEMU's built-in "user" network gives fixed addresses: we are 10.0.2.15,
the gateway (and the PC) is 10.0.2.2, the name server is 10.0.2.3.

## How it is tested

- **53 boot self-checks** in `kernel/tests/apps_test.c` ("Network and
  apps"): address parsing, refused bad URLs and https, ARP and ping to the
  gateway, bad connection numbers; Code typing, indent, save and Run;
  Player frames following the clock, pause and seek, refused non-videos;
  Browser text, entities, hidden scripts, title, links, relative links,
  Back; Files places and open-with; the new shell commands and Tab
  completion.
- **A real page over TCP:** `scripts/test_boot.py` starts a small web
  server on the PC, then types `fetch http://10.0.2.2:18080/net-test.txt`
  at the BharatOS prompt and waits for the page's text.
- **The tour** now opens Code, Browser and Player.
- Totals: **622 boot self-checks** pass with 4 CPUs and with 1 CPU; unit
  tests pass; zero compiler warnings.

## Numbers

- Ping to the gateway: about 40 ms under QEMU without hardware
  acceleration (emulated card and CPU).
- Boot with every check: about 26 s (budget 90 s).
- Kernel size: about 20,000 lines (budget raised to 21,500, decision D42).

## Known limits

- No HTTPS (needs TLS; BearSSL is planned), no DHCP (fixed QEMU
  addresses), IPv4 only, polled (no interrupts), four TCP connections.
- The Browser shows text and links only: no images, CSS or JavaScript; a
  page loads on the desktop thread, so the desktop waits (a few seconds
  at most).
- The Player has no sound; the whole video is read into memory.
- Code runs shell scripts only; C programs need user mode.
- These are native apps, not ports of VS Code, VLC or Chrome (decision
  D40); porting small open-source apps comes after user mode (FINAL.md).
