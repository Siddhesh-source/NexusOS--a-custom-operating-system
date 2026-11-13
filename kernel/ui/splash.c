#include "splash.h"
#include "theme.h"
#include "fbcon.h"
#include "serial.h"
#include "string.h"

static struct gfx_surface screen;
static bool have_screen, shown;
static unsigned done;
static int32_t area_h;              /* splash height; the boot log is below */

struct gfx_surface *splash_screen(void)
{
    return have_screen ? &screen : NULL;
}

unsigned splash_steps_done(void)
{
    return done;
}

static void draw_progress(const char *what)
{
    int32_t w = screen.w * 2 / 3, x = (screen.w - w) / 2, y = area_h - 72;
    gfx_target(&screen);
    gfx_frame(gfx_rect(x, y, w, 16), COL_BORDER);
    gfx_fill(gfx_rect(x + 1, y + 1, w - 2, 14), COL_SURFACE);
    gfx_fill(gfx_rect(x + 2, y + 2, (w - 4) * (int32_t)done / SPLASH_STEPS, 12), COL_SAFFRON);
    gfx_fill(gfx_rect(0, y + 24, screen.w, GFX_CHAR_H), COL_BG);
    if (what != NULL) {
        int32_t tw = (int32_t)strlen(what) * GFX_CHAR_W;
        gfx_text((screen.w - tw) / 2, y + 24, what, COL_MUTED, 1);
    }
}

void splash_start(void *addr, uint64_t width, uint64_t height, uint64_t pitch,
                  uint16_t bpp, uint8_t red_shift, uint8_t green_shift, uint8_t blue_shift)
{
    if (!fbcon_init(addr, width, height, pitch, bpp, red_shift, green_shift, blue_shift)) {
        serial_puts("[fbcon] framebuffer format not supported; serial only\n");
        return;
    }
    gfx_set_format(red_shift, green_shift, blue_shift);
    screen.px = addr;
    screen.w = (int32_t)width;
    screen.h = (int32_t)height;
    screen.stride = (int32_t)(pitch / 4);
    have_screen = true;
    if (width < 640 || height < 600)
        return;                          /* too small: text console only */

    /* The boot log keeps 25 rows (what the console promises); the
     * splash takes the rest, at most half the screen. */
    area_h = (int32_t)height - 25 * GFX_CHAR_H;
    if (area_h > (int32_t)height / 2)
        area_h = (int32_t)height / 2;
    if (!fbcon_set_top((uint64_t)area_h))
        return;
    gfx_target(&screen);
    gfx_fill(gfx_rect(0, 0, screen.w, area_h), COL_BG);
    gfx_fill(gfx_rect(0, area_h - 4, screen.w, 4), COL_SAFFRON);
    const char *name = "BharatOS";
    int32_t tw = (int32_t)strlen(name) * GFX_CHAR_W * 4;
    int32_t ty = area_h / 2 - 96;
    gfx_text((screen.w - tw) / 2, ty, name, COL_SAFFRON, 4);
    const char *sub = "an operating system written from scratch";
    gfx_text((screen.w - (int32_t)strlen(sub) * GFX_CHAR_W) / 2, ty + 72, sub, COL_TEXT, 1);
    shown = true;
    draw_progress("Starting");
}

void splash_step(const char *what)
{
    if (done < SPLASH_STEPS)
        done++;
    if (shown)
        draw_progress(what);
}
