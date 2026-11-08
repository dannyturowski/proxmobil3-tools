/* wt32-r5.1.3: buttons and the selector never need Wi-Fi.
 *
 * The firmware's own decision code, in loop() order, for ten simulated minutes per case: the real link core
 * (nori_dev.c: HOST/HHB session, EV frames, HELLO/HB), the real debouncers (nori_input.c), and the real saved-network
 * state machine (nori_wnsm.c) on a fake radio.  Inputs are sampled every 5 ms, the Wi-Fi machine ticks every 250 ms.
 *   away     both saved networks out of range (the outing): scans, no joins, never up
 *   flap     the phone hotspot appearing and vanishing every 30 s, joined and lost over and over
 *   off      WIFIEN on=0
 *   nocfg    no network saved
 *   home     control: the primary in range
 * In every case each press, release and lever move must reach the wire exactly once, the first press 300 ms after
 * boot (before any scan has finished), and none may be dropped.
 *
 * What this cannot show: how long the real Arduino/ESP-IDF calls behind the HAL take.  That is what the task watchdog
 * and the phase record (nori_lwdt.h, nori_phase.h) are for.  Test network names and passwords only. */
#include "nori_dev.h"
#include "nori_wnet.h"
#include "nori_wnsm.h"

#include <stdio.h>
#include <string.h>

#if !ND_DIAG_JOIN || !ND_PRESENCE || !ND_EXT
#error "build with -DND_DIAG_JOIN=1 -DND_PRESENCE=1 -DND_EXT=1 (the wt32 firmware's flags)"
#endif

static int checks, fails;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

/* ---- the fake radio (as tests/test_wifinet.c) ---- */
enum { R_IDLE, R_CONNECTING, R_ASSOC, R_UP };
static struct {
    uint32_t now;
    int in_range[2];
    int flap;                         /* the hotspot (index 1) toggles in and out of range every 30 s */
    int on, st, target, assoc, ip;
    uint32_t t_scan, t_next, disc;
    uint8_t reason;
    int scans, joins, ups, losses;
} R;
static nw_table T;
static nws S;
static nws_in IN;

static void r_on(void *c) { (void)c; R.on = 1; }
static void r_down(uint8_t why) { int was = R.st != R_IDLE; R.st = R_IDLE; R.assoc = R.ip = 0; if (was) { R.reason = why; R.disc++; } }
static void r_off(void *c) { (void)c; R.on = 0; r_down(8); }
static int r_scan_start(void *c, int bg) { (void)c; R.scans++; R.t_scan = R.now + (bg ? 1500 : 3000); return 0; }
static int r_scan_poll(void *c, nws_seen *o)
{
    (void)c;
    if ((int32_t)(R.now - R.t_scan) < 0) return 0;
    for (int i = 0; i < T.n && i < 2; i++) if (R.in_range[i]) { o[i].vis = 1; o[i].rssi = -60; }
    return 1;
}
static void r_scan_stop(void *c) { (void)c; }
static int r_join(void *c, int i) { (void)c; R.joins++; R.target = i; R.st = R_CONNECTING; R.t_next = R.now + 1000; return 0; }
static void r_leave(void *c) { (void)c; r_down(8); }
static const nws_hal WHAL = { NULL, r_on, r_off, r_scan_start, r_scan_poll, r_scan_stop, r_join, r_leave, NULL };

static void world(void)
{
    if (R.flap) R.in_range[1] = (R.now / 30000) % 2 == 0;
    if (R.st == R_CONNECTING && (int32_t)(R.now - R.t_next) >= 0) {
        if (!R.in_range[R.target]) r_down(201);
        else { R.st = R_ASSOC; R.assoc = 1; R.t_next = R.now + 1000; }
    } else if (R.st == R_ASSOC && (int32_t)(R.now - R.t_next) >= 0) {
        R.st = R_UP; R.ip = 1; R.ups++;
    }
    if (R.st >= R_ASSOC && !R.in_range[R.target]) { if (R.st == R_UP) R.losses++; r_down(200); }
}

