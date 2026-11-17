# Quick wins: low effort, high impact

The things that make BharatOS look and feel like a real OS fastest, for
the least work. Each one is small (hours to a couple of days), shows up
in a demo, and backs one of the USPs (selling points) in
[docs/PROJECT_OVERVIEW.md](docs/PROJECT_OVERVIEW.md).

**Effort:** S = under a day, M = 1–3 days. **Impact:** what a viewer
notices. ✅ = done in this round.

## Done in the first round ✅

| # | Quick win | Effort | Why it matters |
|---|---|---|---|
| 1 | **2 GB of RAM for QEMU** (was 512 MB); files up to 4 MB, 32 MB of file space | S | room for video, pages and code; everything feels quicker |
| 2 | **Network stack from scratch**: Intel e1000 driver, ARP, IPv4, ICMP (ping), UDP, DNS, TCP, HTTP | M | "my own OS fetches web pages over its own TCP/IP" is the strongest single demo line |
| 3 | **Browser app**: address bar, links, Back, headings, lists; local HTML too | M | a browser is what people expect of a "real" OS |
| 4 | **Player app**: plays a Big Buck Bunny clip (Blender Foundation, CC BY) at 12 frames a second | S | moving pictures make a demo memorable |
| 5 | **Code app**: explorer, line numbers, colours for C/Python/shell/Markdown, Run for scripts, status bar | M | a VS Code–style editor shows the desktop can host real work |
| 6 | **Files: Places and a disk bar** (Home, Code, Videos, Web pages, Documents, Whole disk; space used) | S | "see the disk, open and close folders" in one glance |
| 7 | **Open with the right app**: video → Player, .html → Browser, code → Code | S | feels like a finished desktop, not a tech demo |
| 8 | **Terminal upgrades**: Tab completion, `tree cp mv head wc grep df`, `net ping host fetch` | S | the terminal goes from toy to useful |
| 9 | **Hands-free tour now shows Code, Browser and Player** | S | `python build.py demo --auto` sells the whole OS with no typing |
| 10 | **Boot test fetches a page served by your PC** over BharatOS TCP | S | proof, not a claim: every `python build.py test` runs it |

## Done in the second round ✅

Details, tests and limits: [docs/phases/phase-11-quick-wins-2.md](docs/phases/phase-11-quick-wins-2.md).

| # | Quick win | Effort | What was built |
|---|---|---|---|
| 11 | ✅ **DHCP** instead of fixed addresses | S | discover, offer, request, ack at start-up; QEMU's fixed settings stay as the fallback; `net` says where the settings came from |
| 12 | ✅ **`why` command, first version** | S | `why slow` watches for half a second: CPU load, the busiest thread (and how to stop it), memory, file space; `why crash` explains the last crash in plain words |
| 13 | ✅ **Hindi/Marathi greeting and labels** on the login screen | M | नमस्ते · नमस्कार always shown; F1 switches the labels between English, हिन्दी and मराठी; phrases shaped at build time (Noto Sans Devanagari) |
| 14 | ✅ **Screenshot key** | S | Print Screen saves `/home/<you>/screenshot-N.ppm` and a notice says where |
| 15 | ✅ **Code: tabs and find (Ctrl+F)** | S | up to 4 files as tabs (Ctrl+T next, Ctrl+Q close, asks first if unsaved); find bar with highlighted matches |
| 16 | ✅ **Player: playlist of every .bvid in /media**, full-screen with F | S | N/P and Next, plays on at the end, broken videos skipped; a second clip (waves.bvid, drawn by `make_video.py`) |
| 17 | ✅ **Browser: bookmarks and history page** | S | Ctrl+D bookmarks (kept in `/home/<you>/bookmarks.txt`); Bookmarks and History buttons |
| 18 | ✅ **Files: copy/paste and rename** buttons | S | Copy/Paste (Ctrl+C/V, `name-copy.ext` when taken), Rename (`r`) for files and folders, on a new `fs_rename` |
| 19 | ✅ **Terminal: scrollback** (Page Up) and colours for `ls` | S | 200 lines of scrollback; ANSI colours; `ls` colours folders, scripts, videos and web pages |
| 20 | ✅ **Rust service** (a `no_std` "hello" service) and a Rust build step skipped cleanly when `rustc` is missing | M | linked into the kernel on its own thread behind a mailbox until user mode exists (decision D43); moves to user mode in Phase 13 |
| 21 | ✅ **Boot-time number on the splash** | S | "Ready: booted in 22.5 s" (from power-on, with every self-check; under QEMU without acceleration) on the splash and the login screen |

## Next quick wins (pick from the top)

| # | Quick win | Effort | Impact |
|---|---|---|---|
| 22 | **HTTPS** with a small TLS library (BearSSL: freestanding, MIT) | M+ | most of the web becomes reachable. Not started this round: it brings a large third-party library into the kernel (a budget and placement decision, since D39 moves networking to a service in Phase 19), and needs a trusted-certificate list and a good random-number source first |
| 23 | **Boot faster without the self-checks**: a "quick boot" that runs the checks after the desktop is up | S | the splash number drops from ~22 s to a few seconds |
| 24 | **`why` for memory and network** (`why full`, `why offline`) | S | more of the "explains itself" USP |
| 25 | **Hindi/Marathi for the desktop's top bar and dock labels** | S | more of "made for Bharat" |

## What we deliberately did *not* do

Running the real **VS Code**, **VLC** or **Chrome/Chromium** binaries is
not a quick win: each needs a full Linux or Windows environment (a C
library, dynamic linking, threads, sockets, a GPU stack, fonts, audio)
and tens of millions of lines of code. BharatOS has its own native
apps built on the same ideas instead: **Code**, **Player** and
**Browser**. The path to porting real open-source apps is in
[FINAL.md](FINAL.md) (user mode → a small C library → ports of
lightweight projects such as NetSurf and a minimal media decoder).
