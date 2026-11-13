#ifndef BHARAT_FBCON_H
#define BHARAT_FBCON_H

/* Framebuffer console: kernel text on the screen, next to serial.
 *
 * Draws every byte written to the serial port onto the bootloader's
 * framebuffer, using a built-in 8x8 font drawn at double height (8x16
 * character cells). UTF-8 box-drawing characters are shown as + - |.
 *
 * Instead of scrolling (moving the whole screen up for every new line,
 * which is slow without a graphics card driver), the console wraps: after
 * the last row it continues at the top, and each new line is cleared just
 * before it is written. The row being written is always the newest.
 *
 * Limits: 32-bit-per-pixel framebuffers only; one console; no cursor. */

#include <stdint.h>
#include <stdbool.h>

struct fbcon_info {
    bool active;
    uint64_t width, height;     /* pixels */
    unsigned cols, rows;        /* character cells */
    unsigned col, row;          /* next character position */
    uint64_t chars_drawn;
};

/* Take over the framebuffer at `addr` (a kernel virtual address) and start
 * mirroring serial output. Returns false (and stays off) if the format is
 * not supported. */
bool fbcon_init(void *addr, uint64_t width, uint64_t height, uint64_t pitch,
                uint16_t bpp, uint8_t red_shift, uint8_t green_shift, uint8_t blue_shift);

void fbcon_putc(char c);
void fbcon_get_info(struct fbcon_info *out);

/* Colour of one pixel (0x00RRGGBB), for self-tests. */
uint32_t fbcon_read_pixel(uint64_t x, uint64_t y);

#endif /* BHARAT_FBCON_H */
