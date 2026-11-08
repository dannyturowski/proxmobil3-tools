/* wt32-r5.1: which saved network to be on.  Portable C, no time source and no Wi-Fi of its own: the firmware
 * calls nws_tick() every 250 ms with what it sees and nws_hal does the work.  tests/test_wifinet.c drives this
 * same file with a fake clock and a fake radio.  Nothing here may block.
 *
 *   OFF -> SCAN -> JOIN -> UP       a scan shows which known networks are in range; the highest priority one that
 *            ^      |       |       is not backing off is tried.  A failed attempt goes to the next one, and when
 *            |      v       v       none is left to WAIT, then a new scan.
 *          WAIT <- LEAVE   BG       UP on a fallback only: a rare scan for something better.
 *
 * Failure, per network:
 *   NOAP   not in range: no penalty if it is simply not seen; a hidden network has its blind tries backed off.
 *   AUTH / ASSOC / DHCP / LOST: exponential backoff, also while it stays visible.  LOST = dropped before NWS_STABLE_MS.
 *
 * Stability: on the primary there is never a scan.  On a fallback there is one every NWS_BG_PERIOD_MS, and the link
 * moves only after two consecutive sightings of something better at a usable signal.  A failed move returns at once
 * and holds that network off for NWS_UPGRADE_HOLD_MS.  A stable link that drops is rejoined first, without a scan.
 *
 * in.hold (an OTA owns the network): no scan, no join, no leave, whatever the link does; a background scan in
 * progress is stopped.  in.no_bg (image not yet proven, tunnel open): no background scan or move, but a lost link is
 * still recovered.  Hidden networks cannot displace a working link: a scan cannot show them. */
#ifndef NORI_WNSM_H
#define NORI_WNSM_H

#include "nori_wnet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NWS_SCAN_MAX_MS 8000u        /* a scan that has not finished by then counts as failed: a blind round */
#define NWS_ASSOC_MS 15000u          /* join -> associated */
#define NWS_DHCP_MS 15000u           /* associated -> address */
#define NWS_SETTLE_MS 250u           /* after leaving, for the disconnect event to pass */
#define NWS_LEAVE_MAX_MS 2000u
#define NWS_RESCAN_MIN_MS 10000u
#define NWS_RESCAN_MAX_MS 60000u
#define NWS_BACKOFF_MS 20000u        /* AUTH/ASSOC/DHCP/LOST: doubles per consecutive failure */
#define NWS_BACKOFF_MAX_MS 300000u
#define NWS_BLIND_MS 30000u          /* blind tries of a network no scan shows: doubles per failure */
#define NWS_BLIND_MAX_MS 600000u
#define NWS_STALE_MS 10000u          /* seen in a scan, then not found by the join */
#define NWS_STABLE_MS 30000u         /* an address held this long clears the failure history */
#define NWS_IP_LOSS_MS 10000u        /* associated without an address this long = a failed link */
#define NWS_BG_FIRST_MS 60000u
#define NWS_BG_PERIOD_MS 300000u
#define NWS_BG_CONFIRM_MS 30000u
#define NWS_BG_MAX_MS 5000u
#define NWS_UPGRADE_HOLD_MS 900000u
#define NWS_UPGRADE_RSSI (-78)

enum { NWS_OFF, NWS_SCAN, NWS_JOIN, NWS_UP, NWS_BG, NWS_LEAVE, NWS_WAIT, NWS_STATES };
enum { NWF_NONE, NWF_NOAP, NWF_AUTH, NWF_ASSOC, NWF_DHCP, NWF_LOST, NWF_BEGIN, NWF_COUNT };
enum { NWW_NONE, NWW_BOOT, NWW_LOST, NWW_FAILED, NWW_UPGRADE, NWW_CONFIG, NWW_COUNT };   /* why the current attempt began */
enum { NWE_JOIN = 1, NWE_UP, NWE_FAIL, NWE_UPGRADE, NWE_SCANFAIL };
extern const char *const NWS_STATE_NAME[NWS_STATES];
extern const char *const NWF_NAME[NWF_COUNT];
extern const char *const NWW_NAME[NWW_COUNT];

typedef struct { uint8_t vis; int8_t rssi; } nws_seen;

typedef struct {
    void *ctx;
    void (*radio_on)(void *ctx);
    void (*radio_off)(void *ctx);
    int (*scan_start)(void *ctx, int bg);              /* 0 started */
    int (*scan_poll)(void *ctx, nws_seen *seen);       /* 0 running, 1 done (seen[i] for table index i), -1 failed */
    void (*scan_stop)(void *ctx);
    int (*join)(void *ctx, int index);                 /* 0 started */
    void (*leave)(void *ctx);
    void (*note)(void *ctx, int what, int index, int cls, int reason);   /* NWE_*; may be NULL */
} nws_hal;

typedef struct {
    int wanted;                /* Wi-Fi on and at least one network saved */
    int assoc, ip;             /* associated; has an address */
    uint32_t disc_seq;         /* disconnect events so far */
    uint8_t reason;            /* reason of the last one */
    int hold, no_bg;
} nws_in;

typedef struct {
    uint32_t tries, oks, fails[NWF_COUNT];
    uint32_t nb, bnb;          /* not before: any attempt / a blind attempt */
    uint8_t nb_on, bnb_on, fc, bfc;
    uint8_t vis, tried, last_cls, last_reason;
    int8_t rssi;
} nws_prof;

typedef struct {
    const nws_hal *hal;
    int state, cur;            /* cur: network in use or being joined, -1 none */
    int kind, prev;            /* this attempt: normal / fast rejoin / upgrade (prev = where to return) */
    int after, after_idx, after_kind;
    int blind, kick, held, stable, ipl_on, assoc_seen, cand, cand_n, why;
    uint32_t t_join, t_assoc, t_up, t_ipl, t_leave, t_scan, wait_until, rescan_ms, next_bg, disc_base;
    uint32_t scans, bg_scans, scan_fails, joins, switches, upgrades, upgrade_fails, rounds, holds, last_up;
    uint8_t last_cls, last_reason;
    nws_prof p[NW_MAX];
} nws;

void nws_init(nws *s, const nws_hal *hal);
void nws_tick(nws *s, const nw_table *t, const nws_in *in, uint32_t now);
/* The table changed from *old to *now_t: runtime state follows each SSID; a network whose login changed starts clean. */
void nws_remap(nws *s, const nw_table *old, const nw_table *now_t, uint32_t now);
/* Reselect now (WIFISET named a new primary).  Waits for hold.  Does nothing if already on the primary. */
void nws_kick(nws *s);
int nws_active(const nws *s);  /* table index with a working link, or -1 */
int nws_class(uint8_t reason); /* disconnect reason -> NWF_* */

#ifdef __cplusplus
}
#endif
#endif
