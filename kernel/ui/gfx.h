#ifndef BHARAT_GFX_H
#define BHARAT_GFX_H

/* Graphics base: shapes and text drawn into pixel surfaces.
 *
 * A surface is a block of 32-bit pixels: the screen itself (the
 * bootloader's framebuffer) or a "back buffer" in ordinary memory. The
 * desktop draws only into its back buffer, then copies the parts that
 * changed (the damage rectangle) to the screen with gfx_copy(). Drawing
 * in normal memory and copying once is faster than drawing on the screen
 * piece by piece, and the screen never shows a half-drawn window.
 *
 * Drawing goes to one target surface at a time and is clipped to a clip
 * rectangle: nothing outside it changes. The desktop sets the clip to the
 * damage rectangle, so a redraw only touches what changed.
 *
 * Smooth edges come from blending: a pixel that is partly covered (a
 * rounded corner, the edge of a letter, a shadow) gets a mix of the new
 * colour and the colour already there, in proportion to "alpha" (0 =
 * keep the old colour, 255 = all new). Everything is integer maths: the
 * kernel has no floating point.
 *
 * Colours are given as 0xRRGGBB and packed into the screen's own pixel
 * layout (gfx_set_format) as they are drawn.
 *
 * Limits: one drawing thread at a time (the boot thread, then the
 * desktop); 32-bit pixels only. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "font.h"

struct gfx_rect {
    int32_t x, y, w, h;
};

struct gfx_surface {
    uint32_t *px;
    int32_t w, h;
    int32_t stride;              /* pixels from one row to the next */
};

/* Pixel layout: the bit position of red, green and blue. */
void gfx_set_format(uint8_t red_shift, uint8_t green_shift, uint8_t blue_shift);
uint32_t gfx_pack(uint32_t rgb);
uint32_t gfx_unpack(uint32_t px);
/* Mix two colours: alpha 0 gives `a`, 255 gives `b`. */
uint32_t gfx_mix(uint32_t a, uint32_t b, unsigned alpha);

/* Rectangle helpers. An empty rectangle has w <= 0 or h <= 0. */
static inline struct gfx_rect gfx_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct gfx_rect r = { x, y, w, h };
    return r;
}
static inline bool gfx_empty(struct gfx_rect r) { return r.w <= 0 || r.h <= 0; }
static inline bool gfx_inside(struct gfx_rect r, int32_t x, int32_t y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static inline struct gfx_rect gfx_grow(struct gfx_rect r, int32_t by)
{
    return gfx_rect(r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by);
}
struct gfx_rect gfx_intersect(struct gfx_rect a, struct gfx_rect b);
/* Smallest rectangle holding both (an empty one is ignored). */
struct gfx_rect gfx_union(struct gfx_rect a, struct gfx_rect b);

/* Where drawing goes, and the part of it that may change. Setting a
 * target resets the clip to the whole surface. */
void gfx_target(struct gfx_surface *s);
void gfx_clip(struct gfx_rect clip);
struct gfx_rect gfx_get_clip(void);

void gfx_fill(struct gfx_rect r, uint32_t rgb);
void gfx_frame(struct gfx_rect r, uint32_t rgb);          /* 1-pixel outline */
/* See-through fill: `alpha` of `rgb` over what is there. */
void gfx_blend(struct gfx_rect r, uint32_t rgb, unsigned alpha);
/* Rounded rectangle with smooth corners (radius 0: square). */
void gfx_round(struct gfx_rect r, int32_t radius, uint32_t rgb, unsigned alpha);
/* Top-to-bottom colour fade. */
void gfx_gradient(struct gfx_rect r, uint32_t top, uint32_t bottom);
/* A soft shadow under rounded rectangle `r`, moved down by `dy`, fading
 * out over `blur` pixels, darkest `alpha`. The inside of `r` is left
 * alone (the window covers it). */
void gfx_shadow(struct gfx_rect r, int32_t radius, int32_t blur, int32_t dy, unsigned alpha);

/* Text, `y` = top of the line. Draws up to `n` characters (stops early at
 * a 0). Returns the width drawn. */
int32_t gfx_text_n(int32_t x, int32_t y, const char *s, unsigned n, uint32_t rgb, enum font_id f);
int32_t gfx_text(int32_t x, int32_t y, const char *s, uint32_t rgb, enum font_id f);
int32_t gfx_text_width(const char *s, enum font_id f);
int32_t gfx_line_height(enum font_id f);
/* A pre-shaped Hindi/Marathi phrase (font.h), top-left at (x, y). Returns
 * its width; 0 (nothing drawn) for an unknown phrase. */
int32_t gfx_phrase(int32_t x, int32_t y, enum phrase_id id, uint32_t rgb);
int32_t gfx_phrase_width(enum phrase_id id);

/* An image of w x h palette indexes (one byte per pixel), each pixel
 * drawn as a scale x scale square. `packed` holds 256 colours already
 * packed with gfx_pack (the Player's video frames). Clipped like
 * everything else. */
void gfx_image8(int32_t x, int32_t y, const uint8_t *idx, int32_t w, int32_t h, int32_t scale,
                const uint32_t *packed);

/* Copy rectangle `r` from `src` to the same place in `dst` (clipped to
 * both). The desktop's "present" step. */
void gfx_copy(struct gfx_surface *dst, const struct gfx_surface *src, struct gfx_rect r);

/* Colour (0xRRGGBB) of one pixel; 0 outside the surface. */
uint32_t gfx_get(const struct gfx_surface *s, int32_t x, int32_t y);

#endif /* BHARAT_GFX_H */
