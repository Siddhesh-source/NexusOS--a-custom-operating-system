#include "net.h"
#include "e1000.h"
#include "spinlock.h"
#include "sched.h"
#include "clock.h"
#include "cpu.h"
#include "heap.h"
#include "kprintf.h"
#include "string.h"

/* Addresses are kept as plain numbers: 10.0.2.15 is 0x0A00020F. They
 * start as QEMU's fixed settings and are replaced by what DHCP hands out. */
#define FIXED_IP 0x0A00020Fu
static uint32_t my_ip = FIXED_IP, gateway = 0x0A000202u, dns_server = 0x0A000203u,
                netmask = 0xFFFFFF00u, lease_s;
static bool from_dhcp;

#define ETH_IP   0x0800
#define ETH_ARP  0x0806
#define PROTO_ICMP 1
#define PROTO_TCP  6
#define PROTO_UDP  17

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_RX_MAX (64 * 1024)
#define TCP_MSS    1460

static struct spinlock net_lock = SPINLOCK_INIT(LOCK_CLASS_NET);
static bool up;
static uint8_t my_mac[6];
static struct net_stats stats;
static uint8_t frame_in[E1000_BUF], frame_out[E1000_BUF];

/* ---- byte order ----------------------------------------------------------- */

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* The Internet checksum: one's-complement sum of 16-bit words. */
static uint32_t sum_add(uint32_t sum, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i += 2)
        sum += get16(p + i);
    if (n & 1)
        sum += (uint32_t)p[n - 1] << 8;
    return sum;
}
static uint16_t sum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint64_t now_ms(void) { return clock_now_ns() / 1000000; }

/* ---- small public helpers ------------------------------------------------- */

bool net_up(void) { return up; }
uint32_t net_ip(void) { return my_ip; }
uint32_t net_gateway(void) { return gateway; }

void net_get_config(struct net_config *out)
{
    SPIN_GUARD(&net_lock);
    out->ip = my_ip;
    out->netmask = netmask;
    out->gateway = gateway;
    out->dns = dns_server;
    out->lease_s = lease_s;
    out->from_dhcp = from_dhcp;
}

void net_mac(uint8_t mac[6]) { memcpy(mac, my_mac, 6); }

void net_get_stats(struct net_stats *out)
{
    SPIN_GUARD(&net_lock);
    *out = stats;
}

bool net_parse_ip(const char *s, uint32_t *ip)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; part++) {
        unsigned n = 0, digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) {
            n = n * 10 + (unsigned)(*s++ - '0');
            digits++;
        }
        if (digits == 0 || n > 255 || (part < 3 && *s++ != '.'))
            return false;
        v = (v << 8) | n;
    }
    if (*s != '\0')
        return false;
    *ip = v;
    return true;
}

