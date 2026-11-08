/* wt32-r4: the private Ethernet LAN to the PM3 -- the DHCP server (nori_dhcps.c) bound to lwIP, plus the
 * guard that keeps it off any other network.
 *
 * Binding (Ethernet only, never upstream):
 *   - one UDP pcb on port 67, bound to the Ethernet netif (udp_bind_netif): lwIP delivers it nothing that
 *     arrived on Wi-Fi, so a DHCP DISCOVER on the Wi-Fi LAN is never answered;
 *   - the receive callback re-checks the input netif (ip_current_input_netif() == Ethernet) and the
 *     client port (68), and drops anything else;
 *   - replies leave with udp_sendto_if() on the Ethernet netif only (broadcast or unicast to ciaddr);
 *   - lwIP never forwards broadcasts, so the PM3's DISCOVERs never reach Wi-Fi either.
 *
 * Stand-down (the Ethernet cable plugged into a real LAN instead of the PM3 -- it happened on the bench):
 *   a sniffer wraps the Ethernet netif's input (read-only, like nori_nat's Wi-Fi guard) and looks for
 *   evidence that the segment is not the private point-to-point link:
 *     - a DHCP reply (UDP 67 -> 68) from any address other than ours: another DHCP server is there;
 *     - an IPv4 / ARP sender equal to the Wi-Fi upstream gateway (within NLA_EVIDENCE_MS of link-up);
 *     - NLA_LAN_SOURCES or more distinct IPv4 / ARP senders outside the private subnet (0.0.0.0 and
 *       169.254/16 excluded; the PM3 alone has one, maybe a stale second one from its last LAN lease),
 *       also only within NLA_EVIDENCE_MS of link-up: a home LAN shows both within seconds, while a PM3
 *       slowly leaking extra source addresses over weeks must not cost it its lease.
 *   Any of these stops the server (no OFFER/ACK/NAK at all) until the Ethernet link goes down.  After
 *   every link-up the server also stays silent for NLA_LISTEN_MS, so evidence can arrive first.
 *
 * Threading: nla_reconcile() and the receive callback run in the lwIP core context (tcpip thread, or a
 * NO_SYS host test).  The sniffer runs where netif->input is called (the Ethernet driver's receive task)
 * and only reads the frame, writes its own counters, and reads three words set by nla_reconcile(). */
#ifndef NORI_LAN_H
#define NORI_LAN_H

#include <stdint.h>

#include "nori_dhcps.h"

#ifdef __cplusplus
extern "C" {
#endif

struct netif;

#define NLA_LISTEN_MS 2000u
#define NLA_EVIDENCE_MS 120000u   /* upstream-gateway / many-hosts evidence counts this long after link-up */
#define NLA_EVENT_LAG_MS 1500u    /* a link-event counter change this soon after an observed up is its echo */
#define NLA_LAN_SOURCES 3

/* stand-down reasons */
enum { NLA_SD_NONE = 0, NLA_SD_FOREIGN_DHCP, NLA_SD_UPSTREAM_GW, NLA_SD_MANY_HOSTS, NLA_SD_COUNT };
extern const char *const NLA_SD_NAME[NLA_SD_COUNT];

/* nla_cfg.on_lan what= */
enum { NLA_EV_LINK_UP = 1, NLA_EV_LINK_DOWN, NLA_EV_STANDDOWN, NLA_EV_PCB_FAIL };

typedef struct {
    ndh_cfg dhcp;
    int enabled;                                        /* 0: no pcb, no answers (diagnostics still run) */
    /* hooks, called in the lwIP core context; may be NULL */
    void (*on_dhcp)(const ndh_out *o);
    void (*on_lan)(int what, int arg);
} nla_cfg;

typedef struct {
    ndh_state dhcp;                                     /* copy of the server state */
    uint8_t pcb_ok;                                     /* the port-67 pcb exists and is bound to Ethernet */
    uint8_t sniff;                                      /* the Ethernet sniffer is installed */
    uint8_t link;                                       /* Ethernet link up (lwIP's view) */
    uint8_t listening;                                  /* in the post-link-up quiet window */
    uint8_t evidence;                                   /* gateway / many-hosts evidence still counts (first NLA_EVIDENCE_MS) */
    uint8_t standdown;                                  /* NLA_SD_* (0 = serving) */
    uint8_t up_known;                                   /* the upstream (Wi-Fi) subnet is known to the sniffer */
    uint32_t link_ups, link_downs;                      /* link transitions seen by nla_reconcile */
    uint32_t standdowns;                                /* times the server stood down */
    uint32_t rx_wrong_if;                               /* reached the callback from another netif (must stay 0) */
    uint32_t rx_wrong_port;                             /* source port not 68 */
    uint32_t rx_too_big;                                /* larger than NDH_REQ_MAX */
    uint32_t rx_quiet;                                  /* dropped: listen window, stand-down or disabled */
    uint32_t tx_ok, tx_err;
    uint32_t pcb_fails;
    /* sniffer (since boot; the per-link evidence is reset on every link cycle) */
    uint32_t sn_frames, sn_foreign_dhcp, sn_gw_seen, sn_lan_srcs;
    uint32_t sn_last_foreign;                           /* network order: last foreign DHCP server / LAN source */
    uint32_t dns[2];                                    /* resolvers handed out (network order) */
} nla_state;

void nla_init(nla_state *s);

/* Bring the LAN service to the wanted state and refresh *s.  eth / wifi may be NULL.  dns: the upstream
 * resolvers (network order, 0 = none).  eth_link_events: a counter of link-up events seen elsewhere (the
 * firmware's ETH event handler), so a down/up between two calls is still noticed.  Time is lwIP's sys_now().
 * lwIP core context. */
void nla_reconcile(struct netif *eth, struct netif *wifi, const nla_cfg *c, nla_state *s,
                   const uint32_t dns[2], uint32_t eth_link_events);

/* The sniffer's frame parser (called by the installed input wrapper; exported for the host tests).
 * ethernet: b starts with an Ethernet header; 0: with the IPv4 header. */
void nla_sniff(const uint8_t *b, size_t n, int ethernet);

/* One-word state for diagnostics: off nopcb nolink listen standdown idle lease ("lease" once .2 is bound). */
const char *nla_state_word(const nla_state *s);

#ifdef __cplusplus
}
#endif
#endif
