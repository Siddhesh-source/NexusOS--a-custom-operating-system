# BharatOS: Low-Level Design (Phases 6–12)

Exact structures, numbers and function signatures for the next phases.
These are **starting designs**: change them if implementation shows a
better simple way, and update this file in the same commit.

Existing code that these build on: `proc.h` (`struct process`,
`struct thread`), `sched.h`, `vmm.h` (`VMM_USER`, `VMM_WRITE`, `VMM_EXEC`),
`gdt.h`, `idt.h`, `cpu.h` (`IRQ_GUARD()`).

---

## Phase 6: user mode and system calls

### GDT (new order)

| Index | Selector | Segment | Notes |
|---|---|---|---|
| 0 | `0x00` | null | |
| 1 | `0x08` | kernel code | `GDT_CODE_SEL` (unchanged) |
| 2 | `0x10` | kernel data | `GDT_DATA_SEL` (unchanged) |
| 3 | `0x18` | user data | used as `0x1B` (RPL 3) |
| 4 | `0x20` | user code | used as `0x23` (RPL 3), long-mode bit set, DPL 3 |
| 5–6 | `0x28` | TSS (16 bytes) | `GDT_TSS_SEL` moves from `0x18` to `0x28` |

This order is what `syscall`/`sysret` needs, so a later switch costs nothing.

### Additions to `struct thread` and `struct process`

```c
struct thread {
    /* ...existing fields... */
    bool     is_user;           /* has a ring-3 part */
    uint64_t kstack_top;        /* written to TSS.rsp0 when switched to */
    uint8_t  fpu[512] __attribute__((aligned(16)));  /* fxsave area */
    bool     fpu_used;          /* skip fxrstor if never used (optional) */
};

struct process {
    /* ...existing fields... */
    uint64_t user_brk;          /* end of heap (mem_grow) */
    struct cap_list caps;       /* Phase 6: empty; Phase 10: real grants */
    struct proc_stats stats;    /* Phase 8 */
    int32_t  exit_code;
    struct wait_queue waiters;  /* threads in wait(pid) */
    struct file *files[FD_MAX]; /* Phase 7; FD_MAX = 32 */
};
```

### User address-space layout

| Constant | Value | Meaning |
|---|---|---|
| `USER_BASE` | `0x0000000000400000` | first program byte (null page region stays unmapped) |
| `USER_TOP` | `0x0000800000000000` | first address *not* in user space |
| `USER_STACK_TOP` | `0x00007fffffffe000` | initial stack pointer area |
| `USER_STACK_SIZE` | 64 KiB, grows on demand later | one unmapped guard page below |
| `USER_SHM_BASE` | `0x00007ffff0000000` | shared-memory window (Phase 11) |

### Entering ring 3

```c
/* arch/x86_64/usermode.asm. Builds an iretq frame and jumps.
 * Stack pushed: SS=0x1B, RSP=user_rsp, RFLAGS=0x202 (IF on),
 * CS=0x23, RIP=entry. Clears all general registers first so no kernel
 * value leaks into user space. */
void enter_user_mode(uint64_t entry, uint64_t user_rsp) __attribute__((noreturn));
```

### System call ABI

| Register | Meaning |
|---|---|
| `rax` | call number in, result out |
| `rdi, rsi, rdx, r10, r8` | arguments 1–5 |
| result | `>= 0` success, `< 0` = `-E_*` error code |