void net_format_ip(uint32_t ip, char *buf)
{
    ksnprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

const char *net_status_str(net_status_t s)
{
    switch (s) {
    case NET_OK:              return "ok";
    case NET_ERR_NO_CARD:     return "no network card";
    case NET_ERR_INVALID:     return "not a valid address";
    case NET_ERR_TIMEOUT:     return "no answer in time";
    case NET_ERR_NOT_FOUND:   return "no such name";
    case NET_ERR_REFUSED:     return "the other side refused the connection";
    case NET_ERR_NO_SPACE:    return "out of room";
    case NET_ERR_UNSUPPORTED: return "secure (https) sites are not supported yet";
    }
    return "unknown error";
}

/* ---- ARP ------------------------------------------------------------------ */

#define ARP_CACHE 8
static struct { uint32_t ip; uint8_t mac[6]; bool valid; } arp_cache[ARP_CACHE];
static unsigned arp_victim;

static bool arp_lookup_locked(uint32_t ip, uint8_t mac[6])
{
    for (unsigned i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return true;
        }
    return false;
}

static void arp_learn_locked(uint32_t ip, const uint8_t mac[6])
{
    for (unsigned i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    unsigned i = arp_victim++ % ARP_CACHE;
    arp_cache[i].ip = ip;
    memcpy(arp_cache[i].mac, mac, 6);
    arp_cache[i].valid = true;
}

static void eth_header(uint8_t *f, const uint8_t dst[6], uint16_t type)
{
    memcpy(f, dst, 6);
    memcpy(f + 6, my_mac, 6);
    put16(f + 12, type);
}

static void arp_send_locked(uint16_t op, const uint8_t dst_mac[6], uint32_t target_ip)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t *f = frame_out, *a = f + 14;
    eth_header(f, op == 1 ? bcast : dst_mac, ETH_ARP);
    put16(a, 1);
    put16(a + 2, ETH_IP);
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    memcpy(a + 8, my_mac, 6);
    put32(a + 14, my_ip);
    memset(a + 18, 0, 6);
    if (op == 2)
        memcpy(a + 18, dst_mac, 6);
    put32(a + 24, target_ip);
    if (e1000_send(f, 14 + 28))
        stats.frames_out++;
}

static void arp_input_locked(const uint8_t *a, size_t n)
{
    if (n < 28 || get16(a) != 1 || get16(a + 2) != ETH_IP)
        return;
    uint16_t op = get16(a + 6);
    uint32_t sender = get32(a + 14), target = get32(a + 24);
    arp_learn_locked(sender, a + 8);
    if (op == 1 && target == my_ip) {
        arp_send_locked(2, a + 8, sender);
        stats.arp_replies++;
    }
}

net_status_t net_arp_resolve(uint32_t ip, uint8_t mac[6], unsigned timeout_ms)
{
    if (!up)
        return NET_ERR_NO_CARD;
    uint32_t hop = (ip & netmask) == (my_ip & netmask) ? ip : gateway;
    uint64_t end = now_ms() + timeout_ms, next_ask = 0;
    for (;;) {
        net_poll();
        {
            SPIN_GUARD(&net_lock);
            if (arp_lookup_locked(hop, mac))
                return NET_OK;
            if (now_ms() >= next_ask) {
                arp_send_locked(1, NULL, hop);
                next_ask = now_ms() + 250;
            }
        }
        if (now_ms() >= end)
            return NET_ERR_TIMEOUT;
        sched_sleep(1);
    }
}

/* ---- IPv4 ----------------------------------------------------------------- */

static uint16_t ip_ident;

/* Send an IP packet whose payload is already at frame_out + 34. */
static bool ip_send_locked(const uint8_t dst_mac[6], uint32_t dst, uint8_t proto, size_t len)
{
    if (len > 1500 - 20)
        return false;
    uint8_t *f = frame_out, *ip = f + 14;
    eth_header(f, dst_mac, ETH_IP);
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint16_t)(20 + len));
    put16(ip + 4, ip_ident++);
    put16(ip + 6, 0x4000);                    /* don't fragment */
    ip[8] = 64;
    ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, my_ip);
    put32(ip + 16, dst);
    put16(ip + 10, sum_fold(sum_add(0, ip, 20)));
    bool ok = e1000_send(f, 14 + 20 + len);
    if (ok)
        stats.frames_out++;
    return ok;
}

/* ---- ICMP (ping) ------------------------------------------------------------ */

static struct { uint16_t id, seq; bool got; uint64_t at_ns; } ping_wait;

static void icmp_input_locked(const uint8_t *src_mac, uint32_t src, const uint8_t *p, size_t n)
{
    if (n < 8)
        return;
    if (p[0] == 8) {                          /* someone pings us: answer */
        uint8_t *o = frame_out + 34;
        if (n > 1400)
            return;
        memcpy(o, p, n);
        o[0] = 0;
        put16(o + 2, 0);
        put16(o + 2, sum_fold(sum_add(0, o, n)));
        if (ip_send_locked(src_mac, src, PROTO_ICMP, n))
            stats.pings_answered++;
    } else if (p[0] == 0 && get16(p + 4) == ping_wait.id && get16(p + 6) == ping_wait.seq) {
        ping_wait.got = true;
        ping_wait.at_ns = clock_now_ns();
    }
}

net_status_t net_ping(uint32_t ip, uint16_t seq, unsigned timeout_ms, uint64_t *rtt_us)
{
    uint8_t mac[6];
    net_status_t st = net_arp_resolve(ip, mac, timeout_ms);
    if (st != NET_OK)
        return st;
    uint64_t sent;
    {
        SPIN_GUARD(&net_lock);
        ping_wait.id = 0xB05;
        ping_wait.seq = seq;
        ping_wait.got = false;
        uint8_t *o = frame_out + 34;
        o[0] = 8;
        o[1] = 0;
        put16(o + 2, 0);
        put16(o + 4, ping_wait.id);
        put16(o + 6, seq);
        for (int i = 0; i < 32; i++)
            o[8 + i] = (uint8_t)('a' + i % 26);
        put16(o + 2, sum_fold(sum_add(0, o, 40)));
        sent = clock_now_ns();
        if (!ip_send_locked(mac, ip, PROTO_ICMP, 40))
            return NET_ERR_NO_SPACE;
    }
    uint64_t end = now_ms() + timeout_ms;
    for (;;) {
        net_poll();
        {
            SPIN_GUARD(&net_lock);
            if (ping_wait.got) {
                if (rtt_us != NULL)
                    *rtt_us = (ping_wait.at_ns - sent) / 1000;
                return NET_OK;
            }
        }
        if (now_ms() >= end)
            return NET_ERR_TIMEOUT;
        sched_sleep(1);
    }
}

