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

uint32_t gfx_mix(uint32_t a, uint32_t b, unsigned alpha)
{
    uint32_t out = 0;
    for (unsigned sh = 0; sh <= 16; sh += 8) {
        uint32_t ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= ((ca * (255 - alpha) + cb * alpha + 127) / 255) << sh;
    }
    return out;
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

static inline uint32_t *at(int32_t x, int32_t y)
{
    return tgt->px + (int64_t)y * tgt->stride + x;
}

/* One pixel, `alpha` of the way to `rgb` (caller has clipped). */
static inline void put(int32_t x, int32_t y, uint32_t rgb, unsigned alpha)
{
    uint32_t *p = at(x, y);
    if (alpha >= 255)
        *p = gfx_pack(rgb);
    else if (alpha > 0)
        *p = gfx_pack(gfx_mix(gfx_unpack(*p), rgb, alpha));
}

void gfx_fill(struct gfx_rect r, uint32_t rgb)
{
    r = gfx_intersect(r, clip);
    if (gfx_empty(r))
        return;
    uint32_t v = gfx_pack(rgb);
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        uint32_t *p = at(r.x, y);
        for (int32_t x = 0; x < r.w; x++)
            p[x] = v;
    }
}

void gfx_blend(struct gfx_rect r, uint32_t rgb, unsigned alpha)
{
    r = gfx_intersect(r, clip);
    for (int32_t y = r.y; y < r.y + r.h; y++)
        for (int32_t x = r.x; x < r.x + r.w; x++)
            put(x, y, rgb, alpha);
}

void gfx_frame(struct gfx_rect r, uint32_t rgb)
{
    gfx_fill(gfx_rect(r.x, r.y, r.w, 1), rgb);
    gfx_fill(gfx_rect(r.x, r.y + r.h - 1, r.w, 1), rgb);
    gfx_fill(gfx_rect(r.x, r.y, 1, r.h), rgb);
    gfx_fill(gfx_rect(r.x + r.w - 1, r.y, 1, r.h), rgb);
}

void gfx_gradient(struct gfx_rect r, uint32_t top, uint32_t bottom)
{
    struct gfx_rect v = gfx_intersect(r, clip);
    for (int32_t y = v.y; y < v.y + v.h; y++) {
        unsigned a = r.h > 1 ? (unsigned)((y - r.y) * 255 / (r.h - 1)) : 0;
        gfx_fill(gfx_rect(v.x, y, v.w, 1), gfx_mix(top, bottom, a));
    }
}

/* How much of pixel (x, y) lies inside a circle of radius `rad` centred
 * on (cx, cy): 0..16, from a 4 x 4 grid of sample points. */
static unsigned corner_cover(int32_t x, int32_t y, int32_t cx, int32_t cy, int32_t rad)
{
    int64_t r8 = (int64_t)rad * 8, n = 0;
    for (int32_t i = 0; i < 4; i++)
        for (int32_t j = 0; j < 4; j++) {
            int64_t dx = (int64_t)x * 8 + 2 * i + 1 - (int64_t)cx * 8;
            int64_t dy = (int64_t)y * 8 + 2 * j + 1 - (int64_t)cy * 8;
            n += dx * dx + dy * dy <= r8 * r8;
        }
    return (unsigned)n;
}

void gfx_round(struct gfx_rect r, int32_t rad, uint32_t rgb, unsigned alpha)
{
    rad = min32(rad, min32(r.w, r.h) / 2);
    struct gfx_rect v = gfx_intersect(r, clip);
    for (int32_t y = v.y; y < v.y + v.h; y++) {
        bool top = y < r.y + rad, bottom = y >= r.y + r.h - rad;
        if (!top && !bottom) {
            if (alpha >= 255)
                gfx_fill(gfx_rect(v.x, y, v.w, 1), rgb);
            else
                gfx_blend(gfx_rect(v.x, y, v.w, 1), rgb, alpha);
            continue;
        }
        int32_t cy = top ? r.y + rad : r.y + r.h - rad;
        for (int32_t x = v.x; x < v.x + v.w; x++) {
            bool left = x < r.x + rad, right = x >= r.x + r.w - rad;
            unsigned cov = !left && !right ? 16
                : corner_cover(x, y, left ? r.x + rad : r.x + r.w - rad, cy, rad);
            put(x, y, rgb, alpha * cov / 16);
        }
    }
}

