#ifndef BHARAT_GFX_H
#define BHARAT_GFX_H

/* Graphics base: rectangles and text drawn into pixel surfaces.
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
 * Colours are given as 0xRRGGBB and packed into the screen's own pixel
 * layout (gfx_set_format) as they are drawn.
 *
 * Text uses the built-in 8x8 font at double height: one character is
 * 8 x 16 pixels at scale 1, (8s) x (16s) at scale s.
 *
 * Limits: one drawing thread at a time (the boot thread, then the
 * desktop); 32-bit pixels only; no transparency. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GFX_CHAR_W 8
#define GFX_CHAR_H 16

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
/* Draw `n` characters (stops early at a 0). Returns the width drawn. */
int32_t gfx_text_n(int32_t x, int32_t y, const char *s, unsigned n, uint32_t rgb, int32_t scale);
int32_t gfx_text(int32_t x, int32_t y, const char *s, uint32_t rgb, int32_t scale);

/* Copy rectangle `r` from `src` to the same place in `dst` (clipped to
 * both). The desktop's "present" step. */
void gfx_copy(struct gfx_surface *dst, const struct gfx_surface *src, struct gfx_rect r);

/* Colour (0xRRGGBB) of one pixel; 0 outside the surface. */
uint32_t gfx_get(const struct gfx_surface *s, int32_t x, int32_t y);

#endif /* BHARAT_GFX_H */
