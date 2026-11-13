#ifndef BHARAT_CONSOLE_H
#define BHARAT_CONSOLE_H

/* The console app: login, then the shell, on the text console (the
 * serial terminal and the screen). It runs as one kernel thread; keys come
 * from the input queue (keyboard or serial), text goes to both outputs.
 *
 * Login asks for a name. Names listed in /etc/users ("name:password")
 * also need their password; any other valid name logs in at once. This
 * is a greeting, not a security boundary: permissions come in Phase 18.
 *
 * If the console thread crashes, its crash is contained (crash.h) and
 * console_watch() starts it again. */

#include <stdbool.h>
#include "proc.h"

#define LOGIN_NAME_MAX 16

/* A name may use letters, digits, '-' and '_', 1..16 characters. */
bool login_name_ok(const char *name);

/* May `user` log in with `password`? *needs_password says whether
 * /etc/users lists a password for them (then it must match). */
bool login_check(const char *user, const char *password, bool *needs_password);

/* Start the console thread. */
void console_start(void);

/* Restart the console thread if it has ended. Called every second by the
 * kernel's main thread. */
void console_watch(void);

/* The logged-in user ("" before login). */
const char *session_user(void);

#endif /* BHARAT_CONSOLE_H */
