#ifndef BHARAT_APPS_H
#define BHARAT_APPS_H

/* The desktop's apps. Each is a struct app (wm.h): the window manager
 * calls it to draw, and hands it keys, clicks and a once-a-second tick.
 *
 *   Terminal        the shell in a window
 *   Files           browse folders; open, create and delete files
 *   Monitor         live load of every CPU, threads, memory
 *   Health          self-check results, the lock-order and crash demos
 *   About           what BharatOS is and how it is built
 *   Code            a code editor: explorer, line numbers, colours, Run
 *   Player          plays .bvid videos (the bundled Big Buck Bunny clip)
 *   Browser         web pages over HTTP, and local HTML files
 *   Text editor     opened from Files (or with a path)
 *
 * V1: apps are code inside the kernel, run by the desktop thread, using
 * only the small APIs (fs_*, shell_*, sched/smp statistics). They move to
 * user mode after Phase 13.
 *
 * Limits: an app's work runs on the desktop thread, so a long command in
 * the Terminal (checks) pauses the desktop until it ends. */

#include "wm.h"
#include "textbuf.h"

extern const struct app app_terminal, app_files, app_monitor, app_health,
                        app_about, app_editor, app_code, app_player, app_browser;

/* The apps in the dock, in order (F1..F8). */
extern const struct app *const desktop_apps[];
extern const unsigned desktop_app_count;

/* Dock icons (icons.c), drawn on the app's tile. */
void icon_terminal(struct gfx_rect t);
void icon_files(struct gfx_rect t);
void icon_monitor(struct gfx_rect t);
void icon_health(struct gfx_rect t);
void icon_about(struct gfx_rect t);
void icon_code(struct gfx_rect t);
void icon_player(struct gfx_rect t);
void icon_browser(struct gfx_rect t);

/* The app that opens a file, picked by its name (.bvid: Player, .html:
 * Browser, .c .h .py .sh .md: Code, anything else: Text editor). */
const struct app *app_for_file(const char *path);

/* The text a Terminal, Health or About window shows; NULL for others. */
struct textbuf *app_text(struct window *w);

/* Files: select the entry called `name` (false if there is none) and the
 * name of the selected entry. */
bool files_select(struct window *w, const char *name);
const char *files_selected(struct window *w);
const char *files_folder(struct window *w);

/* Text editor: its text (0-terminated) and whether it has unsaved changes. */
const char *editor_text(struct window *w, bool *unsaved);

/* Code: its text, whether it is unsaved, and the Run output (of the tab
 * shown). code_open_tab opens a file in a new tab (or shows its tab);
 * code_tabs gives the number of tabs, the one shown, its cursor and the
 * status message. */
#define CODE_TABS 4
const char *code_text(struct window *w, bool *unsaved, struct textbuf **out);
bool code_open_tab(struct window *w, const char *path);
unsigned code_tabs(struct window *w, unsigned *active, size_t *cursor, const char **msg);

/* Player: a .bvid file's header is sound; the frame being shown, frames
 * drawn so far and whether it is playing. */
bool player_parse(const uint8_t *d, size_t size, uint16_t *w, uint16_t *h, uint16_t *fps,
                  uint16_t *frames);
unsigned player_frame(struct window *w, unsigned *shown, bool *playing);
/* Player: videos in the playlist, the one playing, its name and the
 * message line ("" or why a video was skipped). */
#define PLAYER_LIST_MAX 16
unsigned player_playlist(struct window *w, unsigned *at, const char **name, const char **msg);

/* Browser: whether the page shows `text`; its address and link count;
 * follow link `i`; go to an address; go back. */
bool browser_page_has(struct window *w, const char *text);
const char *browser_url(struct window *w, unsigned *links);
bool browser_follow(struct window *w, unsigned i);
void browser_go(struct window *w, const char *url);
void browser_back(struct window *w);

/* Monitor: busy % of CPU `cpu` over the last second. */
unsigned monitor_busy(struct window *w, unsigned cpu);

/* The user logged in to the desktop (empty if none). */
const char *desktop_user(void);

#endif /* BHARAT_APPS_H */
