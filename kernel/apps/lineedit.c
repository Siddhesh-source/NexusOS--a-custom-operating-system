#include "lineedit.h"
#include "input.h"
#include "string.h"

static void out(struct lineedit *le, const char *s, size_t n)
{
    if (le->out != NULL && n > 0)
        le->out(le->ctx, s, n);
}

void le_init(struct lineedit *le, le_output_t fn, void *ctx)
{
    memset(le, 0, sizeof(*le));
    le->out = fn;
    le->ctx = ctx;
    le->browse = -1;
}

void le_begin(struct lineedit *le, bool secret)
{
    le->len = 0;
    le->buf[0] = '\0';
    le->secret = secret;
    le->browse = -1;
}

/* Erase what is shown and show `text` instead. */
static void replace_line(struct lineedit *le, const char *text)
{
    for (size_t i = 0; i < le->len; i++)
        out(le, "\b \b", 3);
    le->len = strlcpy(le->buf, text, LE_MAX);
    if (le->len >= LE_MAX)
        le->len = LE_MAX - 1;
    out(le, le->buf, le->len);
}

/* History line `back` steps before the newest (0 = newest). */
static const char *history_at(struct lineedit *le, unsigned back)
{
    unsigned slot = (le->hist_head + LE_HISTORY - 1 - back) % LE_HISTORY;
    return le->history[slot];
}

enum le_result le_key(struct lineedit *le, uint16_t key)
{
    switch (key) {
    case '\n':
        out(le, "\n", 1);
        le->buf[le->len] = '\0';
        return LE_DONE;
    case '\b':
        if (le->len > 0) {
            le->len--;
            le->buf[le->len] = '\0';
            out(le, "\b \b", 3);
        }
        return LE_MORE;
    case 3:                                 /* Ctrl+C */
        out(le, "^C\n", 3);
        le->len = 0;
        le->buf[0] = '\0';
        return LE_CANCEL;
    case 21:                                /* Ctrl+U */
        replace_line(le, "");
        return LE_MORE;
    case INPUT_KEY_UP:
        if (le->secret || le->hist_count == 0 || le->browse + 1 >= (int)le->hist_count)
            return LE_MORE;
        if (le->browse < 0) {
            le->buf[le->len] = '\0';
            strlcpy(le->draft, le->buf, LE_MAX);
        }
        le->browse++;
        replace_line(le, history_at(le, (unsigned)le->browse));
        return LE_MORE;
    case INPUT_KEY_DOWN:
        if (le->browse < 0)
            return LE_MORE;
        le->browse--;
        replace_line(le, le->browse < 0 ? le->draft : history_at(le, (unsigned)le->browse));
        return LE_MORE;
    case '\t':
        key = ' ';                          /* no completion yet: a space */
        break;
    }
    if (key < 0x20 || key >= 0x7F)
        return LE_MORE;                     /* other special keys: ignored */
    if (le->len + 1 >= LE_MAX)
        return LE_MORE;                     /* full: extra characters dropped */
    char c = (char)key;
    le->buf[le->len++] = c;
    le->buf[le->len] = '\0';
    out(le, le->secret ? "*" : &c, 1);
    return LE_MORE;
}

void le_add_history(struct lineedit *le, const char *line)
{
    if (line == NULL || line[0] == '\0')
        return;
    if (le->hist_count > 0 && strcmp(history_at(le, 0), line) == 0)
        return;
    strlcpy(le->history[le->hist_head], line, LE_MAX);
    le->hist_head = (le->hist_head + 1) % LE_HISTORY;
    if (le->hist_count < LE_HISTORY)
        le->hist_count++;
}
