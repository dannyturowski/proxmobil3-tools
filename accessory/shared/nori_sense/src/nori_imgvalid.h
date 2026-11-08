/* wt32-r3: when may a rollback-pending OTA image be marked valid?  (portable, host-tested in test_sense.c)
 *
 * Only once remote management is proven on this boot, never on time alone:
 *   - uptime has reached min_ms (latched, so a millis() wrap after 49.7 days cannot undo it), and
 *   - a network address, ArduinoOTA and the HTTP listener are all up NOW, and
 *   - either an authenticated HTTP request was served this boot (NIV_AUTH),
 *     or that health has held continuously for healthy_ms (NIV_SUSTAINED).
 * Health is tracked with an explicit flag and the timestamp it started; elapsed time is computed with
 * unsigned subtraction, so it is wrap-safe.  (wt32-r3 review fix: the first version used "now | 1" as a
 * started-sentinel, which could lie 1 ms in the future and made the elapsed time underflow to ~2^32.) */
#ifndef NORI_IMGVALID_H
#define NORI_IMGVALID_H

#include <stdint.h>

enum { NIV_WAIT = 0, NIV_AUTH = 1, NIV_SUSTAINED = 2 };

typedef struct {
    int min_reached;       /* latched */
    int healthy;           /* network + ArduinoOTA + HTTP all up at the last step */
    uint32_t since;        /* when the current healthy period began (valid while healthy) */
} niv_state;

/* One step (call it every few hundred ms).  Returns NIV_WAIT / NIV_AUTH / NIV_SUSTAINED and points
 * *why at a short reason: min-uptime no-network no-otakey ota-down health-wait auth-request
 * sustained-health. */
static inline int niv_step(niv_state *s, uint32_t now, uint32_t min_ms, uint32_t healthy_ms, int net_up, int have_key,
                           int ota_up, int http_up, uint32_t auth_ok, const char **why)
{
    int healthy = net_up && ota_up && http_up;
    if (!healthy) s->healthy = 0;
    else if (!s->healthy) { s->healthy = 1; s->since = now; }
    if (now >= min_ms) s->min_reached = 1;
    if (!s->min_reached) { *why = "min-uptime"; return NIV_WAIT; }
    if (!net_up) { *why = "no-network"; return NIV_WAIT; }
    if (!have_key) { *why = "no-otakey"; return NIV_WAIT; }
    if (!healthy) { *why = "ota-down"; return NIV_WAIT; }
    if (auth_ok) { *why = "auth-request"; return NIV_AUTH; }
    if ((uint32_t)(now - s->since) >= healthy_ms) { *why = "sustained-health"; return NIV_SUSTAINED; }
    *why = "health-wait";
    return NIV_WAIT;
}

/* wt32-r4: the "network up" input of niv_step() for the router image.  Management must be proven over the
 * Wi-Fi upstream AND the router must be doing its job: routing complete (nori_nat's route state "ok": NAPT,
 * guard, Ethernet link + address, default route, DNAT bound) AND the PM3 holding its DHCP lease (.2).  An
 * image with broken DHCP or NAPT therefore stays pending and rolls back on the next reset, even though
 * authentication over Wi-Fi works (r4 parent/Fable review).  Returns 1 when all hold; otherwise 0 with
 * *why = no-network / route-wait / lease-wait (the firmware shows it as img_why). */
static inline int niv_router_net(int wifi_up, int route_ok, int lease_ok, const char **why)
{
    if (!wifi_up) { *why = "no-network"; return 0; }
    if (!route_ok) { *why = "route-wait"; return 0; }
    if (!lease_ok) { *why = "lease-wait"; return 0; }
    *why = "ok";
    return 1;
}

#endif
