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

```text
┌──────────────────────────────────────────────────────────────────────┐
│  ◉ BharatOS    Terminal   Files   System   ...      ✓ Healthy  14:32 │  top bar (32 px)
├──────────────────────────────────────────────────────────────────────┤
│                                                                      │
│   ┌──────────────────────────────┐                                   │
│   │ Files                    – ✕ │  window title bar (28 px)         │
│   ├──────────────────────────────┤                                   │
│   │ 📁 notes          History ⟲  │                                   │
│   │ 📄 todo.txt       History ⟲  │                                   │
│   │                              │                                   │
│   └──────────────────────────────┘                                   │
│                                                                      │
└──────────────────────────────────────────────────────────────────────┘
```

- **Grid:** everything aligns to 8 px. Padding inside panels 16 px.
- **Top bar:** app switcher on the left, health badge and clock on the
  right. Clicking the badge opens System → Health.
- **Windows:** plain rectangles, 1 px border, title bar with close and
  minimise. No overlapping shadows (keeps drawing cheap).
- **Dialogs** (permission prompts, errors): centred, dimmed background,
  max 3 buttons, the safe choice ("Deny") is never the default for
  permissions.

## Colours

Two themes. Defined once in the desktop as named colours; apps use the
names, never raw values.

| Name | Light | Dark | Used for |
|---|---|---|---|
| `bg` | `#F7F6F2` | `#16171B` | desktop background |
| `surface` | `#FFFFFF` | `#202228` | windows, panels |
| `text` | `#1C1D21` | `#ECEDEF` | main text |
| `text-muted` | `#5B5E66` | `#A3A7B0` | secondary text (still ≥ 4.5:1 contrast) |
| `accent` | `#2F5BD3` | `#7C9BFF` | buttons, focus, selection |
| `saffron` | `#E07A1F` | `#F29A4A` | brand mark, highlights (sparingly) |
| `ok` | `#1F8A4C` | `#4CC27F` | health badge, "allowed" |
| `warn` | `#B7791F` | `#E3B04B` | low memory, "asked" |
| `danger` | `#C53030` | `#F07272` | crashes, "denied" |
| `border` | `#DEDCD5` | `#33363E` | lines, window borders |

## Type

- One bitmap font family in 3 sizes: 12 (labels), 16 (body), 20 (headings).
- Latin from a PSF font; Devanagari glyph strips pre-rendered at build time
  (see ROADMAP Phase 12). Line height = 1.5 × size so matras don't clip.

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
