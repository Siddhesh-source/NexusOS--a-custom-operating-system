#ifndef BHARAT_SHELL_H
#define BHARAT_SHELL_H

/* The shell: runs the commands typed in the terminal.
 *
 *   help ls cd pwd cat echo mkdir rm touch clear whoami uptime mem cpus
 *   ps top kill run checks lockdemo crashdemo about reboot logout
 *
 * Any command's output can go to a file: `ls > list.txt` replaces the
 * file, `echo more >> list.txt` adds to its end.
 *
 * The shell does not read keys itself: a caller (the console app, a
 * desktop window, a self-test) hands it one line at a time with
 * shell_run(). Output goes through an output function, so the same shell
 * works on the text console, in a window (Phase 11) and in tests, where
 * the output is collected and checked.
 *
 * V1: the shell is a kernel thread calling kernel functions directly
 * (fs_*, sched, proc). It moves to user mode in Phase 15; keeping it on
 * those small APIs is what will make that move mechanical.
 *
 * Limits: no pipes, no scripts, no variables; one redirection per line;
 * redirected output is cut at SHELL_CAPTURE_MAX bytes. */

#include <stdbool.h>
#include <stddef.h>
#include "fs.h"
#include "lineedit.h"

#define SHELL_USER_MAX     17             /* 16 characters + 0 */
#define SHELL_MAX_ARGS     16
#define SHELL_CAPTURE_MAX  (16 * 1024)

struct shell {
    char user[SHELL_USER_MAX];
    char home[FS_PATH_MAX];
    char cwd[FS_PATH_MAX];
    le_output_t out;
    void *out_ctx;
    /* For commands that repeat until a key is pressed (top): returns true
     * once a key was pressed. NULL: such commands show one screen only. */
    bool (*key_pressed)(void *ctx);
    void *key_ctx;
    bool logout;                  /* set by `logout` */
    int status;                   /* 0 if the last command succeeded */
    /* Redirected output being collected (NULL when not redirecting). */
    char *cap;
    size_t cap_len;
    bool cap_full;
};

/* Set up a shell for `user`, starting in their home folder
 * (/home/<user>, created if missing). */
void shell_init(struct shell *sh, const char *user, le_output_t out, void *ctx);

/* Run one command line. Returns 0 on success (also in sh->status). */
int shell_run(struct shell *sh, const char *line);

/* The prompt text, e.g. "asha@bharat:~/notes$ ". */
void shell_prompt(const struct shell *sh, char *buf, size_t size);

/* Print to the shell's output (or the file it is redirected to). */
void shell_printf(struct shell *sh, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void shell_write(struct shell *sh, const char *s, size_t n);

#endif /* BHARAT_SHELL_H */
