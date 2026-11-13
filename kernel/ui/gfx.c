#include "gfx.h"
#include "font8x8.h"

static uint8_t rs = 16, gs = 8, bs = 0;
static struct gfx_surface *tgt;
static struct gfx_rect clip;

void gfx_set_format(uint8_t red_shift, uint8_t green_shift, uint8_t blue_shift)
{
    rs = red_shift;
    gs = green_shift;
    bs = blue_shift;
}

uint32_t gfx_pack(uint32_t rgb)
{
    return (((rgb >> 16) & 0xFF) << rs) | (((rgb >> 8) & 0xFF) << gs) | ((rgb & 0xFF) << bs);
}

uint32_t gfx_unpack(uint32_t v)
{
    return (((v >> rs) & 0xFF) << 16) | (((v >> gs) & 0xFF) << 8) | ((v >> bs) & 0xFF);
}

static int32_t max32(int32_t a, int32_t b) { return a > b ? a : b; }
static int32_t min32(int32_t a, int32_t b) { return a < b ? a : b; }

struct gfx_rect gfx_intersect(struct gfx_rect a, struct gfx_rect b)
{
    int32_t x0 = max32(a.x, b.x), y0 = max32(a.y, b.y);
    int32_t x1 = min32(a.x + a.w, b.x + b.w), y1 = min32(a.y + a.h, b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return gfx_rect(0, 0, 0, 0);
    return gfx_rect(x0, y0, x1 - x0, y1 - y0);
}

struct gfx_rect gfx_union(struct gfx_rect a, struct gfx_rect b)
{
    if (gfx_empty(a))
        return b;
    if (gfx_empty(b))
        return a;
    int32_t x0 = min32(a.x, b.x), y0 = min32(a.y, b.y);
    int32_t x1 = max32(a.x + a.w, b.x + b.w), y1 = max32(a.y + a.h, b.y + b.h);
    return gfx_rect(x0, y0, x1 - x0, y1 - y0);
}

void gfx_target(struct gfx_surface *s)
{
    tgt = s;
    clip = s != NULL ? gfx_rect(0, 0, s->w, s->h) : gfx_rect(0, 0, 0, 0);
}

void gfx_clip(struct gfx_rect c)
{
    clip = tgt != NULL ? gfx_intersect(c, gfx_rect(0, 0, tgt->w, tgt->h)) : gfx_rect(0, 0, 0, 0);
}

struct gfx_rect gfx_get_clip(void)
{
    return clip;
}

void gfx_fill(struct gfx_rect r, uint32_t rgb)
{
    r = gfx_intersect(r, clip);
    if (gfx_empty(r))
        return;
    uint32_t v = gfx_pack(rgb);
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        uint32_t *p = tgt->px + (int64_t)y * tgt->stride + r.x;
        for (int32_t x = 0; x < r.w; x++)
            p[x] = v;
    }
}

void gfx_frame(struct gfx_rect r, uint32_t rgb)
{
    gfx_fill(gfx_rect(r.x, r.y, r.w, 1), rgb);
    gfx_fill(gfx_rect(r.x, r.y + r.h - 1, r.w, 1), rgb);
    gfx_fill(gfx_rect(r.x, r.y, 1, r.h), rgb);
    gfx_fill(gfx_rect(r.x + r.w - 1, r.y, 1, r.h), rgb);
}

static void glyph(int32_t x0, int32_t y0, unsigned char c, uint32_t v, int32_t s)
{
    struct gfx_rect cell = gfx_rect(x0, y0, GFX_CHAR_W * s, GFX_CHAR_H * s);
    struct gfx_rect vis = gfx_intersect(cell, clip);
    if (gfx_empty(vis))
        return;                              /* the common case when clipped */
    const uint8_t *g = font8x8[c >= 0x20 && c < 0x7F ? c - 0x20 : '?' - 0x20];
    for (int32_t y = vis.y; y < vis.y + vis.h; y++) {
        uint8_t bits = g[(y - y0) / (2 * s)];
        if (bits == 0)
            continue;
        uint32_t *p = tgt->px + (int64_t)y * tgt->stride;
        for (int32_t x = vis.x; x < vis.x + vis.w; x++)
            if ((bits >> ((x - x0) / s)) & 1)
                p[x] = v;
    }
}

int32_t gfx_text_n(int32_t x, int32_t y, const char *s, unsigned n, uint32_t rgb, int32_t scale)
{
    if (tgt == NULL || s == NULL || scale < 1)
        return 0;
    uint32_t v = gfx_pack(rgb);
    int32_t x0 = x;
    for (unsigned i = 0; i < n && s[i] != '\0'; i++) {
        glyph(x, y, (unsigned char)s[i], v, scale);
        x += GFX_CHAR_W * scale;
    }
    return x - x0;
}

int32_t gfx_text(int32_t x, int32_t y, const char *s, uint32_t rgb, int32_t scale)
{
    return gfx_text_n(x, y, s, ~0u, rgb, scale);
}

void gfx_copy(struct gfx_surface *dst, const struct gfx_surface *src, struct gfx_rect r)
{
    if (dst == NULL || src == NULL)
        return;
    r = gfx_intersect(r, gfx_rect(0, 0, dst->w, dst->h));
    r = gfx_intersect(r, gfx_rect(0, 0, src->w, src->h));
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        uint32_t *d = dst->px + (int64_t)y * dst->stride + r.x;
        const uint32_t *s = src->px + (int64_t)y * src->stride + r.x;
        for (int32_t x = 0; x < r.w; x++)
            d[x] = s[x];
    }
}

uint32_t gfx_get(const struct gfx_surface *s, int32_t x, int32_t y)
{
    if (s == NULL || x < 0 || y < 0 || x >= s->w || y >= s->h)
        return 0;
    return gfx_unpack(s->px[(int64_t)y * s->stride + x]);
}
