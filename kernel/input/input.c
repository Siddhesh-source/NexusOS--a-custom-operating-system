#include "input.h"
#include "spinlock.h"
#include "waitq.h"
#include "clock.h"
#include "kprintf.h"
#include "string.h"

/* ---- the queue ---------------------------------------------------------- */

static struct spinlock input_lock = SPINLOCK_INIT(LOCK_CLASS_INPUT);

/* Two queues: the text console's (serial, tests) and, once the desktop
 * runs, the desktop's (PS/2 keyboard and mouse). */
struct input_queue {
    struct input_event ring[INPUT_QUEUE_SIZE];
    unsigned head, count;              /* oldest event, events queued */
    struct waitq wq;
};
static struct input_queue console_q = { .wq = WAITQ_INIT }, desktop_q = { .wq = WAITQ_INIT };
static bool desktop_on;
static struct input_stats stats;
static int32_t screen_w = 1024, screen_h = 768;
static int32_t mouse_x = 512, mouse_y = 384;

void input_init(void)
{
    uint64_t f;
    spin_lock_irqsave(&input_lock, &f);
    console_q.head = console_q.count = desktop_q.head = desktop_q.count = 0;
    spin_unlock_irqrestore(&input_lock, f);
}

void input_set_desktop(bool on)
{
    uint64_t f;
    spin_lock_irqsave(&input_lock, &f);
    desktop_on = on;
    desktop_q.head = desktop_q.count = 0;
    spin_unlock_irqrestore(&input_lock, f);
}

