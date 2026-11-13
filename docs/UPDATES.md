# BharatOS: Updates to the Plan (read this before ROADMAP, HLD and LLD)

This file records the decisions made after the first plan was written.
**Where this file and `ROADMAP.md`, `design/HLD.md` or `design/LLD.md`
disagree, this file wins.** Those documents are still useful for the
user-facing features (USPs), the file-system design, the UI and the
details of finished phases, but the architecture and phase order have
changed as described here.

Contents:
1. What changed and why
2. Answering "why not Linux or Windows?"
3. The core idea
4. What makes BharatOS stand out (user-facing and engineering)
5. The new architecture
6. Upgrades that make it a serious OS
7. New phase plan (Phases 6–20)
8. Key designs (low-level detail for the new parts)
9. How we build and test
10. Decision log
11. Interview kit
12. Future use cases
13. What we are not doing
14. Old plan to new plan mapping

---

## 1. What changed and why

The first plan was a good teaching OS with friendly features. It had
two weaknesses:

- **It was not defensible.** Undo, permissions and language packs already
  exist in Linux, Windows and Android. "Why use this?" had no strong
  answer.
- **It used teaching-kernel shortcuts.** It relied on old interrupt and
  timer chips, one CPU only, no hardware discovery, a simple page
  allocator, no CPU security features, and no call traces in crash
  reports.

We have the freedom to make large, expensive changes. So the plan now:

1. Keeps BharatOS a **general-purpose OS** that can be specialised later
   (see §12), rather than targeting one niche today.
2. Builds around **one core idea** (§3) that makes hard features natural.
3. Upgrades the foundations to the level of a **serious, modern kernel**
   (§6).
4. Changes the architecture to a **microkernel with async shared rings
   and handles** (§5).
5. Adds **standout engineering features** that show depth in an SDE
   interview: concurrency, distributed systems, storage, networking,
   performance, testing and security (§4.2, §11).

The five user-facing USPs from `PROJECT_OVERVIEW.md` stay. They now sit
on top of a much stronger engine.

---

## 2. Answering "why not Linux or Windows?"

Be honest first: BharatOS will never beat Linux or Windows on drivers,
apps or hardware support, and it doesn't try to.

What it offers instead is the payoff of starting clean:

- **The kernel knows every piece of state and every change to it.**
  Linux and Windows carry 30 years of scattered state (global file
  paths, process IDs, signals, driver buffers). That is why they can't
  easily:
  - resume a program exactly after a power cut,
  - replay a crash step by step,
  - move a running program to another machine,
  - undo a system-wide change, or
  - say exactly what a program touched.

  BharatOS is designed so all of these are natural.
- **Small enough to verify.** The kernel stays small (a lines-of-code
  budget enforced in CI). Its key protocols are written as machine-checked
  TLA+ specifications (a precise model of a design that a tool checks for
  errors) and tested against millions of simulated failures. Big systems
  can't be checked this way.
- **Safe by design, not by lockdown.** Programs can only use what they
  were handed (handles). Drivers and the file system are restartable
  programs, not kernel code. A crashing driver does not take the OS down.

**One-sentence answer:** *"BharatOS is a multi-core microkernel where all
I/O goes through async shared rings and all access goes through handles.
Because the kernel knows all of a program's state, it can resume programs
after a power cut, replay any crash and even move a running program to
another machine. The file system's commit protocol is machine-checked and
tested against a million simulated power cuts."*

---

## 3. The core idea

> **Every resource is a kernel-tracked object, reached only through a
> handle, and every change goes through one recorded path (the rings).**

What follows from that one rule:

| Because... | ...we get |
|---|---|
| all of a program's state lives in kernel objects it holds handles to | save/resume after power cut, live migration to another machine |
| all input to a program arrives through ring completions | record and replay of any run |
| all access is through handles given explicitly | permissions with no way around them, "what did this app touch?" |
| all changes to files go through one commit path | crash safety, undo, system-wide transactions |
| drivers and services are ordinary programs holding handles | restart on crash without rebooting |

Most of these ideas have earlier research or industry work behind them
(cited in §4.2). What is new is having all of them in one small,
verified kernel, which an old OS can't add afterwards.

---

## 4. What makes BharatOS stand out

### 4.1 User-facing USPs (unchanged, now built on the core idea)

| USP | Built on |
|---|---|
| 1. **Checks itself every boot**: self-checks, safe mode, verified boot with automatic rollback | existing self-tests + verified A/B boot (Phase 15) |
| 2. **Explains itself**: `why slow`, `why crash`, plain sentences, offline | event diary + per-program budgets and receipts (Phase 12) |
| 3. **Undo for your files, safe from power cuts**: and now **resume your whole session** after a power cut | copy-on-write BharatFS (Phase 14) + checkpoint/resume (Phase 17) |
| 4. **Apps ask first**: no access by default, every use logged | handles + permission service (Phases 10, 15) |
| 5. **Made for Bharat**: 128 MB RAM, offline, Hindi and Marathi | memory compression, language packs, real low-end hardware (Phase 19) |

### 4.2 Engineering features (the interview-grade depth)