/* ---- the device ---- */
static nd_dev dev;
static int n_b1d, n_b1u, n_b2d, n_b2u, n_trk5, n_trk1, n_hello, hb_joining, hb_up, hb_off, hb_nocfg;
static uint32_t first_b1d_at, first_scan_done_at;
static size_t h_room(void *c) { (void)c; return 4096; }
static size_t h_queued(void *c) { (void)c; return 0; }
static int has(const char *s, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++) if (!memcmp(s + i, needle, k)) return 1;
    return 0;
}
static void h_write(void *c, const char *s, size_t n)
{
    (void)c;
    if (has(s, n, " EV b1d ")) { n_b1d++; if (!first_b1d_at) first_b1d_at = R.now; }
    n_b1u += has(s, n, " EV b1u ");
    n_b2d += has(s, n, " EV b2d ");
    n_b2u += has(s, n, " EV b2u ");
    n_trk5 += has(s, n, " EV trk5 ");
    n_trk1 += has(s, n, " EV trk1 ");
    n_hello += has(s, n, " HELLO ");
    if (has(s, n, " HB ")) {
        hb_joining += has(s, n, "wifi=joining");
        hb_up += has(s, n, "wifi=up");
        hb_off += has(s, n, "wifi=off");
        hb_nocfg += has(s, n, "wifi=nocfg");
    }
}
/* As the firmware's w_status(): the upstream station, nothing else. */
static int h_wifi(void *c, char *ip, size_t cap, int *rssi)
{
    (void)c;
    if (R.on && R.ip) { snprintf(ip, cap, "192.0.2.10"); *rssi = -60; return ND_WIFI_UP; }
    if (dev.cfg.wifi_on && T.n) return ND_WIFI_JOINING;
    if (dev.cfg.wifi_on) return ND_WIFI_NOCFG;
    return ND_WIFI_OFF;
}
static const nd_hal HAL = { .uart_room = h_room, .uart_queued = h_queued, .uart_write = h_write, .wifi_status = h_wifi };

static void host_frame(uint32_t *seq, const char *type, const char *args, uint32_t now)
{
    char line[NL_LINE_MAX + 2];
    int n = nl_format(line, sizeof line, ++*seq, type, args);
    nd_uart_input(&dev, (const uint8_t *)line, (size_t)n, now);
}

enum { C_AWAY, C_FLAP, C_OFF, C_NOCFG, C_HOME };
static const char *const CASE[] = { "away", "flap", "off", "nocfg", "home" };

/* Press schedules, all starting before the first scan can have finished. */
static int b1_raw(uint32_t t) { return t >= 300 && (t - 300) % 2000 < 120; }
static int b2_raw(uint32_t t) { return t >= 700 && (t - 700) % 3000 < 80; }
static int five_raw(uint32_t t) { return t >= 1000 && (t - 1000) % 4000 < 250; }
static int one_raw(uint32_t t) { return t >= 3000 && (t - 3000) % 4000 < 250; }
static int expected(uint32_t start, uint32_t period, uint32_t end) { return end > start ? (int)((end - start + period - 1) / period) : 0; }

