#include "font.h"
#include "string.h"

/* fontdata.asm: the bytes of fonts.bin. */
extern const uint8_t font_blob[], font_blob_end[];

static const struct font *fonts[FONT_COUNT];
static const uint8_t *coverage;

bool font_init(void)
{
    size_t size = (size_t)(font_blob_end - font_blob);
    size_t heads = 8 + FONT_COUNT * sizeof(struct font);
    uint32_t count;
    memcpy(&count, font_blob + 4, 4);
    if (size < heads || memcmp(font_blob, "BFN1", 4) != 0 || count != FONT_COUNT)
        return false;
    size_t data = size - heads;
    const struct font *f = (const struct font *)(font_blob + 8);
    for (unsigned i = 0; i < FONT_COUNT; i++) {
        if (f[i].line_h == 0 || f[i].line_h > 128)
            return false;
        for (unsigned c = 0; c < 95; c++) {
            const struct glyph *g = &f[i].g[c];
            if ((uint64_t)g->off + (uint64_t)g->w * g->h > data)
                return false;               /* would read past the end */
        }
    }
    if (f[FONT_MONO].mono_adv != FONT_MONO_W || f[FONT_MONO].line_h != FONT_MONO_H)
        return false;                       /* apps size windows from these */
    coverage = font_blob + heads;
    for (unsigned i = 0; i < FONT_COUNT; i++)
        fonts[i] = &f[i];
    return true;
}

const struct font *font_get(enum font_id id)
{
    return (unsigned)id < FONT_COUNT ? fonts[id] : NULL;
}

const uint8_t *font_coverage(const struct font *f, const struct glyph *g)
{
    (void)f;
    return coverage + g->off;
}
