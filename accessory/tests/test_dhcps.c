/* wt32-r4: the DHCP server core (shared/nori_router/nori_dhcps.c) on the host, ASan/UBSan.
 * Covered: the full DISCOVER/OFFER/REQUEST/ACK exchange with every option checked byte for byte;
 * renew (unicast, ciaddr), rebind, INIT-REBOOT with the PM3's old LAN address (NAK) and with .2 after a
 * WT32 reboot (ACK); another server selected; RELEASE / DECLINE / INFORM; pool exhaustion; the second
 * client; the stale take-over after a link cycle (cable swap and back); expiry and the 49.7-day millis
 * wrap; the short lease without DNS; malformed / hostile packets (truncation at every length, bad
 * cookie, bad option lengths, option overload, relays, BOOTREPLY, zero/multicast MAC) and a random
 * fuzz that must never read or write out of bounds; configuration validation. */
#include "nori_dhcps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static uint32_t A(int a, int b, int c, int d)        /* network order */
{
    uint32_t v;
    uint8_t *p = (uint8_t *)&v;
    p[0] = (uint8_t)a; p[1] = (uint8_t)b; p[2] = (uint8_t)c; p[3] = (uint8_t)d;
    return v;
}

static ndh_cfg CFG;
static const uint8_t PM3[6] = { 0x00, 0x1e, 0x06, 0x42, 0x11, 0x22 };
static const uint8_t LAPTOP[6] = { 0x3c, 0x22, 0xfb, 0x01, 0x02, 0x03 };
static uint32_t DNS[2], NODNS[2];

/* ---- request builder ---- */
typedef struct { uint8_t b[600]; size_t n; } msg;
static void m_init(msg *m, uint8_t type, const uint8_t mac[6], uint32_t xid, uint32_t ciaddr)
{
    memset(m, 0, sizeof *m);
    m->b[0] = 1; m->b[1] = 1; m->b[2] = 6;
    memcpy(m->b + 4, &xid, 4);
    memcpy(m->b + 12, &ciaddr, 4);
    memcpy(m->b + 28, mac, 6);
    m->b[236] = 99; m->b[237] = 130; m->b[238] = 83; m->b[239] = 99;
    m->n = 240;
    m->b[m->n++] = 53; m->b[m->n++] = 1; m->b[m->n++] = type;
}
static void m_opt(msg *m, uint8_t code, const void *v, uint8_t len)
{
    m->b[m->n++] = code; m->b[m->n++] = len;
    memcpy(m->b + m->n, v, len);
    m->n += len;
}
static void m_end(msg *m)
{
    static const uint8_t prl[] = { 1, 3, 6, 15, 28, 51, 58, 59 };
    m_opt(m, 55, prl, sizeof prl);
    m_opt(m, 12, "pm3", 3);
    m->b[m->n++] = 255;
    while (m->n < 300) m->b[m->n++] = 0;
}

/* ---- reply parser ---- */
typedef struct {
    int ok;
    uint8_t op, type;
    uint32_t xid, ciaddr, yiaddr, sid, lease, t1, t2, mask, router, bcast, dns[3];
    int ndns, has_lease, has_mask, has_router;
    uint8_t cid[255], cid_len;
    uint8_t mac[6];
    uint16_t flags;
    size_t len;
} rep;
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static rep parse(const uint8_t *b, size_t n)
{
    rep r;
    memset(&r, 0, sizeof r);
    r.len = n;
    if (n < 240 || b[236] != 99 || b[237] != 130 || b[238] != 83 || b[239] != 99) return r;
    r.op = b[0];
    memcpy(&r.xid, b + 4, 4);
    r.flags = (uint16_t)(b[10] << 8 | b[11]);
    memcpy(&r.ciaddr, b + 12, 4);
    memcpy(&r.yiaddr, b + 16, 4);
    memcpy(r.mac, b + 28, 6);
    size_t i = 240;
    int end = 0;
    while (i < n) {
        uint8_t c = b[i++];
        if (c == 0) continue;
        if (c == 255) { end = 1; break; }
        if (i >= n) return r;
        uint8_t l = b[i++];
        if (i + l > n) return r;
        const uint8_t *v = b + i;
        switch (c) {
        case 53: r.type = v[0]; break;
        case 54: memcpy(&r.sid, v, 4); break;
        case 51: r.lease = be32(v); r.has_lease = 1; break;
        case 58: r.t1 = be32(v); break;
        case 59: r.t2 = be32(v); break;
        case 1: memcpy(&r.mask, v, 4); r.has_mask = 1; break;
        case 3: memcpy(&r.router, v, 4); r.has_router = 1; break;
        case 28: memcpy(&r.bcast, v, 4); break;
        case 6: r.ndns = l / 4; for (int k = 0; k < r.ndns && k < 3; k++) memcpy(&r.dns[k], v + 4 * k, 4); break;
        case 61: memcpy(r.cid, v, l); r.cid_len = l; break;
        default: break;
        }
        i += l;
    }
    r.ok = end;
    return r;
}