/* ---- UDP (one waiting reply) -------------------------------------------- */

static struct { uint16_t port; bool ready; size_t len; uint8_t data[600]; } udp_wait;

static void udp_input_locked(const uint8_t *p, size_t n)
{
    if (n < 8)
        return;
    uint16_t dport = get16(p + 2), ulen = get16(p + 4);
    if (ulen < 8 || ulen > n || dport != udp_wait.port || udp_wait.port == 0)
        return;
    size_t len = ulen - 8u < sizeof(udp_wait.data) ? ulen - 8u : sizeof(udp_wait.data);
    memcpy(udp_wait.data, p + 8, len);
    udp_wait.len = len;
    udp_wait.ready = true;
}

/* ---- DNS ------------------------------------------------------------------ */

/* Skip a (possibly compressed) name; returns the offset after it, or 0. */
static size_t dns_skip_name(const uint8_t *m, size_t n, size_t off)
{
    while (off < n) {
        uint8_t l = m[off];
        if (l == 0)
            return off + 1;
        if ((l & 0xC0) == 0xC0)
            return off + 2;
        off += 1 + l;
    }
    return 0;
}

/* Read the name server's answer if it has come: NET_ERR_TIMEOUT while
 * still waiting. */
static net_status_t dns_answer(uint16_t id, uint32_t *ip)
{
    SPIN_GUARD(&net_lock);
    if (!udp_wait.ready)
        return NET_ERR_TIMEOUT;
    const uint8_t *m = udp_wait.data;
    size_t n = udp_wait.len;
    udp_wait.port = 0;
    udp_wait.ready = false;
    if (n < 12 || get16(m) != id)
        return NET_ERR_INVALID;
    if ((m[3] & 0x0F) == 3)
        return NET_ERR_NOT_FOUND;
    unsigned an = get16(m + 6);
    size_t off = dns_skip_name(m, n, 12);
    if (off == 0)
        return NET_ERR_NOT_FOUND;
    off += 4;
    for (unsigned i = 0; i < an && off < n; i++) {
        off = dns_skip_name(m, n, off);
        if (off == 0 || off + 10 > n)
            break;
        uint16_t type = get16(m + off), len = get16(m + off + 8);
        off += 10;
        if (type == 1 && len == 4 && off + 4 <= n) {
            *ip = get32(m + off);
            return NET_OK;
        }
        off += len;
    }
    return NET_ERR_NOT_FOUND;
}

net_status_t net_resolve(const char *host, uint32_t *ip)
{
    if (host == NULL || host[0] == '\0' || strlen(host) > 200)
        return NET_ERR_INVALID;
    if (net_parse_ip(host, ip))
        return NET_OK;
    if (strcmp(host, "localhost") == 0) {
        *ip = my_ip;
        return NET_OK;
    }
    uint8_t mac[6];
    net_status_t st = net_arp_resolve(dns_server, mac, 1000);
    if (st != NET_OK)
        return st;
    uint16_t id = (uint16_t)rdtsc();
    for (int attempt = 0; attempt < 3; attempt++) {
        {
            SPIN_GUARD(&net_lock);
            udp_wait.port = (uint16_t)(40000 + (id & 0x3FF));
            udp_wait.ready = false;
            uint8_t *u = frame_out + 34, *q = u + 8;
            put16(q, id);
            put16(q + 2, 0x0100);             /* please look it up for us */
            put16(q + 4, 1);
            put16(q + 6, 0);
            put16(q + 8, 0);
            put16(q + 10, 0);
            size_t o = 12;
            const char *s = host;
            while (*s != '\0') {
                const char *dot = strchr(s, '.');
                size_t l = dot != NULL ? (size_t)(dot - s) : strlen(s);
                if (l == 0 || l > 63)
                    return NET_ERR_INVALID;
                q[o++] = (uint8_t)l;
                memcpy(q + o, s, l);
                o += l;
                s += l + (dot != NULL);
            }
            q[o++] = 0;
            put16(q + o, 1);                  /* type A: an IPv4 address */
            put16(q + o + 2, 1);
            o += 4;
            put16(u, udp_wait.port);
            put16(u + 2, 53);
            put16(u + 4, (uint16_t)(8 + o));
            put16(u + 6, 0);                  /* no checksum (allowed on IPv4) */
            ip_send_locked(mac, dns_server, PROTO_UDP, 8 + o);
        }
        uint64_t end = now_ms() + 1500;
        while (now_ms() < end) {
            net_poll();
            net_status_t r = dns_answer(id, ip);
            if (r != NET_ERR_TIMEOUT)
                return r;
            sched_sleep(1);
        }
    }
    return NET_ERR_TIMEOUT;
}

