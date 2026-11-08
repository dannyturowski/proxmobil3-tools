/* wt32-r5.1.3: what loop() was doing, retained across resets (RTC slow memory, RTC_NOINIT_ATTR in main.cpp).
 *
 * The question it answers after a reset: was loop() stuck in one call (a network call that never returned), or was it
 * running normally when the board reset (a brownout, a power cut)?
 *  - ph / ph_ms: the phase loop() is in and when it entered it.  loop() passes through ~12 phases per millisecond pass,
 *    so in normal operation ph_ms is never more than a few ms old.
 *  - alive_ms: written every NPH_ALIVE_MS by an esp_timer, independent of loop().  At the reset, alive_ms - ph_ms is
 *    how long loop() had been in its last phase ("stuck" below): a few ms for a reset during normal work, ~5000 ms
 *    when the task watchdog took a loop() that stopped feeding it.
 *  - the longest phase of the boot and the number of phases that took NPH_SLOW_MS or more.
 *
 * BEST EFFORT, like nori_rst.h: plain RAM, no checksum (it changes thousands of times a second).  Validity is a magic,
 * the boot id and its complement, and every phase code in range.  A power-on loses it ("none"); a brownout normally
 * keeps it (the ESP32 brownout handler restarts in software).  Nothing here is a credential or a network name.
 * Portable C, no allocation: tests/test_phase.c runs it on the host. */
#ifndef NORI_PHASE_H
#define NORI_PHASE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NPH_MAGIC 0x4e504831u     /* "NPH1" */
#define NPH_SLOW_MS 1000u         /* a phase this long is counted, and logged (NLG_SLOW) */
#define NPH_ALIVE_MS 100u         /* the esp_timer period that writes alive_ms */
#define NPH_DIAG_MAX 200          /* nph_diag() worst case incl. NUL; tests check it */
#define NPH_JSON_MAX 400          /* nph_json() worst case incl. NUL; tests check it */

/* Append only: codes are what a previous boot's record holds. */
enum {
    NPH_NONE, NPH_SETUP, NPH_ETH_BEGIN, NPH_TOF_BEGIN,
    NPH_UART, NPH_INPUT, NPH_TOF, NPH_NET,
    NPH_WIFI_ON, NPH_WIFI_OFF, NPH_SCAN_START, NPH_SCAN_POLL, NPH_SCAN_STOP, NPH_JOIN, NPH_LEAVE, NPH_ETH_CFG,
    NPH_NAT, NPH_OTA_SVC, NPH_OTA_STOP, NPH_OTA_BEGIN, NPH_HTTP_BEGIN, NPH_HTTP_STOP, NPH_IMG_MARK,
    NPH_OTA_POLL, NPH_OTA_XFER, NPH_HTTP, NPH_HTTP_REQ, NPH_HTTP_UP, NPH_HTTP_DRAIN, NPH_HTTP_TX,
    NPH_LINK, NPH_TICK, NPH_IDLE, NPH_WDT_TEST,
    NPH_COUNT
};
static const char *const NPH_NAME[NPH_COUNT] = {
    "none", "setup", "eth_begin", "tof_begin",
    "uart", "input", "tof", "net",
    "wifi_on", "wifi_off", "scan_start", "scan_poll", "scan_stop", "join", "leave", "eth_cfg",
    "nat", "ota_svc", "ota_stop", "ota_begin", "http_begin", "http_stop", "img_mark",
    "ota_poll", "ota_xfer", "http", "http_req", "http_up", "http_drain", "http_tx",
    "link", "tick", "idle", "wdt_test",
};
static inline const char *nph_name(uint32_t ph) { return ph < NPH_COUNT ? NPH_NAME[ph] : "?"; }

/* Loop watchdog state, as nori_lwdt.h keeps it (NWD_*), mirrored here so the previous boot's is known. */
static const char *const NPH_WDT_NAME[] = { "off", "on", "ota" };
static inline const char *nph_wdt_name(uint32_t w) { return w < 3 ? NPH_WDT_NAME[w] : "?"; }

typedef struct {
    uint32_t magic, boot_id, boot_inv;
    volatile uint32_t ph, ph_ms;        /* loop() only */
    volatile uint32_t alive_ms;         /* esp_timer only */
    uint32_t worst_ph, worst_ms, worst_at_ms;
    uint32_t slow, passes;
    volatile uint32_t wdt;              /* NWD_*: 0 off (setup), 1 loop() subscribed, 2 ArduinoOTA transfer (guarded) */
    volatile uint32_t guard_ms;         /* esp_timer only: uptime at which the ArduinoOTA guard fired, 0 never */
} nph_rec;

static inline int nph_valid(const nph_rec *r)
{
    return r->magic == NPH_MAGIC && r->boot_inv == ~r->boot_id && r->ph < NPH_COUNT && r->worst_ph < NPH_COUNT && r->wdt < 3;
}

/* At boot: copy the previous boot's record to *prev (returns 1) or report none (0), then restart it for this boot. */
static inline int nph_take(nph_rec *rtc, nph_rec *prev, uint32_t boot_id, uint32_t now)
{
    int ok = nph_valid(rtc);
    if (ok) *prev = *rtc;
    else memset(prev, 0, sizeof *prev);
    memset(rtc, 0, sizeof *rtc);
    rtc->boot_id = boot_id;
    rtc->boot_inv = ~boot_id;
    rtc->ph = NPH_SETUP;
    rtc->ph_ms = now;
    rtc->alive_ms = now;
    rtc->worst_ph = NPH_NONE;
    rtc->magic = NPH_MAGIC;
    return ok;
}