static ndh_state S;
static uint8_t out[NDH_REPLY_MAX];
static ndh_out O;
static rep R;
static uint32_t T = 5000;

static size_t send_msg(const msg *m, const uint32_t dns[2])
{
    memset(out, 0xAA, sizeof out);
    size_t n = ndh_handle(&S, &CFG, m->b, m->n, T, dns, out, sizeof out, &O);
    R = n ? parse(out, n) : (rep){0};
    return n;
}

static void discover(const uint8_t mac[6], uint32_t xid, const uint32_t *req)
{
    msg m;
    m_init(&m, NDH_DISCOVER, mac, xid, 0);
    if (req) m_opt(&m, 50, req, 4);
    m_end(&m);
    send_msg(&m, DNS);
}
static void request(const uint8_t mac[6], uint32_t xid, uint32_t ciaddr, const uint32_t *req, const uint32_t *sid, const uint32_t dns[2])
{
    msg m;
    m_init(&m, NDH_REQUEST, mac, xid, ciaddr);
    if (req) m_opt(&m, 50, req, 4);
    if (sid) m_opt(&m, 54, sid, 4);
    m_end(&m);
    send_msg(&m, dns);
}

static void t_exchange(void)
{
    ndh_init(&S);
    uint32_t ip2 = A(192, 168, 77, 2), sid = A(192, 168, 77, 1);
    discover(PM3, 0x11111111, NULL);
    check("DISCOVER -> OFFER", R.ok && R.op == 2 && R.type == NDH_OFFER && O.ev == NDH_EV_OFFER);
    check("OFFER yiaddr .2, xid echoed, chaddr echoed", R.yiaddr == ip2 && R.xid == 0x11111111 && !memcmp(R.mac, PM3, 6));
    check("OFFER broadcast destination", O.dst_ip == 0xffffffffu);
    check("OFFER options: server id, mask /24, router .1, broadcast .255",
          R.sid == sid && R.mask == A(255, 255, 255, 0) && R.router == sid && R.bcast == A(192, 168, 77, 255));
    check("OFFER options: lease 3600, T1 1800, T2 3150", R.lease == 3600 && R.t1 == 1800 && R.t2 == 3150);
    check("OFFER options: upstream DNS 192.168.0.1", R.ndns == 1 && R.dns[0] == A(192, 168, 0, 1));
    check("reply length 300..548", R.len >= NDH_REPLY_MIN && R.len <= NDH_REPLY_MAX);
    check("lease table: .2 offered to the PM3", S.l[0].state == NDH_OFFERED && !memcmp(S.l[0].mac, PM3, 6));

    T += 100;
    request(PM3, 0x11111111, 0, &ip2, &sid, DNS);
    check("REQUEST (selecting) -> ACK .2", R.ok && R.type == NDH_ACK && R.yiaddr == ip2 && O.ev == NDH_EV_ACK);
    check("ACK broadcast (client has no address yet)", O.dst_ip == 0xffffffffu);
    check("ACK carries router/mask/lease/DNS", R.has_router && R.has_mask && R.lease == 3600 && R.ndns == 1);
    check("lease bound, 1 ack, primary .2", S.l[0].state == NDH_BOUND && S.l[0].acks == 1 && ndh_primary(&S) == 0 && ndh_bound(&S) == 1);
    check("event: mac + ip + lease reported", !memcmp(O.mac, PM3, 6) && O.yiaddr == ip2 && O.lease_s == 3600);
    check("last client recorded", !memcmp(S.last_mac, PM3, 6) && S.last_ip == ip2 && S.last_ev == NDH_EV_ACK);

    /* renew at T1: unicast from ciaddr, no server id / requested address */
    T += 1800u * 1000u;
    request(PM3, 0x22222222, ip2, NULL, NULL, DNS);
    check("RENEW -> ACK unicast to ciaddr", R.type == NDH_ACK && R.yiaddr == ip2 && R.ciaddr == ip2 && O.dst_ip == ip2);
    check("renew refreshes the lease", ndh_remaining_ms(&S.l[0], T) == 3600u * 1000u);
    /* rebind (T2, broadcast but ciaddr set) */
    T += 3150u * 1000u;
    request(PM3, 0x33333333, ip2, NULL, NULL, DNS);
    check("REBIND -> ACK", R.type == NDH_ACK && R.yiaddr == ip2 && S.l[0].acks == 3);
    /* renew with an address that is not ours */
    uint32_t lan = A(192, 168, 0, 45);
    request(PM3, 0x44444444, lan, NULL, NULL, DNS);
    check("RENEW of a foreign address -> NAK broadcast", R.type == NDH_NAK && O.dst_ip == 0xffffffffu && R.yiaddr == 0);
    check("NAK has server id, no lease/router", R.sid == sid && !R.has_lease && !R.has_router);
    check("the real lease survives a NAK for another address", S.l[0].state == NDH_BOUND);
}