void gfx_shadow(struct gfx_rect r, int32_t rad, int32_t blur, int32_t dy, unsigned alpha)
{
    struct gfx_rect s = gfx_rect(r.x, r.y + dy, r.w, r.h);
    struct gfx_rect v = gfx_intersect(gfx_grow(s, blur), clip);
    int32_t ix0 = s.x + rad, ix1 = s.x + s.w - 1 - rad;
    int32_t iy0 = s.y + rad, iy1 = s.y + s.h - 1 - rad;
    for (int32_t y = v.y; y < v.y + v.h; y++) {
        int32_t qy = y < iy0 ? iy0 - y : y > iy1 ? y - iy1 : 0;
        for (int32_t x = v.x; x < v.x + v.w; x++) {
            if (gfx_inside(r, x, y)) {
                x = r.x + r.w - 1;               /* skip what the window covers */
                continue;
            }
            int32_t qx = x < ix0 ? ix0 - x : x > ix1 ? x - ix1 : 0;
            /* Distance outside the rounded shape; the length of (qx, qy)
             * is estimated as max + 3/8 min (within about 7%). */
            int32_t d = max32(qx, qy) + min32(qx, qy) * 3 / 8 - rad;
            if (d >= blur)
                continue;
            unsigned a = alpha;
            if (d > 0)
                a = (unsigned)((int64_t)alpha * (blur - d) * (blur - d) / ((int64_t)blur * blur));
            put(x, y, 0x000000, a);
        }
    }
}

/* ---- text ----------------------------------------------------------------- */

/* The built-in 8x8 font at double height, if the smooth fonts are missing. */
static int32_t text_fallback(int32_t x, int32_t y, const char *s, unsigned n, uint32_t rgb)
{
    int32_t x0 = x;
    for (unsigned i = 0; i < n && s[i] != '\0'; i++, x += 8) {
        unsigned char c = (unsigned char)s[i];
        const uint8_t *g = font8x8[c >= 0x20 && c < 0x7F ? c - 0x20 : '?' - 0x20];
        struct gfx_rect vis = gfx_intersect(gfx_rect(x, y, 8, 16), clip);
        for (int32_t py = vis.y; py < vis.y + vis.h; py++)
            for (int32_t px = vis.x; px < vis.x + vis.w; px++)
                if ((g[(py - y) / 2] >> (px - x)) & 1)
                    put(px, py, rgb, 255);
    }
    return x - x0;
}

int32_t gfx_text_n(int32_t x, int32_t y, const char *s, unsigned n, uint32_t rgb, enum font_id fid)
{
    if (tgt == NULL || s == NULL)
        return 0;
    const struct font *f = font_get(fid);
    if (f == NULL)
        return text_fallback(x, y, s, n, rgb);
    int32_t x0 = x;
    for (unsigned i = 0; i < n && s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        const struct glyph *g = &f->g[c >= 0x20 && c < 0x7F ? c - 0x20 : '?' - 0x20];
        struct gfx_rect box = gfx_rect(x + g->x, y + g->y, g->w, g->h);
        struct gfx_rect vis = gfx_intersect(box, clip);
        const uint8_t *cov = font_coverage(f, g);
        for (int32_t py = vis.y; py < vis.y + vis.h; py++)
            for (int32_t px = vis.x; px < vis.x + vis.w; px++)
                put(px, py, rgb, cov[(py - box.y) * g->w + (px - box.x)]);
        x += g->advance;
    }
    return x - x0;
}

int32_t gfx_text(int32_t x, int32_t y, const char *s, uint32_t rgb, enum font_id f)
{
    return gfx_text_n(x, y, s, ~0u, rgb, f);
}

int32_t gfx_text_width(const char *s, enum font_id fid)
{
    const struct font *f = font_get(fid);
    int32_t w = 0;
    for (; s != NULL && *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        w += f == NULL ? 8 : f->g[c >= 0x20 && c < 0x7F ? c - 0x20 : '?' - 0x20].advance;
    }
    return w;
}

int32_t gfx_line_height(enum font_id fid)
{
    const struct font *f = font_get(fid);
    return f != NULL ? f->line_h : 16;
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
