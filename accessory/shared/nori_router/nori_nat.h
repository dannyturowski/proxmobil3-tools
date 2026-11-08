/* wt32-r2 router policy on top of esp-lwip's IPv4 NAPT (lwip/src/core/ipv4/ip4_napt.c, ESP-IDF 4.4.7).
 *
 * Topology: Wi-Fi station = upstream (DHCP, default route); Ethernet = downstream link to the PM3
 * (static 192.168.77.1/24, no gateway).  What this module maintains:
 *   - NAPT on the Ethernet netif (netif->napt).  esp-lwip translates in ip4_forward() when the
 *     INPUT netif has napt set: PM3 -> anywhere leaves with the Wi-Fi address as source, and
 *     ip4_input() on the Wi-Fi netif maps replies back (ip_napt_recv).
 *   - netif_default = the Wi-Fi netif whenever it is usable (ESP-IDF picks it too: route_prio 100 vs
 *     Ethernet 50; this is the belt to that).
 *   - one TCP port map (DNAT)  <Wi-Fi address>:mport -> daddr:dport, re-bound when the Wi-Fi address
 *     changes and removed while Wi-Fi has no address.
 *   - an inbound guard on the Wi-Fi netif: IPv4 packets addressed to the downstream subnet are dropped
 *     before lwIP sees them.  Without it a Wi-Fi LAN host with a manual route "192.168.77.0/24 via
 *     <WT32>" would reach every PM3 port untranslated (ip_napt_recv only looks at packets for the
 *     Wi-Fi address, and ip4_forward skips ip_napt_forward when the OUTPUT netif has napt set).  With
 *     it, the only ways in from Wi-Fi are NAPT replies and the one DNAT port.  The guard wraps
 *     netif->input (tcpip_input), i.e. it runs in the Wi-Fi driver's receive context and only reads
 *     the frame; it is (re)installed whenever esp_netif re-adds the station netif.
 *   - 32-bit forwarding counters (lwIP keeps 16-bit ones) and a snapshot for diagnostics.
 *
 * Threading: every function except nn_init touches lwIP state and MUST run in the lwIP core context:
 * the tcpip thread (the firmware calls nn_reconcile through tcpip_api_call) or a NO_SYS host test.
 *
 * esp-lwip 4.4.7 quirks handled here (see tests/napt_host):
 *   - ip_portmap_add() re-registering an existing port appends a second entry instead of updating
 *     (its loop does not return after the update), so a mapping is always removed before it is added.
 *   - ip_napt_enable_no(n, 0) calls ip_napt_deinit(), which frees the whole NAPT table AND the port
 *     maps; this module therefore never disables NAPT, it only (re-)enables it on Ethernet.
 *   - the port map table only exists after NAPT was enabled once (ip_napt_init).
 */
#ifndef NORI_NAT_H
#define NORI_NAT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct netif;

enum { NN_DEF_NONE, NN_DEF_WIFI, NN_DEF_ETH, NN_DEF_OTHER };

typedef struct {
    uint32_t dnat_daddr;        /* downstream host (PM3), network byte order */
    uint16_t dnat_mport;        /* TCP port on the Wi-Fi address, host order */
    uint16_t dnat_dport;        /* TCP port on dnat_daddr, host order */
    uint32_t down_net, down_mask;   /* downstream subnet (network order); Wi-Fi packets to it are dropped.
                                     * down_mask 0: no guard */
} nn_cfg;

typedef struct {
    /* state read back from lwIP on the last nn_reconcile() */
    uint8_t wifi_netif, eth_netif;  /* the netif exists */
    uint8_t wifi_up, eth_up;        /* netif up + link up + IPv4 address */
    uint8_t eth_link;               /* Ethernet link (cable + PHY), regardless of address */
    uint8_t napt;                   /* napt flag on the Ethernet netif */
    uint8_t wifi_napt;              /* napt flag on the Wi-Fi netif (must stay 0) */
    uint8_t def;                    /* NN_DEF_*: which netif is lwIP's default */
    uint8_t pm_ok;                  /* the port map is in lwIP's table exactly as wanted */
    uint8_t guard;                  /* the Wi-Fi inbound guard is installed */
    uint32_t wifi_ip, wifi_mask, wifi_gw;   /* network byte order, 0 = none */
    uint32_t eth_ip, eth_mask;
    uint32_t pm_maddr;              /* Wi-Fi address the port map is bound to, 0 = none */
    /* event counters since boot */
    uint32_t reconciles, napt_enables, def_fixes, pm_binds, pm_fails, guard_installs;
    uint32_t guard_drops;           /* Wi-Fi packets to the downstream subnet dropped by the guard */
    /* lwIP counters, widened to 32 bits (valid when built with LWIP_STATS) */
    uint32_t fw, rterr, drop;       /* ip.fw forwarded, ip.rterr no route, ip.drop dropped */
    uint16_t nat_tcp, nat_udp, nat_icmp, nat_evict;   /* NAPT table entries in use / forced evictions */
    uint16_t last_fw, last_rterr, last_drop;          /* raw 16-bit values seen last time */
    uint8_t stats;                  /* 1: lwIP statistics were compiled in */
} nn_state;

void nn_init(nn_state *s);

/* Bring lwIP to the wanted state (idempotent; cheap when nothing changed) and refresh *s.
 * wifi / eth may be NULL (netif not created yet).  lwIP core context only. */
void nn_reconcile(struct netif *wifi, struct netif *eth, const nn_cfg *c, nn_state *s);

/* ---- pure helpers (no lwIP calls; any context, on a copy of the state) ---- */

/* "a.b.c.d" into buf (>= 16 bytes); addr in network byte order. */
const char *nn_ip_str(uint32_t addr, char *buf);
/* Prefix length of a contiguous netmask (network byte order), -1 if not contiguous. */
int nn_prefix_len(uint32_t mask);
/* "none" / "wifi" / "eth" / "other" */
const char *nn_def_name(uint8_t def);
/* One-word routing summary: "ok" when PM3 traffic can flow both ways, else the first missing piece:
 * init (never reconciled), nonapt, nowifi, noguard, nolink (Ethernet cable/PHY), noethip, defroute, nodnat. */
const char *nn_route_state(const nn_state *s);
/* Space-separated diagnostic tokens, snprintf semantics (returns what it wanted to write, never
 * writes past cap, always terminates when cap > 0).
 *   full 0 (DIAG):    rt=ok fwd=1234 nat=3
 *   full 1 (NETDIAG): rt=ok napt=1 def=wifi dnat=192.168.0.123:2222>192.168.77.2:2222 grd=1/0 fwd=1234
 *                     rterr=0 drop=0 nat=t2/u1/i0/e0 pmb=1 pmf=0 dfx=1   (dnat=-:2222>... while unbound;
 *                     grd=<installed>/<dropped>) */
int nn_format(const nn_state *s, const nn_cfg *c, int full, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
