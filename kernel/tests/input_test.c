/* Phase 10 self-tests: keyboard, mouse and serial input, the event queue
 * and wait queues. */

#include "input_test.h"
#include "selftest.h"
#include "input.h"
#include "inputdev.h"
#include "waitq.h"
#include "sched.h"
#include "proc.h"
#include "percpu.h"
#include "kprintf.h"
#include "string.h"

#define CHECK(c, d) st_check((c), (d))

/* Decode a list of bytes; return the keys produced (up to max). */
static unsigned kbd_keys(const uint8_t *sc, unsigned n, uint16_t *keys, unsigned max)
{
    struct kbd_decoder d = { 0 };
    struct input_event ev;
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++)
        if (kbd_decode(&d, sc[i], &ev) && k < max)
            keys[k++] = ev.key;
    return k;
}

static void test_keyboard_decoder(void)
{
    st_section("Keyboard decoder (scancode set 1)");
    uint16_t k[8];
    const uint8_t a[] = { 0x1E, 0x9E };                     /* a down, a up */
    CHECK(kbd_keys(a, 2, k, 8) == 1 && k[0] == 'a', "a press gives 'a'; the release gives nothing");
    const uint8_t shift_a[] = { 0x2A, 0x1E, 0x9E, 0xAA, 0x1E };
    CHECK(kbd_keys(shift_a, 5, k, 8) == 2 && k[0] == 'A' && k[1] == 'a',
          "Shift+a gives 'A'; after Shift is released, 'a' again");
    const uint8_t caps[] = { 0x3A, 0xBA, 0x1E, 0x02, 0x2A, 0x1E, 0xAA };
    CHECK(kbd_keys(caps, 7, k, 8) == 3 && k[0] == 'A' && k[1] == '1' && k[2] == 'a',
          "Caps Lock: letters upper case, digits unchanged, Shift flips back");
    const uint8_t ctrl_c[] = { 0x1D, 0x2E, 0x9D };
    CHECK(kbd_keys(ctrl_c, 3, k, 8) == 1 && k[0] == 3, "Ctrl+C gives code 3");
    const uint8_t arrows[] = { 0xE0, 0x48, 0xE0, 0xC8, 0xE0, 0x50, 0xE0, 0x4B, 0xE0, 0x4D };
    CHECK(kbd_keys(arrows, 10, k, 8) == 4 && k[0] == INPUT_KEY_UP && k[1] == INPUT_KEY_DOWN
          && k[2] == INPUT_KEY_LEFT && k[3] == INPUT_KEY_RIGHT, "E0-prefixed arrows decode, releases ignored");
    const uint8_t misc[] = { 0x1C, 0x0E, 0x39, 0x3B, 0x58, 0x47, 0xE0, 0x1C };
    CHECK(kbd_keys(misc, 8, k, 8) == 7 && k[0] == '\n' && k[1] == '\b' && k[2] == ' '
          && k[3] == INPUT_KEY_F1 && k[4] == INPUT_KEY_F1 + 11 && k[5] == '7' && k[6] == '\n',
          "Enter, Backspace, Space, F1, F12, keypad 7 (keypad Enter: newline)");
    const uint8_t junk[] = { 0xFA, 0x00, 0xFF, 0xE1, 0x7F, 0x60 };
    CHECK(kbd_keys(junk, 6, k, 8) == 0, "acks, errors and unknown codes produce no keys");
}

static unsigned serial_keys(const char *s, unsigned n, uint16_t *keys, unsigned max)
{
    struct serial_decoder d = { 0 };
    struct input_event ev;
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++)
        if (serial_decode(&d, (uint8_t)s[i], &ev) && k < max)
            keys[k++] = ev.key;
    return k;
}

static void test_serial_decoder(void)
{
    st_section("Terminal (serial) decoder");
    uint16_t k[8];
    CHECK(serial_keys("ab\r\n", 4, k, 8) == 3 && k[0] == 'a' && k[1] == 'b' && k[2] == '\n',
          "\"ab\\r\\n\" gives a, b and one Enter");
    CHECK(serial_keys("\x1b[A\x1b[B\x1b[3~\x1bOH", 13, k, 8) == 4 && k[0] == INPUT_KEY_UP
          && k[1] == INPUT_KEY_DOWN && k[2] == INPUT_KEY_DELETE && k[3] == INPUT_KEY_HOME,
          "escape sequences: Up, Down, Delete, Home");
    CHECK(serial_keys("\x7f\x08\x03\t", 4, k, 8) == 4 && k[0] == '\b' && k[1] == '\b'
          && k[2] == 3 && k[3] == '\t', "Backspace (DEL and BS), Ctrl+C, Tab");
    CHECK(serial_keys("\x1b[1;5C\x80\xff", 8, k, 8) == 1 && k[0] == INPUT_KEY_RIGHT,
          "Ctrl+Right decodes as Right; bytes above 0x7F are ignored");
}