| Area | Feature | Earlier work to cite | Phase |
|---|---|---|---|
| Architecture | **Microkernel**: drivers, file system, network as restartable user programs | QNX, MINIX 3, seL4, Fuchsia | 11, 13 |
| Performance | **Async shared rings** for all system calls and IPC; zero-copy shared memory | Linux io_uring | 10, 11 |
| Security | **Handles only** (capabilities): no global names, no ambient access | seL4, Fuchsia, Capsicum | 10 |
| Concurrency | **Multi-core** with spinlocks, **lock-order checker**, per-CPU run queues, **RCU** (readers never wait for a lock; writers publish a new copy) | Linux lockdep, Linux RCU | 9 |
| Storage | **Crash-safe copy-on-write file system**, versions, **system-wide transactions**, commit protocol written in **TLA+** and model-checked | ZFS, btrfs, TxOS, NixOS rollbacks | 14 |
| Testing | **Deterministic simulation testing**: kernel code run on the PC with fake hardware, millions of injected failures, any bug reproducible from a seed | FoundationDB, TigerBeetle | 6, 14 |
| Testing | **System-call fuzzing** in CI | Google syzkaller | 10 |
| Networking | **Own TCP/IP stack in Rust** as a user-space service, benchmarked against Linux | lwIP, smoltcp | 16 |
| Reliability | **Checkpoint and resume after power cut** | EROS/KeyKOS, CRIU | 17 |
| Debugging | **Record and replay**: rerun any crash exactly | Mozilla rr | 17 |
| Distributed | **Live process migration** between two BharatOS machines | Sprite, MOSIX, VM live migration | 17 |
| Distributed | **Replicated folders using Raft** (keeps 3 machines in agreement; survives 1 failure) | Raft, etcd | 20 |
| Observability | **Safe tracing programs**: small bytecode programs a **verifier** proves safe, run on kernel events | Linux eBPF | 20 |
| Apps | **WebAssembly apps** with WASI-style handles: any language, any CPU, sandboxed by design | WASI, wasm3 | 18 |
| Language safety | **Rust for new user-space services** (no standard library); C kernel core | Rust in Linux/Android/Windows | 11+ |
| Portability | **Second CPU architecture** (RISC-V) behind `arch/` | — | later |

### 4.3 Supporting features (from the brainstorm, smaller)

- **Budgets and receipts:** each program has CPU, memory, disk-write and
  network budgets, on by default. The OS keeps a receipt of use, and this
  powers `why`.
- **Restartable services** with automatic reconnect.
- **Always-on self-checks:** the kernel checks its own invariants (heap
  links, run queues, page tables) when the CPU is idle.
- **Typed messages:** message formats are declared in a small definition
  file, and stubs for C and Rust are generated from it.
- **File provenance:** every file records which program made it, and from
  what.
- **Compressed memory** for low-RAM machines (zram-style).
- **Focus-aware scheduling:** the program you are looking at gets
  guaranteed response time.
- **Hard budgets in CI:** kernel size, boot time, idle memory and input
  delay are tested numbers.

---

## 5. The new architecture

```text
┌──────────────────────────────────────────────────────────────────────────┐
│ APPS (ring 3)  WebAssembly apps · native apps · bsh shell · desktop apps  │
├──────────────────────────────────────────────────────────────────────────┤
│ SERVICES (ring 3, restartable, mostly Rust)                              │
│  service manager (init, names, restarts) · fs (BharatFS) · net (TCP/IP)  │
│  drivers: virtio-blk/net/input, NVMe, xHCI+HID, PS/2, framebuffer        │
│  desktop/compositor · permission service · explain/why · migration       │
├──────────────────────────────────────────────────────────────────────────┤
│ USER RUNTIMES  C library (bharat.h) · Rust runtime (no_std) · Wasm runtime│
╞════ async shared rings (submit / complete) + handles ═════════════════════╡
│ MICROKERNEL (ring 0, C)                                                  │
│  objects + handle tables · IPC channels + shared memory · rings          │
│  threads + scheduler (SMP, per-CPU queues) · memory (buddy, slab, paging) │
│  interrupts → handles for user drivers · timers · checkpoint/replay hooks│
│  event diary · tracing + verifier · self-checks                          │
│  arch/x86_64: APIC/IOAPIC, ACPI, PCI scan, SMP bring-up, TSC, SMEP/SMAP  │
├──────────────────────────────────────────────────────────────────────────┤
│ Limine (BIOS/UEFI) · verified A/B system images                          │
└──────────────────────────────────────────────────────────────────────────┘
```

**What stays in the kernel:** threads and scheduling, memory, objects and
handles, IPC, rings, interrupt delivery, timers, the PCI/ACPI discovery
needed to hand devices to drivers, the serial debug console, tracing and
self-checks.

**What moves out to services:** every device driver except serial and
timer, the file system, networking, the desktop, permission prompts and
`why`.

**Changes from the old HLD:**
- "Monolithic kernel with drivers inside" is replaced by "microkernel
  plus restartable services".
- `int 0x80` system calls are replaced by rings (a few synchronous calls
  remain for setup).
- The `caps` list is replaced by handle tables.
- Phase 9's in-kernel ATA driver is replaced by virtio-blk as a user
  service (ATA PIO only as a fallback).

---