static void t_reboot_paths(void)
{
    uint32_t ip2 = A(192, 168, 77, 2), lan = A(192, 168, 0, 45);
    /* PM3 that was on the home LAN: INIT-REBOOT asks for its old LAN lease */
    ndh_init(&S);
    request(PM3, 1, 0, &lan, NULL, DNS);
    check("INIT-REBOOT with the old LAN address -> NAK (client restarts at once)", R.type == NDH_NAK && O.ev == NDH_EV_NAK);
    check("no lease created by a NAK", ndh_bound(&S) == 0);
    /* WT32 rebooted (table empty), PM3 asks for .2 again */
    request(PM3, 2, 0, &ip2, NULL, DNS);
    check("INIT-REBOOT .2 after a WT32 reboot -> ACK .2", R.type == NDH_ACK && R.yiaddr == ip2 && S.l[0].state == NDH_BOUND);
    /* REQUEST with nothing to go on */
    request(PM3, 3, 0, NULL, NULL, DNS);
    check("REQUEST without ciaddr / option 50 -> ignored", O.ev == NDH_EV_BAD && R.len == 0);
    /* address outside the pool but inside the subnet */
    uint32_t ip50 = A(192, 168, 77, 50);
    request(PM3, 4, 0, &ip50, NULL, DNS);
    check("INIT-REBOOT .50 (outside the pool) -> NAK", R.type == NDH_NAK);
    uint32_t srv = A(192, 168, 77, 1);
    request(PM3, 5, 0, &srv, NULL, DNS);
    check("request for the server's own address -> NAK", R.type == NDH_NAK);
}

