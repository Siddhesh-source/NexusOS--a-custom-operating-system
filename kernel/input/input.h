#ifndef BHARAT_INPUT_H
#define BHARAT_INPUT_H

/* Input: keyboard and mouse events in one queue that apps read.
 *
 * Devices turn raw bytes into events with the decoders below and push
 * them with input_push() (usually from their interrupt). Apps read with
 * input_poll() (never waits) or input_wait() (sleeps until an event
 * arrives).
 *
 * Keys arrive from two places, decoded to the same events:
 *   - the PS/2 keyboard (scancode set 1), when QEMU shows a window;
 *   - the serial port, when QEMU runs in the terminal (-nographic): what
 *     you type in the terminal reaches the kernel as serial bytes.
 *
 * A key event carries the character it types (ASCII, after Shift, Caps
 * Lock and Ctrl) or one of the INPUT_KEY_* codes for keys that type
 * nothing. Only key presses are queued (releases only update the
 * modifier state).
 *
 * Mouse events carry the movement and the new position, kept inside the
 * screen, and the buttons held.
 *
 * Limits: one queue for the whole system (V1 has one app reading at a
 * time); INPUT_QUEUE_SIZE events, newer ones are dropped and counted when
 * it is full; US keyboard layout only. */

#include <stdbool.h>
#include <stdint.h>

#define INPUT_QUEUE_SIZE 256

enum input_type {
    INPUT_KEY = 1,
    INPUT_MOUSE = 2,
};

enum input_source {
    INPUT_SRC_PS2_KBD = 1,
    INPUT_SRC_SERIAL = 2,
    INPUT_SRC_PS2_MOUSE = 3,
    INPUT_SRC_TEST = 4,
};

/* Keys that type no character. Characters use their ASCII codes, with
 * Enter = '\n', Backspace = '\b', Tab = '\t', Escape = 0x1B and
 * Ctrl+letter = 1..26. */
enum input_key {
    INPUT_KEY_UP = 0x100,
    INPUT_KEY_DOWN,
    INPUT_KEY_LEFT,
    INPUT_KEY_RIGHT,
    INPUT_KEY_HOME,
    INPUT_KEY_END,
    INPUT_KEY_DELETE,
    INPUT_KEY_PAGE_UP,
    INPUT_KEY_PAGE_DOWN,
    INPUT_KEY_INSERT,
    INPUT_KEY_F1,        /* F1..F12 follow in order */
};

#define INPUT_MOD_SHIFT  (1u << 0)
#define INPUT_MOD_CTRL   (1u << 1)
#define INPUT_MOD_ALT    (1u << 2)
#define INPUT_MOD_CAPS   (1u << 3)

#define INPUT_BUTTON_LEFT   (1u << 0)
#define INPUT_BUTTON_RIGHT  (1u << 1)
#define INPUT_BUTTON_MIDDLE (1u << 2)

struct input_event {
    uint8_t type;           /* enum input_type */
    uint8_t source;         /* enum input_source */
    uint8_t mods;           /* INPUT_MOD_* at the time of the key */
    uint8_t buttons;        /* mouse: INPUT_BUTTON_* held */
    uint16_t key;           /* key: character or INPUT_KEY_* */
    int16_t dx, dy;         /* mouse: movement (dy > 0 is down the screen) */
    int32_t x, y;           /* mouse: new position */
    uint64_t time_ns;
};

struct input_stats {
    uint64_t pushed;        /* events queued */
    uint64_t dropped;       /* events lost because the queue was full */
    uint64_t read;          /* events taken by readers */
    uint64_t keys, mouse;   /* events by type */
    uint64_t kbd_irqs, mouse_irqs, serial_irqs;
    unsigned queued;        /* waiting right now */
    bool keyboard, mouse_found, serial;   /* devices that are running */
};

/* ---- the queue ---------------------------------------------------------- */

void input_init(void);

/* Queue an event (time is filled in). Safe from interrupts. Returns false
 * if the queue was full and the event was dropped. */
bool input_push(const struct input_event *ev);

/* Take the oldest event; false if there is none. Never waits. */
bool input_poll(struct input_event *out);

/* Take the oldest event, sleeping until one arrives. */
void input_wait(struct input_event *out);

/* Same, but keys only (mouse events are skipped). Returns the key. */
uint16_t input_wait_key(void);

/* Throw away every queued event. */
void input_flush(void);

void input_get_stats(struct input_stats *out);

/* Called by the device drivers. */
void input_count_irq(enum input_source src);
void input_set_device(enum input_source src, bool present);

/* Size of the area the mouse moves in (the screen); the pointer starts in
 * the middle. */
void input_set_screen(int32_t width, int32_t height);

/* Name of a key for messages ("a", "Enter", "Up", ...). */
const char *input_key_name(uint16_t key, char buf[8]);

/* ---- decoders (pure: no hardware, no locks; host-testable) -------------- */

/* PS/2 keyboard, scancode set 1. Returns true and fills `out` when the
 * byte completes a key press. */
struct kbd_decoder {
    uint8_t mods;           /* INPUT_MOD_* held now */
    bool extended;          /* saw 0xE0 */
};
bool kbd_decode(struct kbd_decoder *d, uint8_t scancode, struct input_event *out);

/* Bytes typed in a terminal (serial): plain characters, Enter as '\r',
 * Backspace as 0x7F or 0x08, arrows as ESC [ A/B/C/D, ESC [ H/F,
 * ESC [ 3 ~ for Delete. */
struct serial_decoder {
    uint8_t state;          /* 0 normal, 1 after ESC, 2 after ESC [ */
    uint8_t num;            /* digit inside ESC [ n ~ */
    bool last_cr;           /* swallow the '\n' of a "\r\n" pair */
};
bool serial_decode(struct serial_decoder *d, uint8_t byte, struct input_event *out);

/* PS/2 mouse, 3-byte packets. Returns true and fills `out` (dx, dy,
 * buttons; not x/y) when a packet is complete. Bytes that cannot start a
 * packet are skipped, so a lost byte only loses one packet. */
struct mouse_decoder {
    uint8_t bytes[3];
    uint8_t count;
    uint64_t resyncs;       /* bytes skipped to find a packet start again */
};
bool mouse_decode(struct mouse_decoder *d, uint8_t byte, struct input_event *out);

#endif /* BHARAT_INPUT_H */
