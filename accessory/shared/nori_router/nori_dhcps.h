/* wt32-r4: a small DHCPv4 server for the private Ethernet link to the PM3 (RFC 2131 / 2132 subset).
 *
 * This file is the portable core: packet parsing, the lease table and reply building.  It makes no lwIP
 * or ESP-IDF call, so it is host-tested as is (tests/test_dhcps.c).  nori_lan.c binds it to lwIP: one UDP
 * pcb on port 67, bound to the Ethernet netif only (udp_bind_netif), replies sent with udp_sendto_if on
 * that netif only.
 *
 * Network: server 192.168.77.1/24 (the WT32's static Ethernet address), pool 192.168.77.2 .. .2+n-1.
 * The first address (.2) is the DNAT target of <Wi-Fi IP>:2222, so a single client on the point-to-point
 * cable always gets .2 (see "stale" below).  The firmware uses a pool of ONE address (.2): only .2 is
 * reachable through the DNAT, and the cable is point-to-point (the core supports up to NDH_POOL_MAX).
 * Options sent: 53 type, 54 server id, 61 the client identifier echoed unchanged when the client sent one
 * (RFC 6842), 51 lease, 58 T1, 59 T2, 1 netmask, 3 router (= the server), 28 broadcast, 6 DNS (the upstream
 * Wi-Fi resolvers, when known).  While no upstream DNS is known the lease is short (short_lease_s) so the
 * client renews and picks the resolvers up once Wi-Fi has them.  Leases are keyed by chaddr (the MAC).
 *
 * Parsing is bounded: the BOOTP header is fixed, the options walk never reads past the packet, each
 * option we use must have its exact length (61: 2..NDH_CID_MAX), the magic cookie must be present,
 * htype/hlen must be Ethernet, op must be BOOTREQUEST.  Relayed packets (giaddr != 0), overload of
 * sname/file, a client identifier longer than NDH_CID_MAX, and packets from a broadcast/multicast/zero
 * chaddr are ignored.  Replies are at most NDH_REPLY_MAX bytes and are padded to the 300-byte BOOTP minimum.
 *
 * Message handling (RFC 2131 4.3):
 *   DISCOVER  -> OFFER (address held NDH_OFFER_HOLD_MS for this MAC), or nothing if the pool is full.
 *   REQUEST   SELECTING (server id present): ours -> ACK the requested address if it is free or this
 *             MAC's, NAK if another live client holds it; another server's id -> our offer is withdrawn,
 *             no reply.
 *             INIT-REBOOT (no server id, requested address, ciaddr 0) and RENEWING/REBINDING (ciaddr set):
 *             ACK when the address is in the pool and available to this MAC, NAK otherwise (we are the
 *             only server on this link, so we answer authoritatively; a NAK makes a client that still
 *             holds an old LAN lease start over at once).
 *   DECLINE   -> the address is blocked for NDH_DECLINE_HOLD_MS, the binding dropped.
 *   RELEASE   -> the binding is released (the MAC stays remembered as the address's last owner).
 *   INFORM    -> ACK with options, no lease.
 * Reply destination: NAK -> broadcast; ciaddr set -> unicast to ciaddr; otherwise broadcast (the client
 * has no address yet and we do not inject ARP entries).
 *
 * "stale": ndh_link_cycle() marks every lease stale when the Ethernet link went down.  A stale lease is
 * still honoured for its own MAC, but another MAC may take it over.  Swapping the cable to another
 * device therefore gives the new device .2 at once, and the PM3 gets .2 back when it returns.
 *
 * Time is a wrap-safe millisecond counter; ndh_tick() (call it at least every few seconds) expires
 * leases, offers and decline holds, so a millis() wrap after 49.7 days cannot revive an old lease. */
#ifndef NORI_DHCPS_H
#define NORI_DHCPS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NDH_POOL_MAX 8
#define NDH_REQ_MIN 240          /* 236-byte BOOTP header + magic cookie */
#define NDH_REQ_MAX 1500
#define NDH_REPLY_MAX 548        /* 576 - IP - UDP headers: always deliverable */
#define NDH_REPLY_MIN 300        /* BOOTP minimum; some clients drop shorter replies */
#define NDH_OFFER_HOLD_MS 30000u
#define NDH_DECLINE_HOLD_MS 60000u   /* a declined address is held this long (1 min: the pool may be a single address) */
#define NDH_CID_MAX 64               /* longest client identifier (option 61) accepted; echoed unchanged (RFC 6842) */

