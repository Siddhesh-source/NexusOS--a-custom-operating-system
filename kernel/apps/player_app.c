/* Player: plays a .bvid video (scripts/make_video.py) from the files.
 *
 * A .bvid is a header, one 256-colour palette and then raw frames of one
 * byte per pixel, so playing needs no codec: each frame is a table lookup
 * per pixel, drawn three times bigger (gfx_image8). The clock decides
 * which frame to show (frame = time since Play x frames per second), so
 * the video keeps its speed even if the desktop is busy and some frames
 * are skipped. Only the picture and the progress bar are redrawn.
 *
 * Buttons: Play/Pause, Restart, Back 2 s, Forward 2 s. Keys: Space
 * (play/pause), Left/Right (2 s back/forward), Home (restart). Clicking
 * the progress bar jumps there. The video loops at the end.
 *
 * Limits: no sound; the whole file is read into memory when opened
 * (videos are at most FS_FILE_MAX bytes). */

#include "apps.h"
#include "theme.h"
#include "fs.h"
#include "input.h"
#include "heap.h"
#include "clock.h"
#include "kprintf.h"
#include "string.h"

#define SCALE    3
#define VID_W    192                 /* the window is sized for 192 x 108 */
#define VID_H    108
#define HDR      32
#define PAL      (256 * 3)
#define PIC_Y    (UI_BAR_H)
#define BAR_Y    (PIC_Y + VID_H * SCALE + 14)

struct player {
    char path[FS_PATH_MAX];
    uint8_t *data;
    size_t size;
    uint16_t w, h, fps, frames;
    uint32_t packed[256];
    bool playing;
    uint64_t base_ns;                /* when frame 0 would have been shown */
    unsigned frame, shown;           /* current frame; frames drawn so far */
    char msg[80];
};

static const char *const player_buttons[] = { "Pause", "Restart", "< 2 s", "2 s >" };
static const char *const player_buttons_paused[] = { "Play", "Restart", "< 2 s", "2 s >" };

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Check a .bvid file in memory; true if every frame is inside it. */
bool player_parse(const uint8_t *d, size_t size, uint16_t *w, uint16_t *h, uint16_t *fps,
                  uint16_t *frames)
{
    if (d == NULL || size < HDR + PAL || memcmp(d, "BVID", 4) != 0)
        return false;
    *w = rd16(d + 4);
    *h = rd16(d + 6);
    *fps = rd16(d + 8);
    *frames = rd16(d + 10);
    if (*w == 0 || *h == 0 || *w > VID_W || *h > VID_H || *fps == 0 || *fps > 60 || *frames == 0)
        return false;
    return (uint64_t)*w * *h * *frames <= size - HDR - PAL;
}

static void set_frame_clock(struct player *p)
{
    p->base_ns = clock_now_ns() - (uint64_t)p->frame * 1000000000ull / p->fps;
}

