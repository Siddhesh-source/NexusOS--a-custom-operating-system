#ifndef BHARAT_SPLASH_H
#define BHARAT_SPLASH_H

/* Boot splash: the first thing on the screen.
 *
 * The top of the screen shows the BharatOS name and a progress bar that
 * moves on at each real boot step (memory, threads, CPUs, ... the
 * self-checks of each part). The lower part keeps the scrolling boot log
 * (the screen console), so what the kernel is doing stays visible.
 *
 * It also remembers the screen as a drawing surface for the desktop.
 *
 * Limits: needs a 32-bit framebuffer at least 640 x 600; smaller screens
 * get the plain text console only. */

#include <stdbool.h>
#include <stdint.h>
#include "gfx.h"

#define SPLASH_STEPS 12

/* Take the framebuffer (kernel virtual address), draw the splash and
 * start the screen console below it. */
void splash_start(void *addr, uint64_t width, uint64_t height, uint64_t pitch,
                  uint16_t bpp, uint8_t red_shift, uint8_t green_shift, uint8_t blue_shift);

/* One boot step finished; `what` names the next one ("Checking memory"). */
void splash_step(const char *what);

/* Steps done so far (for self-tests). */
unsigned splash_steps_done(void);

/* The screen as a surface; NULL if there is no usable framebuffer. */
struct gfx_surface *splash_screen(void);

#endif /* BHARAT_SPLASH_H */
