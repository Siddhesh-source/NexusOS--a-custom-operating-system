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
                        app_about, app_editor;

/* The apps on the top bar, in order (F1..F5). */
extern const struct app *const desktop_apps[];
extern const unsigned desktop_app_count;

/* The text a Terminal, Health or About window shows; NULL for others. */
struct textbuf *app_text(struct window *w);

/* Files: select the entry called `name` (false if there is none) and the
 * name of the selected entry. */
bool files_select(struct window *w, const char *name);
const char *files_selected(struct window *w);
const char *files_folder(struct window *w);

/* Text editor: its text (0-terminated) and whether it has unsaved changes. */
const char *editor_text(struct window *w, bool *unsaved);

/* Monitor: busy % of CPU `cpu` over the last second. */
unsigned monitor_busy(struct window *w, unsigned cpu);

/* The user logged in to the desktop (empty if none). */
const char *desktop_user(void);

#endif /* BHARAT_APPS_H */