void input_set_screen(int32_t width, int32_t height)
{
    if (width <= 0 || height <= 0)
        return;
    uint64_t f;
    spin_lock_irqsave(&input_lock, &f);
    screen_w = width;
    screen_h = height;
    mouse_x = width / 2;
    mouse_y = height / 2;
    spin_unlock_irqrestore(&input_lock, f);
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

bool input_push(const struct input_event *ev)
{
    if (ev == NULL || (ev->type != INPUT_KEY && ev->type != INPUT_MOUSE))
        return false;
    bool queued = false;
    uint64_t f;
    spin_lock_irqsave(&input_lock, &f);
    struct input_queue *q = desktop_on && (ev->source == INPUT_SRC_PS2_KBD
        || ev->source == INPUT_SRC_PS2_MOUSE) ? &desktop_q : &console_q;
    if (q->count < INPUT_QUEUE_SIZE) {
        struct input_event *slot = &q->ring[(q->head + q->count) % INPUT_QUEUE_SIZE];
        *slot = *ev;
        slot->time_ns = clock_ready() ? clock_now_ns() : 0;
        if (ev->type == INPUT_MOUSE) {
            /* The pointer position lives here, so every reader agrees. */
            mouse_x = clamp(mouse_x + ev->dx, 0, screen_w - 1);
            mouse_y = clamp(mouse_y + ev->dy, 0, screen_h - 1);
            slot->x = mouse_x;
            slot->y = mouse_y;
            stats.mouse++;
        } else {
            stats.keys++;
        }
        __atomic_store_n(&q->count, q->count + 1, __ATOMIC_RELEASE);
        stats.pushed++;
        queued = true;
    } else {
        stats.dropped++;
    }
    spin_unlock_irqrestore(&input_lock, f);
    if (queued)
        waitq_wake_all(&q->wq);
    return queued;
}

static bool poll_queue(struct input_queue *q, struct input_event *out)
{
    bool got = false;
    uint64_t f;
    spin_lock_irqsave(&input_lock, &f);
    if (q->count > 0) {
        if (out != NULL)
            *out = q->ring[q->head];
        q->head = (q->head + 1) % INPUT_QUEUE_SIZE;
        q->count--;
        stats.read++;
        got = true;
    }
    spin_unlock_irqrestore(&input_lock, f);
    return got;
}

bool input_poll(struct input_event *out)
{
    return poll_queue(&console_q, out);
}

bool input_desktop_poll(struct input_event *out)
{
    return poll_queue(&desktop_q, out);
}

static bool has_event(void *arg)
{
    (void)arg;
    return __atomic_load_n(&console_q.count, __ATOMIC_ACQUIRE) > 0;
}

void input_wait(struct input_event *out)
{
    for (;;) {
        waitq_wait(&console_q.wq, has_event, NULL);
        if (input_poll(out))
            return;
    }
}

uint16_t input_wait_key(void)
{
    struct input_event ev;
    do {
        input_wait(&ev);
    } while (ev.type != INPUT_KEY);
    return ev.key;
}

void input_flush(void)
{
    while (input_poll(NULL))
        ;
}

void input_get_stats(struct input_stats *out)
{
    uint64_t f;
    spin_lock_irqsave(&input_lock, &f);
    *out = stats;
    out->queued = console_q.count;
    spin_unlock_irqrestore(&input_lock, f);
}

void input_count_irq(enum input_source src)
{
    /* Interrupts of one device arrive on one CPU, one at a time. */
    if (src == INPUT_SRC_PS2_KBD)
        __atomic_fetch_add(&stats.kbd_irqs, 1, __ATOMIC_RELAXED);
    else if (src == INPUT_SRC_PS2_MOUSE)
        __atomic_fetch_add(&stats.mouse_irqs, 1, __ATOMIC_RELAXED);
    else if (src == INPUT_SRC_SERIAL)
        __atomic_fetch_add(&stats.serial_irqs, 1, __ATOMIC_RELAXED);
}

void input_set_device(enum input_source src, bool present)
{
    if (src == INPUT_SRC_PS2_KBD)
        stats.keyboard = present;
    else if (src == INPUT_SRC_PS2_MOUSE)
        stats.mouse_found = present;
    else if (src == INPUT_SRC_SERIAL)
        stats.serial = present;
}

const char *input_key_name(uint16_t key, char buf[8])
{
    static const char *const special[] = {
        "Up", "Down", "Left", "Right", "Home", "End", "Delete", "PageUp", "PageDown", "Insert",
    };
    if (key >= INPUT_KEY_UP && key < INPUT_KEY_F1)
        return special[key - INPUT_KEY_UP];
    if (key >= INPUT_KEY_F1 && key < INPUT_KEY_F1 + 12) {
        ksnprintf(buf, 8, "F%u", (unsigned)(key - INPUT_KEY_F1 + 1));
        return buf;
    }
    switch (key) {
    case '\n': return "Enter";
    case '\b': return "Backspace";
    case '\t': return "Tab";
    case 0x1B: return "Escape";
    case ' ':  return "Space";
    }
    if (key >= 1 && key <= 26) {
        ksnprintf(buf, 8, "Ctrl+%c", 'A' + key - 1);
        return buf;
    }
    if (key > 0x20 && key < 0x7F) {
        buf[0] = (char)key;
        buf[1] = 0;
        return buf;
    }
    return "?";
}

/* ---- PS/2 keyboard, scancode set 1 --------------------------------------- */

/* Index = scancode (press). 0 = no character. */
static const char kbd_plain[0x3A] = {
    0, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
static const char kbd_shift[0x3A] = {
    0, 0x1B, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};
/* Number pad 0x47..0x53 (Num Lock assumed on). */
static const char kbd_keypad[13] = { '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.' };

#define SC_LCTRL  0x1D
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_LALT   0x38
#define SC_CAPS   0x3A

static uint16_t kbd_extended_key(uint8_t code)
{
    switch (code) {
    case 0x48: return INPUT_KEY_UP;
    case 0x50: return INPUT_KEY_DOWN;
    case 0x4B: return INPUT_KEY_LEFT;
    case 0x4D: return INPUT_KEY_RIGHT;
    case 0x47: return INPUT_KEY_HOME;
    case 0x4F: return INPUT_KEY_END;
    case 0x53: return INPUT_KEY_DELETE;
    case 0x49: return INPUT_KEY_PAGE_UP;
    case 0x51: return INPUT_KEY_PAGE_DOWN;
    case 0x52: return INPUT_KEY_INSERT;
    case 0x1C: return '\n';                 /* keypad Enter */
    case 0x35: return '/';                  /* keypad / */
    }
    return 0;
}

static bool make_key(uint16_t key, uint8_t mods, uint8_t src, struct input_event *out)
{
    if (key == 0)
        return false;
    memset(out, 0, sizeof(*out));
    out->type = INPUT_KEY;
    out->source = src;
    out->mods = mods;
    out->key = key;
    return true;
}

bool kbd_decode(struct kbd_decoder *d, uint8_t sc, struct input_event *out)
{
    if (d == NULL || out == NULL)
        return false;
    if (sc == 0xE0) {
        d->extended = true;
        return false;
    }
    if (sc == 0xE1 || sc == 0x00 || sc == 0xFA || sc == 0xFE || sc == 0xFF)
        return false;                       /* Pause prefix, acks and errors */
    bool ext = d->extended;
    d->extended = false;
    bool release = sc & 0x80;
    uint8_t code = sc & 0x7F;

    /* Modifiers (Right Ctrl/Alt are E0 1D / E0 38; E0 2A is a fake shift). */
    uint8_t mod = 0;
    if (code == SC_LCTRL)
        mod = INPUT_MOD_CTRL;
    else if (code == SC_LALT)
        mod = INPUT_MOD_ALT;
    else if (!ext && (code == SC_LSHIFT || code == SC_RSHIFT))
        mod = INPUT_MOD_SHIFT;
    else if (ext && (code == SC_LSHIFT || code == SC_RSHIFT))
        return false;
    if (mod != 0) {
        d->mods = release ? d->mods & ~mod : d->mods | mod;
        return false;
    }
    if (release)
        return false;
    if (code == SC_CAPS) {
        d->mods ^= INPUT_MOD_CAPS;
        return false;
    }

    uint16_t key = 0;
    if (ext) {
        key = kbd_extended_key(code);
    } else if (code < sizeof(kbd_plain)) {
        bool shift = d->mods & INPUT_MOD_SHIFT;
        char c = shift ? kbd_shift[code] : kbd_plain[code];
        /* Caps Lock flips the case of letters only. */
        if ((d->mods & INPUT_MOD_CAPS) && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
            c = shift ? kbd_plain[code] : kbd_shift[code];
        key = (uint8_t)c;
        if ((d->mods & INPUT_MOD_CTRL) && ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z')))
            key &= 0x1F;
    } else if (code >= 0x3B && code <= 0x44) {
        key = INPUT_KEY_F1 + (code - 0x3B);
    } else if (code == 0x57 || code == 0x58) {
        key = INPUT_KEY_F1 + 10 + (code - 0x57);
    } else if (code >= 0x47 && code <= 0x53) {
        key = (uint8_t)kbd_keypad[code - 0x47];
    }
    return make_key(key, d->mods, INPUT_SRC_PS2_KBD, out);
}

/* ---- terminal bytes (serial) --------------------------------------------- */

static uint16_t tilde_key(uint8_t n)
{
    switch (n) {
    case 1: case 7: return INPUT_KEY_HOME;
    case 4: case 8: return INPUT_KEY_END;
    case 2: return INPUT_KEY_INSERT;
    case 3: return INPUT_KEY_DELETE;
    case 5: return INPUT_KEY_PAGE_UP;
    case 6: return INPUT_KEY_PAGE_DOWN;
    }
    return 0;
}

bool serial_decode(struct serial_decoder *d, uint8_t b, struct input_event *out)
{
    if (d == NULL || out == NULL)
        return false;
    bool after_cr = d->last_cr;
    d->last_cr = false;

    if (d->state == 1) {                    /* after ESC */
        if (b == '[' || b == 'O') {
            d->state = 2;
            d->num = 0;
            return false;
        }
        d->state = 0;                       /* a lone ESC: treat b normally */
    } else if (d->state == 2) {             /* inside ESC [ ... */
        uint16_t key = 0;
        switch (b) {
        case 'A': key = INPUT_KEY_UP; break;
        case 'B': key = INPUT_KEY_DOWN; break;
        case 'C': key = INPUT_KEY_RIGHT; break;
        case 'D': key = INPUT_KEY_LEFT; break;
        case 'H': key = INPUT_KEY_HOME; break;
        case 'F': key = INPUT_KEY_END; break;
        case '~': key = tilde_key(d->num); break;
        default:
            if (b >= '0' && b <= '9') {
                d->num = (uint8_t)(d->num * 10 + (b - '0'));
                return false;
            }
            if (b == ';')
                return false;               /* modifier parameter: ignored */
            break;
        }
        d->state = 0;
        return make_key(key, 0, INPUT_SRC_SERIAL, out);
    }

    if (b == 0x1B) {
        d->state = 1;
        return false;
    }
    if (b == '\r') {
        d->last_cr = true;
        return make_key('\n', 0, INPUT_SRC_SERIAL, out);
    }
    if (b == '\n')
        return after_cr ? false : make_key('\n', 0, INPUT_SRC_SERIAL, out);
    if (b == 0x7F || b == 0x08)
        return make_key('\b', 0, INPUT_SRC_SERIAL, out);
    if (b == '\t')
        return make_key('\t', 0, INPUT_SRC_SERIAL, out);
    if (b >= 1 && b <= 26)
        return make_key(b, INPUT_MOD_CTRL, INPUT_SRC_SERIAL, out);
    if (b >= 0x20 && b < 0x7F)
        return make_key(b, 0, INPUT_SRC_SERIAL, out);
    return false;                           /* other bytes: ignored */
}

/* ---- PS/2 mouse ----------------------------------------------------------- */

#define MOUSE_ALWAYS_ONE  0x08
#define MOUSE_X_SIGN      0x10
#define MOUSE_Y_SIGN      0x20
#define MOUSE_X_OVERFLOW  0x40
#define MOUSE_Y_OVERFLOW  0x80

bool mouse_decode(struct mouse_decoder *d, uint8_t b, struct input_event *out)
{
    if (d == NULL || out == NULL)
        return false;
    if (d->count == 0 && !(b & MOUSE_ALWAYS_ONE)) {
        d->resyncs++;                       /* cannot be a first byte */
        return false;
    }
    d->bytes[d->count++] = b;
    if (d->count < 3)
        return false;
    d->count = 0;

    uint8_t flags = d->bytes[0];
    if (flags & (MOUSE_X_OVERFLOW | MOUSE_Y_OVERFLOW))
        return false;                       /* the values are meaningless */
    int dx = d->bytes[1] - ((flags & MOUSE_X_SIGN) ? 256 : 0);
    int dy = d->bytes[2] - ((flags & MOUSE_Y_SIGN) ? 256 : 0);
    memset(out, 0, sizeof(*out));
    out->type = INPUT_MOUSE;
    out->source = INPUT_SRC_PS2_MOUSE;
    out->buttons = flags & 0x07;            /* left, right, middle */
    out->dx = (int16_t)dx;
    out->dy = (int16_t)-dy;                 /* PS/2 counts up; the screen counts down */
    return true;
}