static void t_other_server_release_decline_inform(void)
{
    uint32_t ip2 = A(192, 168, 77, 2), sid = A(192, 168, 77, 1), other = A(192, 168, 77, 99);
    ndh_init(&S);
    discover(PM3, 7, NULL);
    request(PM3, 7, 0, &ip2, &other, DNS);
    check("REQUEST naming another server -> silent, offer withdrawn", R.len == 0 && O.ev == NDH_EV_OTHER_SERVER && S.l[0].state == NDH_FREE);
    check("other_server counted", S.other_server == 1);
    discover(PM3, 8, NULL);
    request(PM3, 8, 0, &ip2, &sid, DNS);
    /* RELEASE */
    msg m;
    m_init(&m, NDH_RELEASE, PM3, 9, ip2);
    m_opt(&m, 54, &sid, 4);
    m_end(&m);
    send_msg(&m, DNS);
    check("RELEASE -> no reply, lease free, MAC affinity kept", R.len == 0 && O.ev == NDH_EV_RELEASE && S.l[0].state == NDH_FREE &&
          S.l[0].has_mac);
    discover(PM3, 10, NULL);
    check("after RELEASE the same MAC gets .2 again", R.type == NDH_OFFER && R.yiaddr == ip2);
    request(PM3, 10, 0, &ip2, &sid, DNS);
    /* RELEASE from another MAC must not free the PM3's lease */
    m_init(&m, NDH_RELEASE, LAPTOP, 11, ip2);
    m_end(&m);
    send_msg(&m, DNS);
    check("RELEASE by another MAC does not free the lease", S.l[0].state == NDH_BOUND);
    /* DECLINE */
    m_init(&m, NDH_DECLINE, PM3, 12, 0);
    m_opt(&m, 50, &ip2, 4);
    m_opt(&m, 54, &sid, 4);
    m_end(&m);
    send_msg(&m, DNS);
    check("DECLINE -> no reply, address held", R.len == 0 && S.l[0].state == NDH_DECLINED && !S.l[0].has_mac);
    discover(PM3, 13, NULL);
    check("declined .2 skipped: offer .3", R.type == NDH_OFFER && R.yiaddr == A(192, 168, 77, 3));
    T += NDH_DECLINE_HOLD_MS;
    ndh_tick(&S, T);
    check("decline hold expires", S.l[0].state == NDH_FREE);
    /* INFORM */
    uint32_t ci = A(192, 168, 77, 3);
    m_init(&m, NDH_INFORM, PM3, 14, ci);
    m_end(&m);
    send_msg(&m, DNS);
    check("INFORM -> ACK unicast, no yiaddr, no lease options, has router/DNS",
          R.type == NDH_ACK && R.yiaddr == 0 && R.ciaddr == ci && O.dst_ip == ci && !R.has_lease && R.has_router && R.ndns == 1);
    m_init(&m, NDH_INFORM, PM3, 15, A(10, 0, 0, 5));
    m_end(&m);
    send_msg(&m, DNS);
    check("INFORM from another subnet -> ignored", R.len == 0);
}

static void t_pool_and_takeover(void)
{
    uint32_t ip2 = A(192, 168, 77, 2), sid = A(192, 168, 77, 1);
    ndh_init(&S);
    discover(PM3, 1, NULL);
    request(PM3, 1, 0, &ip2, &sid, DNS);
    /* a second device on the same segment (no link cycle): .3 */
    discover(LAPTOP, 2, NULL);
    check("second client, no link cycle -> .3", R.type == NDH_OFFER && R.yiaddr == A(192, 168, 77, 3));
    uint32_t ip3 = A(192, 168, 77, 3);
    request(LAPTOP, 2, 0, &ip3, &sid, DNS);
    check("second client ACK .3", R.type == NDH_ACK && ndh_bound(&S) == 2);
    /* the laptop asks for .2 while the PM3 holds it live */
    request(LAPTOP, 3, 0, &ip2, NULL, DNS);
    check("INIT-REBOOT for an address held by another live client -> NAK", R.type == NDH_NAK && S.l[0].state == NDH_BOUND &&
          !memcmp(S.l[0].mac, PM3, 6));

    /* cable swap: link goes down, the laptop alone is plugged in */
    ndh_init(&S);
    discover(PM3, 4, NULL);
    request(PM3, 4, 0, &ip2, &sid, DNS);
    ndh_link_cycle(&S);
    check("link cycle marks the lease stale", S.l[0].stale && S.l[0].state == NDH_BOUND);
    discover(LAPTOP, 5, NULL);
    check("cable swap: the new device gets .2 (stale take-over)", R.type == NDH_OFFER && R.yiaddr == ip2);
    request(LAPTOP, 5, 0, &ip2, &sid, DNS);
    check("laptop bound to .2", R.type == NDH_ACK && !memcmp(S.l[0].mac, LAPTOP, 6) && !S.l[0].stale);
    /* and back to the PM3 */
    ndh_link_cycle(&S);
    request(PM3, 6, 0, &ip2, NULL, DNS);
    check("PM3 back: INIT-REBOOT .2 -> ACK (laptop's stale lease taken over)", R.type == NDH_ACK && !memcmp(S.l[0].mac, PM3, 6));
    check("one binding per MAC", ndh_bound(&S) == 1);
    /* the stale lease's own MAC renews -> keeps it */
    ndh_link_cycle(&S);
    request(PM3, 7, ip2, NULL, NULL, DNS);
    check("own stale lease renewed -> ACK, no longer stale", R.type == NDH_ACK && !S.l[0].stale);

    /* exhaustion */
    ndh_init(&S);
    for (int i = 0; i < CFG.pool_n; i++) {
        uint8_t mac[6] = { 0x02, 0, 0, 0, 0, (uint8_t)(i + 1) };
        discover(mac, 100u + (uint32_t)i, NULL);
        uint32_t y = R.yiaddr;
        request(mac, 100u + (uint32_t)i, 0, &y, &sid, DNS);
    }
    check("pool fully bound", ndh_bound(&S) == CFG.pool_n);
    uint8_t extra[6] = { 0x02, 0, 0, 0, 0, 0x77 };
    discover(extra, 200, NULL);
    check("pool full -> no OFFER, counted", R.len == 0 && O.ev == NDH_EV_FULL && S.full == 1);
    /* an unanswered offer frees after 30 s */
    ndh_init(&S);
    discover(PM3, 300, NULL);
    T += NDH_OFFER_HOLD_MS;
    ndh_tick(&S, T);
    check("unanswered OFFER released after the hold", S.l[0].state == NDH_FREE);
    /* DISCOVER asking for a specific free address */
    uint32_t ip4 = A(192, 168, 77, 4);
    discover(LAPTOP, 301, &ip4);
    check("DISCOVER with requested free address -> that address", R.yiaddr == ip4);
}

