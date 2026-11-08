/* wt32-r4 private Ethernet LAN: the DHCP server on lwIP and its guards; see nori_lan.h.
 * lwIP core context only, except the sniffer (sn_input), which runs in the Ethernet receive path. */
#include "nori_lan.h"

#include <string.h>

#include "lwip/opt.h"
#include "lwip/def.h"
#include "lwip/ip.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"
#include "lwip/udp.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

const char *const NLA_SD_NAME[NLA_SD_COUNT] = { "none", "foreign-dhcp", "upstream-gw", "many-hosts" };

/* ---- live state (lwIP core context) ---- */
static struct {
    nla_state s;                     /* the public view; .dhcp is the live server state */
    nla_cfg cfg;
    struct udp_pcb *pcb;
    u8_t pcb_idx;                    /* netif index the pcb is bound to */
    struct netif *eth;               /* NULL while Ethernet does not exist */
    uint32_t ev_seen;                /* last eth_link_events value */
    uint32_t up_at;                  /* when the current link-up was observed */
    uint8_t listen_active;           /* in the post-link-up quiet window (latched: wrap-safe) */
    uint8_t evid_active;             /* gateway / many-hosts evidence still counts (latched) */
    uint8_t lag_active;              /* a link-event counter change now is the lagging echo of this up */
    int inited;
} L;

/* ---- Ethernet sniffer: runs in the driver's receive path; read-only on the frame ----
 * Single writer per variable: the sniffer owns the sn_* evidence and counters; nla_reconcile owns
 * sn_own_*, sn_up_gw and sn_reset_req.  A link cycle asks for an evidence reset through sn_reset_req;
 * evidence is only trusted once the sniffer has acknowledged it (sn_reset_ack). */
static netif_input_fn sn_next;
static volatile uint32_t sn_own_ip, sn_own_mask, sn_up_gw;
static volatile uint32_t sn_reset_req, sn_reset_ack;
static volatile uint32_t sn_frames, sn_foreign, sn_gw, sn_lan_srcs, sn_last;
static volatile uint8_t sn_ev_foreign, sn_ev_gw, sn_ev_many;
static uint32_t sn_srcs[NLA_LAN_SOURCES];
static unsigned sn_nsrc;

static void sn_source(uint32_t src)                     /* network order */
{
    uint32_t h = lwip_ntohl(src);
    if (!src || src == 0xffffffffu || (h & 0xffff0000u) == 0xa9fe0000u) return;   /* any, broadcast, link-local */
    if (sn_own_mask && (src & sn_own_mask) == (sn_own_ip & sn_own_mask)) return;    /* the private subnet */
    if (sn_up_gw && src == sn_up_gw) { sn_ev_gw = 1; sn_gw++; sn_last = src; }
    for (unsigned i = 0; i < sn_nsrc; i++) if (sn_srcs[i] == src) return;
    if (sn_nsrc < NLA_LAN_SOURCES) {
        sn_srcs[sn_nsrc++] = src;
        sn_lan_srcs++;
        sn_last = src;
        if (sn_nsrc >= NLA_LAN_SOURCES) sn_ev_many = 1;
    }
}

/* One received frame (first pbuf only: every header we look at is in it).  ethernet: the frame starts
 * with a 14-byte Ethernet header (the firmware); 0: it starts with the IPv4 header (the host test netifs). */
void nla_sniff(const uint8_t *b, size_t n, int ethernet)
{
    if (sn_reset_ack != sn_reset_req) {                 /* a new link: forget the old evidence */
        sn_nsrc = 0;
        sn_ev_foreign = sn_ev_gw = sn_ev_many = 0;
        sn_reset_ack = sn_reset_req;
    }
    sn_frames++;
    size_t off = ethernet ? 14 : 0;
    int ip = !ethernet || (n >= 14 && b[12] == 0x08 && b[13] == 0x00);
    if (ethernet && n >= 42 && b[12] == 0x08 && b[13] == 0x06 && b[14] == 0 && b[15] == 1 && b[16] == 0x08 && b[17] == 0) {
        uint32_t spa;                                   /* ARP over Ethernet/IPv4: sender protocol address */
        memcpy(&spa, b + 28, 4);
        sn_source(spa);
    } else if (ip && n >= off + 20 && (b[off] >> 4) == 4) {
        uint32_t src;
        size_t ihl = (size_t)(b[off] & 15) * 4;
        int first_frag = (b[off + 6] & 0x1f) == 0 && b[off + 7] == 0;
        memcpy(&src, b + off + 12, 4);
        sn_source(src);
        if (b[off + 9] == 17 && ihl >= 20 && first_frag && n >= off + ihl + 4) {
            const uint8_t *u = b + off + ihl;
            if (((u[0] << 8) | u[1]) == DHCP_SERVER_PORT && ((u[2] << 8) | u[3]) == DHCP_CLIENT_PORT && src != sn_own_ip) {
                sn_ev_foreign = 1;                      /* a DHCP reply that is not ours: another server */
                sn_foreign++;
                sn_last = src;
            }
        }
    }
}