## 6. Upgrades that make it a serious OS

| Area | Today | Upgrade | Phase |
|---|---|---|---|
| Interrupts | 8259 PIC | Local APIC + IOAPIC | 7 |
| Time | PIT, fixed 100 Hz | TSC (calibrated) + APIC timer, **tickless** (timer set only for the next event); HPET fallback | 7 |
| Discovery | none | ACPI tables (CPUs, interrupt routing), PCI/PCIe scan | 7 |
| Console | serial only | framebuffer console early in boot (serial kept for tests) | 7 |
| CPUs | 1 | SMP bring-up (INIT/SIPI), per-CPU data and TSS | 9 |
| Locking | `IRQ_GUARD()` (interrupts off) | spinlocks + interrupts off, lock-order checker, RCU | 9 |
| Physical memory | bitmap, linear scan | **buddy allocator** | 8 |
| Kernel objects | first-fit heap | **slab allocator** for fixed-size objects; heap kept for odd sizes | 8 |
| Virtual memory | mapped up front | **demand paging**, **copy-on-write**, memory-mapped files, page cache | 8, 14 |
| TLB | single CPU flush | **TLB shootdown** across CPUs | 9 |
| Low memory | fail | reclaim cache, compressed memory, clear out-of-memory policy | 14, 19 |
| CPU security | no-execute only | **SMEP/SMAP**, stack protector on, **KASLR**, randomised user layout, basic Spectre mitigations | 6, 10, 15 |
| Debugging | register dump | **call traces with symbols**, `build.py debug` (QEMU + GDB), heap poisoning (KASAN-lite), event tracing, crash dumps | 6 |
| Drivers | serial, PIT | driver model; virtio (blk, net, input, gpu); NVMe; xHCI + USB HID | 13, 19 |
| Process | unit tests at boot only | CI, host-side unit tests, simulation testing, fuzzing, benchmarks, real hardware | 6 onward |

---

## 7. New phase plan (Phases 6–20)

Rules from `ROADMAP.md` still apply to every phase: small steps, tests at
boot, bad-input tests, a one-minute demo, and docs. **New rules:**
- Every phase that touches performance adds to the benchmark table
  (`python build.py bench`).
- Every hard bug goes into the bug journal (§11.3).
- Every big choice gets a line in the decision log (§10).

| Phase | Name | Main USP / showcase | Size |
|---|---|---|---|
| 6 | Debug and hardening foundation | engineering quality | M |
| 7 | Modern hardware | serious OS | L |
| 8 | Memory that scales | serious OS | L |
| 9 | Multi-core | concurrency | L |
| 10 | User mode, handles and async rings | security, performance | L |
| 11 | Microkernel IPC and services | architecture | L |
| 12 | Shell, self-check and explain | USP 1, 2 | M |
| 13 | Drivers as services | architecture | L |
| 14 | BharatFS: crash-safe, versions, transactions | USP 3, storage, testing | XL |
| 15 | Permissions and verified boot | USP 1, 4, security | M |
| 16 | Networking (Rust TCP/IP) | networking | L |
| 17 | Resume, replay and live migration | USP 3, distributed, debugging | XL |
| 18 | Desktop and WebAssembly apps | all USPs visible | L |
| 19 | Made for Bharat and real hardware | USP 5 | L |
| 20 | Standout extras: Raft folders, safe tracing programs | distributed, observability | L |
| later | RISC-V port, self-hosting | portability | XL |

### Phase 6: Debug and hardening foundation
**Goal:** make every later phase easier to debug and harder to break.
1. **CI (swift):** one GitHub Actions job on Ubuntu with a cached
   toolchain: build + one BIOS boot test (`build.py test`), short timeout.
2. **Call traces in panics:** walk saved frame pointers (build with
   `-fno-omit-frame-pointer`). Embed a symbol table generated from the ELF
   by a Python step, and print `function+offset`.
3. **`python build.py debug`:** QEMU with `-s -S` plus a ready GDB script.
4. **Stack protector on:** remove `-fno-stack-protector`, add
   `__stack_chk_guard` and `__stack_chk_fail` (panic with a call trace).
5. **Heap poisoning (KASAN-lite):** fill freed blocks with a pattern;
   check it on reuse; add red zones around allocations.
6. **General tracing:** turn the scheduler trace into one fixed-size
   buffer of timestamped events for any subsystem; `trace_dump()`.
7. **Host-side unit tests:** `python build.py unit` compiles allocator and
   scheduler-queue code with the host compiler against small fakes, and
   runs thousands of cases.
8. **Benchmark harness:** `python build.py bench` boots a bench build and
   records context-switch time and allocator speed (TSC cycles) into
   `docs/benchmarks.md`.
9. **Budgets in CI:** kernel line count and boot time are recorded and
   checked against limits.

**Done when:** CI is green; a forced panic prints a correct symbolised
call trace; a stack-smash fault demo is caught by the stack protector; a
use-after-free test is caught by poisoning; the unit and bench commands
work.
**Demo:** a crash with a full symbolised call trace; GDB stepping through
`context_switch`.

### Phase 7: Modern hardware
1. Parse ACPI tables via Limine's RSDP: the MADT (CPU and interrupt
   controller list) and HPET.