/* ---- DHCP ----------------------------------------------------------------- */

/* A DHCP message: a fixed 236-byte part, the "magic cookie", then
 * options (code, length, value), ending with 255. */
#define DHCP_FIXED  236
#define DHCP_MAGIC  0x63825363u

bool net_dhcp_parse(const uint8_t *m, size_t n, uint32_t xid, struct net_dhcp_reply *r)
{
    if (m == NULL || r == NULL || n < DHCP_FIXED + 4 || m[0] != 2 || get32(m + 4) != xid
        || get32(m + DHCP_FIXED) != DHCP_MAGIC)
        return false;
    memset(r, 0, sizeof(*r));
    r->ip = get32(m + 16);
    for (size_t o = DHCP_FIXED + 4; o < n;) {
        uint8_t code = m[o];
        if (code == 255)
            break;
        if (code == 0) {                       /* padding */
            o++;
            continue;
        }
        if (o + 2 > n || o + 2 + m[o + 1] > n)
            return false;                      /* an option runs past the end */
        const uint8_t *v = m + o + 2;
        uint8_t len = m[o + 1];
        if (code == 53 && len == 1) r->type = v[0];
        if (code == 1 && len == 4)  r->netmask = get32(v);
        if (code == 3 && len >= 4)  r->router = get32(v);
        if (code == 6 && len >= 4)  r->dns = get32(v);
        if (code == 51 && len == 4) r->lease_s = get32(v);
        if (code == 54 && len == 4) r->server = get32(v);
        o += 2 + (size_t)len;
    }
    return r->type != 0 && r->ip != 0;
}

/* Broadcast one DHCP message (1 = discover, 3 = request) and wait for an
 * answer of type `want` (2 = offer, 5 = ack; 6 = nak also ends it). */
static net_status_t dhcp_exchange(uint8_t type, uint32_t xid, const struct net_dhcp_reply *offer,
                                  uint8_t want, struct net_dhcp_reply *r, unsigned timeout_ms)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint64_t end = now_ms() + timeout_ms;
    while (now_ms() < end) {
        {
            SPIN_GUARD(&net_lock);
            udp_wait.port = 68;
            udp_wait.ready = false;
            uint8_t *u = frame_out + 34, *d = u + 8;
            memset(d, 0, DHCP_FIXED + 64);
            d[0] = 1;                          /* a request from a client */
            d[1] = 1;                          /* Ethernet */
            d[2] = 6;
            put32(d + 4, xid);
            put16(d + 10, 0x8000);             /* please answer by broadcast */
            memcpy(d + 28, my_mac, 6);
            put32(d + DHCP_FIXED, DHCP_MAGIC);
            uint8_t *o = d + DHCP_FIXED + 4;
            *o++ = 53; *o++ = 1; *o++ = type;
            if (offer != NULL) {
                *o++ = 50; *o++ = 4; put32(o, offer->ip); o += 4;
                *o++ = 54; *o++ = 4; put32(o, offer->server); o += 4;
            }
            *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;   /* mask, router, DNS */
            *o++ = 255;
            size_t len = (size_t)(o - d);
            put16(u, 68);
            put16(u + 2, 67);
            put16(u + 4, (uint16_t)(8 + len));
            put16(u + 6, 0);
            ip_send_locked(bcast, 0xFFFFFFFFu, PROTO_UDP, 8 + len);
        }
        uint64_t retry = now_ms() + 1000;
        while (now_ms() < retry && now_ms() < end) {
            net_poll();
            bool got = false;
            {
                SPIN_GUARD(&net_lock);
                if (udp_wait.ready) {
                    got = net_dhcp_parse(udp_wait.data, udp_wait.len, xid, r);
                    udp_wait.ready = false;
                }
            }
            if (got && (r->type == want || r->type == 6))
                return r->type == 6 ? NET_ERR_REFUSED : NET_OK;
            sched_sleep(1);
        }
    }
    return NET_ERR_TIMEOUT;
}

