#ifndef BHARAT_FONT_H
#define BHARAT_FONT_H

/* Smoothed fonts for the desktop.
 *
 * The glyphs are drawn ahead of time (scripts/make_fonts.py, from IBM
 * Plex Sans and IBM Plex Mono, SIL Open Font License) into
 * kernel/ui/fonts.bin, which is linked into the kernel as data
 * (fontdata.asm). Each glyph is a block of coverage values: 0 = none of
 * the pixel is ink, 255 = all of it. gfx blends the text colour with what
 * is already there by that amount, which is what makes edges smooth.
 *
 * If the data is missing or damaged, font_get() returns NULL and gfx
 * falls back to the built-in 8x8 font, so text never disappears.
 *
 * Hindi and Marathi: Devanagari letters join and change shape, so whole
 * phrases are shaped ahead of time by the same script (Noto Sans
 * Devanagari, OFL) into phrases.bin, and drawn as finished pictures
 * (gfx_phrase). There is no shaping engine in the kernel (decision D16).
 *
 * Limits: printable ASCII only for typed text; Devanagari only for the
 * fixed phrases below; no kerning; sizes fixed when the files are made. */

#include <stdbool.h>
#include <stdint.h>

enum font_id {
    FONT_SMALL,          /* 13 px: labels, captions */
    FONT_UI,             /* 15 px: body text */
    FONT_MEDIUM,         /* 15 px medium: titles, buttons */
    FONT_HEADING,        /* 22 px semibold */
    FONT_DISPLAY,        /* 56 px light: wordmark, clock */
    FONT_MONO,           /* 15 px monospace: terminal, editor */
    FONT_COUNT
};

/* The monospace cell, fixed so app sizes can be constants. */
#define FONT_MONO_W 9
#define FONT_MONO_H 19

struct glyph {
    int16_t x, y;        /* where the coverage block starts, from the pen
                            position and the top of the line */
    uint16_t w, h;
    uint16_t advance;    /* pen movement */
    uint16_t pad;
    uint32_t off;        /* into the coverage data */
};

struct font {
    uint16_t line_h, baseline, mono_adv, pad;
    struct glyph g[95];  /* 0x20..0x7E */
};

/* Fixed Hindi (HI) and Marathi (MR) phrases, in phrases.bin order. */
enum phrase_id {
    PHRASE_HI_GREETING, PHRASE_MR_GREETING, PHRASE_HI_NAME, PHRASE_MR_NAME, PHRASE_PASSWORD,
    PHRASE_HI_ENTER, PHRASE_MR_ENTER, PHRASE_HINDI, PHRASE_MARATHI,
    PHRASE_COUNT
};

/* Check the font data; false (and NULL fonts) if it is damaged. Also
 * checks the phrases (missing or damaged phrases are simply not drawn). */
bool font_init(void);
/* A phrase's coverage picture (w x h bytes), or NULL. */
const uint8_t *font_phrase(enum phrase_id id, uint16_t *w, uint16_t *h);
const struct font *font_get(enum font_id id);
const uint8_t *font_coverage(const struct font *f, const struct glyph *g);

#endif /* BHARAT_FONT_H */