2. Local APIC on; legacy PIC masked; IOAPIC routing for device IRQs.
3. TSC calibrated against HPET/PIT; APIC timer one-shot. **Tickless**
   scheduler: the timer is set to the earliest of the slice end and the
   next sleeper wake-up.
4. PCI/PCIe scan (config space via ECAM when available), device table
   printed at boot.
5. Framebuffer console (text on screen) next to serial.

**Done when:** the timer runs via the APIC with the PIC fully masked; all
existing tests pass; idle produces no periodic ticks (counted); sleep
accuracy is within 1 ms; the PCI list matches QEMU's `info pci`.
**Demo:** boot on screen, PCI device list, `idle: 0 ticks in 5 s`.

### Phase 8: Memory that scales
1. Buddy allocator replaces the bitmap (keep the same `pmm_` API).
2. Slab caches for threads, processes, handles and messages.
3. Demand paging: user memory regions are reserved, and pages are
   allocated on first touch.
4. Copy-on-write mappings (needed by checkpointing and the file system).
5. Memory regions as objects (prepares for handles).

**Done when:** all 114 memory checks are ported and pass; new tests cover
buddy split/merge, slab exhaustion, fault-in and copy-on-write; host-side
unit tests run 1M random alloc/free sequences; bench shows the speedup.

### Phase 9: Multi-core
1. Wake the other cores (INIT/SIPI) using the trampoline from Limine's
   SMP feature.
2. Per-CPU data (`current` thread, run queue, TSS, GDT) via `GS` base.
3. `IRQ_GUARD()` becomes `spin_lock_irqsave()` on named locks.
4. **Lock-order checker:** every lock has a class; the first time class A
   is taken while holding B, record "B before A"; the reverse order later
   panics with both call traces.
5. Per-CPU run queues with work stealing; load balancing.
6. TLB shootdown through inter-processor interrupts.
7. **RCU** for read-mostly tables (process lookup, handle lookup).

**Done when:** tests pass on `-smp 1, 2, 4`; a deliberate lock inversion
is caught; 1M-operation stress on 4 CPUs finds no lost updates; context
switches scale per CPU (bench).
**Demo:** a 4-CPU live dashboard; the lock checker catching an inversion.

### Phase 10: User mode, handles and async rings
1. GDT user segments (order from LLD Phase 6), `TSS.rsp0`, FPU save
   (`fxsave`/`xsave`), ring-3 entry.
2. **SMEP/SMAP on**; `copy_from_user`/`copy_to_user` use `stac`/`clac`.
3. **Kernel objects and handle tables** (§8.1).
4. **Rings** (§8.2): a small set of synchronous calls (`exit`,
   `ring_setup`, `ring_enter`, `debug_write`, `handle_close`,
   `handle_dup`); everything else is a ring operation.
5. User faults kill only the process; the plain report from the old plan
   is kept.
6. **System-call and ring fuzzer** (random and malformed entries) in CI.
7. Embedded user test programs (as in the old Phase 6).

**Done when:** all old Phase 6 isolation tests pass; SMAP catches a direct
kernel read of user memory (fault demo); the fuzzer runs 1M operations with
no kernel crash; a bench compares a ring batch of 64 operations against 64
single calls.
**Demo:** two isolated user programs doing batched I/O; a crash is
contained.

### Phase 11: Microkernel IPC and services
1. Channels: messages of up to 4 KiB plus up to 4 handles; zero-copy
   shared memory objects (§8.3).
2. Message definition files (`*.idl`) and a Python generator for C and
   Rust stubs.
3. Startup bundle (tar), static ELF loader, `spawn` with an explicit list
   of handles for the child.
4. **Service manager** (first user program): starts services from a
   manifest, registers names, restarts crashed services (max 3/min);
   clients reconnect by name.
5. User runtimes: the C library `bharat.h`, and a Rust `no_std` runtime
   crate. `build.py` gains Rust builds (`x86_64-unknown-none`).
6. First services: console service, PS/2 keyboard service (via an
   interrupt handle), and test services.

**Done when:** killing the keyboard service mid-typing restarts it and
typing continues; malformed messages are rejected; IPC round-trip time is
benchmarked; the Rust service and C client talk through generated stubs.
**Demo:** kill a driver; the OS keeps running; it comes back.

### Phase 12: Shell, self-check and explain (USP 1, 2)
Old Phase 7 and old Phase 8 combined, on top of services:
1. `bsh` shell: `help`, `ls`, `run`, `ps`, `mem`, `kill`, `uptime`,
   `health`, `why`, `trace`.
2. Event diary in the kernel (fixed ring), read through a handle.
3. **Budgets and receipts** per process (CPU, memory, disk writes,
   network); enforced by the scheduler, memory and services.
4. `why` rules (in user space; design in old LLD Phase 8).
5. Self-check results stored; safe mode if a core check fails; continuous
   idle-time invariant checks.
6. Focus-aware scheduling: foreground gets a guaranteed share.

**Done when:** the old Phase 7 and 8 "done when" lists pass; going over a
budget is reported and enforced.

### Phase 13: Drivers as services
1. Driver model: the kernel hands each driver service a device handle
   (PCI config access), memory-mapped I/O region handles, an interrupt
   handle, and DMA memory handles with known physical addresses.