static bool player_open(struct window *w, const char *arg)
{
    struct player *p = kzalloc(sizeof(*p));
    if (p == NULL)
        return false;
    strlcpy(p->path, arg != NULL ? arg : "/media/bunny.bvid", sizeof(p->path));
    struct fs_stat s;
    if (fs_stat(p->path, &s) != FS_OK || s.type != FS_FILE || s.size == 0
        || (p->data = kmalloc(s.size)) == NULL
        || fs_read_file(p->path, p->data, s.size, &p->size) != FS_OK
        || !player_parse(p->data, p->size, &p->w, &p->h, &p->fps, &p->frames)) {
        kfree(p->data);
        kfree(p);
        return false;
    }
    for (unsigned i = 0; i < 256; i++) {
        const uint8_t *c = p->data + HDR + i * 3;
        p->packed[i] = gfx_pack(((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2]);
    }
    w->st = p;
    const char *name = strrchr(p->path, '/');
    char t[48];
    ksnprintf(t, sizeof(t), "Player: %s", name != NULL ? name + 1 : p->path);
    wm_set_title(w, t);
    p->playing = true;
    set_frame_clock(p);
    return true;
}

static void player_close(struct window *w)
{
    kfree(((struct player *)w->st)->data);
}

static struct gfx_rect pic_rect(struct gfx_rect body)
{
    return gfx_rect(body.x + UI_PAD, body.y + PIC_Y, VID_W * SCALE, VID_H * SCALE);
}

static struct gfx_rect bar_rect(struct gfx_rect body)
{
    return gfx_rect(body.x + UI_PAD, body.y + BAR_Y, VID_W * SCALE, 8);
}

static void player_draw(struct window *w, struct gfx_rect body)
{
    struct player *p = w->st;
    ui_buttons(body, p->playing ? player_buttons : player_buttons_paused, 4, 0, true, -1, -1);
    struct gfx_rect pic = pic_rect(body);
    gfx_fill(pic, 0x000000);
    const uint8_t *frame = p->data + HDR + PAL + (size_t)p->frame * p->w * p->h;
    gfx_image8(pic.x + (pic.w - p->w * SCALE) / 2, pic.y + (pic.h - p->h * SCALE) / 2,
               frame, p->w, p->h, SCALE, p->packed);
    p->shown++;
    struct gfx_rect bar = bar_rect(body);
    gfx_round(bar, 4, COL_LINE, 255);
    int32_t done = (int32_t)((uint64_t)bar.w * (p->frame + 1) / p->frames);
    gfx_round(gfx_rect(bar.x, bar.y, done, bar.h), 4, COL_PRIMARY, 255);
    unsigned now = p->frame / p->fps, total = p->frames / p->fps;
    char t[64];
    ksnprintf(t, sizeof(t), "%u:%02u / %u:%02u    %ux%u, %u frames a second", now / 60, now % 60,
              total / 60, total % 60, p->w, p->h, p->fps);
    gfx_text(bar.x, bar.y + 16, t, COL_MUTED, FONT_SMALL);
    const char *credit = "Big Buck Bunny (c) Blender Foundation, CC BY 3.0";
    if (p->msg[0] != '\0')
        credit = p->msg;
    gfx_text(bar.x + bar.w - gfx_text_width(credit, FONT_SMALL), bar.y + 16, credit, COL_MUTED, FONT_SMALL);
}

static void seek(struct player *p, int delta_frames)
{
    int f = (int)p->frame + delta_frames;
    if (f < 0)
        f = 0;
    if (f >= p->frames)
        f = p->frames - 1;
    p->frame = (unsigned)f;
    set_frame_clock(p);
}

static void action(struct window *w, int which)
{
    struct player *p = w->st;
    switch (which) {
    case 0:
        p->playing = !p->playing;
        if (p->playing)
            set_frame_clock(p);
        break;
    case 1: p->frame = 0; set_frame_clock(p); break;
    case 2: seek(p, -2 * p->fps); break;
    case 3: seek(p, 2 * p->fps); break;
    }
    wm_redraw(w);
}

static void player_key(struct window *w, uint16_t key)
{
    if (key == ' ')
        action(w, 0);
    else if (key == INPUT_KEY_HOME)
        action(w, 1);
    else if (key == INPUT_KEY_LEFT)
        action(w, 2);
    else if (key == INPUT_KEY_RIGHT)
        action(w, 3);
}

static void player_click(struct window *w, int32_t x, int32_t y)
{
    struct player *p = w->st;
    struct gfx_rect b = wm_body(w);
    int hit = ui_buttons(gfx_rect(0, 0, b.w, b.h), p->playing ? player_buttons : player_buttons_paused,
                         4, 0, false, x, y);
    struct gfx_rect bar = bar_rect(gfx_rect(0, 0, b.w, b.h));
    if (hit >= 0) {
        action(w, hit);
    } else if (gfx_inside(gfx_grow(bar, 6), x, y)) {
        int32_t at = x - bar.x;
        at = at < 0 ? 0 : at >= bar.w ? bar.w - 1 : at;
        p->frame = (unsigned)((uint64_t)at * p->frames / (uint64_t)bar.w);
        set_frame_clock(p);
        wm_redraw(w);
    } else if (gfx_inside(pic_rect(gfx_rect(0, 0, b.w, b.h)), x, y)) {
        action(w, 0);                    /* click the picture: play/pause */
    }
}

/* Every desktop frame: move to the frame the clock says, redraw only the
 * picture and the progress line under it. */
static void player_anim(struct window *w)
{
    struct player *p = w->st;
    if (!p->playing)
        return;
    uint64_t f = (clock_now_ns() - p->base_ns) * p->fps / 1000000000ull;
    if (f >= p->frames) {                 /* loop */
        p->frame = 0;
        set_frame_clock(p);
    } else if (f == p->frame) {
        return;
    } else {
        p->frame = (unsigned)f;
    }
    struct gfx_rect body = wm_body(w);
    struct gfx_rect pic = pic_rect(body);
    wm_damage(gfx_rect(pic.x, pic.y, pic.w, BAR_Y - PIC_Y + 34));
}

/* Self-tests: the current frame and how many have been drawn. */
unsigned player_frame(struct window *w, unsigned *shown, bool *playing)
{
    if (w == NULL || w->app != &app_player)
        return 0;
    struct player *p = w->st;
    if (shown != NULL)
        *shown = p->shown;
    if (playing != NULL)
        *playing = p->playing;
    return p->frame;
}

const struct app app_player = {
    .name = "Player",
    .w = VID_W * SCALE + 2 * UI_PAD,
    .h = BAR_Y + 44,
    .dark = false,
    .tile = 0x2B1E3A,
    .icon = icon_player,
    .open = player_open,
    .close = player_close,
    .draw = player_draw,
    .key = player_key,
    .click = player_click,
    .anim = player_anim,
};