static void test_mouse_decoder(void)
{
    st_section("Mouse decoder (PS/2 packets)");
    struct mouse_decoder d = { 0 };
    struct input_event ev;
    bool first = mouse_decode(&d, 0x09, &ev);
    bool second = mouse_decode(&d, 5, &ev);
    bool got = !first && !second && mouse_decode(&d, 3, &ev);
    CHECK(got && ev.dx == 5 && ev.dy == -3 && ev.buttons == INPUT_BUTTON_LEFT,
          "packet 09 05 03: right 5, up 3, left button");
    CHECK(mouse_decode(&d, 0x38, &ev) == false && mouse_decode(&d, 0xFE, &ev) == false
          && mouse_decode(&d, 0xFD, &ev) && ev.dx == -2 && ev.dy == 3 && ev.buttons == 0,
          "negative movement uses the sign bits");
    uint64_t rs = d.resyncs;
    bool none = !mouse_decode(&d, 0x00, &ev) && !mouse_decode(&d, 0x02, &ev);
    bool after = !mouse_decode(&d, 0x0A, &ev) && !mouse_decode(&d, 1, &ev) && mouse_decode(&d, 1, &ev);
    CHECK(none && d.resyncs == rs + 2 && after && ev.buttons == INPUT_BUTTON_RIGHT,
          "bytes that cannot start a packet are skipped; the next packet decodes");
    bool ovf = !mouse_decode(&d, 0x48, &ev) && !mouse_decode(&d, 1, &ev) && !mouse_decode(&d, 1, &ev);
    CHECK(ovf, "a packet with the overflow bit set is dropped");
}

static void test_queue(void)
{
    st_section("Event queue");
    input_flush();
    struct input_stats s0, s1;
    input_get_stats(&s0);
    struct input_event ev = { .type = INPUT_KEY, .source = INPUT_SRC_TEST, .key = 'x' };
    input_push(&ev);
    ev.key = 'y';
    input_push(&ev);
    struct input_event a, b;
    CHECK(input_poll(&a) && input_poll(&b) && a.key == 'x' && b.key == 'y' && !input_poll(&a),
          "events come out in order, then the queue is empty");

    unsigned queued = 0;
    for (unsigned i = 0; i < INPUT_QUEUE_SIZE + 5; i++)
        queued += input_push(&ev);
    input_get_stats(&s1);
    CHECK(queued == INPUT_QUEUE_SIZE && s1.dropped == s0.dropped + 5 && s1.queued == INPUT_QUEUE_SIZE,
          "a full queue keeps 256 events and counts the 5 it dropped");
    input_flush();
    input_get_stats(&s1);
    CHECK(s1.queued == 0, "flush empties it");

    struct input_event bad = { .type = 9 };
    CHECK(!input_push(&bad) && !input_push(NULL), "events of unknown type are refused");

    input_set_screen(100, 50);
    struct input_event m = { .type = INPUT_MOUSE, .dx = 10, .dy = -5 };
    input_push(&m);
    m.dx = 500;
    m.dy = 500;
    input_push(&m);
    struct input_event m1, m2;
    CHECK(input_poll(&m1) && input_poll(&m2) && m1.x == 60 && m1.y == 20 && m2.x == 99 && m2.y == 49,
          "mouse position starts mid-screen, moves and stays on the screen");
}

static void test_ps2_feed(void)
{
    st_section("PS/2 bytes through the driver");
    input_flush();
    inputdev_feed(false, 0x23);             /* h */
    inputdev_feed(false, 0x17);             /* i */
    inputdev_feed(true, 0x0A);              /* mouse: right button */
    inputdev_feed(true, 0x00);
    inputdev_feed(true, 0x00);
    struct input_event e1, e2, e3;
    CHECK(input_poll(&e1) && input_poll(&e2) && input_poll(&e3)
          && e1.key == 'h' && e2.key == 'i' && e1.source == INPUT_SRC_PS2_KBD
          && e3.type == INPUT_MOUSE && e3.buttons == INPUT_BUTTON_RIGHT,
          "keyboard and mouse bytes become a key 'h', 'i' and a mouse event");
}

