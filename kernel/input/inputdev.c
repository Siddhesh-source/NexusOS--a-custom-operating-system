/* Input devices: the PS/2 controller (i8042: keyboard and mouse) and the
 * serial port's receive side. Each byte that arrives is decoded
 * (input.c) and queued as an event.
 *
 * The i8042 has two registers: 0x60 data (bytes from either device, or
 * to the keyboard) and 0x64 status (read) / command (write). Status bit 0
 * says a byte is waiting, bit 1 that the controller is still busy with
 * our last write, bit 5 that the waiting byte came from the mouse. */

#include "input.h"
#include "inputdev.h"
#include "cpu.h"
#include "apic.h"
#include "interrupts.h"
#include "serial.h"
#include "kprintf.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define ST_OUT_FULL  0x01
#define ST_IN_FULL   0x02
#define ST_AUX       0x20

#define CFG_KBD_IRQ    0x01
#define CFG_MOUSE_IRQ  0x02
#define CFG_MOUSE_OFF  0x20

#define PS2_WAIT 100000

static struct kbd_decoder kbd;
static struct mouse_decoder mouse;
static struct serial_decoder serial_dec;
static bool kbd_ok, mouse_ok, serial_ok;

static bool wait_write(void)
{
    for (unsigned i = 0; i < PS2_WAIT; i++) {
        if (!(inb(PS2_STATUS) & ST_IN_FULL))
            return true;
        io_wait();
    }
    return false;
}

static bool wait_read(void)
{
    for (unsigned i = 0; i < PS2_WAIT; i++) {
        if (inb(PS2_STATUS) & ST_OUT_FULL)
            return true;
        io_wait();
    }
    return false;
}

static bool ps2_command(uint8_t cmd)
{
    if (!wait_write())
        return false;
    outb(PS2_CMD, cmd);
    return true;
}

static bool ps2_write_data(uint8_t v)
{
    if (!wait_write())
        return false;
    outb(PS2_DATA, v);
    return true;
}

static bool ps2_read_data(uint8_t *v)
{
    if (!wait_read())
        return false;
    *v = inb(PS2_DATA);
    return true;
}

/* Send a byte to a device (the mouse when `aux`) and expect its ACK. */
static bool device_send(bool aux, uint8_t v)
{
    if (aux && !ps2_command(0xD4))
        return false;
    if (!ps2_write_data(v))
        return false;
    uint8_t r;
    for (unsigned tries = 0; tries < 4; tries++) {
        if (!ps2_read_data(&r))
            return false;
        if (r == 0xFA)
            return true;
    }
    return false;
}

static void flush_output(void)
{
    for (unsigned i = 0; i < 64 && (inb(PS2_STATUS) & ST_OUT_FULL); i++)
        (void)inb(PS2_DATA);
}

void inputdev_feed(bool aux, uint8_t byte)
{
    struct input_event ev;
    bool done = aux ? mouse_decode(&mouse, byte, &ev) : kbd_decode(&kbd, byte, &ev);
    if (done)
        input_push(&ev);
}

/* Both PS/2 interrupts read whatever is waiting; status bit 5 says which
 * device it came from, so a byte is never fed to the wrong decoder. */
static void ps2_irq(struct interrupt_frame *frame)
{
    input_count_irq(frame->vector == INPUTDEV_MOUSE_VECTOR ? INPUT_SRC_PS2_MOUSE
                                                           : INPUT_SRC_PS2_KBD);
    for (unsigned i = 0; i < 16; i++) {
        uint8_t st = inb(PS2_STATUS);
        if (!(st & ST_OUT_FULL))
            break;
        inputdev_feed(st & ST_AUX, inb(PS2_DATA));
    }
    lapic_eoi();
}

static void serial_irq(struct interrupt_frame *frame)
{
    (void)frame;
    input_count_irq(INPUT_SRC_SERIAL);
    uint8_t b;
    struct input_event ev;
    for (unsigned i = 0; i < 64 && serial_try_read(&b); i++)
        if (serial_decode(&serial_dec, b, &ev))
            input_push(&ev);
    lapic_eoi();
}

static bool ps2_init(void)
{
    if (inb(PS2_STATUS) == 0xFF)
        return false;                       /* no controller answers */
    ps2_command(0xAD);                      /* keyboard port off */
    ps2_command(0xA7);                      /* mouse port off */
    flush_output();

    uint8_t cfg;
    if (!ps2_command(0x20) || !ps2_read_data(&cfg))
        return false;
    /* Interrupts off while we set up; keep translation to scancode set 1. */
    cfg &= (uint8_t)~(CFG_KBD_IRQ | CFG_MOUSE_IRQ);
    ps2_command(0x60);
    ps2_write_data(cfg);

    ps2_command(0xAE);                      /* keyboard port on */
    kbd_ok = device_send(false, 0xF4);      /* start sending keys */

    ps2_command(0xA8);                      /* mouse port on */
    ps2_command(0x20);
    uint8_t cfg2 = cfg;
    ps2_read_data(&cfg2);
    if (!(cfg2 & CFG_MOUSE_OFF))            /* a second port exists */
        mouse_ok = device_send(true, 0xF6) && device_send(true, 0xF4);
    flush_output();

    cfg = cfg2 & (uint8_t)~CFG_MOUSE_OFF;
    if (kbd_ok)
        cfg |= CFG_KBD_IRQ;
    if (mouse_ok)
        cfg |= CFG_MOUSE_IRQ;
    ps2_command(0x60);
    ps2_write_data(cfg);

    if (kbd_ok) {
        interrupt_set_handler(INPUTDEV_KBD_VECTOR, ps2_irq);
        kbd_ok = ioapic_route_irq(1, INPUTDEV_KBD_VECTOR);
    }
    if (mouse_ok) {
        interrupt_set_handler(INPUTDEV_MOUSE_VECTOR, ps2_irq);
        mouse_ok = ioapic_route_irq(12, INPUTDEV_MOUSE_VECTOR);
    }
    return kbd_ok || mouse_ok;
}

bool inputdev_loopback(bool aux, uint8_t byte)
{
    if (!(aux ? mouse_ok : kbd_ok))
        return false;
    /* 0xD2 / 0xD3: "put this byte in the output buffer as if the keyboard
     * / mouse sent it". The controller raises the device's interrupt. */
    return ps2_command(aux ? 0xD3 : 0xD2) && ps2_write_data(byte);
}

void inputdev_init(void)
{
    input_init();
    ps2_init();

    interrupt_set_handler(INPUTDEV_SERIAL_VECTOR, serial_irq);
    serial_enable_rx_irq();
    serial_ok = ioapic_route_irq(4, INPUTDEV_SERIAL_VECTOR);

    input_set_device(INPUT_SRC_PS2_KBD, kbd_ok);
    input_set_device(INPUT_SRC_PS2_MOUSE, mouse_ok);
    input_set_device(INPUT_SRC_SERIAL, serial_ok);
    kprintf("[input] PS/2 keyboard %s (IRQ 1), PS/2 mouse %s (IRQ 12), "
            "serial input %s (IRQ 4)\n",
            kbd_ok ? "on" : "not found", mouse_ok ? "on" : "not found",
            serial_ok ? "on" : "not routed");
}