static err_t sn_input(struct pbuf *p, struct netif *inp)
{
    nla_sniff((const uint8_t *)p->payload, p->len, (inp->flags & (NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET)) != 0);
    return sn_next(p, inp);
}

static void sn_install(struct netif *eth)
{
    if (eth->input == sn_input || !eth->input) return;
    sn_next = eth->input;
    __sync_synchronize();                               /* sn_next before the pointer that leads to it */
    eth->input = sn_input;
}

/* ---- helpers ---- */
static struct netif *listed(struct netif *n)
{
    struct netif *i;
    if (!n) return NULL;
    NETIF_FOREACH(i) if (i == n) return n;
    return NULL;
}
static int usable(const struct netif *n)
{
    return n && netif_is_up(n) && netif_is_link_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n));
}
static int quiet(void)
{
    /* listen_active is cleared by nla_reconcile once the window is over (it runs every second), so the elapsed
     * time below is only ever looked at within a few seconds of up_at: no wrap problem after weeks of link
     * (review fix: a plain signed "now - until < 0" turned true again after 24.8 days and silenced the server) */
    if (!L.cfg.enabled || !L.s.link || L.s.standdown || !L.eth) return 1;
    /* live netif state, not only the last reconcile's view (parent review): the link may have dropped since */
    if (!netif_is_up(L.eth) || !netif_is_link_up(L.eth)) return 1;
    if (ip4_addr_get_u32(netif_ip4_addr(L.eth)) != L.cfg.dhcp.server_ip) return 1;   /* our address is not (yet) on Ethernet */
    return L.listen_active && (uint32_t)(sys_now() - L.up_at) < NLA_LISTEN_MS;
}

/* ---- the port-67 pcb ---- */
static uint8_t rxbuf[NDH_REQ_MAX];
static uint8_t txbuf[NDH_REPLY_MAX];

static void recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)arg;
    (void)addr;
    struct netif *inp = ip_current_input_netif();
    if (!L.eth || inp != L.eth) { L.s.rx_wrong_if++; pbuf_free(p); return; }        /* belt to udp_bind_netif's braces */
    if (port != DHCP_CLIENT_PORT) { L.s.rx_wrong_port++; pbuf_free(p); return; }
    if (p->tot_len > sizeof rxbuf) { L.s.rx_too_big++; pbuf_free(p); return; }
    if (quiet()) { L.s.rx_quiet++; pbuf_free(p); return; }
    u16_t n = pbuf_copy_partial(p, rxbuf, p->tot_len, 0);
    pbuf_free(p);
    ndh_out o;
    size_t len = ndh_handle(&L.s.dhcp, &L.cfg.dhcp, rxbuf, n, sys_now(), L.s.dns, txbuf, sizeof txbuf, &o);
    if (o.ev != NDH_EV_NONE && o.ev != NDH_EV_BAD && L.cfg.on_dhcp) L.cfg.on_dhcp(&o);
    if (!len) return;
    struct pbuf *q = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
    if (!q) { L.s.tx_err++; return; }
    memcpy(q->payload, txbuf, len);
    ip_addr_t dst;
    ip_addr_set_ip4_u32_val(dst, o.dst_ip);
    if (udp_sendto_if(pcb, q, &dst, DHCP_CLIENT_PORT, L.eth) == ERR_OK) L.s.tx_ok++;   /* the Ethernet netif, nothing else */
    else L.s.tx_err++;
    pbuf_free(q);
}

static void pcb_drop(void)
{
    if (!L.pcb) return;
    udp_remove(L.pcb);
    L.pcb = NULL;
}
static void pcb_ensure(struct netif *eth)
{
    if (L.pcb && L.pcb_idx == netif_get_index(eth)) return;
    pcb_drop();
    struct udp_pcb *pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb && udp_bind(pcb, IP_ADDR_ANY, DHCP_SERVER_PORT) == ERR_OK) {
        udp_bind_netif(pcb, eth);
        ip_set_option(pcb, SOF_BROADCAST);
        udp_recv(pcb, recv_cb, NULL);
        L.pcb = pcb;
        L.pcb_idx = netif_get_index(eth);
        return;
    }
    if (pcb) udp_remove(pcb);
    L.s.pcb_fails++;
    if (L.cfg.on_lan) L.cfg.on_lan(NLA_EV_PCB_FAIL, 0);
}

/* ---- public ---- */
void nla_init(nla_state *s) { memset(s, 0, sizeof *s); }