/* Wait for the queue to hold `n` events (the interrupt may land on any
 * CPU's clock; give it up to ~0.5 s). */
static bool wait_queued(unsigned n)
{
    struct input_stats s;
    for (unsigned i = 0; i < 50; i++) {
        input_get_stats(&s);
        if (s.queued >= n)
            return true;
        sched_sleep(1);
    }
    return false;
}

static void test_hardware(void)
{
    st_section("Devices and interrupts");
    struct input_stats s0, s1;
    input_get_stats(&s0);
    CHECK(s0.keyboard, "the PS/2 keyboard answered and IRQ 1 is routed");
    CHECK(s0.mouse_found, "the PS/2 mouse answered and IRQ 12 is routed");
    CHECK(s0.serial, "serial input is routed (IRQ 4)");

    input_flush();
    bool sent = inputdev_loopback(false, 0x2C);      /* z, via the controller */
    struct input_event ev;
    CHECK(sent && wait_queued(1) && input_poll(&ev) && ev.key == 'z' && ev.source == INPUT_SRC_PS2_KBD,
          "a byte injected at the controller arrives through IRQ 1 as key 'z'");
    sent = inputdev_loopback(true, 0x08) && inputdev_loopback(true, 7) && inputdev_loopback(true, 0);
    CHECK(sent && wait_queued(1) && input_poll(&ev) && ev.type == INPUT_MOUSE && ev.dx == 7,
          "a mouse packet injected at the controller arrives through IRQ 12");
    input_get_stats(&s1);
    CHECK(s1.kbd_irqs > s0.kbd_irqs && s1.mouse_irqs > s0.mouse_irqs, "both interrupts were counted");
}

/* ---- waiting --------------------------------------------------------------- */

static volatile uint16_t waited_key;
static volatile bool waiter_done;

static int64_t key_waiter(void *arg)
{
    (void)arg;
    waited_key = input_wait_key();
    waiter_done = true;
    return 0;
}

static struct waitq test_wq = WAITQ_INIT;
static volatile int flag;
static volatile unsigned flag_seen;

static bool flag_set(void *arg)
{
    (void)arg;
    return flag != 0;
}

static int64_t flag_waiter(void *arg)
{
    (void)arg;
    waitq_wait(&test_wq, flag_set, NULL);
    __atomic_fetch_add(&flag_seen, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static void test_waiting(void)
{
    st_section("Sleeping until input arrives");
    input_flush();
    waiter_done = false;
    proc_status_t err;
    struct thread *t = thread_create(process_kernel(), "keywait", key_waiter, NULL, &err);
    sched_sleep(3);
    CHECK(t != NULL && !waiter_done && t->state == TASK_BLOCKED,
          "a reader with nothing to read sleeps (BLOCKED, using no CPU)");
    struct input_event ev = { .type = INPUT_KEY, .source = INPUT_SRC_TEST, .key = 'k' };
    input_push(&ev);
    for (unsigned i = 0; i < 100 && !waiter_done; i++)
        sched_sleep(1);
    CHECK(waiter_done && waited_key == 'k', "a key wakes it and it gets that key");

    /* Several sleepers, all woken at once. */
    flag = 0;
    flag_seen = 0;
    unsigned made = 0;
    for (unsigned i = 0; i < 3; i++)
        made += thread_create(process_kernel(), "wqwait", flag_waiter, NULL, &err) != NULL;
    sched_sleep(3);
    CHECK(made == 3 && waitq_sleepers(&test_wq) == 3 && flag_seen == 0, "three threads sleep on one queue");
    flag = 1;
    unsigned woken = waitq_wake_all(&test_wq);
    for (unsigned i = 0; i < 100 && flag_seen < 3; i++)
        sched_sleep(1);
    CHECK(woken == 3 && flag_seen == 3 && waitq_sleepers(&test_wq) == 0, "one wake-up releases all three");
    CHECK(waitq_wake_all(&test_wq) == 0, "waking an empty queue does nothing");
}

bool input_run_tests(void)
{
    st_begin();
    kprintf("\n[test] ===== Phase 10: input =====\n");
    test_keyboard_decoder();
    test_serial_decoder();
    test_mouse_decoder();
    test_queue();
    test_ps2_feed();
    test_hardware();
    test_waiting();
    input_flush();
    return st_end("Input");
}
