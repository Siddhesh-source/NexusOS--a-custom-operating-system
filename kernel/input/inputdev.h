#ifndef BHARAT_INPUTDEV_H
#define BHARAT_INPUTDEV_H

/* Input devices: PS/2 keyboard (IRQ 1), PS/2 mouse (IRQ 12) and serial
 * input (IRQ 4), all routed through the I/O APIC to the boot CPU. Each
 * byte is decoded and queued as an input event (input.h).
 *
 * Limits: no hot-plug; a missing PS/2 controller (some UEFI-only PCs)
 * just leaves serial input. */

#include <stdbool.h>
#include <stdint.h>

#define INPUTDEV_KBD_VECTOR     0x41
#define INPUTDEV_MOUSE_VECTOR   0x42
#define INPUTDEV_SERIAL_VECTOR  0x43

/* Needs the I/O APIC (apic_init) and the clock. */
void inputdev_init(void);

/* Feed one PS/2 byte as if it came from the keyboard (or the mouse, when
 * `aux`). Used by the interrupt and by self-tests. */
void inputdev_feed(bool aux, uint8_t byte);

/* Ask the PS/2 controller to deliver `byte` as if the keyboard (or the
 * mouse, when `aux`) sent it: it arrives through the real interrupt path.
 * For self-tests. False if that device is not running. */
bool inputdev_loopback(bool aux, uint8_t byte);

#endif /* BHARAT_INPUTDEV_H */
