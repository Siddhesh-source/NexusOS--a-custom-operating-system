#ifndef BHARAT_THEME_H
#define BHARAT_THEME_H

/* The desktop's named colours (docs/design/UI.md, light theme). Screens
 * use these names, never raw values, so the look changes in one place.
 * The terminal keeps a dark text area, like every terminal. */

#define COL_BG          0xF7F6F2u     /* desktop background */
#define COL_SURFACE     0xFFFFFFu     /* windows, panels */
#define COL_TEXT        0x1C1D21u
#define COL_MUTED       0x5B5E66u     /* secondary text */
#define COL_ACCENT      0x2F5BD3u     /* buttons, focus, selection */
#define COL_SAFFRON     0xE07A1Fu     /* brand mark */
#define COL_OK          0x1F8A4Cu
#define COL_WARN        0xB7791Fu
#define COL_DANGER      0xC53030u
#define COL_BORDER      0xDEDCD5u
#define COL_TERM_BG     0x16171Bu
#define COL_TERM_TEXT   0xECEDEFu
#define COL_SELECT      0xDCE5FBu     /* selected row: a pale accent */

/* Layout grid (UI.md): 8 px steps. */
#define UI_TOPBAR_H     32
#define UI_TITLE_H      28
#define UI_PAD          8

#endif /* BHARAT_THEME_H */
