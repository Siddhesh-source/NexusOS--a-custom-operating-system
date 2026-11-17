#include "splash.h"
#include "theme.h"
#include "fbcon.h"
#include "serial.h"
#include "string.h"
#include "kprintf.h"

static struct gfx_surface screen;
static bool have_screen, shown;
static unsigned done;
static int32_t area_h;              /* splash height; the boot log is below */
static uint64_t boot_ms;

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
    int32_t w = 320, x = (screen.w - w) / 2, y = area_h - 64;
    gfx_target(&screen);
    gfx_fill(gfx_rect(x - 4, y - 4, w + 8, 12), COL_WALL_TOP);
    gfx_round(gfx_rect(x, y, w, 4), 2, 0x2B3648, 255);
    gfx_round(gfx_rect(x, y, w * (int32_t)done / SPLASH_STEPS, 4), 2, COL_SAFFRON, 255);
    gfx_fill(gfx_rect(0, y + 16, screen.w, gfx_line_height(FONT_SMALL)), COL_WALL_TOP);
    if (what != NULL)
        gfx_text((screen.w - gfx_text_width(what, FONT_SMALL)) / 2, y + 16, what,
                 COL_ON_DARK_MUTED, FONT_SMALL);
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

    /* The boot log keeps 25 rows (what the console promises), dimmed into
     * the same dark background; the splash takes the rest, at most half
     * the screen. */
    area_h = (int32_t)height - 25 * 16;
    if (area_h > (int32_t)height / 2)
        area_h = (int32_t)height / 2;
    if (!fbcon_set_top((uint64_t)area_h))
        return;
    gfx_target(&screen);
    gfx_fill(gfx_rect(0, 0, screen.w, area_h), COL_WALL_TOP);
    const char *name = "BharatOS";
    int32_t ty = area_h / 2 - 96;
    gfx_text((screen.w - gfx_text_width(name, FONT_DISPLAY)) / 2, ty, name, COL_ON_DARK, FONT_DISPLAY);
    const char *sub = "an operating system written from scratch";
    gfx_text((screen.w - gfx_text_width(sub, FONT_UI)) / 2, ty + 76, sub, COL_ON_DARK_MUTED, FONT_UI);
    shown = true;
    draw_progress("Starting");
}

uint64_t splash_boot_ms(void)
{
    return boot_ms;
}

void splash_finish(uint64_t ms, unsigned checks)
{
    char s[80];
    boot_ms = ms;
    ksnprintf(s, sizeof(s), "Ready: booted in %lu.%lu s, %u self-checks passed", ms / 1000, (ms / 100) % 10, checks);
    splash_step(s);
}

void splash_step(const char *what)
{
    if (done < SPLASH_STEPS)
        done++;
    if (shown)
        draw_progress(what);
}
