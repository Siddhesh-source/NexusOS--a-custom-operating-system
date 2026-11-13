# BharatOS: High-Level Design

How the big pieces fit together, what each one is responsible for, and
the main flows through the system. Exact structures and function
signatures are in [LLD.md](LLD.md). The plan for building it is in
[../ROADMAP.md](../ROADMAP.md).

## 1. Design rules

These rules decide arguments. When two designs both work, pick by these.

1. **Simplest design that works, written down.** If we pick a slower or
   smaller option on purpose (e.g. `int 0x80`, ATA PIO, tar), note when
   we'd upgrade.
2. **One check point per rule.** Permissions are checked only at the
   system-call boundary; user pointers are touched only through
   `copy_from_user`/`copy_to_user`. One place to get right, one place to
   test.
3. **Fail loudly, never silently.** Bad input gets a clear error. Broken
   state gets a panic with a report. Nothing is "probably fine".
4. **Fixed sizes where possible.** Tables, rings and message sizes are
   fixed. No surprises under memory pressure, and easy to test limits.
5. **Every feature has a test and a demo.**

## 2. The layers

```text
┌───────────────────────────────────────────────────────────────────────────┐
│ APPS (ring 3)   Home · Terminal(bsh) · Files · System "Why?" · Settings   │  Ph 7, 11
│                 hello · cat · spin · crash · user test programs           │
├───────────────────────────────────────────────────────────────────────────┤
│ SERVICES (ring 3)  Desktop (owns screen) · Supervisor (restarts services) │  Ph 8, 11
├───────────────────────────────────────────────────────────────────────────┤
│ USER LIBRARY    bharat.h: syscall wrappers, printf, malloc, why-rules,    │  Ph 7, 8, 12
│                 msg(ID) language lookup                                   │
╞══════════════════ system-call wall (int 0x80) ═══════ cap_check() here ═══╡  Ph 6, 10
│ KERNEL (ring 0)                                                           │
│  ┌─────────────┬──────────────┬────────────────┬──────────────────────┐   │
│  │ Programs    │ Files (VFS)  │ Permissions    │ Explain              │   │
│  │ ELF loader  │ /sys tar     │ per-process    │ event diary          │   │
│  │ spawn/wait  │ /home BharatFS│ list + check  │ per-app stats        │   │
│  │             │ /dev devices │                │ self-check results   │   │
│  ├─────────────┴──────────────┴────────────────┴──────────────────────┤   │
│  │ IPC: channels + shared memory                                      │   │  Ph 11
│  ├────────────────────────────────────────────────────────────────────┤   │
│  │ Scheduler (done) · Processes & threads (done) · Memory (done)      │   │  Ph 4-5, 3
│  ├────────────────────────────────────────────────────────────────────┤   │
│  │ Drivers: serial(done) · timer(done) · keyboard · mouse · ATA disk  │   │  Ph 7, 9, 11
│  │          framebuffer                                               │   │
│  ├────────────────────────────────────────────────────────────────────┤   │
│  │ CPU: GDT/TSS · IDT · exceptions · FPU state (Ph 6)                 │   │  Ph 2, 6
│  └────────────────────────────────────────────────────────────────────┘   │
├───────────────────────────────────────────────────────────────────────────┤
│ Limine bootloader (BIOS/UEFI): memory map, framebuffer, startup bundle    │
└───────────────────────────────────────────────────────────────────────────┘
```

**Kernel style:** one kernel binary with drivers inside it (a small
"monolithic" kernel). Simplest to build and debug. The things most likely
to be buggy *and* user-facing (desktop, apps, the "why" rules) live in
user space, where a crash is survivable.

## 3. Components and their jobs

| Component | Folder | Job | Owns | Never does |
|---|---|---|---|---|
| CPU setup | `kernel/arch/x86_64` | GDT/TSS, IDT, entry/exit to ring 3, context switch, timer | CPU tables | policy decisions |
| Memory | `kernel/mm` | physical pages, page tables, kernel heap, page-fault reports | all RAM | know about files or apps |
| Processes | `kernel/proc` | create/destroy processes and threads, kernel stacks, program loading (Ph 7) | process & thread tables | pick who runs |
| Scheduler | `kernel/sched` | pick who runs, preemption, sleep, blocking, priorities (Ph 8) | ready/sleep lists | allocate memory in the timer path |
| System calls | `kernel/syscall` (Ph 6) | the only door from apps into the kernel; validates every argument | the call table | trust a user pointer |
| Permissions | `kernel/security` (Ph 10) | per-process permission list, `cap_check` | grants | prompt the user itself (asks a service) |
| Files | `kernel/fs` (Ph 7, 9) | VFS + tar bundle + BharatFS + devices | open-file tables, disk cache | bypass permissions |
| Explain | `kernel/explain` (Ph 8) | event diary, per-app stats history, self-check results | diary ring | build sentences (user library does) |
| IPC | `kernel/ipc` (Ph 11) | channels and shared memory between programs | channel table | interpret messages |
| Drivers | `kernel/drivers` | serial, keyboard, mouse, ATA, framebuffer | device state | contain policy |
| Self-tests | `kernel/tests` | boot-time checks for each subsystem | — | run in release-only paths |
| User library | `user/lib` | wrappers, printf, malloc, why-rules, messages | — | — |
| Apps & services | `user/apps`, `user/services` | shell, desktop, built-in apps | their windows/data | touch hardware |