net_status_t net_dhcp(unsigned timeout_ms)
{
    if (!up)
        return NET_ERR_NO_CARD;
    uint32_t xid = (uint32_t)rdtsc();
    uint32_t old_ip;
    {
        SPIN_GUARD(&net_lock);
        old_ip = my_ip;
        my_ip = 0;                             /* no address until the lease */
    }
    struct net_dhcp_reply offer, ack;
    net_status_t st = dhcp_exchange(1, xid, NULL, 2, &offer, timeout_ms);
    if (st == NET_OK)
        st = dhcp_exchange(3, xid, &offer, 5, &ack, timeout_ms);
    SPIN_GUARD(&net_lock);
    udp_wait.port = 0;
    if (st != NET_OK) {
        my_ip = old_ip;                        /* keep what worked before */
        return st;
    }
    my_ip = ack.ip;
    netmask = ack.netmask != 0 ? ack.netmask : 0xFFFFFF00u;
    gateway = ack.router != 0 ? ack.router : gateway;
    dns_server = ack.dns != 0 ? ack.dns : dns_server;
    lease_s = ack.lease_s;
    from_dhcp = true;
    return NET_OK;
}

/* ---- TCP ------------------------------------------------------------------ */

enum tcp_state { T_FREE, T_SYN_SENT, T_OPEN, T_CLOSED };

struct tcp_conn {
    enum tcp_state state;
    uint8_t mac[6];
    uint32_t rip;
    uint16_t lport, rport;
    uint32_t snd_una, snd_nxt, rcv_nxt;
    bool peer_fin, reset;
    uint8_t *rx;
    size_t rx_len;
    /* The one segment waiting to be acknowledged (stop and wait). */
    uint8_t out[TCP_MSS];
    size_t out_len;
    uint8_t out_flags;
    uint32_t out_seq;
    bool in_flight;
    uint64_t sent_ms;
    unsigned tries;
};

static struct tcp_conn conns[NET_MAX_TCP];
static uint16_t next_port;

static void tcp_emit_locked(struct tcp_conn *c, uint32_t seq, uint8_t flags, const uint8_t *data,
                            size_t len)
{
    uint8_t *t = frame_out + 34;
    size_t hl = (flags & TCP_SYN) ? 24 : 20;
    put16(t, c->lport);
    put16(t + 2, c->rport);
    put32(t + 4, seq);
    put32(t + 8, (flags & TCP_ACK) ? c->rcv_nxt : 0);
    t[12] = (uint8_t)((hl / 4) << 4);
    t[13] = flags;
    size_t room = TCP_RX_MAX - c->rx_len;
    put16(t + 14, (uint16_t)(room > 0xFFFF ? 0xFFFF : room));
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (hl == 24) {                            /* tell the peer our largest segment */
        t[20] = 2; t[21] = 4;
        put16(t + 22, TCP_MSS);
    }
    if (len > 0)
        memcpy(t + hl, data, len);
    uint8_t pseudo[12];
    put32(pseudo, my_ip);
    put32(pseudo + 4, c->rip);
    pseudo[8] = 0;
    pseudo[9] = PROTO_TCP;
    put16(pseudo + 10, (uint16_t)(hl + len));
    put16(t + 16, sum_fold(sum_add(sum_add(0, pseudo, 12), t, hl + len)));
    ip_send_locked(c->mac, c->rip, PROTO_TCP, hl + len);
}

/* Send something that must be acknowledged (SYN, data, FIN). */
static void tcp_send_reliable_locked(struct tcp_conn *c, uint8_t flags, const uint8_t *data, size_t len)
{
    c->out_seq = c->snd_nxt;
    c->out_flags = flags;
    c->out_len = len;
    if (len > 0)
        memcpy(c->out, data, len);
    c->snd_nxt += (uint32_t)len + ((flags & (TCP_SYN | TCP_FIN)) ? 1 : 0);
    c->in_flight = true;
    c->tries = 0;
    c->sent_ms = now_ms();
    tcp_emit_locked(c, c->out_seq, flags, data, len);
}

static bool seq_after(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

static void tcp_input_locked(uint32_t src, const uint8_t *t, size_t n)
{
    if (n < 20)
        return;
    size_t hl = (size_t)(t[12] >> 4) * 4;
    if (hl < 20 || hl > n)
        return;
    uint16_t sport = get16(t), dport = get16(t + 2);
    uint32_t seq = get32(t + 4), ack = get32(t + 8);
    uint8_t flags = t[13];
    const uint8_t *data = t + hl;
    size_t len = n - hl;
    stats.tcp_segments_in++;
    struct tcp_conn *c = NULL;
    for (unsigned i = 0; i < NET_MAX_TCP; i++)
        if (conns[i].state != T_FREE && conns[i].lport == dport && conns[i].rport == sport
            && conns[i].rip == src)
            c = &conns[i];
    if (c == NULL || c->state == T_CLOSED)
        return;
    if (flags & TCP_RST) {
        c->reset = true;
        c->state = T_CLOSED;
        return;
    }
    if (c->state == T_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == c->snd_nxt) {
            c->rcv_nxt = seq + 1;
            c->snd_una = ack;
            c->in_flight = false;
            c->state = T_OPEN;
            tcp_emit_locked(c, c->snd_nxt, TCP_ACK, NULL, 0);
        }
        return;
    }
    if ((flags & TCP_ACK) && seq_after(ack, c->snd_una) && !seq_after(ack, c->snd_nxt)) {
        c->snd_una = ack;
        if (c->snd_una == c->snd_nxt)
            c->in_flight = false;
    }
    bool reply = false;
    if (len > 0) {
        if (seq == c->rcv_nxt && c->rx_len + len <= TCP_RX_MAX) {
            memcpy(c->rx + c->rx_len, data, len);
            c->rx_len += len;
            c->rcv_nxt += (uint32_t)len;
        }
        reply = true;                         /* in order or not: say what we have */
    }
    if ((flags & TCP_FIN) && seq + (uint32_t)len == c->rcv_nxt && !c->peer_fin) {
        c->rcv_nxt++;
        c->peer_fin = true;
        reply = true;
    }
    if (reply)
        tcp_emit_locked(c, c->snd_nxt, TCP_ACK, NULL, 0);
}

