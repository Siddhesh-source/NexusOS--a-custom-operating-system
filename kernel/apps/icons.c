/* Dock icons: simple shapes on each app's tile, drawn with the same
 * rounded rectangles and text as everything else (no image files). */

#include "apps.h"
#include "theme.h"
#include "string.h"

static void centred(struct gfx_rect t, const char *s, uint32_t rgb, enum font_id f)
{
    gfx_text(t.x + (t.w - gfx_text_width(s, f)) / 2, t.y + (t.h - gfx_line_height(f)) / 2, s, rgb, f);
}

void icon_terminal(struct gfx_rect t)
{
    centred(t, ">_", COL_SAFFRON, FONT_MEDIUM);
}

void icon_files(struct gfx_rect t)
{
    gfx_round(gfx_rect(t.x + 10, t.y + 12, 12, 8), 2, 0xC98A3A, 255);      /* folder tab */
    gfx_round(gfx_rect(t.x + 10, t.y + 15, 24, 17), 3, 0xE0A458, 255);
}

void icon_monitor(struct gfx_rect t)
{
    static const int32_t h[3] = { 10, 20, 14 };
    for (int32_t i = 0; i < 3; i++)
        gfx_round(gfx_rect(t.x + 11 + i * 8, t.y + 33 - h[i], 6, h[i]), 2, COL_SURFACE, 255);
}

void icon_health(struct gfx_rect t)
{
    gfx_round(gfx_rect(t.x + 19, t.y + 11, 6, 22), 2, COL_SURFACE, 255);
    gfx_round(gfx_rect(t.x + 11, t.y + 19, 22, 6), 2, COL_SURFACE, 255);
}

void icon_about(struct gfx_rect t)
{
    centred(t, "i", COL_SURFACE, FONT_HEADING);
}

void icon_code(struct gfx_rect t)
{
    centred(t, "</>", COL_SURFACE, FONT_MEDIUM);
}

void icon_player(struct gfx_rect t)
{
    /* a play triangle, drawn as narrowing bars */
    for (int32_t i = 0; i < 16; i++) {
        int32_t h = 22 - (i * 22) / 16;
        gfx_fill(gfx_rect(t.x + 16 + i, t.y + t.h / 2 - h / 2, 1, h), COL_SAFFRON);
    }
}

void icon_browser(struct gfx_rect t)
{
    struct gfx_rect g = gfx_rect(t.x + t.w / 2 - 13, t.y + t.h / 2 - 13, 26, 26);
    gfx_round(g, 13, COL_SURFACE, 255);
    gfx_round(gfx_grow(g, -2), 11, 0x2A7C76, 255);
    gfx_fill(gfx_rect(g.x + 2, g.y + 12, 22, 2), COL_SURFACE);           /* equator */
    gfx_fill(gfx_rect(g.x + 12, g.y + 2, 2, 22), COL_SURFACE);           /* meridian */
}

const struct app *app_for_file(const char *path)
{
    const char *dot = path != NULL ? strrchr(path, '.') : NULL;
    if (dot == NULL)
        return &app_editor;
    if (strcmp(dot, ".bvid") == 0)
        return &app_player;
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return &app_browser;
    if (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 || strcmp(dot, ".py") == 0
        || strcmp(dot, ".sh") == 0 || strcmp(dot, ".md") == 0)
        return &app_code;
    return &app_editor;
}