static void t_expiry_wrap_shortlease(void)
{
    uint32_t ip2 = A(192, 168, 77, 2), sid = A(192, 168, 77, 1);
    ndh_init(&S);
    T = 0xffffffffu - 1000u;                          /* just before the millis() wrap */
    discover(PM3, 1, NULL);
    request(PM3, 1, 0, &ip2, &sid, DNS);
    check("lease across the wrap: remaining 3600 s", ndh_remaining_ms(&S.l[0], T) == 3600000u);
    T += 3599u * 1000u;                               /* wraps */
    ndh_tick(&S, T);
    check("not expired 1 s before the end (across the wrap)", S.l[0].state == NDH_BOUND && ndh_remaining_ms(&S.l[0], T) == 1000u);
    T += 1000u;
    ndh_tick(&S, T);
    check("expired at the end", S.l[0].state == NDH_EXPIRED && S.expired == 1 && ndh_bound(&S) == 0);
    /* 49.7 days later the old lease must not come back to life */
    for (int i = 0; i < 60; i++) { T += 3600u * 1000u * 20u; ndh_tick(&S, T); }
    check("an expired lease stays expired over a full counter period", S.l[0].state == NDH_EXPIRED);
    discover(PM3, 2, NULL);
    check("expired lease: same MAC gets .2 again", R.yiaddr == ip2);
    /* no DNS known: short lease, no option 6 */
    request(PM3, 2, 0, &ip2, &sid, NODNS);
    check("no upstream DNS: short lease 120 s and no DNS option", R.type == NDH_ACK && R.lease == 120 && R.ndns == 0);
    uint32_t two[2] = { A(192, 168, 0, 1), A(1, 1, 1, 1) };
    request(PM3, 3, ip2, NULL, NULL, two);
    check("renew once DNS is known: full lease, both resolvers", R.type == NDH_ACK && R.lease == 3600 && R.ndns == 2 &&
          R.dns[0] == two[0] && R.dns[1] == two[1]);
    T = 5000;
}