const char *nla_state_word(const nla_state *s)
{
    if (!s->pcb_ok) return s->link_ups || s->pcb_fails ? "nopcb" : "off";
    if (!s->link) return "nolink";
    if (s->standdown) return "standdown";
    if (s->listening) return "listen";
    return ndh_primary(&s->dhcp) >= 0 ? "lease" : ndh_bound(&s->dhcp) ? "lease2" : "idle";
}

void nla_reconcile(struct netif *eth, struct netif *wifi, const nla_cfg *c, nla_state *s,
                   const uint32_t dns[2], uint32_t eth_link_events)
{
    uint32_t now = sys_now();
    if (!L.inited) { memset(&L, 0, sizeof L); L.inited = 1; ndh_init(&L.s.dhcp); }
    L.cfg = *c;
    if (!ndh_cfg_valid(&L.cfg.dhcp)) L.cfg.enabled = 0;
    eth = listed(eth);
    wifi = listed(wifi);
    L.eth = eth;
    L.s.dns[0] = dns ? dns[0] : 0;
    L.s.dns[1] = dns ? dns[1] : 0;

    /* sniffer inputs, then the sniffer itself */
    sn_own_ip = c->dhcp.server_ip;
    sn_own_mask = c->dhcp.netmask;
    sn_up_gw = usable(wifi) ? ip4_addr_get_u32(netif_ip4_gw(wifi)) : 0;
    L.s.up_known = sn_up_gw != 0;
    if (eth) sn_install(eth);
    L.s.sniff = (uint8_t)(eth && eth->input == sn_input);

    /* link transitions; a down/up between two calls shows up as a new link event.  The event counter is
     * bumped by the firmware's event task, which runs AFTER esp_netif has already set the netif link up, so
     * the first counter change right after an observed up is that up's own echo, not a new cycle (review fix:
     * it used to mark leases stale and undo a correct stand-down). */
    int link = eth && netif_is_link_up(eth);
    int cycled = eth_link_events != L.ev_seen;
    L.ev_seen = eth_link_events;
    if (L.lag_active && (uint32_t)(now - L.up_at) >= NLA_EVENT_LAG_MS) L.lag_active = 0;
    if (cycled && L.lag_active && link) { cycled = 0; L.lag_active = 0; }
    int was = L.s.link;
    if (was && (!link || cycled)) {
        L.s.link_downs++;
        ndh_link_cycle(&L.s.dhcp);
        L.s.standdown = NLA_SD_NONE;
        sn_reset_req++;
        was = 0;
        if (c->on_lan) c->on_lan(NLA_EV_LINK_DOWN, 0);
    }
    if (!was && link) {
        L.s.link_ups++;
        L.up_at = now;
        L.listen_active = L.evid_active = 1;
        L.lag_active = !cycled;              /* its own counter change may still be on the way */
        if (c->on_lan) c->on_lan(NLA_EV_LINK_UP, 0);
    }
    L.s.link = (uint8_t)link;
    /* latched windows: cleared here (every second) so their elapsed time never grows towards a wrap */
    if (L.listen_active && (uint32_t)(now - L.up_at) >= NLA_LISTEN_MS) L.listen_active = 0;
    if (L.evid_active && (uint32_t)(now - L.up_at) >= NLA_EVIDENCE_MS) L.evid_active = 0;

    /* evidence that the segment is a real LAN -> stand down until the link goes down.  A foreign DHCP server
     * counts at any time; the upstream gateway and "many hosts" only within NLA_EVIDENCE_MS of link-up (a
     * home LAN shows them within seconds; over weeks a PM3 could leak a few extra source addresses -- review
     * fix: that used to stand the server down for good and cost the PM3 its lease). */
    if (link && !L.s.standdown && sn_reset_ack == sn_reset_req) {
        int why = sn_ev_foreign ? NLA_SD_FOREIGN_DHCP
                : !L.evid_active ? NLA_SD_NONE
                : sn_ev_gw ? NLA_SD_UPSTREAM_GW : sn_ev_many ? NLA_SD_MANY_HOSTS : NLA_SD_NONE;
        if (why) {
            L.s.standdown = (uint8_t)why;
            L.s.standdowns++;
            if (c->on_lan) c->on_lan(NLA_EV_STANDDOWN, why);
        }
    }
    L.s.listening = (uint8_t)(link && L.listen_active);
    L.s.evidence = (uint8_t)(link && L.evid_active);

    /* the pcb exists exactly while the service is wanted and Ethernet exists */
    if (L.cfg.enabled && eth) pcb_ensure(eth);
    else pcb_drop();
    L.s.pcb_ok = L.pcb != NULL;

    ndh_tick(&L.s.dhcp, now);
    L.s.sn_frames = sn_frames;
    L.s.sn_foreign_dhcp = sn_foreign;
    L.s.sn_gw_seen = sn_gw;
    L.s.sn_lan_srcs = sn_lan_srcs;
    L.s.sn_last_foreign = sn_last;
    *s = L.s;
}
