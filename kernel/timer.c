#include "timer.h"
#include "serial.h"
#include "idt.h"
#include "interrupts.h"
#include "gdt.h"
#include "cpu.h"

volatile uint64_t timer_ticks = 0;

/* I/O port definitions for PIT */
#define PIT_CHANNEL0_DATA   0x40
#define PIT_CHANNEL1_DATA   0x41
#define PIT_CHANNEL2_DATA   0x42
#define PIT_COMMAND_PORT    0x43

/* I/O port definitions for PIC */
#define PIC_MASTER_CMD   0x20
#define PIC_MASTER_DATA  0x21
#define PIC_SLAVE_CMD    0xA0
#define PIC_SLAVE_DATA   0xA1

/* PIT command byte bits */
#define PIT_CHANNEL(n)      (((n) & 3) << 6)
#define PIT_ACCESS_LOHI     (3 << 4)   /* LSB then MSB */
#define PIT_MODE(n)         (((n) & 7) << 1)
#define PIT_MODE_SQUAREWAVE 3          /* Mode 3: Square wave generator */
#define PIT_MODE_RATEGEN    2          /* Mode 2: Rate generator */
#define PIT_BCD             0          /* Binary mode */
#define PIT_LATCH           0          /* Latch command */

/* Remap the 8259 PICs so IRQ0-15 land on vectors 32-47 instead of colliding
 * with CPU exceptions (IRQ0 defaults to vector 8 = #DF), then mask every
 * line except IRQ0. */
static void pic_remap(void)
{
    outb(PIC_MASTER_CMD, 0x11);  io_wait();   /* ICW1: init + expect ICW4 */
    outb(PIC_SLAVE_CMD, 0x11);   io_wait();
    outb(PIC_MASTER_DATA, IRQ0); io_wait();   /* ICW2: vector offsets */
    outb(PIC_SLAVE_DATA, IRQ8);  io_wait();
    outb(PIC_MASTER_DATA, 0x04); io_wait();   /* ICW3: slave on IRQ2 */
    outb(PIC_SLAVE_DATA, 0x02);  io_wait();
    outb(PIC_MASTER_DATA, 0x01); io_wait();   /* ICW4: 8086 mode */
    outb(PIC_SLAVE_DATA, 0x01);  io_wait();

    outb(PIC_MASTER_DATA, 0xFE);              /* unmask IRQ0 only */
    outb(PIC_SLAVE_DATA, 0xFF);
}

void timer_init(void)
{
    /* The IDT gate for IRQ0 (vector 32) is installed by idt_init(). */
    pic_remap();

    /* Configure PIT channel 0 for square wave mode */
    /* Frequency = 1193180 / divisor */
    /* For ~100Hz: divisor = 1193180 / 100 = 11931 */
    uint16_t divisor = 11931; /* ~100Hz */

    outb(PIT_COMMAND_PORT, PIT_CHANNEL(0) | PIT_ACCESS_LOHI | PIT_MODE(PIT_MODE_SQUAREWAVE) | PIT_BCD);
    outb(PIT_CHANNEL0_DATA, divisor & 0xFF);
    outb(PIT_CHANNEL0_DATA, (divisor >> 8) & 0xFF);

    serial_puts("[timer] PIT initialized (~100Hz).\n");
}

void timer_irq(void)
{
    timer_ticks++;

    /* Output a dot every 100 ticks (~1 second at 100Hz) */
    if (timer_ticks % 100 == 0)
        serial_puts(".");

    outb(PIC_MASTER_CMD, 0x20);   /* EOI */
}

void timer_wait(uint32_t ticks)
{
    /* Simple busy wait - not used yet but kept for future */
    (void)ticks;
}