/* Enter phase ph at now.  Returns the phase left; *left_ms (if not NULL) how long it lasted. */
static inline uint32_t nph_enter(nph_rec *r, uint32_t ph, uint32_t now, uint32_t *left_ms)
{
    uint32_t was = r->ph, d = now - r->ph_ms;
    if ((int32_t)d < 0) d = 0;
    if (d > r->worst_ms) { r->worst_ms = d; r->worst_ph = was; r->worst_at_ms = r->ph_ms; }
    if (d >= NPH_SLOW_MS) r->slow++;
    r->ph = ph < NPH_COUNT ? ph : (uint32_t)NPH_NONE;
    r->ph_ms = now;
    if (left_ms) *left_ms = d;
    return was;
}

static inline void nph_alive(nph_rec *r, uint32_t now) { r->alive_ms = now; }

/* How long loop() had been in its phase at `at` (now for this boot, alive_ms for the previous one). */
static inline uint32_t nph_in_ms(const nph_rec *r, uint32_t at)
{
    uint32_t d = at - r->ph_ms;
    return (int32_t)d < 0 ? 0 : d;
}

/* DIAG p=ph tokens, most useful first (the link cuts a long page at the end; the pass count is in the JSON only):
 *   ph=link pms=0 wph=http wms=2981 wat=812 slow=1 wdt=on
 *   prev=ok pph=join pin=4998 pal=812 pwdt=on pgd=0 pwph=http pwms=2981 pslow=0      (pin: ms in pph at the reset)
 * or prev=none. */
static inline int nph_diag(const nph_rec *cur, uint32_t now, const nph_rec *prev, int prev_ok, char *out, size_t cap)
{
    int n = snprintf(out, cap, "ph=%s pms=%lu wph=%s wms=%lu wat=%lu slow=%lu wdt=%s", nph_name(cur->ph),
                     (unsigned long)nph_in_ms(cur, now), nph_name(cur->worst_ph), (unsigned long)cur->worst_ms,
                     (unsigned long)(cur->worst_at_ms / 1000), (unsigned long)cur->slow, nph_wdt_name(cur->wdt));
    if (n < 0 || (size_t)n >= cap) return n;
    int k;
    if (prev_ok)
        k = snprintf(out + n, cap - (size_t)n, " prev=ok pph=%s pin=%lu pal=%lu pwdt=%s pgd=%lu pwph=%s pwms=%lu pslow=%lu",
                     nph_name(prev->ph), (unsigned long)nph_in_ms(prev, prev->alive_ms), (unsigned long)(prev->alive_ms / 1000),
                     nph_wdt_name(prev->wdt), (unsigned long)(prev->guard_ms / 1000), nph_name(prev->worst_ph),
                     (unsigned long)prev->worst_ms, (unsigned long)prev->slow);
    else
        k = snprintf(out + n, cap - (size_t)n, " prev=none");
    return k < 0 ? k : n + k;
}

/* /diag JSON members (no braces around the pair, no leading/trailing comma):
 *   "phase":{"now":"link","ms":0,"worst":"http","worst_ms":2981,"worst_at_s":812,"slow":1,"passes":123456,"wdt":"on"},
 *   "phase_prev":{"valid":1,"last":"join","in_ms":4998,"alive_ms":812345,"worst":"join","worst_ms":4998,"worst_at_s":807,
 *                 "slow":0,"passes":99,"wdt":"on","guard_s":0}                         ("phase_prev":{"valid":0} if none) */
static inline int nph_json(const nph_rec *cur, uint32_t now, const nph_rec *prev, int prev_ok, char *out, size_t cap)
{
    int n = snprintf(out, cap, "\"phase\":{\"now\":\"%s\",\"ms\":%lu,\"worst\":\"%s\",\"worst_ms\":%lu,\"worst_at_s\":%lu,"
                               "\"slow\":%lu,\"passes\":%lu,\"wdt\":\"%s\"},",
                     nph_name(cur->ph), (unsigned long)nph_in_ms(cur, now), nph_name(cur->worst_ph), (unsigned long)cur->worst_ms,
                     (unsigned long)(cur->worst_at_ms / 1000), (unsigned long)cur->slow, (unsigned long)cur->passes,
                     nph_wdt_name(cur->wdt));
    if (n < 0 || (size_t)n >= cap) return n;
    int k;
    if (prev_ok)
        k = snprintf(out + n, cap - (size_t)n,
                     "\"phase_prev\":{\"valid\":1,\"last\":\"%s\",\"in_ms\":%lu,\"alive_ms\":%lu,\"worst\":\"%s\",\"worst_ms\":%lu,"
                     "\"worst_at_s\":%lu,\"slow\":%lu,\"passes\":%lu,\"wdt\":\"%s\",\"guard_s\":%lu}",
                     nph_name(prev->ph), (unsigned long)nph_in_ms(prev, prev->alive_ms), (unsigned long)prev->alive_ms,
                     nph_name(prev->worst_ph), (unsigned long)prev->worst_ms, (unsigned long)(prev->worst_at_ms / 1000),
                     (unsigned long)prev->slow, (unsigned long)prev->passes, nph_wdt_name(prev->wdt),
                     (unsigned long)(prev->guard_ms / 1000));
    else
        k = snprintf(out + n, cap - (size_t)n, "\"phase_prev\":{\"valid\":0}");
    return k < 0 ? k : n + k;
}

#endif
