#ifndef BHARAT_WHY_H
#define BHARAT_WHY_H

/* The shell's `why` command: plain answers from live data (why slow, why
 * crash). See why.c. Limits: two questions so far; `why slow` watches for
 * half a second, so it pauses the terminal that long. */

struct shell;

int why_command(struct shell *sh, int argc, char **argv);

#endif /* BHARAT_WHY_H */
