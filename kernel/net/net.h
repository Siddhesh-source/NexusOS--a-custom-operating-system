#ifndef BHARAT_NET_H
#define BHARAT_NET_H

/* The network stack: enough of TCP/IP to ping, look up names and fetch
 * web pages over HTTP.
 *
 *   Ethernet  frames to and from the card (e1000.c)
 *   ARP       finds the card address (MAC) that owns an IP address
 *   IPv4      addresses and checksums; no fragments
 *   ICMP      ping, and answering other machines' pings
 *   UDP       one waiting reply at a time (used by DNS)
 *   DNS       turns "example.com" into an IP address
 *   TCP       reliable byte streams: connect, send, receive, close, with
 *             resending of anything not acknowledged
 *   HTTP      GET a page (HTTP/1.0, follows a few redirects)
 *
 *   DHCP      asks the network for our address, gateway and name server
 *
 * Settings come from DHCP at start-up. If nobody answers, QEMU's fixed
 * "user" network settings are used: our address 10.0.2.15, the gateway
 * 10.0.2.2 (which is also the host PC), the name server 10.0.2.3.
 *
 * How it runs: nothing runs in the background. Whoever waits for the
 * network (ping, the browser) calls net_poll() in a loop, which takes the
 * packets that arrived, answers ARP and pings, and files TCP data into the
 * right connection. One spinlock ("network") guards all of it; waiting
 * happens outside the lock (sched_sleep), never while holding it.
 *
 * Limits: IPv4 only; the DHCP lease is not renewed; NET_MAX_TCP connections;
 * TCP keeps data that arrives in order and asks again for the rest; no
 * HTTPS (that needs TLS, a later step); a page is at most NET_PAGE_MAX
 * bytes. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_MAX_TCP   4
#define NET_PAGE_MAX  (512 * 1024)

typedef enum {
    NET_OK = 0,
    NET_ERR_NO_CARD,      /* no network card */
    NET_ERR_INVALID,      /* bad address, URL or argument */
    NET_ERR_TIMEOUT,      /* no answer in time */
    NET_ERR_NOT_FOUND,    /* the name does not exist */
    NET_ERR_REFUSED,      /* the other side said no (connection reset) */
    NET_ERR_NO_SPACE,     /* out of connections or memory, or page too big */
    NET_ERR_UNSUPPORTED,  /* e.g. an https:// address */
} net_status_t;

struct net_stats {
    uint64_t frames_in, frames_out, arp_replies, pings_answered, tcp_segments_in, tcp_resent;
};

/* Where we are on the network: given by DHCP, or QEMU's fixed settings
 * if no DHCP server answered. */
struct net_config {
    uint32_t ip, netmask, gateway, dns, lease_s;
    bool from_dhcp;
};

/* One DHCP answer, as read by net_dhcp_parse. type: 2 offer, 5 ack, 6 no. */
struct net_dhcp_reply {
    uint8_t type;
    uint32_t ip, server, netmask, router, dns, lease_s;
};

/* Start the card and the stack, then ask DHCP for an address. False if
 * there is no card. */
bool net_init(void);
bool net_up(void);
/* Our address. Addresses are plain numbers: 10.0.2.15 is 0x0A00020F. */
uint32_t net_ip(void);
uint32_t net_gateway(void);
void net_get_config(struct net_config *out);
/* Ask the network's DHCP server for an address (discover, offer,
 * request, ack). On failure the old settings stay. */
net_status_t net_dhcp(unsigned timeout_ms);
/* Read a DHCP answer for transaction `xid`; false if it is not one or is
 * damaged (an option running past the end, no message type, ...). */
bool net_dhcp_parse(const uint8_t *m, size_t n, uint32_t xid, struct net_dhcp_reply *r);
void net_mac(uint8_t mac[6]);
void net_get_stats(struct net_stats *out);

/* Handle every packet that has arrived. Cheap when there are none. */
void net_poll(void);

/* "10.0.2.2" -> address; false if it is not dotted numbers. */
bool net_parse_ip(const char *s, uint32_t *ip);
/* Address -> "10.0.2.2" (buf at least 16 bytes). */
void net_format_ip(uint32_t ip, char *buf);

/* Send one ping and wait up to timeout_ms for the answer; the round trip
 * in microseconds goes to *rtt_us. */
net_status_t net_ping(uint32_t ip, uint16_t seq, unsigned timeout_ms, uint64_t *rtt_us);
/* Find the card address of `ip` (or of the gateway, for other networks). */
net_status_t net_arp_resolve(uint32_t ip, uint8_t mac[6], unsigned timeout_ms);
/* Name or dotted address -> address, asking the name server if needed. */
net_status_t net_resolve(const char *host, uint32_t *ip);

/* TCP: a connection number 0..NET_MAX_TCP-1. */
net_status_t tcp_connect(uint32_t ip, uint16_t port, int *conn, unsigned timeout_ms);
net_status_t tcp_send(int conn, const void *data, size_t len, unsigned timeout_ms);
/* Collect everything the other side sends until it closes (or `cap` is
 * full: NET_ERR_NO_SPACE). */
net_status_t tcp_receive_all(int conn, void *buf, size_t cap, size_t *len, unsigned timeout_ms);
void tcp_close(int conn);

/* Fetch an http:// URL. The body (headers removed) goes into `buf`;
 * *status gets the HTTP status (200, 404, ...). `final_url` (if not NULL,
 * 256 bytes) gets the address after redirects. */
net_status_t http_get(const char *url, char *buf, size_t cap, size_t *len, int *status,
                      char *final_url);

const char *net_status_str(net_status_t s);

#endif /* BHARAT_NET_H */
