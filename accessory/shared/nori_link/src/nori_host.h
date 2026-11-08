/* Host-side accessory session, shared by the Nori app and the link helper.
 *
 * Lifecycle per opened port:
 *   WAIT_HELLO: send HOST now and every second; accept nothing but a HELLO with
 *               proto=1.  No HELLO within NH_HELLO_GIVEUP s => caller closes and
 *               retries later (backoff is the caller's job).
 *   READY:      HELLO accepted; HB/EV from that boot are accepted in sequence
 *               order.  Host sends HHB every second.  Silence > 3.5 s => back to
 *               WAIT_HELLO; silence > NH_SILENT_CLOSE s => close and reopen.
 * A HELLO or HB carrying a different boot id is a firmware reboot: the sequence
 * window restarts and events wait for the new HELLO.
 * Events are dropped when duplicated/out of order (seq), stale (device clock
 * more than NL_EV_STALE_MS behind the newest frame) or not from the current
 * boot.  Times are seconds (double) on the host. */
#ifndef NORI_HOST_H
#define NORI_HOST_H

#include "nori_link.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { NH_WAIT_HELLO, NH_READY };
enum { NH_F_IGNORED, NH_F_HELLO, NH_F_REBOOT, NH_F_HB, NH_F_EVENT, NH_F_DROPPED, NH_F_OTHER };

#define NH_HELLO_GIVEUP 5.0
#define NH_SILENT_CLOSE 8.0

typedef struct {
    int state;
    uint32_t boot, last_seq, dev_ms;
    int have_boot;
    double opened, last_rx, next_tx, hello_wait_since;
    char fw[24], sensor[24], wifi[12];
    int tun_cap;                     /* device HELLO tun=1: Wi-Fi tunnel opted in on the device */
    const char *role;                /* "app" or "linkd" */
    int want_tun;
    uint32_t seq_tx;
    uint32_t n_dup, n_stale, n_early, n_reboot, n_badproto, n_events;
    char hello_raw[NL_LINE_MAX + 2]; /* last accepted HELLO line, verbatim (linkd re-serves it) */
    size_t hello_len;
} nh_session;

typedef struct { int kind; uint32_t t; } nh_event;

/* Start a session on a freshly opened port (zero-initialise *s before the first call). */
void nh_open(nh_session *s, const char *role, int want_tun, double now);
/* Classify one frame from the device (raw: the line as received). */
int nh_frame(nh_session *s, const nl_frame *f, const char *raw, size_t rawlen, double now, nh_event *ev);
/* Something to send now?  Writes one line into out and returns its length (0: nothing).
 * *close_now is set when the caller should close the port and retry later. */
int nh_tick(nh_session *s, double now, char *out, size_t cap, int *close_now);
/* Format a host frame with the session's own sequence counter. */
int nh_format(nh_session *s, char *out, size_t cap, const char *type, const char *args);

#ifdef __cplusplus
}
#endif
#endif