IDT vector `0x80`, gate DPL 3 (so ring 3 may call it), interrupt gate (IF
cleared on entry; handler re-enables interrupts once it's safe).

```c
/* kernel/syscall/syscall.h */
typedef int64_t (*syscall_fn)(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5);

enum syscall_no {
    SYS_EXIT = 0,  SYS_WRITE = 1,  SYS_GETPID = 2, SYS_YIELD = 3,
    SYS_SLEEP = 4, SYS_UPTIME = 5,
    /* Phase 7 */
    SYS_READ = 6,  SYS_SPAWN = 7,  SYS_WAIT = 8,  SYS_MEM_GROW = 9,
    SYS_OPEN = 10, SYS_CLOSE = 11, SYS_LIST_DIR = 12, SYS_KILL = 13,
    /* Phase 8 */
    SYS_PROC_LIST = 14, SYS_EVENTS_READ = 15, SYS_HEALTH = 16,
    /* Phase 9 */
    SYS_STAT = 17, SYS_REMOVE = 18, SYS_HISTORY = 19, SYS_RESTORE = 20,
    /* Phase 10 */
    SYS_PERM_LIST = 21, SYS_PERM_SET = 22,
    /* Phase 11 */
    SYS_CHAN_CREATE = 23, SYS_CHAN_SEND = 24, SYS_CHAN_RECV = 25,
    SYS_SHM_CREATE = 26, SYS_SHM_MAP = 27, SYS_SCREEN_MAP = 28,
    SYS_INPUT_READ = 29,
    SYS_COUNT
};

enum err {
    E_OK = 0, E_INVALID = 1, E_FAULT = 2 /* bad user pointer */,
    E_NOMEM = 3, E_NOTFOUND = 4, E_DENIED = 5, E_BUSY = 6,
    E_EXISTS = 7, E_NOSPACE = 8, E_CORRUPT = 9, E_TOOBIG = 10,
};

/* Called from the int 0x80 stub with the saved register frame. */
void syscall_dispatch(struct interrupt_frame *f);
```

### Copying user memory (the only way the kernel touches user pointers)

```c
/* kernel/syscall/usercopy.c
 * Valid only if: len small enough, ptr+len doesn't overflow,
 * [ptr, ptr+len) entirely below USER_TOP, and every page is mapped with
 * VMM_USER (and VMM_WRITE for copy_to_user) in the caller's address
 * space. Checks all pages first, then copies. Returns E_OK or -E_FAULT. */
int copy_from_user(void *dst, uint64_t user_src, size_t len);
int copy_to_user(uint64_t user_dst, const void *src, size_t len);
/* Copies a NUL-terminated string of at most max bytes (incl. NUL). */
int copy_string_from_user(char *dst, uint64_t user_src, size_t max);
```

### User fault handling

```c
/* In the exception handler: if (frame->cs & 3) == 3 the fault came from
 * ring 3. */
void user_fault(struct interrupt_frame *f, const char *reason);
/* -> explain_event(EV_APP_CRASHED, pid, vector, cr2)
 * -> prints the plain-language report
 * -> process_terminate(current process); schedule(); never returns */
```

---

## Phase 7: programs, input, shell

### Startup bundle (tar, read-only)

- Built by `build.py` from `user/` output into `build/initrd.tar`, added
  to `limine.conf` as a module, found via the Limine module request.
- Format: 512-byte ustar header (name[100], size as octal text[12],
  typeflag) then data padded to 512. Two zero blocks end it.
- `tar_find(name) -> {ptr, size}` walks headers; no copying, no index.

### ELF loader

```c
/* kernel/proc/elf.c. Accepts only: ELF64, little-endian, x86-64,
 * ET_EXEC, PT_LOAD segments fully inside [USER_BASE, USER_TOP),
 * filesz <= memsz, total size <= 64 MiB. Anything else: -E_INVALID. */
int elf_load(struct process *p, const void *image, size_t size,
             uint64_t *entry_out);
```

Segment permissions: `PF_W` → `VMM_WRITE`, `PF_X` → `VMM_EXEC`, never both
(W^X). Memory after `filesz` is zeroed.

### Initial user stack

```text
USER_STACK_TOP
  argv strings (copied)
  padding to 16 bytes
  NULL
  argv[argc-1] ... argv[0]   (pointers)
  argc                       <- rsp at entry
```

### Input

```c
/* kernel/drivers/input.c. One line-buffered console input. */
#define INPUT_BUF 256
void input_push_char(char c);          /* from keyboard IRQ1 or serial IRQ4 */
int64_t input_read_line(char *buf, size_t max);  /* blocks (TASK_BLOCKED) */
```

Keyboard: scancode set 1, US layout table first; handles shift, caps,
backspace, enter. Serial input uses the same buffer (so tests can type).

### Blocking (wait queues)

```c
struct wait_queue { struct thread *head; };
void wait_on(struct wait_queue *q);    /* current -> BLOCKED, schedule() */
void wake_all(struct wait_queue *q);   /* each -> READY */
```

### Spawn and wait

```c
int64_t sys_spawn(const char *path, const char *const *argv); /* returns pid */
int64_t sys_wait(pid_t pid, int32_t *exit_code);              /* blocks */
```

Child gets: its own address space, a *subset* of parent's permissions
(Phase 10), stdin/stdout = parent's console.

### File handles (Phase 7 subset of VFS)

```c
struct file {
    const struct file_ops *ops;
    void    *node;       /* backend-specific */
    uint64_t pos;
    uint32_t flags;      /* O_READ, O_WRITE */
    uint32_t refs;
};
struct file_ops {
    int64_t (*read)(struct file *, void *buf, size_t n);
    int64_t (*write)(struct file *, const void *buf, size_t n);
    int     (*close)(struct file *);
};
```

---

## Phase 8: explain-it layer

### Per-process stats

```c
struct proc_stats {
    uint64_t cpu_ticks_total;
    uint8_t  cpu_pct_history[60];   /* one entry per second, ring */
    uint8_t  history_pos;
    uint32_t pages_in_use;
    uint64_t syscalls;
    uint32_t crashes;
    uint64_t started_tick;
};
```

A once-per-second kernel step (from the timer, deferred to a kernel
thread, never in the interrupt itself) fills `cpu_pct_history`.

### Event diary

```c
#define DIARY_SIZE 1024              /* power of two */
enum event_kind {
    EV_APP_STARTED, EV_APP_EXITED, EV_APP_CRASHED, EV_APP_RESTARTED,
    EV_LOW_MEMORY, EV_PERM_ASKED, EV_PERM_ALLOWED, EV_PERM_DENIED,
    EV_FILE_RESTORED, EV_SELFCHECK_FAILED, EV_DISK_CORRUPT,
};
struct event {
    uint64_t tick;
    uint32_t seq;                    /* increases forever; detects overwrite */
    uint16_t kind;                   /* enum event_kind */
    uint16_t pid;
    char     name[16];               /* app name at the time */
    uint64_t a, b;                   /* kind-specific: fault vector, address, ... */
};
void explain_event(enum event_kind k, pid_t pid, uint64_t a, uint64_t b);
/* sys_events_read(after_seq, buf, max) copies events newer than after_seq. */
```

Writes are `IRQ_GUARD()`-protected; no allocation; oldest overwritten.

### "Why" rules (user library, `user/lib/why.c`)

```c
struct why_rule {
    const char *topic;                         /* "slow", "crash", "memory", "access" */
    bool (*match)(const struct why_snapshot *s, const char *arg);
    void (*explain)(const struct why_snapshot *s, const char *arg, struct out *o);
};
```

`why_snapshot` = process list + stats + recent events, fetched once. Rules
run top to bottom; first match answers. All sentences use `msg(ID, ...)`.

### Self-check results

```c
struct selfcheck { char suite[16]; uint16_t passed, failed; bool core; };
/* Each *_test.c registers its totals; SYS_HEALTH returns the array.
 * Any core suite with failed > 0 => boot flag SAFE_MODE. */
```

### Priorities

Two ready queues: `FOREGROUND`, `BACKGROUND`. Pick foreground first, but
after 4 foreground slices in a row with background waiting, run one
background thread. The focused app (shell or desktop's focused window)
is foreground.

---

## Phase 9: BharatFS

### Disk

ATA PIO, primary slave (data disk), 512-byte sectors, LBA28. BharatFS
uses 4 KiB blocks (8 sectors). `disk_read(lba, count, buf)`,
`disk_write(...)`, `disk_flush()` (ATA CACHE FLUSH).

### On-disk layout

```text
block 0        superblock: magic "BHARATFS", version, block count, block size
block 1        root slot A
block 2        root slot B
block 3..N     allocation bitmap (1 bit per block)
then           data and metadata blocks (copy-on-write)
```

```c
struct fs_root {            /* fits in one block */
    char     magic[8];      /* "BFSROOT\0" */
    uint64_t seq;           /* bigger = newer */
    uint64_t time;          /* seconds since boot epoch / RTC */
    uint64_t inode_table;   /* block of the inode-table tree */
    uint64_t version_list;  /* block of the list of older roots */
    uint64_t free_blocks;
    uint32_t checksum;      /* CRC32 of everything above */
};

struct fs_inode {           /* 128 bytes */
    uint32_t id;
    uint16_t type;          /* FILE / DIR */
    uint16_t links;
    uint64_t size;
    uint64_t mtime;
    uint64_t extents[8][2]; /* (start block, block count); 1 extra block if more */
    uint64_t extra_extents;
    uint32_t checksum;
};

struct fs_block_tail {      /* last 8 bytes of every metadata block */
    uint32_t checksum;      /* CRC32 of the block minus the tail */
    uint32_t block_no;      /* catches misdirected writes */
};
```

Directories are files containing `{inode id, name[60]}` entries.

### Commit (the one rule)

1. Allocate new blocks for changed data and every metadata block on the
   path to the root (copy-on-write).
2. Write them. `disk_flush()`.
3. Write new `fs_root` with `seq + 1` into slot `(seq + 1) % 2`.
   `disk_flush()`.
4. Old root appended to the version list; its blocks stay reserved until
   the cleaner releases that version.

Mount: read both slots; pick the highest `seq` with a valid checksum.

### Versions API

```c
int fs_history(const char *path, struct version_info *out, int max);
int fs_restore(const char *path, uint64_t version_seq);   /* new commit */
```

Restore never deletes newer versions; it commits a copy of the old one.

---

## Phase 10: permissions

```c
enum cap_kind  { CAP_FILES, CAP_DEVICE, CAP_SCREEN, CAP_NETWORK };
enum cap_right { CAP_R = 1, CAP_W = 2 };

struct cap {
    uint8_t kind;
    uint8_t rights;
    uint8_t once;           /* "allow once": removed after first use */
    char    target[61];     /* normalised path prefix or device name */
};
struct cap_list { struct cap items[16]; uint8_t count; };

/* Returns E_OK, -E_DENIED, or blocks to ask the user if the app's manifest
 * lists this need. `target` must already be normalised (no "..", ".",
 * double slashes). Prefix match on whole path components only:
 * "/home/notes" allows "/home/notes/a" but not "/home/notes2". */
int cap_check(struct process *p, enum cap_kind k, const char *target, uint8_t rights);
```

Manifest (`<app>.app`, plain text, in the bundle next to the program):

```text
name = Notes
needs = files:/home/notes rw
```

Saved answers: `/home/.permissions`, one line per grant.

---

## Phase 11: IPC and desktop

```c
#define MSG_SIZE 256
struct msg { uint32_t type; uint32_t len; uint8_t data[MSG_SIZE - 8]; };

/* Channel = fixed ring of 32 messages, two wait queues (readers/writers). */
int64_t sys_chan_create(void);                       /* returns handle */
int64_t sys_chan_send(int64_t h, const struct msg *m);   /* blocks if full */
int64_t sys_chan_recv(int64_t h, struct msg *m);         /* blocks if empty */

/* Shared memory: whole pages, mapped into USER_SHM window. */
int64_t sys_shm_create(size_t bytes);                /* returns handle */
int64_t sys_shm_map(int64_t h);                      /* returns user address */
```

Desktop protocol (message `type` values):

| Type | Direction | Data |
|---|---|---|
| `WIN_CREATE` | app → desktop | width, height, title, shm handle of pixel buffer |
| `WIN_FRAME_READY` | app → desktop | dirty rect x, y, w, h |
| `WIN_CLOSE` | either | — |
| `INPUT_KEY` | desktop → app | key code, char, modifiers |
| `INPUT_MOUSE` | desktop → app | x, y (window-relative), buttons |
| `PERM_ASK` | kernel → desktop | app name, permission text id, request id |
| `PERM_ANSWER` | desktop → kernel | request id, allow/once/deny |

Pixel format: 32-bit `0x00RRGGBB`, row stride = width × 4.

---

## Phase 12: languages

```c
/* user/lib/msg.c. Language packs are files: /sys/lang/<code>.txt
 * Lines: ID<TAB>text with {1} {2} placeholders. Missing IDs fall back to
 * English, and in test builds print a warning. */
const char *msg(uint32_t id);
int msgf(char *out, size_t max, uint32_t id, ...);
```

Devanagari: `scripts/render_glyphs.py` shapes every fixed UI string with
a real shaping library at build time and stores glyph images + positions
in `/sys/lang/hi.glyphs`. The desktop draws those images directly.
