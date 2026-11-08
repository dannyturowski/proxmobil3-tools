/* wt32-r2 router policy on esp-lwip NAPT; see nori_nat.h.  lwIP core context only (except nn_init/nn_ip_str). */
#include "nori_nat.h"

#include <stdio.h>
#include <string.h>

#include "lwip/opt.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/prot/ip.h"
#include "lwip/stats.h"
#include "lwip/lwip_napt.h"

/* The whole point of wt32-r2: refuse to build a "router" whose lwIP cannot forward or translate. */
#if !IP_FORWARD
#error "wt32-r2 needs lwIP built with IP_FORWARD (CONFIG_LWIP_IP_FORWARD=y)"
#endif
#if !IP_NAPT || !ESP_LWIP
#error "wt32-r2 needs esp-lwip built with IP_NAPT (CONFIG_LWIP_IPV4_NAPT=y)"
#endif

void nn_init(nn_state *s)
{
    memset(s, 0, sizeof *s);
}

const char *nn_ip_str(uint32_t addr, char *buf)
{
    const uint8_t *b = (const uint8_t *)&addr;   /* network byte order: first octet first */
    snprintf(buf, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

int nn_prefix_len(uint32_t mask)
{
    uint32_t h = lwip_ntohl(mask);
    int n = 0;
    while (n < 32 && (h & (0x80000000u >> n))) n++;
    return (n == 32 || (h << n) == 0) ? n : -1;
}

const char *nn_def_name(uint8_t def)
{
    static const char *const name[] = { "none", "wifi", "eth", "other" };
    return def < 4 ? name[def] : "?";
}

const char *nn_route_state(const nn_state *s)
{
    if (!s->reconciles) return "init";
    if (!s->napt) return "nonapt";
    if (!s->wifi_up) return "nowifi";
    if (!s->guard) return "noguard";
    if (!s->eth_link) return "nolink";
    if (!s->eth_up) return "noethip";
    if (s->def != NN_DEF_WIFI) return "defroute";
    if (!s->pm_ok) return "nodnat";
    return "ok";
}

int nn_format(const nn_state *s, const nn_cfg *c, int full, char *out, size_t cap)
{
    char m[16], d[16];
    if (!full)
        return snprintf(out, cap, "rt=%s fwd=%lu nat=%u", nn_route_state(s), (unsigned long)s->fw,
                        (unsigned)s->nat_tcp + s->nat_udp + s->nat_icmp);
    return snprintf(out, cap, "rt=%s napt=%u def=%s dnat=%s:%u>%s:%u grd=%u/%lu fwd=%lu rterr=%lu drop=%lu nat=t%u/u%u/i%u/e%u pmb=%lu pmf=%lu dfx=%lu",
                    nn_route_state(s), s->napt, nn_def_name(s->def), s->pm_ok ? nn_ip_str(s->pm_maddr, m) : "-",
                    (unsigned)c->dnat_mport, nn_ip_str(c->dnat_daddr, d), (unsigned)c->dnat_dport, s->guard,
                    (unsigned long)s->guard_drops, (unsigned long)s->fw,
                    (unsigned long)s->rterr, (unsigned long)s->drop, (unsigned)s->nat_tcp, (unsigned)s->nat_udp,
                    (unsigned)s->nat_icmp, (unsigned)s->nat_evict, (unsigned long)s->pm_binds, (unsigned long)s->pm_fails,
                    (unsigned long)s->def_fixes);
}

static int usable(const struct netif *n)
{
    return n && netif_is_up(n) && netif_is_link_up(n) && !ip4_addr_isany_val(*netif_ip4_addr(n));
}

/* 1 if lwIP's port map table holds exactly what is wanted for c->dnat_mport: the mapping bound to
 * maddr, or (maddr 0) no mapping at all.  Only valid once NAPT was enabled (the table exists). */
static int portmap_is(const nn_cfg *c, uint32_t maddr)
{
    u32_t m = 0, d = 0;
    u16_t dp = 0;
    int have = ip_portmap_get(IP_PROTO_TCP, c->dnat_mport, &m, &d, &dp);
    if (!maddr) return !have;
    return have && m == maddr && d == c->dnat_daddr && dp == c->dnat_dport;
}

#if LWIP_STATS && IP_STATS
static void widen(uint16_t now, uint16_t *last, uint32_t *total)
{
    *total += (uint16_t)(now - *last);           /* 16-bit lwIP counters wrap; read often enough not to lose laps */
    *last = now;
}
#endif

/* ---- Wi-Fi inbound guard ----
 * Runs where netif->input is called: the Wi-Fi driver's receive task, before the tcpip mailbox.  It only
 * reads the frame and three words that are written before the wrapper is installed. */
static netif_input_fn guard_next;          /* what netif->input was (tcpip_input) */
static uint32_t guard_net, guard_mask;     /* network order */
static volatile uint32_t guard_drops;

static err_t guard_input(struct pbuf *p, struct netif *inp)
{
    const uint8_t *b = (const uint8_t *)p->payload;
    size_t off = (inp->flags & (NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET)) ? 14 : 0;   /* as tcpip_input decides */
    if (p->len >= off + 20 && (!off || (b[12] == 0x08 && b[13] == 0x00)) && (b[off] >> 4) == 4) {
        uint32_t dst;
        memcpy(&dst, b + off + 16, 4);
        if ((dst & guard_mask) == guard_net) {
            guard_drops++;
            pbuf_free(p);
            return ERR_OK;                   /* consumed */
        }
    }
    return guard_next(p, inp);
}

static void guard_install(struct netif *wifi, const nn_cfg *c, nn_state *s)
{
    if (!c->down_mask || wifi->input == guard_input || !wifi->input) return;
    guard_next = wifi->input;
    guard_net = c->down_net & c->down_mask;
    guard_mask = c->down_mask;
    __sync_synchronize();                    /* the words above before the pointer that leads to them */
    wifi->input = guard_input;
    s->guard_installs++;
}

/* A netif that is not (or no longer) in netif_list is treated as absent: esp_netif removes the station
 * netif from lwIP when Wi-Fi stops.  Pointers are only compared here, never dereferenced. */
static struct netif *listed(struct netif *n)
{
    struct netif *i;
    NETIF_FOREACH(i) if (i == n) return n;
    return NULL;
}

void nn_reconcile(struct netif *wifi, struct netif *eth, const nn_cfg *c, nn_state *s)
{
    wifi = listed(wifi);
    eth = listed(eth);
    s->reconciles++;
    s->wifi_netif = wifi != NULL;
    s->eth_netif = eth != NULL;

    /* NAPT on the downstream side.  Enabling also allocates the NAPT and port map tables (once). */
    if (eth && !eth->napt) {
        ip_napt_enable_no(eth->num, 1);
        s->napt_enables++;
    }
    s->napt = eth ? eth->napt : 0;
    s->wifi_napt = wifi ? wifi->napt : 0;
    if (wifi) guard_install(wifi, c, s);
    s->guard = (uint8_t)(wifi && wifi->input == guard_input);
    s->guard_drops = guard_drops;

    s->wifi_up = (uint8_t)usable(wifi);
    s->eth_up = (uint8_t)usable(eth);
    s->eth_link = (uint8_t)(eth && netif_is_link_up(eth));
    s->wifi_ip = s->wifi_up ? ip4_addr_get_u32(netif_ip4_addr(wifi)) : 0;
    s->wifi_mask = s->wifi_up ? ip4_addr_get_u32(netif_ip4_netmask(wifi)) : 0;
    s->wifi_gw = s->wifi_up ? ip4_addr_get_u32(netif_ip4_gw(wifi)) : 0;
    s->eth_ip = eth ? ip4_addr_get_u32(netif_ip4_addr(eth)) : 0;
    s->eth_mask = eth ? ip4_addr_get_u32(netif_ip4_netmask(eth)) : 0;

    /* Upstream default route: always Wi-Fi while it is usable. */
    if (s->wifi_up && netif_default != wifi) {
        netif_set_default(wifi);
        s->def_fixes++;
    }
    s->def = !netif_default ? NN_DEF_NONE : netif_default == wifi ? NN_DEF_WIFI : netif_default == eth ? NN_DEF_ETH : NN_DEF_OTHER;

    /* DNAT follows the Wi-Fi address.  Checked against lwIP's own table every time, so a table that
     * was rebuilt or changed behind our back is repaired too. */
    uint32_t want = (s->wifi_up && s->napt) ? s->wifi_ip : 0;
    if (s->napt && !portmap_is(c, want)) {
        while (ip_portmap_remove(IP_PROTO_TCP, c->dnat_mport)) {}   /* never add on top of an entry: it duplicates */
        if (want) {
            if (ip_portmap_add(IP_PROTO_TCP, want, c->dnat_mport, c->dnat_daddr, c->dnat_dport) && portmap_is(c, want))
                s->pm_binds++;
            else
                s->pm_fails++;
        }
    }
    s->pm_ok = (uint8_t)(want && s->napt && portmap_is(c, want));
    s->pm_maddr = s->pm_ok ? want : 0;

#if LWIP_STATS && IP_STATS
    s->stats = 1;
    widen(lwip_stats.ip.fw, &s->last_fw, &s->fw);
    widen(lwip_stats.ip.rterr, &s->last_rterr, &s->rterr);
    widen(lwip_stats.ip.drop, &s->last_drop, &s->drop);
#endif
#if LWIP_STATS && IP_NAPT_STATS
    s->nat_tcp = lwip_stats.ip_napt.nr_active_tcp;
    s->nat_udp = lwip_stats.ip_napt.nr_active_udp;
    s->nat_icmp = lwip_stats.ip_napt.nr_active_icmp;
    s->nat_evict = lwip_stats.ip_napt.nr_forced_evictions;
#endif
}