static void t_malformed(void)
{
    msg m;
    ndh_init(&S);
    m_init(&m, NDH_DISCOVER, PM3, 1, 0);
    m_end(&m);
    /* every truncation length */
    int bad_reply = 0;
    for (size_t n = 0; n < m.n; n++) {
        uint8_t *cp = malloc(n ? n : 1);                 /* exact-size heap copy: ASan catches any over-read */
        memcpy(cp, m.b, n);
        size_t r = ndh_handle(&S, &CFG, cp, n, T, DNS, out, sizeof out, &O);
        /* a packet cut after the end option (or inside the padding) is still a full DISCOVER */
        if (r && n < 240 + 3) bad_reply = 1;
        free(cp);
        ndh_init(&S);
    }
    check("truncated packets never answered before the type option, never over-read", !bad_reply);
    msg c = m; c.b[236] = 0;
    check("bad magic cookie -> no reply", send_msg(&c, DNS) == 0 && O.ev == NDH_EV_BAD);
    c = m; c.b[1] = 6;
    check("htype not Ethernet -> no reply", send_msg(&c, DNS) == 0);
    c = m; c.b[2] = 16;
    check("hlen not 6 -> no reply", send_msg(&c, DNS) == 0);
    c = m; c.b[0] = 2;
    check("BOOTREPLY sent to the server port -> ignored", send_msg(&c, DNS) == 0 && S.ignored > 0);
    c = m; c.b[24] = 10;
    check("relayed (giaddr set) -> ignored", send_msg(&c, DNS) == 0);
    c = m; memset(c.b + 28, 0, 6);
    check("zero chaddr -> ignored", send_msg(&c, DNS) == 0);
    c = m; c.b[28] = 0x01;
    check("multicast chaddr -> ignored", send_msg(&c, DNS) == 0);
    c = m; c.b[242] = 9;
    check("unknown message type 9 -> no reply", send_msg(&c, DNS) == 0);
    c = m; c.b[241] = 2;
    check("type option with length 2 -> no reply", send_msg(&c, DNS) == 0);
    /* option length running past the end */
    m_init(&c, NDH_DISCOVER, PM3, 1, 0);
    c.b[c.n++] = 12; c.b[c.n++] = 200; c.b[c.n++] = 'x';
    check("option length past the packet -> no reply", send_msg(&c, DNS) == 0 && O.ev == NDH_EV_BAD);
    /* requested address with a bad length */
    m_init(&c, NDH_DISCOVER, PM3, 1, 0);
    uint8_t three[3] = { 1, 2, 3 };
    m_opt(&c, 50, three, 3);
    m_end(&c);
    check("option 50 with length 3 -> no reply", send_msg(&c, DNS) == 0);
    /* option overload */
    m_init(&c, NDH_DISCOVER, PM3, 1, 0);
    uint8_t ov = 3;
    m_opt(&c, 52, &ov, 1);
    m_end(&c);
    check("option overload -> ignored", send_msg(&c, DNS) == 0);
    /* no type option at all */
    m_init(&c, NDH_DISCOVER, PM3, 1, 0);
    c.n = 240;
    c.b[c.n++] = 255;
    check("no message type -> no reply", send_msg(&c, DNS) == 0);
    /* missing end option but options consumed exactly: accepted */
    m_init(&c, NDH_DISCOVER, PM3, 1, 0);
    check("no end option, exact length -> OFFER", send_msg(&c, DNS) > 0 && R.type == NDH_OFFER);
    /* oversize packet */
    uint8_t *big = calloc(1, NDH_REQ_MAX + 1);
    memcpy(big, m.b, m.n);
    check("larger than NDH_REQ_MAX -> no reply", ndh_handle(&S, &CFG, big, NDH_REQ_MAX + 1, T, DNS, out, sizeof out, &O) == 0);
    free(big);
    check("NULL packet -> no reply", ndh_handle(&S, &CFG, NULL, 300, T, DNS, out, sizeof out, &O) == 0);
    /* small output buffer: never written */
    uint8_t small[100];
    memset(small, 0x5A, sizeof small);
    size_t r = ndh_handle(&S, &CFG, m.b, m.n, T, DNS, small, sizeof small, &O);
    int untouched = 1;
    for (size_t i = 0; i < sizeof small; i++) untouched &= small[i] == 0x5A;
    check("output buffer below NDH_REPLY_MAX -> no reply, untouched", r == 0 && untouched);

    /* random fuzz: random bytes behind a valid header, and random mutations of valid requests */
    srand(4242);
    int ok = 1;
    for (int it = 0; it < 200000; it++) {
        size_t n = 240 + (size_t)(rand() % 400);
        uint8_t *p = malloc(n);
        if (it & 1) {
            memcpy(p, m.b, n < m.n ? n : m.n);
            for (size_t i = m.n; i < n; i++) p[i] = (uint8_t)rand();
            for (int k = 0; k < 4 && n > 240; k++) p[240 + (size_t)rand() % (n - 240)] = (uint8_t)rand();
        } else {
            for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rand();
            p[1] = 1; p[2] = 6; p[236] = 99; p[237] = 130; p[238] = 83; p[239] = 99; p[0] = 1; p[24] = p[25] = p[26] = p[27] = 0;
            p[28] &= 0xfe;
        }
        memset(out, 0xEE, sizeof out);
        size_t rr = ndh_handle(&S, &CFG, p, n, T, DNS, out, sizeof out, &O);
        if (rr) {
            rep q = parse(out, rr);
            if (!q.ok || rr > NDH_REPLY_MAX || rr < NDH_REPLY_MIN || q.op != 2 || q.sid != A(192, 168, 77, 1)) ok = 0;
            if (q.yiaddr && (q.yiaddr & A(255, 255, 255, 0)) != A(192, 168, 77, 0)) ok = 0;
        }
        T += 7;
        free(p);
        if ((it % 997) == 0) ndh_init(&S);
    }
    check("200000 fuzzed packets: every reply well-formed, in the private subnet, within 300..548 bytes", ok);
    for (int i = 0; i < CFG.pool_n; i++) if (S.l[i].state > NDH_DECLINED) ok = 0;
    check("lease states stay valid under fuzz", ok);
}

