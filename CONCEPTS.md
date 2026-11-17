# CONCEPTS.md: how an operating system works, in plain words

BharatOS is a whole operating system built from nothing: no Linux
underneath, no borrowed kernel. This page explains the big ideas behind
it, one at a time, with everyday comparisons. No background needed.

---

## The operating system

A computer is a pile of parts (a processor, memory, a disk, a screen, a
keyboard, a network card) that know nothing about each other. The
**operating system** is the manager that makes them work together and
shares them fairly between the programs you run.

> Think of a busy railway station. Trains (programs) want platforms
> (processors), time slots and tracks (memory). The station master (the
> operating system) decides who goes where and when, and makes sure two
> trains never use the same track at once.

The core of the operating system is the **kernel**: the part that runs
with full control of the machine. BharatOS's kernel is about 20,000 lines
of C and assembly, all written for this project.

## Booting

When you switch on the computer, a small program called the
**bootloader** (we use Limine) loads the kernel into memory and jumps to
it. The kernel then wakes up each part in turn: the screen, memory, the
clock, the other processors, the keyboard, the disk files, the network.

> Like opening a shop in the morning: lights on, till counted, doors
> unlocked, then the first customer comes in.

BharatOS checks itself while it does this: **622 self-checks** run on
every boot, and the Health app shows the result.

## Memory

Memory is one long row of numbered boxes (bytes). Programs need some of
it, give it back, and must never scribble on each other's boxes.

- **Pages**: memory is handed out in blocks of 4 KB, called pages.
- **The buddy allocator** splits big blocks in halves ("buddies") to
  give out the size asked for, and joins them back when they are free.
- **Page tables** give each program its own private map of memory, so
  a program sees "box 1000" but the kernel decides which real box that is.
- **Copy-on-write**: when two programs share the same data, they share
  one copy until one of them changes it; only then is a private copy made.

> Like a library: you borrow the shared copy of a book, and only if you
> want to write in it do you get your own photocopy.

## Threads and the scheduler

A **thread** is one line of work: "play the video", "draw the screen",
"answer the keyboard". There are more threads than processors, so the
**scheduler** gives each one a short turn (20 milliseconds), then the
next, so fast that everything seems to run at once.

BharatOS keeps one queue of waiting threads per processor. When a
processor runs out of work it **steals** a thread from a busy one, so no
processor sits idle while another is overloaded.

## Locks

When two processors change the same thing at the same moment, the result
can be garbage. A **lock** makes them take turns. BharatOS also has a
**lock-order checker**: if one part of the code takes lock A then B, and
another takes B then A, the two could wait for each other forever (a
"deadlock"). The checker spots this the first time it could happen and
reports it, before it ever freezes anything.

## Crashes that explain themselves

When something goes wrong in the kernel, BharatOS prints a **crash
report**: what happened, in which function, and the chain of functions
that led there (a "call trace"), plus the last things the kernel did. A
crash in a demo app is **contained**: that app stops, the rest keeps
running.

## Files

Files and folders live in memory (a "RAM disk", **ramfs**), filled at
every boot from a bundle on the disk: the start page, sample code, the
video. The Files app shows them with Places on the left and how full the
disk is. Later, **BharatFS** will keep files safely on a real disk, with
undo for every change.

## The desktop

The desktop draws everything into a hidden picture in memory first (a
**back buffer**), then copies only the part that changed to the screen.
That is why windows never flicker and moving the mouse costs almost
nothing.

Windows belong to **apps**: Terminal, Files, Code, Browser, Player,
Monitor, Health, About. Each app only draws its window and reacts to keys
and clicks; the **window manager** does the rest (stacking, dragging,
closing, the dock).

## The network

To fetch a web page, data passes through layers, each with one job:

| Layer | Job | Everyday comparison |
|---|---|---|
| **Network card driver** (e1000) | puts bits on the wire and takes them off | the postman's van |
| **Ethernet** | delivers to the next machine on the same network | the street address |
| **ARP** | finds which machine owns an IP address | asking the neighbours "who lives at number 2?" |
| **IP** | addresses any machine in the world | the city and PIN code |
| **ICMP (ping)** | "are you there?" "yes!" | ringing the doorbell |
| **UDP** | one quick message, no promise it arrives | a postcard |
| **DNS** | turns `example.com` into an address | the phone book |
| **TCP** | a reliable conversation: numbers every byte, resends anything lost, puts it all back in order | a registered letter with a receipt |
| **HTTP** | "please send me this page" | the letter's contents |

BharatOS has **every one of these layers written from scratch**. The
`ping`, `host` and `fetch` commands and the Browser use them. Secure
(https) pages also need encryption (**TLS**), which comes next.

## Video

A video is a fast series of pictures. BharatOS's Player uses a simple
format of its own (**.bvid**): one shared table of 256 colours, then each
frame as one colour number per pixel. Playing it needs no complicated
decoder: the clock says which frame should be on screen ("time since
Play × 12 frames a second"), and the Player draws it three times bigger.

## Self-checks and tests

Every feature comes with checks that run at boot. On top of that,
`python build.py test` boots BharatOS in an emulator, types commands at
the real login, logs in to the desktop with the emulated keyboard, runs
a tour of every app, fetches a web page from the PC over BharatOS's own
TCP, and checks a screenshot. If anything is off, the test fails.

## What's next

**User mode** (programs that cannot crash the system), **HTTPS**, files
that survive power cuts with **undo**, apps that **ask first** before
touching your files, a `why` command that explains slowdowns in plain
words, and **Hindi and Marathi** on screen. The plan is in
[FINAL.md](FINAL.md).
