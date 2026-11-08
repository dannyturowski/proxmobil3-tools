/* wt32-r3 (+ r4 codes): bounded RAM log of network / link / sensor transitions (portable C99, no allocation).
 * Not thread-safe: the firmware wraps every call in one spinlock (events come from the network event
 * task, loop() and the ToF task).  RAM only: nothing here ever touches flash. */
#ifndef NORI_EVLOG_H
#define NORI_EVLOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NLG_NONE,
    NLG_BOOT,        /* a = esp_reset_reason() */
    NLG_ETH_START, NLG_ETH_UP, NLG_ETH_DOWN, NLG_ETH_IP, NLG_ETH_STOP,     /* ETH_UP b = Mbit/s, a = full duplex */
    NLG_WIFI_ON, NLG_WIFI_OFF,                                           /* the station started / stopped (r4: upstream) */
    NLG_WIFI_CONN, NLG_WIFI_DISC, NLG_WIFI_IP, NLG_WIFI_NOIP,           /* WIFI_DISC a = 802.11 reason code */
    NLG_NET,         /* a = 0 none, 1 eth, 2 wifi: the interface HELLO/HB/OTA now use */
    NLG_HOST,        /* a = 1 a host started talking, 0 host silent */
    NLG_OTA,         /* a = 1 ArduinoOTA start, 2 ArduinoOTA error (b = ota_error_t),
                        3 wt32-r5.1.3 guard re-arm failed (b = NWD_* state after it: 1 = back under the watchdog) */
    NLG_HTTP,        /* a = 1 upload start, 2 upload refused/failed, 3 auth refused */
    NLG_IMG_OK,      /* a = 0: the pending image was marked valid (b = 1 authenticated request, 2 sustained health);
                        a = 1: marking it failed (b = esp_err_t), it stays pending and is retried */
    NLG_TOF,         /* a = stage (nori_tofsm NT_S_*) */
    NLG_PRES,        /* a = presence level sent to the host */
    NLG_TUN,         /* a = 1 tunnel client accepted, 0 closed */
    NLG_HEAP,        /* b = free heap in KiB when it first fell below the low-water warning */
    /* wt32-r4 (appended, so r3 tools/traces keep their codes; r3 shows these as "none"/"?") */
    NLG_DHCP,        /* a = NDH_EV_* (1 offer 2 ack 3 nak 4 release 5 decline 6 inform 7 full 8 other-server);
                        b = client MAC last byte << 8 | last octet of the address (full MAC in DIAG p=dhcp, /diag) */
    NLG_LAN,         /* a = NLA_EV_* (1 Ethernet link up, 2 down, 3 DHCP stand-down with b = NLA_SD_*, 4 pcb failure) */
    NLG_ROUTE,       /* a = 1 routing became ok, 0 it stopped being ok; b = nn_route_state index */
    /* wt32-r5 (appended, as above) */
    NLG_TOG,         /* track selector: a = NI_TOG_* (1 five, 2 one, 3 both contacts low: rejected);
                        b = 1 EV queued, 0 dropped (no host, queue, bucket) or refused until the centre */
    NLG_WNET,        /* wt32-r5.1, saved networks, by index only.  a = 0: table loaded or changed (b low byte = networks);
                      * else a = NWE_* << 4 | index, b = NWF_* << 8 | disconnect reason */
    /* wt32-r5.1.3 (appended, as above) */
    NLG_SLOW,        /* a loop() phase took NPH_SLOW_MS or more: a = NPH_* phase code (nori_phase.h), b = ms (65535 = longer) */
    NLG_COUNT
};
extern const char *const NLG_NAME[NLG_COUNT];

typedef struct {
    uint32_t t_ms;   /* uptime */
    uint8_t code, a;
    uint16_t b;
} nlg_ev;

#define NLG_N 48

typedef struct {
    nlg_ev ev[NLG_N];
    uint32_t total;          /* events ever added (the ring keeps the last NLG_N) */
    uint32_t count[NLG_COUNT];
} nlg_ring;

void nlg_init(nlg_ring *r);
void nlg_add(nlg_ring *r, uint32_t t_ms, uint8_t code, uint8_t a, uint16_t b);
uint32_t nlg_kept(const nlg_ring *r);
/* i = 0 the oldest kept event .. nlg_kept() - 1 the newest; 0 ok, -1 out of range */
int nlg_get(const nlg_ring *r, uint32_t i, nlg_ev *out);
/* "12.3:wifi_disc:201" (t in s with 0.1 s, name, a, and b when non-zero) */
int nlg_fmt(const nlg_ev *e, char *out, size_t cap);
/* The newest `n` events as "ev=<fmt>,<fmt>,.." (oldest of them first); whole entries only, always
 * NUL-terminated and never longer than cap - 1.  Returns the length. */
int nlg_tail(const nlg_ring *r, unsigned n, char *out, size_t cap);
/* JSON array of the kept events, oldest first: [{"t":12345,"e":"wifi_disc","a":201,"b":0},...].
 * Whole entries only (it drops the OLDEST ones if cap is short) and always valid JSON if cap >= 3.
 * Returns the length. */
int nlg_json(const nlg_ring *r, char *out, size_t cap);
/* JSON array of a plain event array (used for the previous boot's RTC trace). */
int nlg_json_arr(const nlg_ev *ev, unsigned n, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