static void tcp_timers_locked(void)
{
    uint64_t now = now_ms();
    for (unsigned i = 0; i < NET_MAX_TCP; i++) {
        struct tcp_conn *c = &conns[i];
        if (c->state == T_FREE || c->state == T_CLOSED || !c->in_flight)
            continue;
        if (now - c->sent_ms < (300ull << c->tries))
            continue;
        if (++c->tries > 6) {
            c->state = T_CLOSED;              /* give up */
            continue;
        }
        c->sent_ms = now;
        stats.tcp_resent++;
        tcp_emit_locked(c, c->out_seq, c->out_flags, c->out, c->out_len);
    }
}

net_status_t tcp_connect(uint32_t ip, uint16_t port, int *conn, unsigned timeout_ms)
{
    uint8_t mac[6];
    net_status_t st = net_arp_resolve(ip, mac, timeout_ms);
    if (st != NET_OK)
        return st;
    uint8_t *rx = kmalloc(TCP_RX_MAX);
    if (rx == NULL)
        return NET_ERR_NO_SPACE;
    struct tcp_conn *c = NULL;
    {
        SPIN_GUARD(&net_lock);
        for (unsigned i = 0; i < NET_MAX_TCP && c == NULL; i++)
            if (conns[i].state == T_FREE) {
                c = &conns[i];
                *conn = (int)i;
            }
        if (c != NULL) {
            memset(c, 0, sizeof(*c));
            c->rx = rx;
            memcpy(c->mac, mac, 6);
            c->rip = ip;
            c->rport = port;
            if (next_port < 49152)
                next_port = (uint16_t)(49152 + (rdtsc() & 0x3FFF));
            c->lport = next_port++;
            c->snd_nxt = c->snd_una = (uint32_t)rdtsc();
            c->state = T_SYN_SENT;
            tcp_send_reliable_locked(c, TCP_SYN, NULL, 0);
        }
    }
    if (c == NULL) {
        kfree(rx);
        return NET_ERR_NO_SPACE;
    }
    uint64_t end = now_ms() + timeout_ms;
    for (;;) {
        net_poll();
        enum tcp_state s;
        bool reset;
        {
            SPIN_GUARD(&net_lock);
            s = c->state;
            reset = c->reset;
        }
        if (s == T_OPEN)
            return NET_OK;
        if (s == T_CLOSED || now_ms() >= end) {
            tcp_close(*conn);
            return reset ? NET_ERR_REFUSED : NET_ERR_TIMEOUT;
        }
        sched_sleep(1);
    }
}

net_status_t tcp_send(int conn, const void *data, size_t len, unsigned timeout_ms)
{
    if (conn < 0 || conn >= NET_MAX_TCP)
        return NET_ERR_INVALID;
    struct tcp_conn *c = &conns[conn];
    const uint8_t *p = data;
    while (len > 0) {
        size_t n = len < TCP_MSS ? len : TCP_MSS;
        {
            SPIN_GUARD(&net_lock);
            if (c->state != T_OPEN)
                return c->reset ? NET_ERR_REFUSED : NET_ERR_TIMEOUT;
            tcp_send_reliable_locked(c, TCP_ACK | TCP_PSH, p, n);
        }
        uint64_t end = now_ms() + timeout_ms;
        for (;;) {
            net_poll();
            bool done, dead;
            {
                SPIN_GUARD(&net_lock);
                done = !c->in_flight;
                dead = c->state != T_OPEN;
            }
            if (done)
                break;
            if (dead || now_ms() >= end)
                return NET_ERR_TIMEOUT;
            sched_sleep(1);
        }
        p += n;
        len -= n;
    }
    return NET_OK;
}