2. virtio-blk, virtio-net, virtio-input, virtio-gpu (2D) drivers in Rust.
3. ATA PIO kept only as a fallback.
4. Known limit: no IOMMU yet, so driver services are trusted for DMA
   (documented; IOMMU later).

**Done when:** read/write throughput is benchmarked; killing the disk
driver mid-I/O makes the request fail cleanly or retry after restart,
with no corruption.

### Phase 14: BharatFS, crash-safe, versions and transactions (USP 3)
1. BharatFS as a Rust service (on-disk design from old LLD Phase 9:
   copy-on-write, two root slots, checksums).
2. Versions: `history`, `undo`, `rewind` (old plan).
3. **System-wide transactions:** `tx_begin`, a group of changes across
   files, `tx_commit` → one root switch. "Undo the last install."
4. Page cache in the kernel shared with memory-mapped files; low-memory
   reclaim.
5. **File provenance**: creator program and source recorded per file.
6. **TLA+ spec** of the commit protocol (`specs/fs_commit.tla`),
   model-checked by TLC.
7. **Simulation testing:** the FS code is built on the PC against a fake
   disk that can lose writes, reorder them, flip bits, or cut power at any
   point; 1M seeded runs in CI (fewer per push, full nightly).

**Done when:** the old Phase 9 tests pass, plus the TLC check passes, 1M
simulated power cuts produce zero inconsistencies, and a transaction is
all-or-nothing under a power cut.
**Demo:** kill the VM mid-save; reboot; last good version; undo an install.

### Phase 15: Permissions and verified boot (USP 1, 4)
1. Permission service: app manifests, ask-first prompts, saved answers.
   Handles are only given out after approval (old Phase 10, using handles
   in place of the `caps` list).
2. Access log through the diary: `why access <app>`.
3. **Signed system images**, **A/B slots**: boot the new slot; if
   self-checks fail, mark it bad and boot the old one automatically.
4. **KASLR:** build the kernel position-independent so Limine can place
   it at a random address.

**Done when:** the old Phase 10 tests pass; a tampered image is refused; a
failing update rolls back automatically.

### Phase 16: Networking
1. TCP/IP stack in Rust as a service: Ethernet, ARP, IPv4, ICMP, UDP,
   DHCP, DNS, TCP (connection handshake, retransmission, flow control,
   basic congestion control).
2. Sockets are handles; a `network` permission is required.
3. Test against Linux in QEMU (user-mode networking and tap); packet
   loss injected in tests.

**Done when:** an HTTP GET works from BharatOS; TCP survives 5% packet
loss; throughput and latency are benchmarked against Linux on the same VM.

### Phase 17: Resume, replay and live migration (USP 3, showcase)
1. **Checkpoint:** every few seconds, save each program's changed pages
   (via copy-on-write), thread registers and handle table to BharatFS.
   Device handles are re-acquired by name on restore.
2. **Resume after power cut:** at boot, offer to restore the last session.
3. **Record and replay:** log every ring completion and time reading for a
   program; replay feeds the same log back. Single-threaded programs
   first; multi-threaded programs later (needs the recorded switch
   order).
4. **Live migration:** the migration service sends a checkpoint over TCP
   to another BharatOS machine and the program resumes there. Files are
   pre-copied (with Raft folders later); console handles are re-bound.

**Done when:** a counter program survives a power cut and continues; a
recorded crash replays to the same instruction 100/100 times; a running
program moves between two QEMU machines and keeps its state.
**Demo:** move a running game or counter from machine A to machine B.

### Phase 18: Desktop and WebAssembly apps
1. Desktop/compositor service on virtio-gpu or the framebuffer (look and
   screens from `design/UI.md`).
2. Built-in apps: Home, Terminal, Files (History/Undo), System (Why?),
   Permissions, Settings.
3. **WebAssembly runtime** (wasm3 or similar, small C) as a service. Apps
   get WASI-style handles only, and run unchanged on any CPU.
4. Screenshot tests (old Phase 11).

### Phase 19: Made for Bharat and real hardware (USP 5)
1. Language packs and build-time Devanagari rendering (old Phase 12).
2. Compressed memory; boot and run within 128 MB (CI test).
3. **NVMe** and **xHCI + USB HID** drivers (needed on modern real PCs).
4. Boot on at least one real BIOS PC and one UEFI PC; documented with
   photos.

### Phase 20: Standout extras
1. **Raft-replicated folders:** a folder kept identical across 3
   machines; survives one machine failing; leader election and log
   replication tested in simulation with network partitions.
2. **Safe tracing programs:** a small bytecode, a **verifier** (bounded
   loops, checked memory access, limited helpers), attached to trace
   points, with output to rings.

### Later
- **RISC-V port** on QEMU `virt`: proves `arch/` separation.
- **Self-hosting:** compile a program on BharatOS.
- **IOMMU** for driver isolation from DMA.

---

## 8. Key designs (low-level detail for the new parts)

These replace the old LLD wherever they overlap: system calls, `caps`,
and IPC.

### 8.1 Objects and handles