static void run_case(int k, uint32_t ms)
{
    memset(&R, 0, sizeof R);
    memset(&IN, 0, sizeof IN);
    memset(&T, 0, sizeof T);
    n_b1d = n_b1u = n_b2d = n_b2u = n_trk5 = n_trk1 = n_hello = hb_joining = hb_up = hb_off = hb_nocfg = 0;
    first_b1d_at = first_scan_done_at = 0;
    if (k != C_NOCFG) {
        nw_net_set(&T.net[0], "home-test", 9, "home-test-pass", 14, 0);   /* broadcast networks: hidden=0, as live */
        nw_net_set(&T.net[1], "hotspot-test", 12, "hotspot-test-pass", 17, 0);
        T.n = 2;
    }
    R.in_range[0] = k == C_HOME;
    R.flap = k == C_FLAP;
    nws_init(&S, &WHAL);
    nd_init(&dev, &HAL, 0x5eed0013u, "wt32-r5.1.3", "vl53l5cx", 2, NULL, 0);
    dev.cfg.wifi_on = k != C_OFF;

    ni_button btn[2];
    ni_toggle tog;
    ni_gesture ges;
    ni_button_init(&btn[0], 0, 0);
    ni_button_init(&btn[1], 0, 0);
    ni_toggle_init(&tog, 0, 0, 0);
    ni_gesture_init(&ges, &tog);
    uint32_t seq = 0;
    for (R.now = 5; R.now <= ms; R.now += 5) {
        uint32_t now = R.now;
        if (now == 50) host_frame(&seq, "HOST", "proto=1 role=linkd tun=0", now);
        else if (now > 50 && now % 500 == 0) host_frame(&seq, "HHB", "", now);
        /* loop(): inputs first */
        nd_button(&dev, 0, ni_button_update(&btn[0], b1_raw(now), now, 30), now);
        nd_button(&dev, 1, ni_button_update(&btn[1], b2_raw(now), now, 30), now);
        nd_selector(&dev, &tog, &ges, five_raw(now), one_raw(now), now, 30, NULL);
        /* then net_service() */
        if (now % 250 == 0) {
            world();
            IN.wanted = dev.cfg.wifi_on && T.n > 0;
            IN.assoc = R.assoc; IN.ip = R.ip; IN.disc_seq = R.disc; IN.reason = R.reason;
            nws_tick(&S, &T, &IN, now);
            if (R.scans && !first_scan_done_at && S.state != NWS_SCAN) first_scan_done_at = now;
        }
        nd_poll(&dev, now);
    }
    uint32_t end = ms;
    char what[96];
    int ok = n_b1d == expected(300, 2000, end) && n_b1u == n_b1d && n_b2d == expected(700, 3000, end) && n_b2u == n_b2d &&
             n_trk5 == expected(1000, 4000, end) && n_trk1 == expected(3000, 4000, end);
    snprintf(what, sizeof what, "%s: every press, release and lever move on the wire once", CASE[k]);
    check(what, ok);
    if (!ok) printf("  b1 %d/%d b2 %d/%d trk5 %d trk1 %d\n", n_b1d, n_b1u, n_b2d, n_b2u, n_trk5, n_trk1);
    snprintf(what, sizeof what, "%s: nothing dropped, the host session never lost", CASE[k]);
    check(what, dev.ev_dropped == 0 && dev.host_alive && n_hello >= 1);
    snprintf(what, sizeof what, "%s: the first press went out within 60 ms, before any scan finished", CASE[k]);
    check(what, first_b1d_at && first_b1d_at <= 360 && (!first_scan_done_at || first_b1d_at < first_scan_done_at));
    printf("  %-5s scans=%d joins=%d ups=%d losses=%d HB joining/up/off/nocfg=%d/%d/%d/%d  b1=%d b2=%d trk5=%d trk1=%d\n", CASE[k],
           R.scans, R.joins, R.ups, R.losses, hb_joining, hb_up, hb_off, hb_nocfg, n_b1d, n_b2d, n_trk5, n_trk1);
    switch (k) {
    case C_AWAY:
        check("away: it kept looking (scans) but never joined a network it cannot see, never up", R.scans >= 10 && R.joins == 0 && R.ups == 0);
        check("away: HB says joining", hb_joining > 0 && hb_up == 0);
        break;
    case C_FLAP:
        /* up < 30 s each time: LOST, backed off 20 s, 40 s, ... (nori_wnsm.h), so fewer joins than flaps */
        check("flap: the hotspot was joined and lost again and again", R.ups >= 3 && R.losses >= 3 && R.joins >= R.ups);
        check("flap: HB says up and joining by turns", hb_up > 0 && hb_joining > 0);
        break;
    case C_OFF:
        check("off: no radio use at all", R.scans == 0 && R.joins == 0 && hb_off > 0 && hb_joining == 0);
        break;
    case C_NOCFG:
        check("nocfg: no radio use at all", R.scans == 0 && R.joins == 0 && hb_nocfg > 0);
        break;
    case C_HOME:
        check("home: on the primary and staying there", R.ups == 1 && R.losses == 0 && hb_up > 0);
        break;
    }
}

int main(void)
{
    for (int k = C_AWAY; k <= C_HOME; k++) run_case(k, 10u * 60u * 1000u);
    printf("test_offline: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