static void t_cfg(void)
{
    check("default config valid", ndh_cfg_valid(&CFG));
    ndh_cfg c = CFG; c.pool_n = 0;
    check("empty pool invalid", !ndh_cfg_valid(&c));
    c = CFG; c.pool_n = NDH_POOL_MAX + 1;
    check("pool larger than NDH_POOL_MAX invalid", !ndh_cfg_valid(&c));
    c = CFG; c.pool_first = A(192, 168, 77, 1);
    check("pool containing the server invalid", !ndh_cfg_valid(&c));
    c = CFG; c.pool_first = A(192, 168, 78, 2);
    check("pool outside the subnet invalid", !ndh_cfg_valid(&c));
    c = CFG; c.pool_first = A(192, 168, 77, 250); c.pool_n = 8;
    check("pool reaching the broadcast address invalid", !ndh_cfg_valid(&c));
    c = CFG; c.netmask = A(255, 0, 255, 0);
    check("non-contiguous mask invalid", !ndh_cfg_valid(&c));
    c = CFG; c.lease_s = 10;
    check("lease below 60 s invalid", !ndh_cfg_valid(&c));
    char b[18];
    check("MAC formatting", !strcmp(ndh_mac_str(PM3, b), "00:1e:06:42:11:22"));
    check("event and state names", !strcmp(NDH_EV_NAME[NDH_EV_ACK], "ack") && !strcmp(NDH_STATE_NAME[NDH_BOUND], "bound"));
}

