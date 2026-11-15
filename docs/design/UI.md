# BharatOS: Desktop Look and Feel

The desktop (Phase 11) should look calm, clean and obvious to someone who
has never used a computer much. This file is the reference for every
screen. If a screen breaks a rule here, fix the screen or change the rule
here first.

## Principles

1. **Readable first.** Large text (16 px base, 20 px for headings), high
   contrast, never grey-on-grey.
2. **One thing per screen.** Each app does one job and shows one main
   area.
3. **Plain words.** "Allow Notes to read your notes folder?" not
   "Grant FS_READ capability". Every string comes from a language pack.
4. **Show the safety.** Health badge, undo history and permission state
   are always one click away and never hidden in menus.
5. **Nothing moves without a reason.** No animations except a short
   fade on dialogs. Saves CPU on slow machines.

## Layout

The look is "Midnight and ivory" (chosen in the Phase 11 design review,
`docs/design/improvements.md`): calm dark slate behind everything, warm
ivory windows on top, one burnt-saffron button where it matters.

```text
┌──────────────────────────────────────────────────────────────────────┐
│ ● BharatOS   Terminal         ● 569 checks passed  asha  4 CPUs 14:32 Log out │ top bar (30 px)
│                                                                      │
│          ╭──────────────── Files ──────────────── ● ╮                │ title (40 px)
│          │ [Up] [Open] [New file] [New folder] [Delete]│             │ toolbar (56 px)
│          │ /home/asha                                  │             │
│          │ ▰ notes                          3 items    │             │ rows (32 px)
│          │ ▯ todo.txt                       120 bytes  │             │
│          ╰─────────────────────────────────────────────╯  (shadow)   │
│                                                                      │
│              ╭── >_  ▰  ▮▮▮  ✚  i ──╮                                │ dock
└──────────────────────────────────────────────────────────────────────┘
```

- **Grid:** everything aligns to 8 px. Insets 16 px.
- **Top bar (status only):** brand and the focused app's name on the
  left; health (green dot and check count, click for Health), user, CPU
  count, clock and Log out on the right. See-through dark over the
  wallpaper.
- **Dock (apps):** bottom centre, a rounded dark pill with one tile per
  app and its name; a saffron dot under each running app; the focused
  app's tile is lightly highlighted.
- **Windows:** rounded (12 px) ivory, soft offset shadow (deeper on the
  focused window), title centred in the window's own surface with a
  hairline below, a round close button (red on the focused window). New
  windows open centred. The terminal is the one dark window.
- **Buttons:** rounded (8 px), medium weight. One primary per window in
  `primary`; the rest quiet `surface-2`.
- **Dialogs** (permission prompts, errors): centred, dimmed background,
  max 3 buttons, the safe choice ("Deny") is never the default for
  permissions.
- **Login:** a lock screen on the wallpaper: the time, the brand, a round
  avatar with the first letter of the name, a soft field with a visible
  label above it.

## Colours

One theme. Defined once in `kernel/ui/theme.h`; apps use the names,
never raw values.

| Name | Value | Used for |
|---|---|---|
| `wall-top` → `wall-bottom` | `#121821` → `#26324A` | wallpaper gradient, splash |
| `chrome` | `#0A0E14`, drawn see-through | top bar, dock |
| `surface` | `#FBFAF7` | windows (warm ivory) |
| `surface-2` | `#F1EEE8` | quiet buttons, panels |
| `line` | `#E4E0D8` | hairlines, field borders |
| `text` / `muted` | `#1D232B` / `#68707C` | text on ivory (muted 4.6:1) |
| `on-dark` / `on-dark-muted` | `#ECEAE4` / `#8C96A5` | text on the wallpaper and bars |
| `primary` | `#B35F1A` | the one main button (ivory text 4.7:1) |
| `saffron` | `#E39B4E` | brand dot, caret, progress, running dots: on dark only |
| `accent-soft` | `#F5E6D3` | selected rows |
| `teal` | `#2A7C76` | data (CPU bars, graphs) |
| `ok` / `warn` / `danger` | `#3A9D6B` / `#C98A2E` / `#D25C4D` | status |
| `term-bg` / `term-text` | `#0F141B` / `#E6E8EB` | terminal |

## Type

- IBM Plex Sans for the interface: 13 (labels), 15 (body), 15 medium
  (titles, buttons), 22 semibold (headings), 56 light (wordmark, clock).
  IBM Plex Mono 15 for the terminal, the editor and command output
  (9 x 19 pixel cells). Both under the SIL Open Font License
  (`kernel/ui/fonts-OFL.txt`).
- Drawn smooth: `scripts/make_fonts.py` renders each size once into
  `kernel/ui/fonts.bin` (8-bit coverage per pixel), which the kernel
  blends with what is behind. The build itself needs nothing new.
- Devanagari glyph strips will be pre-rendered the same way (Phase 22).
  Line height about 1.4 x size so matras don't clip.

## Screens

| App | Main area | Key actions |
|---|---|---|
| **Home** | grid of large app tiles + "Self-check: 223/223 passed" card | open app |
| **Terminal** | `bsh` text area | type commands |
| **Files** | folder list | open, rename, delete (undoable), **History** → list of versions with time → **Restore** |
| **System ("Why?")** | per-app bars for CPU and memory, plain-sentence box at top ("Spin is using 85% of the processor") | stop app, open crash details |
| **Permissions** | list: app → what it can use → toggle | revoke, see access log |
| **Settings** | language, theme, text size | — |

## How we test the look

- QEMU screenshots of each app compared with reference images in
  `tests/ui/` (small pixel-difference threshold).
- Contrast of every colour pair in the table checked by a script.
- Screenshots taken in English and Hindi at 1024×768 and 1280×800.