net_status_t tcp_receive_all(int conn, void *buf, size_t cap, size_t *len, unsigned timeout_ms)
{
    if (conn < 0 || conn >= NET_MAX_TCP || buf == NULL)
        return NET_ERR_INVALID;
    struct tcp_conn *c = &conns[conn];
    uint8_t *out = buf;
    size_t got = 0;
    uint64_t end = now_ms() + timeout_ms;
    for (;;) {
        net_poll();
        bool fin, dead, full = false;
        {
            SPIN_GUARD(&net_lock);
            size_t n = c->rx_len;
            bool was_full = TCP_RX_MAX - n < TCP_MSS;
            if (n > cap - got) {
                n = cap - got;
                full = true;
            }
            memcpy(out + got, c->rx, n);
            memmove(c->rx, c->rx + n, c->rx_len - n);
            c->rx_len -= n;
            got += n;
            if (n > 0)
                end = now_ms() + timeout_ms;  /* still talking: keep waiting */
            if (was_full && n > 0)            /* room again: tell the peer */
                tcp_emit_locked(c, c->snd_nxt, TCP_ACK, NULL, 0);
            fin = c->peer_fin && c->rx_len == 0;
            dead = c->state == T_CLOSED;
        }
        *len = got;
        if (full)
            return NET_ERR_NO_SPACE;
        if (fin)
            return NET_OK;
        if (dead)
            return c->reset ? NET_ERR_REFUSED : NET_ERR_TIMEOUT;
        if (now_ms() >= end)
            return NET_ERR_TIMEOUT;
        sched_sleep(1);
    }
}

