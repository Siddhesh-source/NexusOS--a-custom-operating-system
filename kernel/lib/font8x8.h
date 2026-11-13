#ifndef BHARAT_FONT8X8_H
#define BHARAT_FONT8X8_H

/* The one built-in font: ASCII 0x20-0x7E, 8x8 pixels per character, bit 0
 * of each row byte is the leftmost pixel. The screen console and the
 * desktop draw it at double height (8x16), the size UI.md calls "body". */

#include <stdint.h>

extern const uint8_t font8x8[95][8];

#endif /* BHARAT_FONT8X8_H */
