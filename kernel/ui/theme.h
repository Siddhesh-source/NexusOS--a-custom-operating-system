#ifndef BHARAT_THEME_H
#define BHARAT_THEME_H

/* The desktop's named colours and sizes (docs/design/UI.md, "Midnight
 * and ivory"). Screens use these names, never raw values, so the look
 * changes in one place.
 *
 * Calm dark slate behind everything, warm ivory windows on top, one
 * burnt-saffron colour for the single most important button, brighter
 * saffron only on dark (brand, caret, progress), teal for data. */

#define COL_WALL_TOP      0x121821u    /* wallpaper and splash, top */
#define COL_WALL_BOTTOM   0x26324Au    /* wallpaper, bottom */
#define COL_CHROME        0x0A0E14u    /* top bar and dock, drawn see-through */
#define COL_SURFACE       0xFBFAF7u    /* windows */
#define COL_SURFACE_2     0xF1EEE8u    /* quiet buttons, panels */
#define COL_LINE          0xE4E0D8u    /* hairlines, field borders */
#define COL_TEXT          0x1D232Bu
#define COL_MUTED         0x68707Cu    /* 4.6:1 on the surface */
#define COL_ON_DARK       0xECEAE4u    /* text on the wallpaper and bars */
#define COL_ON_DARK_MUTED 0x8C96A5u
#define COL_PRIMARY       0xB35F1Au    /* the one main button (ivory text 4.7:1) */
#define COL_SAFFRON       0xE39B4Eu    /* brand, caret, progress: on dark only */
#define COL_ACCENT_SOFT   0xF5E6D3u    /* selected rows */
#define COL_TEAL          0x2A7C76u    /* data */
#define COL_OK            0x3A9D6Bu
#define COL_WARN          0xC98A2Eu
#define COL_DANGER        0xD25C4Du
#define COL_TERM_BG       0x0F141Bu
#define COL_TERM_TITLE    0x171D26u
#define COL_TERM_TEXT     0xE6E8EBu

/* Layout (8 px grid). */
#define UI_TOPBAR_H     30
#define UI_TITLE_H      40
#define UI_PAD          16
#define UI_RADIUS       12             /* windows */
#define UI_RADIUS_SM    8              /* buttons, rows, fields */
#define UI_DOCK_H       80             /* the dock and the gap below it */

#endif /* BHARAT_THEME_H */
