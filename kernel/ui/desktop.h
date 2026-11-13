#ifndef BHARAT_DESKTOP_H
#define BHARAT_DESKTOP_H

/* The desktop: login screen, then the windows, on the graphics screen.
 *
 * One kernel thread ("desktop") owns the screen once boot is done. It
 * reads the PS/2 keyboard and mouse from its own input queue, hands them
 * to the login screen or the window manager, ticks the apps once a
 * second, and redraws what changed, about 50 times a second at most.
 * Keys typed over serial still go to the text console, so both work at
 * the same time.
 *
 * The login screen asks for a name (and a password if /etc/users lists
 * one), with the same rules as the text console. "Log out" returns to it.
 *
 * F12 starts a scripted tour of the desktop (Shift+F12: a quick one, used
 * by the boot test); a kernel built with BHARAT_DEMO_AUTO logs in and
 * starts the tour by itself (`python build.py demo --auto`).
 *
 * A crash in the desktop thread is contained and desktop_watch() starts
 * it again at the login screen. */

#include <stdbool.h>
#include <stdint.h>

/* Start the desktop thread if there is a screen to draw on. */
void desktop_start(void);
void desktop_watch(void);

/* ---- used by the tour and the self-tests --------------------------------- */

/* Feed one key as if typed (login screen or the focused window). */
void desktop_key(uint16_t key);
bool desktop_logged_in(void);
void desktop_logout(void);

/* Reset the login screen; draw it (the window manager's backdrop). */
void login_reset(void);
void login_draw(void);
/* The login screen's message line ("" if none), for self-tests. */
const char *login_message(void);

/* The tour: a list of steps run by the desktop thread. */
void tour_start(bool fast);
bool tour_running(void);
void tour_tick(void);

#endif /* BHARAT_DESKTOP_H */