/* wt32-r4 parent review: RFC 6842 client-identifier echo, and the firmware's single-address pool. */
static void t_cid_and_pool1(void)
{
    uint32_t ip2 = A(192, 168, 77, 2), sid = A(192, 168, 77, 1), lan = A(192, 168, 0, 45);
    /* systemd-networkd style DUID client id (type 255, IAID + DUID) */
    uint8_t cid[19] = { 255, 0x5b, 0x6e, 0x1f, 0x01, 0x00, 0x02, 0x00, 0x00, 0xab, 0x11, 0x9c, 0x3e, 0x51, 0x7a, 0x02, 0x44, 0x10, 0x01 };
    msg m;
    ndh_init(&S);
    m_init(&m, NDH_DISCOVER, PM3, 1, 0);
    m_opt(&m, 61, cid, sizeof cid);
    m_end(&m);
    send_msg(&m, DNS);
    check("RFC 6842: OFFER echoes option 61 unchanged", R.type == NDH_OFFER && R.cid_len == sizeof cid && !memcmp(R.cid, cid, sizeof cid));
    m_init(&m, NDH_REQUEST, PM3, 1, 0);
    m_opt(&m, 50, &ip2, 4);
    m_opt(&m, 54, &sid, 4);
    m_opt(&m, 61, cid, sizeof cid);
    m_end(&m);
    send_msg(&m, DNS);
    check("RFC 6842: ACK echoes option 61 unchanged", R.type == NDH_ACK && R.cid_len == sizeof cid && !memcmp(R.cid, cid, sizeof cid));
    m_init(&m, NDH_REQUEST, PM3, 2, 0);
    m_opt(&m, 50, &lan, 4);
    m_opt(&m, 61, cid, sizeof cid);
    m_end(&m);
    send_msg(&m, DNS);
    check("RFC 6842: NAK echoes option 61 unchanged", R.type == NDH_NAK && R.cid_len == sizeof cid && !memcmp(R.cid, cid, sizeof cid));
    discover(PM3, 3, NULL);
    check("no option 61 sent -> none echoed", R.type == NDH_OFFER && R.cid_len == 0);
    uint8_t big[NDH_CID_MAX + 1];
    memset(big, 0x42, sizeof big);
    big[0] = 0;
    m_init(&m, NDH_DISCOVER, PM3, 4, 0);
    m_opt(&m, 61, big, NDH_CID_MAX);
    m_end(&m);
    send_msg(&m, DNS);
    check("option 61 of NDH_CID_MAX bytes echoed, reply within 548 bytes", R.type == NDH_OFFER && R.cid_len == NDH_CID_MAX &&
          R.len <= NDH_REPLY_MAX);
    m_init(&m, NDH_DISCOVER, PM3, 5, 0);
    m_opt(&m, 61, big, sizeof big);
    m_end(&m);
    check("option 61 longer than NDH_CID_MAX -> ignored (bounded)", send_msg(&m, DNS) == 0);
    m_init(&m, NDH_DISCOVER, PM3, 6, 0);
    m_opt(&m, 61, cid, 1);
    m_end(&m);
    check("option 61 with length 1 -> malformed, no reply", send_msg(&m, DNS) == 0 && O.ev == NDH_EV_BAD);

    /* the firmware's pool: .2 only */
    ndh_cfg save = CFG;
    CFG.pool_n = 1;
    check("single-address pool is a valid config", ndh_cfg_valid(&CFG));
    ndh_init(&S);
    discover(PM3, 10, NULL);
    request(PM3, 10, 0, &ip2, &sid, DNS);
    check("pool of 1: PM3 bound to .2", R.type == NDH_ACK && R.yiaddr == ip2);
    discover(LAPTOP, 11, NULL);
    check("pool of 1: a second live device gets no offer (never an unreachable .3)", R.len == 0 && O.ev == NDH_EV_FULL);
    uint32_t ip3 = A(192, 168, 77, 3);
    request(LAPTOP, 12, 0, &ip3, NULL, DNS);
    check("pool of 1: .3 requested -> NAK (outside the pool)", R.type == NDH_NAK);
    ndh_link_cycle(&S);
    discover(LAPTOP, 13, NULL);
    check("pool of 1: after a link cycle the device on the cable gets .2", R.type == NDH_OFFER && R.yiaddr == ip2);
    request(LAPTOP, 13, 0, &ip2, &sid, DNS);
    ndh_link_cycle(&S);
    request(PM3, 14, 0, &ip2, NULL, DNS);
    check("pool of 1: PM3 back after the swap -> ACK .2", R.type == NDH_ACK && !memcmp(S.l[0].mac, PM3, 6));
    /* DECLINE with one address: held 1 min, then offered again */
    m_init(&m, NDH_DECLINE, PM3, 15, 0);
    m_opt(&m, 50, &ip2, 4);
    m_opt(&m, 54, &sid, 4);
    m_end(&m);
    send_msg(&m, DNS);
    discover(PM3, 16, NULL);
    check("pool of 1: declined .2 -> no offer during the hold", R.len == 0 && O.ev == NDH_EV_FULL);
    T += NDH_DECLINE_HOLD_MS;
    discover(PM3, 17, NULL);
    check("pool of 1: after the 1-min decline hold .2 is offered again", R.type == NDH_OFFER && R.yiaddr == ip2);
    CFG = save;
}

int main(void)
{
    CFG.server_ip = A(192, 168, 77, 1);
    CFG.netmask = A(255, 255, 255, 0);
    CFG.pool_first = A(192, 168, 77, 2);
    CFG.pool_n = 4;
    CFG.lease_s = 3600;
    CFG.short_lease_s = 120;
    DNS[0] = A(192, 168, 0, 1);
    t_cfg();
    t_exchange();
    t_reboot_paths();
    t_other_server_release_decline_inform();
    t_pool_and_takeover();
    t_expiry_wrap_shortlease();
    t_malformed();
    t_cid_and_pool1();
    printf("test_dhcps: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
