#include "font.h"
#include "string.h"

/* fontdata.asm: the bytes of fonts.bin. */
extern const uint8_t font_blob[], font_blob_end[], phrase_blob[], phrase_blob_end[];

static const struct font *fonts[FONT_COUNT];
static const uint8_t *coverage;

/* phrases.bin: "BPH1", count, then per phrase w, h, offset; then data. */
struct phrase_head {
    uint16_t w, h;
    uint32_t off;
};
static const struct phrase_head *phrases;
static const uint8_t *phrase_data;

static bool phrase_init(void)
{
    size_t size = (size_t)(phrase_blob_end - phrase_blob), heads = 8 + PHRASE_COUNT * sizeof(struct phrase_head);
    uint32_t count;
    memcpy(&count, phrase_blob + 4, 4);
    if (size < heads || memcmp(phrase_blob, "BPH1", 4) != 0 || count != PHRASE_COUNT)
        return false;
    const struct phrase_head *p = (const struct phrase_head *)(phrase_blob + 8);
    for (unsigned i = 0; i < PHRASE_COUNT; i++)
        if (p[i].w == 0 || p[i].h == 0 || p[i].h > 128 || (uint64_t)p[i].off + (uint64_t)p[i].w * p[i].h > size - heads)
            return false;
    phrases = p;
    phrase_data = phrase_blob + heads;
    return true;
}

const uint8_t *font_phrase(enum phrase_id id, uint16_t *w, uint16_t *h)
{
    if (phrases == NULL || (unsigned)id >= PHRASE_COUNT)
        return NULL;
    *w = phrases[id].w;
    *h = phrases[id].h;
    return phrase_data + phrases[id].off;
}

bool font_init(void)
{
    phrase_init();
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
