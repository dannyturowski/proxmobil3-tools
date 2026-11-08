/* wt32-r4 DHCPv4 server core; see nori_dhcps.h.  Portable C, no lwIP. */
#include "nori_dhcps.h"

#include <stdio.h>
#include <string.h>

const char *const NDH_EV_NAME[NDH_EV_COUNT] = { "none", "offer", "ack", "nak", "release", "decline", "inform",
                                                "full", "other", "bad" };
const char *const NDH_STATE_NAME[5] = { "free", "offered", "bound", "expired", "declined" };

/* BOOTP header offsets */
enum { O_OP = 0, O_HTYPE = 1, O_HLEN = 2, O_XID = 4, O_FLAGS = 10, O_CIADDR = 12, O_YIADDR = 16, O_SIADDR = 20,
       O_GIADDR = 24, O_CHADDR = 28, O_COOKIE = 236, O_OPTS = 240 };
static const uint8_t COOKIE[4] = { 99, 130, 83, 99 };
#define BCAST 0xffffffffu

/* ---- byte order helpers: addresses are kept in network order as stored in memory ---- */
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t n2h(uint32_t v)
{
    const uint8_t *b = (const uint8_t *)&v;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static uint32_t h2n(uint32_t h)
{
    uint32_t v;
    uint8_t *b = (uint8_t *)&v;
    b[0] = (uint8_t)(h >> 24); b[1] = (uint8_t)(h >> 16); b[2] = (uint8_t)(h >> 8); b[3] = (uint8_t)h;
    return v;
}
static void put_be32(uint8_t *p, uint32_t h) { p[0] = (uint8_t)(h >> 24); p[1] = (uint8_t)(h >> 16); p[2] = (uint8_t)(h >> 8); p[3] = (uint8_t)h; }

void ndh_init(ndh_state *s) { memset(s, 0, sizeof *s); }

int ndh_cfg_valid(const ndh_cfg *c)
{
    uint32_t m = n2h(c->netmask), sv = n2h(c->server_ip), f = n2h(c->pool_first);
    if (!c->pool_n || c->pool_n > NDH_POOL_MAX || c->lease_s < 60 || c->short_lease_s < 30) return 0;
    if (m == 0 || (m | (m - 1)) != 0xffffffffu || (m & 1)) return 0;   /* contiguous, not /32 */
    if ((uint32_t)(f + c->pool_n - 1) < f) return 0;                   /* no wrap */
    for (uint32_t a = f; a < f + c->pool_n; a++) {
        if ((a & m) != (sv & m) || a == sv) return 0;
        if ((a & ~m) == 0 || (a & ~m) == ~m) return 0;                 /* network / broadcast address */
    }
    return (sv & ~m) != 0 && (sv & ~m) != ~m;
}

const char *ndh_mac_str(const uint8_t mac[6], char *buf)
{
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

/* ---- lease table ---- */
static int pool_index(const ndh_cfg *c, uint32_t ip)     /* -1 if ip (network order) is not a pool address */
{
    uint32_t h = n2h(ip), f = n2h(c->pool_first);
    return h >= f && h - f < c->pool_n ? (int)(h - f) : -1;
}
static uint32_t pool_ip(const ndh_cfg *c, int i) { return h2n(n2h(c->pool_first) + (uint32_t)i); }
static int same_mac(const ndh_lease *l, const uint8_t mac[6]) { return l->has_mac && !memcmp(l->mac, mac, 6); }

/* May this address be given to mac right now? */
static int available_to(const ndh_lease *l, const uint8_t mac[6])
{
    switch (l->state) {
    case NDH_FREE: case NDH_EXPIRED: return 1;
    case NDH_OFFERED: case NDH_BOUND: return same_mac(l, mac) || l->stale;
    default: return 0;                                     /* DECLINED: held */
    }
}
/* This MAC's current entry (offered / bound, else the address it last had), or -1. */
static int own_entry(const ndh_state *s, const ndh_cfg *c, const uint8_t mac[6])
{
    int best = -1;
    for (int i = 0; i < c->pool_n; i++) {
        const ndh_lease *l = &s->l[i];
        if (!same_mac(l, mac) || l->state == NDH_DECLINED) continue;
        if (l->state == NDH_OFFERED || l->state == NDH_BOUND) return i;
        if (best < 0) best = i;
    }
    return best;
}
static void set_state(ndh_lease *l, uint8_t st, const uint8_t *mac, uint32_t now, uint32_t dur)
{
    l->state = st;
    l->stale = 0;
    if (mac) { memcpy(l->mac, mac, 6); l->has_mac = 1; }
    l->t_ms = now;
    l->dur_ms = dur;
}
/* One binding per MAC: forget this MAC's offers / leases on other addresses. */
static void drop_others(ndh_state *s, const ndh_cfg *c, const uint8_t mac[6], int keep)
{
    for (int i = 0; i < c->pool_n; i++) {
        ndh_lease *l = &s->l[i];
        if (i == keep || !same_mac(l, mac)) continue;
        if (l->state == NDH_OFFERED || l->state == NDH_BOUND) l->state = NDH_FREE;
        l->has_mac = 0;
    }
}

int ndh_tick(ndh_state *s, uint32_t now_ms)
{
    int n = 0;
    for (int i = 0; i < NDH_POOL_MAX; i++) {
        ndh_lease *l = &s->l[i];
        if (l->state != NDH_OFFERED && l->state != NDH_BOUND && l->state != NDH_DECLINED) continue;
        if ((uint32_t)(now_ms - l->t_ms) < l->dur_ms) continue;
        if (l->state == NDH_BOUND) { l->state = NDH_EXPIRED; s->expired++; }
        else l->state = NDH_FREE;                           /* an unanswered offer / a decline hold ran out */
        n++;
    }
    return n;
}

void ndh_link_cycle(ndh_state *s)
{
    for (int i = 0; i < NDH_POOL_MAX; i++) if (s->l[i].state == NDH_OFFERED || s->l[i].state == NDH_BOUND) s->l[i].stale = 1;
}

int ndh_bound(const ndh_state *s)
{
    int n = 0;
    for (int i = 0; i < NDH_POOL_MAX; i++) n += s->l[i].state == NDH_BOUND;
    return n;
}
int ndh_primary(const ndh_state *s) { return s->l[0].state == NDH_BOUND ? 0 : -1; }

uint32_t ndh_remaining_ms(const ndh_lease *l, uint32_t now_ms)
{
    if (l->state != NDH_BOUND) return 0;
    uint32_t el = now_ms - l->t_ms;
    return el < l->dur_ms ? l->dur_ms - el : 0;
}

/* ---- parsing ---- */
typedef struct {
    uint8_t type;
    uint8_t has_sid, has_req;
    uint32_t sid, req;                                      /* network order */
    const uint8_t *cid;                                     /* option 61 client identifier (inside the packet), echoed */
    uint8_t cid_len;
} opts_t;

/* 0 ok, -1 malformed, -2 valid but unsupported (option overload) */
static int parse_opts(const uint8_t *p, size_t n, opts_t *o)
{
    size_t i = 0;
    memset(o, 0, sizeof *o);
    while (i < n) {
        uint8_t code = p[i++];
        if (code == 0) continue;                            /* pad */
        if (code == 255) break;                             /* end */
        if (i >= n) return -1;
        uint8_t len = p[i++];
        if (len > n - i) return -1;
        const uint8_t *v = p + i;
        i += len;
        switch (code) {
        case 53:
            if (len != 1) return -1;
            if (!o->type) o->type = v[0];
            break;
        case 50:
            if (len != 4) return -1;
            if (!o->has_req) { o->req = rd32(v); o->has_req = 1; }
            break;
        case 54:
            if (len != 4) return -1;
            if (!o->has_sid) { o->sid = rd32(v); o->has_sid = 1; }
            break;
        case 61:                                            /* RFC 2132: at least type + 1 byte; RFC 6842: echoed unchanged */
            if (len < 2) return -1;
            if (len > NDH_CID_MAX) return -2;                /* bounded: real ones are 7 (MAC) or ~19 (DUID) bytes */
            if (!o->cid) { o->cid = v; o->cid_len = len; }
            break;
        case 52:
            return -2;                                      /* options continued in sname/file: not supported */
        default:
            break;
        }
    }
    if (o->type < NDH_DISCOVER || o->type > NDH_INFORM) return -1;
    return 0;
}

static int mac_ok(const uint8_t *m)
{
    static const uint8_t zero[6];
    if (!memcmp(m, zero, 6)) return 0;
    if (m[0] & 1) return 0;                                 /* broadcast / multicast */
    return 1;
}

/* ---- reply building ---- */
typedef struct { uint8_t *p; size_t n, cap; } wbuf;
static void opt(wbuf *w, uint8_t code, const void *v, uint8_t len)
{
    if (w->n + 2u + len + 1u > w->cap) return;              /* keep room for the end option (never hit: fixed size) */
    w->p[w->n++] = code;
    w->p[w->n++] = len;
    memcpy(w->p + w->n, v, len);
    w->n += len;
}
static void opt32(wbuf *w, uint8_t code, uint32_t net) { opt(w, code, &net, 4); }
static void opt_be(wbuf *w, uint8_t code, uint32_t host_val) { uint8_t b[4]; put_be32(b, host_val); opt(w, code, b, 4); }

static size_t build(const ndh_cfg *c, const uint8_t *req, const opts_t *ro, uint8_t type, uint32_t ciaddr, uint32_t yiaddr, uint32_t lease_s,
                    const uint32_t dns[2], uint8_t *out, size_t cap)
{
    if (cap < NDH_REPLY_MAX) return 0;
    memset(out, 0, NDH_REPLY_MAX);
    out[O_OP] = 2;                                          /* BOOTREPLY */
    out[O_HTYPE] = 1;
    out[O_HLEN] = 6;
    memcpy(out + O_XID, req + O_XID, 4);
    memcpy(out + O_FLAGS, req + O_FLAGS, 2);
    wr32(out + O_CIADDR, ciaddr);
    wr32(out + O_YIADDR, yiaddr);
    memcpy(out + O_CHADDR, req + O_CHADDR, 16);
    memcpy(out + O_COOKIE, COOKIE, 4);
    wbuf w = { out, O_OPTS, NDH_REPLY_MAX };
    opt(&w, 53, &type, 1);
    opt32(&w, 54, c->server_ip);
    if (ro->cid) opt(&w, 61, ro->cid, ro->cid_len);       /* RFC 6842 section 3: returned unchanged, also in a NAK */
    if (type != NDH_NAK) {
        if (lease_s) {
            opt_be(&w, 51, lease_s);
            opt_be(&w, 58, lease_s / 2);
            opt_be(&w, 59, lease_s / 8 * 7);
        }
        opt32(&w, 1, c->netmask);
        opt32(&w, 3, c->server_ip);
        opt32(&w, 28, c->server_ip | ~c->netmask);
        uint8_t d[8];
        uint8_t k = 0;
        for (int i = 0; i < 2; i++) if (dns[i]) { memcpy(d + 4 * k, &dns[i], 4); k++; }
        if (k) opt(&w, 6, d, (uint8_t)(4 * k));
    }
    out[w.n++] = 255;
    return w.n < NDH_REPLY_MIN ? NDH_REPLY_MIN : w.n;
}

static void note(ndh_state *s, ndh_out *o, int ev, const uint8_t *mac, uint32_t ip, uint32_t lease_s, uint32_t dst, uint32_t now)
{
    o->ev = ev;
    o->yiaddr = ip;
    o->lease_s = lease_s;
    o->dst_ip = dst;
    if (mac) memcpy(o->mac, mac, 6);
    s->last_ev = (uint8_t)ev;
    if (mac) memcpy(s->last_mac, mac, 6);
    s->last_ip = ip;
    s->last_ms = now;
    if (lease_s) s->last_lease_s = lease_s;
}

size_t ndh_handle(ndh_state *s, const ndh_cfg *c, const uint8_t *pkt, size_t n, uint32_t now, const uint32_t dns[2],
                  uint8_t *out, size_t cap, ndh_out *o)
{
    opts_t op;
    memset(o, 0, sizeof *o);
    s->rx++;
    if (!pkt || n < NDH_REQ_MIN || n > NDH_REQ_MAX || pkt[O_HTYPE] != 1 || pkt[O_HLEN] != 6 || memcmp(pkt + O_COOKIE, COOKIE, 4)) {
        s->bad++;
        o->ev = NDH_EV_BAD;
        return 0;
    }
    int pr = parse_opts(pkt + O_OPTS, n - O_OPTS, &op);
    if (pr == -1) { s->bad++; o->ev = NDH_EV_BAD; return 0; }
    const uint8_t *mac = pkt + O_CHADDR;
    if (pr == -2 || pkt[O_OP] != 1 || rd32(pkt + O_GIADDR) != 0 || !mac_ok(mac)) {
        s->ignored++;
        o->ev = NDH_EV_BAD;
        return 0;
    }
    ndh_tick(s, now);
    memcpy(o->mac, mac, 6);
    uint32_t ciaddr = rd32(pkt + O_CIADDR);
    uint32_t lease = (dns[0] || dns[1]) ? c->lease_s : c->short_lease_s;
    uint32_t snet = c->server_ip & c->netmask;

    switch (op.type) {
    case NDH_DISCOVER: {
        s->discover++;
        int i = own_entry(s, c, mac);
        if (i >= 0 && !available_to(&s->l[i], mac)) i = -1;
        if (i < 0 && op.has_req) {
            int r = pool_index(c, op.req);
            if (r >= 0 && available_to(&s->l[r], mac)) i = r;
        }
        for (int k = 0; i < 0 && k < c->pool_n; k++) if (available_to(&s->l[k], mac)) i = k;
        if (i < 0) { s->full++; note(s, o, NDH_EV_FULL, mac, 0, 0, 0, now); return 0; }
        drop_others(s, c, mac, i);
        ndh_lease *l = &s->l[i];
        if (!(l->state == NDH_BOUND && same_mac(l, mac))) set_state(l, NDH_OFFERED, mac, now, NDH_OFFER_HOLD_MS);
        else l->stale = 0;                                  /* re-discover while bound: keep the lease, re-offer it */
        s->offer++;
        uint32_t ip = pool_ip(c, i);
        note(s, o, NDH_EV_OFFER, mac, ip, lease, BCAST, now);
        return build(c, pkt, &op, NDH_OFFER, 0, ip, lease, dns, out, cap);
    }
    case NDH_REQUEST: {
        s->request++;
        uint32_t ip;
        if (op.has_sid) {
            if (op.sid != c->server_ip) {                   /* the client chose another server */
                int i = own_entry(s, c, mac);
                if (i >= 0 && s->l[i].state == NDH_OFFERED) s->l[i].state = NDH_FREE;
                s->other_server++;
                note(s, o, NDH_EV_OTHER_SERVER, mac, 0, 0, 0, now);
                return 0;
            }
            ip = op.has_req ? op.req : ciaddr;
        } else {
            ip = ciaddr ? ciaddr : op.has_req ? op.req : 0;
            if (!ip) { s->bad++; o->ev = NDH_EV_BAD; return 0; }
        }
        int i = pool_index(c, ip);
        int ok = i >= 0 && available_to(&s->l[i], mac);   /* never an address offered / leased to another live MAC */
        if (!ok) {
            s->nak++;
            note(s, o, NDH_EV_NAK, mac, ip, 0, BCAST, now);
            return build(c, pkt, &op, NDH_NAK, 0, 0, 0, dns, out, cap);
        }
        drop_others(s, c, mac, i);
        ndh_lease *l = &s->l[i];
        set_state(l, NDH_BOUND, mac, now, lease * 1000u);
        l->acks++;
        s->ack++;
        uint32_t dst = ciaddr ? ciaddr : BCAST;
        note(s, o, NDH_EV_ACK, mac, ip, lease, dst, now);
        return build(c, pkt, &op, NDH_ACK, ciaddr, ip, lease, dns, out, cap);
    }
    case NDH_DECLINE: {
        s->decline++;
        int i = op.has_req ? pool_index(c, op.req) : -1;
        if (i >= 0 && same_mac(&s->l[i], mac) && (s->l[i].state == NDH_OFFERED || s->l[i].state == NDH_BOUND)) {
            set_state(&s->l[i], NDH_DECLINED, NULL, now, NDH_DECLINE_HOLD_MS);
            s->l[i].has_mac = 0;
        }
        note(s, o, NDH_EV_DECLINE, mac, op.has_req ? op.req : 0, 0, 0, now);
        return 0;
    }
    case NDH_RELEASE: {
        s->release++;
        int i = pool_index(c, ciaddr);
        if (i >= 0 && same_mac(&s->l[i], mac) && s->l[i].state == NDH_BOUND) s->l[i].state = NDH_FREE;   /* MAC kept: affinity */
        note(s, o, NDH_EV_RELEASE, mac, ciaddr, 0, 0, now);
        return 0;
    }
    case NDH_INFORM:
        s->inform++;
        if (!ciaddr || (ciaddr & c->netmask) != snet) { s->ignored++; o->ev = NDH_EV_BAD; return 0; }
        note(s, o, NDH_EV_INFORM, mac, 0, 0, ciaddr, now);
        return build(c, pkt, &op, NDH_ACK, ciaddr, 0, 0, dns, out, cap);
    default:                                                /* OFFER / ACK / NAK sent to the server port */
        s->ignored++;
        o->ev = NDH_EV_BAD;
        return 0;
    }
}
