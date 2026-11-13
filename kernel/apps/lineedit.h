#ifndef BHARAT_LINEEDIT_H
#define BHARAT_LINEEDIT_H

/* Line editor: turns key presses into one line of text, as typed at a
 * prompt.
 *
 *   letters, digits, ...   added at the end and echoed
 *   Backspace              removes the last character
 *   Enter                  finishes the line
 *   Up / Down              earlier / later lines from the history
 *   Ctrl+C                 abandons the line
 *   Ctrl+U                 clears the line
 *
 * Echo goes through an output function, so the same editor works on the
 * text console, in a desktop window (Phase 11) and in self-tests. In
 * "secret" mode (passwords) each character is echoed as '*' and the line
 * is not kept in the history.
 *
 * Limits: editing happens at the end of the line only (no moving the
 * cursor inside it); LE_MAX - 1 characters; LE_HISTORY lines kept. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LE_MAX      160
#define LE_HISTORY  16

typedef void (*le_output_t)(void *ctx, const char *s, size_t n);

struct lineedit {
    char buf[LE_MAX];
    size_t len;
    bool secret;
    char history[LE_HISTORY][LE_MAX];
    unsigned hist_count;          /* lines stored (up to LE_HISTORY) */
    unsigned hist_head;           /* slot the next line goes into */
    int browse;                   /* -1: not browsing; else lines back */
    char draft[LE_MAX];           /* what was typed before browsing */
    le_output_t out;
    void *ctx;
};

enum le_result {
    LE_MORE = 0,                  /* keep feeding keys */
    LE_DONE,                      /* line finished: buf holds it */
    LE_CANCEL,                    /* Ctrl+C: buf is empty */
};

void le_init(struct lineedit *le, le_output_t out, void *ctx);

/* Start a new line (secret: echo '*' and skip the history). */
void le_begin(struct lineedit *le, bool secret);

/* Feed one key (input.h codes). */
enum le_result le_key(struct lineedit *le, uint16_t key);

/* Remember a finished line (skips empty lines and repeats). */
void le_add_history(struct lineedit *le, const char *line);

#endif /* BHARAT_LINEEDIT_H */