## 4. Main flows

### 4.1 Boot

```text
Firmware → Limine → kernel entry
  → serial → GDT/TSS → IDT → timer
  → memory (pages → page tables → heap) → memory self-checks
  → processes → process self-checks → scheduler → scheduler self-checks
  → [Ph 8] store self-check results; any core failure → SAFE MODE
  → [Ph 7] mount startup bundle (/sys) → [Ph 9] mount /home
  → start first user program: supervisor
       → desktop (Ph 11) or shell (Ph 7) → user is in control
```

### 4.2 A system call (e.g. `read(fd, buf, n)`)

```text
app: int 0x80 (rax = number, args in rdi, rsi, rdx, r10, r8)
 → CPU switches to the thread's kernel stack (TSS.rsp0)
 → syscall_dispatch: number in range? → handler
 → handler: check fd belongs to caller → cap_check(...)
           → do the work → copy_to_user(buf, data, n) (validates range)
 → return value in rax (≥0 ok, <0 error code)
 → iretq back to ring 3
```

### 4.3 An app crashes

```text
CPU fault in ring 3 (e.g. page fault)
 → fault handler sees "came from user mode"
 → record event {APP_CRASHED, pid, fault type, address} in diary
 → terminate the process (free memory, close files, wake waiters)
 → supervisor restarts it if it is a service (max 3/min)
 → `why crash <app>` later reads this event and explains it
(fault in ring 0 → panic with full report, as today)
```

### 4.4 Saving a file on BharatFS (power-cut safe)

```text
write(fd, data) → buffered in the open file
close/fsync →
  1. write new data blocks (never over live ones)
  2. write new metadata blocks pointing at them
  3. wait for the disk to finish
  4. write new root record (seq+1, checksum) into the *other* root slot
  5. wait for the disk
Power cut before step 4 completes → old root still valid → old version.
After step 4 → new version. There is no in-between state.
Old roots stay in the version list → history / undo / rewind.
```

### 4.5 Permission ask

```text
app: open("/home/notes/todo.txt", READ)
 → cap_check: no grant
   → in manifest? no  → return E_DENIED (logged)
   → in manifest? yes → pause the thread, post "ask" message to the
                         desktop (or shell); user picks Allow/Once/Deny
                       → save answer, log event, resume with result
```

### 4.6 Drawing a window (Phase 11)

```text
app draws into its shared pixel buffer → sends FRAME_READY(rect) on its channel
desktop: copies that rect to the screen buffer, draws frame/title,
         then flips the dirty area to the framebuffer
mouse/keyboard → kernel input queue → desktop → focused app's channel
```

## 5. Memory map

```text
0x0000000000400000   user program code/data (per process)
        ...          user heap (grows up via mem_grow)
0x00007ffff0000000   shared-memory window (IPC, pixel buffers)
0x00007fffffffe000   user stack top (grows down, guard page below)
───────────────────  user / kernel split (non-canonical hole)
0xffff800000000000   direct map of all physical RAM
0xffffc00000000000   kernel heap
0xffffd00000000000   memory-test window
0xffffe00000000000   kernel thread stacks (with guards)
0xffffffff80200000   kernel image
```

## 6. Key decisions (and when we'd revisit)

| Decision | Why | Revisit when |
|---|---|---|
| `int 0x80` for system calls | reuses tested interrupt code; one IDT entry | system-call cost shows up in profiling |
| `spawn` instead of `fork` | no copy-on-write process cloning needed | never, probably |
| Static ELF only | loader stays ~200 lines | many apps share big libraries |
| tar for the startup bundle | format fits on a page | never; it's read-only by design |
| ATA PIO disk | tiny, works in QEMU and old PCs | disk speed matters (→ virtio-blk) |
| Copy-on-write file system with two root slots | one rule gives crash safety + undo | — |
| Permissions checked only at system calls | one choke point to test | — |
| One desktop process draws everything | no window-manager complexity | many windows, animations |
| "Why" = rules table, in user space | explainable, offline, can't crash the kernel | — |
| Devanagari pre-rendered at build time | correct shaping without a shaping engine in the OS | users type a lot of Indic text (→ fuller runtime shaper) |
| Disable interrupts for critical sections | correct on one CPU, one macro to replace | multi-core |
