/* Player: plays .bvid videos (scripts/make_video.py) from the files.
 *
 * A .bvid is a header, one 256-colour palette and then raw frames of one
 * byte per pixel, so playing needs no codec: each frame is a table lookup
 * per pixel, drawn three times bigger (gfx_image8). The clock decides
 * which frame to show (frame = time since Play x frames per second), so
 * the video keeps its speed even if the desktop is busy and some frames
 * are skipped. Only the picture and the progress bar are redrawn.
 *
 * Playlist: every .bvid in the folder of the video opened, in name
 * order. At the end of a video the next one starts (after the last, the
 * first). A video that cannot be read is skipped with a message.
 *
 * Buttons: Play/Pause, Restart, Back 2 s, Forward 2 s, Next. Keys: Space
 * (play/pause), Left/Right (2 s back/forward), Home (restart), End (the
 * last frame), N/P (next/previous video), F (full screen: the picture as
 * big as the screen allows, no buttons; F or Esc again to leave).
 * Clicking the progress bar jumps there.
 *
 * Limits: no sound; a whole video is read into memory when it starts
 * (videos are at most FS_FILE_MAX bytes); PLAYER_LIST_MAX videos in the
 * playlist. */

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
    char dir[FS_PATH_MAX];
    char list[PLAYER_LIST_MAX][FS_NAME_MAX + 1];
    unsigned count, at;              /* playlist and the video playing */
    uint8_t *data;
    size_t size;
    uint16_t w, h, fps, frames;
    uint32_t packed[256];
    bool playing;
    uint64_t base_ns;                /* when frame 0 would have been shown */
    unsigned frame, shown;           /* current frame; frames drawn so far */
    char msg[80];
};

static const char *const player_buttons[] = { "Pause", "Restart", "< 2 s", "2 s >", "Next" };
static const char *const player_buttons_paused[] = { "Play", "Restart", "< 2 s", "2 s >", "Next" };

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

static bool is_video(const char *name)
{
    size_t n = strlen(name);
    return n > 5 && strcmp(name + n - 5, ".bvid") == 0;
}

/* The playlist: the .bvid files in p->dir, sorted by name. */
static void load_list(struct player *p)
{
    struct fs_dirent e;
    p->count = 0;
    for (unsigned i = 0; fs_list(p->dir, i, &e) == FS_OK && p->count < PLAYER_LIST_MAX; i++) {
        if (e.type != FS_FILE || !is_video(e.name))
            continue;
        unsigned k = p->count++;
        while (k > 0 && strcmp(p->list[k - 1], e.name) > 0) {
            strlcpy(p->list[k], p->list[k - 1], sizeof(p->list[k]));
            k--;
        }
        strlcpy(p->list[k], e.name, sizeof(p->list[k]));
    }
}

/* Read and check one video; the old one is kept if this one is bad. */
static bool load_video(struct window *w, const char *path)
{
    struct player *p = w->st;
    struct fs_stat s;
    uint8_t *data = NULL;
    size_t size = 0;
    uint16_t vw, vh, fps, frames;
    if (fs_stat(path, &s) != FS_OK || s.type != FS_FILE || s.size == 0
        || (data = kmalloc(s.size)) == NULL || fs_read_file(path, data, s.size, &size) != FS_OK
        || !player_parse(data, size, &vw, &vh, &fps, &frames)) {
        kfree(data);
        return false;
    }
    kfree(p->data);
    p->data = data;
    p->size = size;
    p->w = vw;
    p->h = vh;
    p->fps = fps;
    p->frames = frames;
    for (unsigned i = 0; i < 256; i++) {
        const uint8_t *c = p->data + HDR + i * 3;
        p->packed[i] = gfx_pack(((uint32_t)c[0] << 16) | ((uint32_t)c[1] << 8) | c[2]);
    }
    const char *name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;
    for (unsigned i = 0; i < p->count; i++)
        if (strcmp(p->list[i], name) == 0)
            p->at = i;
    char t[48];
    ksnprintf(t, sizeof(t), "Player: %s", name);
    wm_set_title(w, t);
    p->frame = 0;
    p->playing = true;
    set_frame_clock(p);
    wm_redraw(w);
    return true;
}

/* Move `step` places along the playlist, skipping videos that cannot be
 * played (and saying so). */
static void next_video(struct window *w, int step)
{
    struct player *p = w->st;
    p->msg[0] = '\0';
    if (p->count <= 1) {                    /* nothing else to play: again */
        p->frame = 0;
        set_frame_clock(p);
        wm_redraw(w);
        return;
    }
    for (unsigned tries = 0; tries < p->count; tries++) {
        p->at = (p->at + p->count + (unsigned)step) % p->count;
        char path[FS_PATH_MAX];
        if (fs_path_resolve(p->dir, p->list[p->at], path, sizeof(path)) == FS_OK && load_video(w, path))
            return;
        ksnprintf(p->msg, sizeof(p->msg), "Skipped %s: not a video it can play", p->list[p->at]);
    }
}

static bool player_open(struct window *w, const char *arg)
{
    struct player *p = kzalloc(sizeof(*p));
    if (p == NULL)
        return false;
    w->st = p;
    const char *path = arg != NULL ? arg : "/media/bunny.bvid";
    strlcpy(p->dir, path, sizeof(p->dir));
    char *slash = strrchr(p->dir, '/');
    if (slash != NULL)
        slash[slash == p->dir ? 1 : 0] = '\0';
    load_list(p);
    if (!load_video(w, path)) {
        kfree(p);
        w->st = NULL;
        return false;
    }
    return true;
}