void tcp_close(int conn)
{
    if (conn < 0 || conn >= NET_MAX_TCP)
        return;
    uint8_t *rx;
    {
        SPIN_GUARD(&net_lock);
        struct tcp_conn *c = &conns[conn];
        if (c->state == T_OPEN)               /* say goodbye; don't wait for it */
            tcp_emit_locked(c, c->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
        else if (c->state == T_SYN_SENT)
            tcp_emit_locked(c, c->snd_nxt, TCP_RST, NULL, 0);
        rx = c->rx;
        memset(c, 0, sizeof(*c));
    }
    kfree(rx);
}

/* ---- receiving -------------------------------------------------------------- */

static void ip_input_locked(const uint8_t *src_mac, const uint8_t *ip, size_t n)
{
    if (n < 20 || (ip[0] >> 4) != 4)
        return;
    size_t ihl = (size_t)(ip[0] & 0x0F) * 4, total = get16(ip + 2);
    if (ihl < 20 || total < ihl || total > n || (get16(ip + 6) & 0x3FFF) != 0)
        return;                               /* bad, or a fragment */
    uint32_t dst = get32(ip + 16);              /* DHCP answers may be broadcast */
    if ((dst != my_ip && !(dst == 0xFFFFFFFFu && ip[9] == PROTO_UDP)) || sum_fold(sum_add(0, ip, ihl)) != 0)
        return;
    uint32_t src = get32(ip + 12);
    const uint8_t *p = ip + ihl;
    size_t len = total - ihl;
    switch (ip[9]) {
    case PROTO_ICMP: icmp_input_locked(src_mac, src, p, len); break;
    case PROTO_UDP:  udp_input_locked(p, len); break;
    case PROTO_TCP:  tcp_input_locked(src, p, len); break;
    }
}

void net_poll(void)
{
    if (!up)
        return;
    SPIN_GUARD(&net_lock);
    for (int budget = 0; budget < 64; budget++) {
        size_t n = e1000_receive(frame_in);
        if (n == 0)
            break;
        stats.frames_in++;
        if (n < 14)
            continue;
        uint16_t type = get16(frame_in + 12);
        if (type == ETH_ARP)
            arp_input_locked(frame_in + 14, n - 14);
        else if (type == ETH_IP)
            ip_input_locked(frame_in + 6, frame_in + 14, n - 14);
    }
    tcp_timers_locked();
}

bool net_init(void)
{
    if (!e1000_init())
        return false;
    e1000_mac(my_mac);
    up = true;
    char ip[16], gw[16], dns[16];
    net_status_t st = net_dhcp(3000);
    net_format_ip(my_ip, ip);
    net_format_ip(gateway, gw);
    net_format_ip(dns_server, dns);
    if (st == NET_OK)
        kprintf("[net] DHCP gave us %s (gateway %s, name server %s, lease %u s)\n", ip, gw, dns, lease_s);
    else
        kprintf("[net] DHCP: %s; using the fixed address %s\n", net_status_str(st), ip);
    return true;
}

/* ---- HTTP ------------------------------------------------------------------- */

/* Split "http://host[:port]/path". */
static net_status_t parse_url(const char *url, char *host, size_t hcap, uint16_t *port, char *path,
                              size_t pcap)
{
    if (strncmp(url, "https://", 8) == 0)
        return NET_ERR_UNSUPPORTED;
    if (strncmp(url, "http://", 7) == 0)
        url += 7;
    size_t h = 0;
    while (url[h] != '\0' && url[h] != '/' && url[h] != ':')
        h++;
    if (h == 0 || h >= hcap)
        return NET_ERR_INVALID;
    memcpy(host, url, h);
    host[h] = '\0';
    url += h;
    *port = 80;
    if (*url == ':') {
        unsigned p = 0;
        url++;
        while (*url >= '0' && *url <= '9')
            p = p * 10 + (unsigned)(*url++ - '0');
        if (p == 0 || p > 65535)
            return NET_ERR_INVALID;
        *port = (uint16_t)p;
    }
    strlcpy(path, *url == '/' ? url : "/", pcap);
    return strlen(url) < pcap ? NET_OK : NET_ERR_INVALID;
}

static bool header_value(const char *hdr, size_t n, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (size_t i = 0; i + nl < n; i++) {
        if ((i == 0 || hdr[i - 1] == '\n') && strncasecmp(hdr + i, name, nl) == 0 && hdr[i + nl] == ':') {
            size_t s = i + nl + 1, k = 0;
            while (s < n && hdr[s] == ' ')
                s++;
            while (s < n && hdr[s] != '\r' && hdr[s] != '\n' && k + 1 < cap)
                out[k++] = hdr[s++];
            out[k] = '\0';
            return true;
        }
    }
    return false;
}

net_status_t http_get(const char *url, char *buf, size_t cap, size_t *len, int *status, char *final_url)
{
    char cur[256], host[128], path[200], req[512];
    if (url == NULL || buf == NULL || cap < 64)
        return NET_ERR_INVALID;
    strlcpy(cur, url, sizeof(cur));
    *len = 0;
    *status = 0;
    for (int hop = 0; hop < 4; hop++) {
        if (final_url != NULL)
            strlcpy(final_url, cur, 256);
        uint16_t port;
        net_status_t st = parse_url(cur, host, sizeof(host), &port, path, sizeof(path));
        if (st != NET_OK)
            return st;
        if (!up)
            return NET_ERR_NO_CARD;
        uint32_t ip;
        if ((st = net_resolve(host, &ip)) != NET_OK)
            return st;
        int c;
        if ((st = tcp_connect(ip, port, &c, 4000)) != NET_OK)
            return st;
        int n = ksnprintf(req, sizeof(req),
                          "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: BharatOS-Browser/1.0\r\n"
                          "Accept: text/html, text/plain, */*\r\nConnection: close\r\n\r\n", path, host);
        st = tcp_send(c, req, (size_t)n, 4000);
        size_t got = 0;
        if (st == NET_OK)
            st = tcp_receive_all(c, buf, cap - 1, &got, 5000);
        tcp_close(c);
        if (st != NET_OK && !(st == NET_ERR_TIMEOUT && got > 0) && st != NET_ERR_NO_SPACE)
            return st;
        buf[got] = '\0';
        /* Status line and headers end at the first empty line. */
        if (got < 12 || strncmp(buf, "HTTP/1.", 7) != 0)
            return NET_ERR_INVALID;
        *status = (buf[9] - '0') * 100 + (buf[10] - '0') * 10 + (buf[11] - '0');
        size_t body = 0;
        for (size_t i = 0; i + 3 < got; i++)
            if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                body = i + 4;
                break;
            }
        if (body == 0)
            body = got;
        char loc[256];
        if (*status >= 300 && *status < 400 && header_value(buf, body, "Location", loc, sizeof(loc))) {
            if (loc[0] == '/') {
                char base[160];
                if (port == 80)
                    ksnprintf(base, sizeof(base), "http://%s", host);
                else
                    ksnprintf(base, sizeof(base), "http://%s:%u", host, port);
                ksnprintf(cur, sizeof(cur), "%s%s", base, loc);
            } else {
                strlcpy(cur, loc, sizeof(cur));
            }
            if (strncmp(cur, "https://", 8) == 0) {
                if (final_url != NULL)
                    strlcpy(final_url, cur, 256);
                return NET_ERR_UNSUPPORTED;
            }
            continue;
        }
        memmove(buf, buf + body, got - body);
        *len = got - body;
        buf[*len] = '\0';
        return st == NET_ERR_NO_SPACE ? NET_ERR_NO_SPACE : NET_OK;
    }
    return NET_ERR_INVALID;                   /* too many redirects */
}