```c
enum obj_type { OBJ_PROCESS, OBJ_THREAD, OBJ_MEMORY, OBJ_CHANNEL,
                OBJ_RING, OBJ_INTERRUPT, OBJ_DEVICE, OBJ_MMIO,
                OBJ_TIMER, OBJ_EVENT_DIARY, OBJ_TRACE };

struct kobject {                 /* first field of every kernel object */
    uint32_t magic;
    uint16_t type;               /* enum obj_type */
    uint32_t refs;               /* freed when 0 */
    uint64_t id;                 /* unique for the life of the system */
    const struct kobj_ops *ops;  /* close, checkpoint, restore */
};

enum rights { R_READ = 1, R_WRITE = 2, R_MAP = 4, R_DUP = 8,
              R_TRANSFER = 16, R_WAIT = 32, R_MANAGE = 64 };

struct handle { struct kobject *obj; uint32_t rights; uint32_t gen; };
/* Per process: table of 256 handles (grows later). A handle value is
 * (index | gen << 16), so a stale handle value is detected, never reused
 * by accident. */
int handle_get(struct process *p, uint32_t h, uint16_t type,
               uint32_t need_rights, struct kobject **out);
```

Rules:
- Rights can only be reduced when duplicating or transferring a handle.
- A child process gets exactly the handles listed in `spawn`.
- No object is reachable any other way. There are no global names in
  the kernel; names live in the service manager.

### 8.2 Async rings

```c
struct ring_sqe {                /* submission entry, 64 bytes */
    uint8_t  op;                 /* RING_OP_* */
    uint8_t  flags;              /* LINK: run after previous succeeds */
    uint16_t _pad;
    uint32_t handle;
    uint64_t arg0, arg1, arg2, arg3;
    uint64_t user_data;          /* returned untouched in the completion */
    uint64_t _rsvd[2];
};
struct ring_cqe {                /* completion entry, 32 bytes */
    uint64_t user_data;
    int64_t  result;             /* >= 0 ok, < 0 = -E_* */
    uint64_t extra;
    uint64_t _rsvd;
};
/* One shared memory object holds: header {sq_head, sq_tail, cq_head,
 * cq_tail, sizes}, SQ entries[256], CQ entries[512].
 * The program writes SQ entries and moves sq_tail; the kernel consumes
 * them in ring_enter() and posts CQ entries. Head/tail use acquire/release
 * ordering. The kernel copies each SQE before validating it (so the
 * program can't change it mid-check). */
int64_t sys_ring_enter(uint32_t ring_h, uint32_t to_submit,
                       uint32_t min_complete, uint64_t timeout_ns);
```

Operations: `CHAN_SEND`, `CHAN_RECV`, `MEM_MAP`, `MEM_UNMAP`,
`MEM_CREATE`, `WAIT_INTERRUPT`, `ACK_INTERRUPT`, `TIMER_SET`,
`SLEEP`, `SPAWN`, `PROC_WAIT`, `DIARY_READ`, `TRACE_READ`, `NOP`.
File and network operations are **messages to services**, not kernel
operations.

Recording hook: every CQE posted to a process being recorded is also
appended to its replay log (§8.6).

### 8.3 IPC channels

```c
struct chan_msg_hdr {
    uint32_t type;               /* from the .idl definition */
    uint32_t len;                /* bytes of data, <= 4096 */
    uint32_t handles[4];         /* moved with the message */
    uint8_t  handle_count;
};
/* A channel is a pair of endpoints; each side has a queue of up to 64
 * messages. Sending moves (not copies) handles. Big data goes through
 * shared memory objects; the message carries the handle. When one side
 * closes (or its process crashes), the peer's next op returns
 * -E_PEER_CLOSED; clients then reconnect by name via the service
 * manager. */
```

IDL example (`idl/fs.idl`), with C and Rust generated by
`scripts/idl_gen.py`:

```text
service fs {
  open(path: str<256>, flags: u32) -> (file: handle)
  read(file: handle, offset: u64, len: u32, buf: handle<memory>) -> (n: u32)
  commit(tx: handle) -> ()
}
```

### 8.4 Interrupts for user-space drivers

1. The kernel's IRQ handler masks the line, marks the interrupt object
   pending, and completes any waiting `WAIT_INTERRUPT` operation.
2. The driver handles the device, then sends `ACK_INTERRUPT`, and the
   kernel unmasks the line.
3. Only the service holding the interrupt handle can wait on it.

### 8.5 Per-CPU, locks and RCU

```c
struct percpu { struct percpu *self; uint32_t cpu_id; struct thread *current;
                struct runqueue rq; struct tss tss; uint64_t lock_depth; };
/* Reached via GS base: percpu()->current */

struct spinlock { volatile uint32_t locked; uint16_t lock_class; uint16_t owner_cpu; };
void spin_lock_irqsave(struct spinlock *l, uint64_t *flags);
void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags);

/* RCU (simple, quiescent-state based): readers do rcu_read_lock/unlock
 * (disable preemption); writers publish a new pointer and call
 * rcu_synchronize(), which waits until every CPU has passed through a
 * context switch, then frees the old copy. */
```

### 8.6 Checkpoint, replay and migration

- Each object type implements `checkpoint(obj, writer)` and
  `restore(reader)`. Objects that can't be saved (devices, interrupts)
  write their service name and are re-acquired on restore.
