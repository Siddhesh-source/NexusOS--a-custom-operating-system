# Quick wins: low effort, high impact

The things that make BharatOS look and feel like a real OS fastest, for
the least work. Each one is small (hours to a couple of days), shows up
in a demo, and backs one of the USPs (selling points) in
[docs/PROJECT_OVERVIEW.md](docs/PROJECT_OVERVIEW.md).

**Effort:** S = under a day, M = 1–3 days. **Impact:** what a viewer
notices. ✅ = done in this round.

## Done in this round ✅

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

## Next quick wins (pick from the top)

| # | Quick win | Effort | Impact |
|---|---|---|---|
| 11 | **DHCP** instead of fixed addresses | S | works on any network, not just QEMU's |
| 12 | **`why` command, first version**: plain-English answers from live data (`why slow`: busiest thread, memory, CPU load) | S | the "explains itself" USP, live |
| 13 | **Hindi/Marathi greeting and labels** on the login screen (pre-rendered Devanagari glyphs) | M | "made for Bharat", visible in the first second |
| 14 | **Screenshot key** (Print Screen saves a .ppm into /home) | S | users share what they see |
| 15 | **Code: tabs and find (Ctrl+F)** | S | feels like a real editor |
| 16 | **Player: playlist of every .bvid in /media**, full-screen with F | S | a media app, not one clip |
| 17 | **Browser: bookmarks and history page** | S | a browser people can live in |
| 18 | **Files: copy/paste and rename** buttons | S | completes the basic workflows |
| 19 | **Terminal: scrollback** (Page Up) and colours for `ls` | S | the terminal feels modern |
| 20 | **Rust user-space service** (a `no_std` "hello" service) once user mode lands, and a Rust build step that is skipped cleanly when `rustc` is missing | M | proves decision D4 (services may be Rust) |
| 21 | **Boot-time number on the splash** ("booted in 1.4 s") | S | speed you can see |
| 22 | **HTTPS** with a small TLS library (BearSSL: freestanding, MIT) | M+ | most of the web becomes reachable |

## What we deliberately did *not* do

Running the real **VS Code**, **VLC** or **Chrome/Chromium** binaries is
not a quick win: each needs a full Linux or Windows environment (a C
library, dynamic linking, threads, sockets, a GPU stack, fonts, audio)
and tens of millions of lines of code. BharatOS has its own native
apps built on the same ideas instead: **Code**, **Player** and
**Browser**. The path to porting real open-source apps is in
[FINAL.md](FINAL.md) (user mode → a small C library → ports of
lightweight projects such as NetSurf and a minimal media decoder).