enum { NDH_DISCOVER = 1, NDH_OFFER, NDH_REQUEST, NDH_DECLINE, NDH_ACK, NDH_NAK, NDH_RELEASE, NDH_INFORM };

/* lease states */
enum { NDH_FREE = 0, NDH_OFFERED, NDH_BOUND, NDH_EXPIRED, NDH_DECLINED };

/* what ndh_handle() did; also the event codes the firmware logs (NLG_DHCP a=) */
enum {
    NDH_EV_NONE = 0,
    NDH_EV_OFFER, NDH_EV_ACK, NDH_EV_NAK, NDH_EV_RELEASE, NDH_EV_DECLINE, NDH_EV_INFORM,
    NDH_EV_FULL,          /* DISCOVER with no address available: no reply */
    NDH_EV_OTHER_SERVER,  /* REQUEST for another server: our offer withdrawn, no reply */
    NDH_EV_BAD,           /* malformed / not for us: no reply */
    NDH_EV_COUNT
};
extern const char *const NDH_EV_NAME[NDH_EV_COUNT];
extern const char *const NDH_STATE_NAME[5];

typedef struct {
    uint32_t server_ip;      /* network byte order */
    uint32_t netmask;        /* network byte order */
    uint32_t pool_first;     /* network byte order */
    uint8_t pool_n;          /* 1..NDH_POOL_MAX; all inside server_ip/netmask, none equal to server_ip */
    uint32_t lease_s;        /* normal lease, seconds (>= 60) */
    uint32_t short_lease_s;  /* lease while no DNS server is known (>= 30) */
} ndh_cfg;

typedef struct {
    uint8_t state;           /* NDH_* state */
    uint8_t stale;           /* the link went down since this lease was granted / renewed */
    uint8_t mac[6];          /* owner (also kept for FREE-after-RELEASE and EXPIRED: affinity) */
    uint8_t has_mac;
    uint32_t t_ms;           /* when the state was entered (ACK / OFFER / DECLINE) */
    uint32_t dur_ms;         /* how long it lasts */
    uint32_t acks;           /* ACKs for this address since boot */
} ndh_lease;

typedef struct {
    ndh_lease l[NDH_POOL_MAX];
    /* counters since boot */
    uint32_t rx, bad, ignored;   /* packets seen, malformed, valid but not for us (relayed, other op, ...) */
    uint32_t discover, request, decline, release, inform;
    uint32_t offer, ack, nak, full, other_server, expired;
    /* last handled client */
    uint8_t last_mac[6];
    uint8_t last_ev;
    uint32_t last_ip, last_ms;
    uint32_t last_lease_s;
} ndh_state;

typedef struct {
    int ev;                  /* NDH_EV_* */
    uint32_t dst_ip;         /* network order; 0xffffffff = broadcast */
    uint32_t yiaddr;         /* network order, 0 if none */
    uint8_t mac[6];          /* client hardware address */
    uint32_t lease_s;        /* granted lease (ACK/OFFER) */
} ndh_out;

void ndh_init(ndh_state *s);
/* 1 if the configuration is usable (pool inside the subnet, excludes server, sizes sane). */
int ndh_cfg_valid(const ndh_cfg *c);

/* Handle one received DHCP packet (the UDP payload).  dns[0..1]: upstream resolvers, network order,
 * 0 = none.  Writes the reply into out (cap >= NDH_REPLY_MAX) and returns its length, 0 if there is no
 * reply.  *o describes what happened.  Never reads outside pkt[0..n) nor writes outside out[0..cap). */
size_t ndh_handle(ndh_state *s, const ndh_cfg *c, const uint8_t *pkt, size_t n, uint32_t now_ms, const uint32_t dns[2],
                  uint8_t *out, size_t cap, ndh_out *o);

/* Expire offers, leases and decline holds whose time is up.  Returns how many changed state. */
int ndh_tick(ndh_state *s, uint32_t now_ms);

/* The Ethernet link went down (and maybe came back with another device): every lease becomes stale. */
void ndh_link_cycle(ndh_state *s);

/* Number of BOUND leases. */
int ndh_bound(const ndh_state *s);
/* Index of the BOUND lease of the first pool address (.2), or -1. */
int ndh_primary(const ndh_state *s);
/* Remaining lease time of l (ms), 0 if not bound. */
uint32_t ndh_remaining_ms(const ndh_lease *l, uint32_t now_ms);

/* "aa:bb:cc:dd:ee:ff" into buf (>= 18 bytes). */
const char *ndh_mac_str(const uint8_t mac[6], char *buf);

#ifdef __cplusplus
}
#endif
#endif