- Memory: copy-on-write marks pages read-only at the checkpoint, so the
  first write after it copies the page; only changed pages are written.
- Replay log entry: `{seq, kind (CQE | TIME), data}`. In replay mode, the
  kernel serves completions and time readings from the log instead of
  the real world.
- Migration: a checkpoint is streamed over TCP to the peer's migration
  service, restored, then the source process is destroyed.

### 8.7 Budgets

```c
struct budget { uint16_t cpu_pct; uint32_t mem_pages_max;
                uint32_t disk_write_kbps; uint32_t net_kbps; };
struct receipt { uint64_t cpu_ns; uint32_t mem_pages_peak;
                 uint64_t disk_written; uint64_t net_bytes; uint32_t over_budget; };
```

The scheduler enforces CPU share, the memory system enforces pages, and
the fs and net services enforce their rates.

### 8.8 New repo folders (create when the phase starts)

```text
kernel/obj/        objects, handle tables            (Ph 10)
kernel/ring/       async rings                       (Ph 10)
kernel/ipc/        channels, shared memory           (Ph 11)
kernel/smp/        CPU bring-up, per-CPU, locks, RCU (Ph 9)
kernel/acpi/, kernel/pci/                            (Ph 7)
kernel/trace/      tracing, diary, verifier          (Ph 6, 12, 20)
kernel/checkpoint/ checkpoint and replay hooks       (Ph 17)
user/runtime/c/, user/runtime/rust/                  (Ph 11)
user/services/     manager, fs, net, drivers, desktop, perms, migrate
user/apps/, user/wasm/                               (Ph 18)
idl/               message definitions               (Ph 11)
specs/             TLA+ specs                        (Ph 14)
sim/               host-side simulation tests        (Ph 6, 14)
docs/benchmarks.md, docs/BUG_JOURNAL.md, docs/decisions.md
```

---

## 9. How we build and test

| Command | What it does | From phase |
|---|---|---|
| `python build.py test` | boot tests (as today) | now |
| `python build.py test-faults` | crash demos (as today) | now |
| `python build.py test --smp 4` | boot tests on 4 CPUs | 9 |
| `python build.py debug` | QEMU paused + GDB script | 6 |
| `python build.py unit` | host-side unit tests | 6 |
| `python build.py sim [--runs N --seed S]` | simulation tests with failure injection | 6, 14 |
| `python build.py fuzz [--ops N]` | system-call and ring fuzzer | 10 |
| `python build.py bench` | benchmarks → `docs/benchmarks.md` | 6 |
| `python build.py tla` | run TLC on `specs/` | 14 |

CI stays swift (see the Git workflow in CLAUDE.md): one job, cached
toolchain, build + one BIOS boot test. Long sim, fuzz, bench and TLC runs
happen only on request, never nightly by default.

**Benchmarks to track** (all in TSC cycles or ns, same QEMU settings,
Linux numbers alongside where possible): context switch, null ring
operation, ring batch of 64, IPC round trip, page fault, 4 KiB file read
(cached), TCP throughput, boot time, idle memory.

---

## 10. Decision log

| # | Decision | Why | Rejected | Revisit when |
|---|---|---|---|---|
| D1 | Microkernel + restartable services | crash isolation, clean interfaces, interview depth | monolithic (old plan) | IPC cost dominates real workloads |
| D2 | Async rings as the main system-call interface | batching, fewer kernel entries, natural recording point | `int 0x80` per call | — |
| D3 | Handles (capabilities) only, no global names in kernel | permissions with no bypass, checkpointable state | user IDs, `caps` list | — |
| D4 | Kernel in C, new services in Rust | keep tested core; safety where bugs are most likely | full Rust rewrite (too costly), C everywhere | Rust in kernel becomes clearly cheaper |
| D5 | WebAssembly as the app format | sandboxed, any language, any CPU | POSIX compatibility | — |
| D6 | Buddy + slab allocators | standard, fast, debuggable | keep bitmap/first-fit | — |
| D7 | Tickless APIC timer | precision, power | fixed PIT tick | — |
| D8 | Simple RCU (quiescent-state based) | enough for read-mostly tables | full Linux RCU variants | — |
| D9 | Copy-on-write FS with two root slots | one rule gives crash safety, undo and transactions | journaling | — |
| D10 | TLA+ for commit and ring protocols | find design bugs before code | testing only | — |
| D11 | Static ELF, `spawn` not `fork` | simple loader; no process cloning | dynamic linking, fork | — |
| D12 | Drivers trusted for DMA (no IOMMU yet) | scope | IOMMU now | after Phase 19 |
| D13 | x86-64 only until Phase 20 | focus | RISC-V now | after Phase 20 |
| D14 | Stay general-purpose; use-case profiles later | flexibility | single niche now | a strong use case appears (§12) |
| D15 | No AI in the core; `why` is rules over real data | explainable, offline | AI assistant | — |
| D16 | Devanagari shaped at build time first | correct text, no shaping engine in OS | runtime shaper first | lots of typed Indic text |

Earlier decisions in `design/HLD.md` §6 still apply unless replaced above.

---

## 11. Interview kit