static void player_close(struct window *w)
{
    kfree(((struct player *)w->st)->data);
}

/* Where the picture goes and how big: 3x in a window; in full screen as
 * big as fits (whole times, so pixels stay square). */
static int32_t layout(struct window *w, struct gfx_rect body, struct gfx_rect *pic, struct gfx_rect *bar)
{
    struct player *p = w->st;
    if (!w->full) {
        *pic = gfx_rect(body.x + UI_PAD, body.y + PIC_Y, VID_W * SCALE, VID_H * SCALE);
        *bar = gfx_rect(body.x + UI_PAD, body.y + BAR_Y, VID_W * SCALE, 8);
        return SCALE;
    }
    int32_t sx = body.w / p->w, sy = (body.h - 50) / p->h, s = sx < sy ? sx : sy;
    s = s < 1 ? 1 : s;
    *pic = gfx_rect(body.x, body.y, body.w, body.h - 50);
    *bar = gfx_rect(body.x + 40, body.y + body.h - 40, body.w - 80, 8);
    return s;
}

static void player_draw(struct window *w, struct gfx_rect body)
{
    struct player *p = w->st;
    struct gfx_rect pic, bar;
    int32_t s = layout(w, body, &pic, &bar);
    if (w->full)
        gfx_fill(body, 0x000000);
    else
        ui_buttons(body, p->playing ? player_buttons : player_buttons_paused, 5, 0, true, -1, -1);
    gfx_fill(pic, 0x000000);
    const uint8_t *frame = p->data + HDR + PAL + (size_t)p->frame * p->w * p->h;
    gfx_image8(pic.x + (pic.w - p->w * s) / 2, pic.y + (pic.h - p->h * s) / 2, frame, p->w, p->h, s, p->packed);
    p->shown++;
    gfx_round(bar, 4, w->full ? 0x333333 : COL_LINE, 255);
    int32_t done = (int32_t)((uint64_t)bar.w * (p->frame + 1) / p->frames);
    gfx_round(gfx_rect(bar.x, bar.y, done, bar.h), 4, COL_PRIMARY, 255);
    unsigned now = p->frame / p->fps, total = p->frames / p->fps;
    char t[80];
    ksnprintf(t, sizeof(t), "%u:%02u / %u:%02u    video %u of %u    N next, F full screen", now / 60, now % 60,
              total / 60, total % 60, p->at + 1, p->count);
    gfx_text(bar.x, bar.y + 16, t, COL_MUTED, FONT_SMALL);
    const char *credit = strstr(p->list[p->at], "bunny") != NULL
                         ? "Big Buck Bunny (c) Blender Foundation, CC BY 3.0" : "";
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
    case 4: next_video(w, 1); break;
    }
    wm_redraw(w);
}

static void player_key(struct window *w, uint16_t key)
{
    struct player *p = w->st;
    if (key == ' ')
        action(w, 0);
    else if (key == INPUT_KEY_HOME)
        action(w, 1);
    else if (key == INPUT_KEY_LEFT)
        action(w, 2);
    else if (key == INPUT_KEY_RIGHT)
        action(w, 3);
    else if (key == 'n' || key == 'N')
        action(w, 4);
    else if (key == 'p' || key == 'P')
        next_video(w, -1);
    else if (key == INPUT_KEY_END)
        seek(p, p->frames);
    else if (key == 'f' || key == 'F' || (key == 0x1B && w->full))
        wm_fullscreen(w, !w->full);
    wm_redraw(w);
}

static void player_click(struct window *w, int32_t x, int32_t y)
{
    struct player *p = w->st;
    struct gfx_rect b = wm_body(w), pic, bar;
    layout(w, gfx_rect(0, 0, b.w, b.h), &pic, &bar);
    int hit = w->full ? -1 : ui_buttons(gfx_rect(0, 0, b.w, b.h),
                                        p->playing ? player_buttons : player_buttons_paused, 5, 0, false, x, y);
    if (hit >= 0) {
        action(w, hit);
    } else if (gfx_inside(gfx_grow(bar, 6), x, y)) {
        int32_t at = x - bar.x;
        at = at < 0 ? 0 : at >= bar.w ? bar.w - 1 : at;
        p->frame = (unsigned)((uint64_t)at * p->frames / (uint64_t)bar.w);
        set_frame_clock(p);
        wm_redraw(w);
    } else if (gfx_inside(pic, x, y)) {
        action(w, 0);                    /* click the picture: play/pause */
    }
}

/* Every desktop frame: move to the frame the clock says, redraw only the
 * picture and the progress line under it. At the end, the next video. */
static void player_anim(struct window *w)
{
    struct player *p = w->st;
    if (!p->playing)
        return;
    uint64_t f = (clock_now_ns() - p->base_ns) * p->fps / 1000000000ull;
    if (f >= p->frames) {
        next_video(w, 1);                 /* one video: it starts again */
        return;
    }
    if (f == p->frame)
        return;
    p->frame = (unsigned)f;
    struct gfx_rect pic, bar;
    layout(w, wm_body(w), &pic, &bar);
    wm_damage(gfx_union(pic, gfx_rect(bar.x, bar.y, bar.w, 34)));
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

unsigned player_playlist(struct window *w, unsigned *at, const char **name, const char **msg)
{
    if (w == NULL || w->app != &app_player)
        return 0;
    struct player *p = w->st;
    if (at != NULL)
        *at = p->at;
    if (name != NULL)
        *name = p->list[p->at];
    if (msg != NULL)
        *msg = p->msg;
    return p->count;
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
