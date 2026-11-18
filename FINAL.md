# FINAL.md: how BharatOS gets finished, fast

This is the finishing plan: what "done" means, the order to build it in,
and the rules that keep it quick. It sits on top of
[docs/UPDATES.md](docs/UPDATES.md) (the detailed phase plan) and
[QUICK_WINS.md](QUICK_WINS.md) (small things with big impact). Where they
disagree on **order**, this file wins; for **designs**, UPDATES wins.

## 1. Where we are now

- Phases 1–11 done: boot, memory, threads, multi-core scheduler,
  debugging tools, input, files in memory, login, terminal, desktop.
- This round added: **2 GB RAM**, a **network stack written from
  scratch** (e1000 driver, ARP, IPv4, ICMP, UDP, DNS, TCP, HTTP), and
  three apps: **Browser**, **Player** (video) and **Code**; Files with
  Places and a disk bar; a terminal with Tab completion and 11 new
  commands; then a second round added DHCP, `why slow`/`why crash`, a
  Hindi and Marathi login and the first Rust service. **707 self-checks
  pass on every boot**, and the boot test
  fetches a real web page from the PC over BharatOS's own TCP.

## 2. What "finished" means

BharatOS is finished when a stranger can do all of this in 5 minutes,
on QEMU and on one real laptop, without a crash:

1. boot in under 3 seconds, log in with their name;
2. browse files, open a video, a web page and a code file in the right app;
3. browse an **https** site;
4. run a program they wrote, in **user mode**, that cannot crash the system;
5. ask `why slow` / `why crash` and get a plain answer;
6. power-cut during a save and lose nothing (**undo for files**);
7. see an app **ask first** before it reads their files;
8. read the login screen in **Hindi or Marathi**.

Every item maps to a USP; nothing else is needed to call it done.

## 3. The ideas we weighed

| Idea | Verdict | Why |
|---|---|---|
| Run real VS Code, VLC, Chrome | **No** | each needs Linux/Windows APIs, dynamic linking, a GPU stack and millions of lines; months of porting for a demo we already have natively |
| Native Code, Player, Browser | **Yes (done)** | same experience in a few thousand lines we understand and test |
| Port small open-source apps after user mode (NetSurf browser, a tiny C library, an MJPEG/MPEG-1 decoder, Lua) | **Yes, later** | real open-source apps, small enough to port in days once there is a C library |
| HTTPS with BearSSL | **Yes** | freestanding, MIT licence, small; turns the browser from a demo into a tool |
| Rust user services (`no_std`) | **Yes, at user mode** | decision D4; a "hello" service first, then the `why` service |
| Our own GPU driver | **No** | the framebuffer is fast enough for 2D; no demo needs 3D |
| Sound | **Maybe** | HDA/AC97 driver in QEMU is ~1 week; nice for the Player, not a USP |
| SMP-safe net stack with interrupts | **Later** | polling is simple and fast enough; move to interrupts with user-mode drivers |

## 4. The order (fastest path to "finished")

Each block ends with `python build.py test` green and a merged PR.
Days are focused working days.

| # | Block | Days | Delivers | Unblocks |
|---|---|---|---|---|
| A | **V1 hardening** (old Phase 12): fuzz the shell and HTML parser, screenshot tests for each app, real-laptop boot | 3 | a V1 you can record | the demo video |
| B | **User mode + handles + rings** (Phase 13) | 8 | programs in ring 3; a crash kills only that program | everything below |
| C | **Tiny C library + `run` of user programs** | 3 | `hello.c` built on the PC, run inside BharatOS | ports, Rust services |
| D | **Rust `no_std` service + build step** | 2 | a Rust service answering over a ring | D4 proven |
| E | **`why` service** (Phase 15 core) | 3 | `why slow`, `why crash` in plain words | USP 2 |
| F | **HTTPS (BearSSL) + DHCP** | 4 | real web browsing on any network | USP demo |
| G | **BharatFS: files that survive power cuts, with undo** (Phase 17) | 8 | the "undo for files" USP | USP 3 |
| H | **Permissions: apps ask first** (Phase 18) | 4 | the "apps ask first" USP | USP 4 |
| I | **Made for Bharat**: Devanagari text, Hindi/Marathi UI, 128 MB mode | 5 | USP 5 | — |
| J | **Ports**: NetSurf (browser), a small video decoder, Lua | 6 | real open-source apps | — |

**Total: about 46 working days.** A–F alone (23 days) give a demo that
covers browsing, programs, Rust and explanations.

## 5. Rules that keep it blazing fast

1. **One branch per block, one PR, merged the same week.** No long-lived branches.
2. **Simplest design that works** (UPDATES §10). Polling before
   interrupts, one lock before many, a fixed table before a dynamic one.
   Upgrade only when a test or a number says so.
3. **Tests first-class, not last**: every block adds headless self-checks;
   the boot test stays under 90 seconds.
4. **Reuse before writing**: BearSSL, NetSurf, Lua and Limine are
   reused; the kernel itself stays ours.
5. **Measure the hot paths** (context switch, page fault, packet in → out,
   frame drawn) and record them in `docs/benchmarks.md` when touched.
6. **CI stays one quick job.** Long runs only on request.
7. **Cut, don't slip**: if a block runs over by 50%, cut its last feature
   into QUICK_WINS.md and move on.

## 6. Performance targets

| What | Target | Today |
|---|---|---|
| Boot to login (QEMU, TCG) | < 3 s on KVM, < 30 s on TCG | about 26 s on TCG with all 622 checks |
| Ping the gateway | < 1 ms on KVM | ~40 ms on TCG (emulated) |
| Video | 12 fps at 576×324 with no dropped frames | 12 fps, frames chosen by the clock |
| Desktop redraw | only the changed rectangle | yes (D36) |
| Context switch | < 300 cycles | see docs/benchmarks.md |

## 7. Risks and how we shrink them

| Risk | Plan |
|---|---|
| User mode takes longer than 8 days | keep V1 apps in the kernel behind their narrow APIs; move one app at a time |
| TLS is hard to get right | use BearSSL unchanged; test against a local HTTPS server in the boot test |
| Real hardware differs from QEMU | test one laptop early (block A); keep e1000 + a fallback "no network" path |
| Scope creep | anything not in §2 goes to QUICK_WINS.md, not into a block |
