#ifndef BHARAT_E1000_H
#define BHARAT_E1000_H

/* Driver for the Intel 8254x ("e1000") network card, the one QEMU and
 * VirtualBox emulate by default.
 *
 * The card copies packets to and from memory on its own (DMA) using two
 * rings of descriptors: small records holding a buffer's physical address
 * and length. To send, we fill the next transmit descriptor and move the
 * "tail" register past it; the card sends it and sets a "done" bit. To
 * receive, we give the card empty buffers; it fills them and sets "done"
 * on each one, and we read them and hand the buffer back by moving the
 * receive tail.
 *
 * We poll the rings instead of using interrupts: the network stack checks
 * for new packets whenever someone is waiting for one. Simple, and fast
 * enough for a browser and ping.
 *
 * Callers hold the network lock (net.c); the driver keeps no lock itself.
 *
 * Limits: one card; 32 receive and 32 transmit buffers of 2 KiB; no
 * checksum offload; no interrupts. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define E1000_BUF 2048

/* Find and start the card. False if there is none (or it will not start). */
bool e1000_init(void);
bool e1000_present(void);
void e1000_mac(uint8_t mac[6]);
/* True when the cable is "plugged in" (link up). */
bool e1000_link_up(void);

/* Queue one frame (14-byte Ethernet header included) for sending. False if
 * the transmit ring is full or len is out of range. */
bool e1000_send(const void *frame, size_t len);
/* Copy the next received frame into `buf` (at least E1000_BUF bytes).
 * Returns its length, or 0 if nothing has arrived. */
size_t e1000_receive(void *buf);

struct e1000_stats {
    uint64_t sent, received, dropped;
};
void e1000_get_stats(struct e1000_stats *out);

#endif /* BHARAT_E1000_H */