### 11.1 Deep-dive topics you can own

| Interview topic | Talk about |
|---|---|
| Concurrency | preemption races found and fixed; spinlocks + lock-order checker; RCU; per-CPU queues; work stealing |
| Memory | page tables, guard pages, buddy/slab, demand paging, copy-on-write, TLB shootdown |
| Storage | copy-on-write FS, two root slots, checksums, transactions, TLA+ proof, 1M simulated power cuts |
| Distributed systems | live migration (what state moves, what is re-bound); Raft folders (leader election, log replication, partitions) |
| Networking | own TCP: handshake, retransmission, congestion control, benchmarks vs Linux |
| Performance | ring batching vs per-call cost, zero-copy IPC, measured with TSC; honest comparison with Linux |
| Security | handles vs user IDs, SMEP/SMAP, KASLR, verified boot + rollback, fuzzing |
| Architecture | microkernel vs monolithic tradeoffs, restartable drivers |
| Testing | in-kernel self-tests, crash demos, simulation testing, fuzzing, CI budgets |
| Debugging | record and replay, symbolised call traces, the bug stories below |

### 11.2 Story structure for any feature (2 minutes)

Problem → the simple design → the hard part → how we proved it works
(test or number) → what we would do differently.

### 11.3 Bug journal (seed `docs/BUG_JOURNAL.md` with these real bugs)

From `phases/phase-2-interrupts.md` and `phases/phase-5-scheduler.md`:

1. **TSS layout:** a reserved field was 4 bytes instead of 8. The CPU read
   the emergency stack pointer (IST1) from the wrong offset and
   triple-faulted (reset) on any IST exception.
2. **Stale segment registers:** after loading the new GDT, CS/SS still held
   the bootloader's selectors (0x28/0x30), outside the new table. The first
   `iretq` would fault. Fixed with a far return into 0x08 and data segment
   reloads.
3. **Interrupt stubs used `call`:** this pushed an extra return address,
   so the error code was read from the CS slot and `iretq` returned to the
   wrong place.
4. **Invalid gate type:** some IDT gates used type 0x6, which is invalid in
   long mode. All gates now use 0xE.
5. **Timer would hit the double-fault vector:** the PIC was not remapped,
   so IRQ0 would arrive as vector 8 (#DF), and `timer_init` overwrote the
   IDT entry with a plain C function.
6. **Races in `thread_create`/`process_create`:** with preemption, another
   thread could fill the table or terminate and reap the target process
   between the check and the insert. Fixed by re-checking inside the
   critical section.
7. **Allocators not preemption-safe:** a thread preempted in the middle of
   `kmalloc` let another corrupt the heap. Fixed with guards on PMM, VMM,
   heap and kstack entry points. The demo's Memory Worker exercises
   exactly this.

Template for new entries: *symptom → how it was found → root cause → fix →
test that now guards it.*

### 11.4 Demo video plan (3 minutes, after Phase 17/18)

Boot (self-check, verified slot) → desktop → kill a driver, it restarts →
edit a file, power cut, resume session → `why slow` → move a running
program to a second machine → replay a recorded crash.

---

## 12. Future use cases (profiles, later)

BharatOS stays general. A **profile** is a chosen set of services, apps
and policies, built from the same OS:

- **Trusted terminal** (exam centres, ration shops, banking agents,
  government kiosks): read-only verified image, allowlisted network only,
  a hash-chained tamper-evident log (each entry includes a fingerprint of
  the previous one), session replay for disputes, no app installs.
- **School / low-end desktop:** Hindi/Marathi, 128 MB, undo everything,
  offline help.
- **Edge appliance:** headless, network services, live migration for
  maintenance.

No profile work happens before Phase 20 unless a strong opportunity
appears (D14).

---

## 13. What we are not doing

- POSIX or Linux binary compatibility; `fork`; dynamic linking.
- A full Rust rewrite of the existing kernel.
- AI features in the core.
- A hypervisor, GPU acceleration, sound before Phase 20.
- Any feature without a test, a benchmark or demo, and a doc entry.

---

## 14. Old plan to new plan mapping

| Old (ROADMAP.md) | New |
|---|---|
| Phase 6: user mode + `int 0x80` system calls | Phase 10, with rings + handles instead of `int 0x80`; CI moves to Phase 6 |
| Phase 7: programs, keyboard, shell | loader and bundle → Phase 11; shell → Phase 12; keyboard as a service → Phase 11 |
| Phase 8: explain + self-check | Phase 12 (plus budgets, receipts and continuous checks) |
| Phase 9: BharatFS with in-kernel ATA | Phase 13 (virtio-blk service) + Phase 14 (FS as a service, plus transactions, TLA+, simulation) |
| Phase 10: permissions with `caps` list | Phase 15, using handles; plus verified boot and KASLR |
| Phase 11: desktop | Phase 18, plus WebAssembly apps |
| Phase 12: made for Bharat | Phase 19, plus NVMe, xHCI and real hardware |
| "Later": SMP, networking, faster disk | now Phases 9, 16, 13 |
| Old LLD: syscall table, `caps`, IPC | replaced by §8 here; the old LLD file system, `why`, diary, language and desktop protocol designs still apply |
