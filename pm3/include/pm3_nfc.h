/* Anonymous NFC presence engine for the PROXmobil3 reader module.
 *
 * I/O free: the caller feeds received bytes and monotonic time, and supplies two
 * callbacks, one that writes a frame to the UART and one that receives a JSON line.
 * The engine
 *   - runs the reader setup (abort card handling, card release, version and status
 *     probes, then enables the configured card functions, each acknowledged),
 *   - re-runs that setup when the module announces a restart (0x0F),
 *   - polls status and correlates each reply with the request it answers,
 *   - turns card reports into presence episodes (accepted / held / cooldown / absent),
 *   - optionally treats the unsolicited 63-byte 0xD1 EMV status frame as anonymous
 *     "phone or payment card" presence (a heuristic, see docs/software/pm3-nfc-presence.md).
 *
 * Privacy: payload bytes of card frames are never copied out, printed or stored.
 * Events carry message types, lengths, counters and timings only.  Reader log
 * messages (0xED) are reported by level and length; their text is never output.
 *
 * Transmit allowlist: 0x02 version, 0x04 status, 0x32 card release, 0x46 abort card
 * handling, 0xE4 function enable.  Nothing else can be sent: no reset, no APDUs,
 * no EMV or payment commands. */
#ifndef PM3_NFC_H
#define PM3_NFC_H

#include "pm3_ipp.h"

#define PM3_NFC_MAX_FUNCS 4
#define PM3_NFC_PEND 16

typedef struct {
    double accept_gap;        /* s: a new presence closer than this to the last accepted one is "cooldown" */
    double remove_confirm;    /* s: absence confirmed this long after a removal frame */
    double presence_timeout;  /* s: no report for this long ends a presence */
    double status_every;      /* s: status request interval once set up */
    double status_deadline;   /* s: a status reply later than this counts as missing */
    double startup_grace;     /* s: phone presence ignored this long after setup completes */
    double resync;            /* s: after a module restart, reports are held cards this long */
    double health_every;      /* s: health line interval, 0 = never */
    double silent_after;      /* s: no valid frame for this long => link silent */
    int phone_presence;       /* 1: report 63-byte unsolicited 0xD1 as anonymous phone/payment presence */
    int release_cards;        /* 1: send one card release (0x32) per card presence, as the stock flow does */
    int rearm_abort;          /* 1: one abort (0x46) per card presence after it ends (re-arms detection) */
    int frames;               /* 1: one metadata line per received frame (type, length, seq; never payload) */
    uint8_t funcs[PM3_NFC_MAX_FUNCS];
    int nfuncs;
} pm3_nfc_cfg;

void pm3_nfc_cfg_defaults(pm3_nfc_cfg *c);

typedef void (*pm3_nfc_send_fn)(void *ctx, const uint8_t *frame, size_t n);
typedef void (*pm3_nfc_emit_fn)(void *ctx, const char *json_line);

enum { PM3_ST_LISTEN, PM3_ST_ABORT, PM3_ST_RELEASE, PM3_ST_PROBE, PM3_ST_ENABLE, PM3_ST_READY };
enum { PM3_KIND_CARD, PM3_KIND_PHONE };

typedef struct {
    pm3_nfc_cfg cfg;
    pm3_nfc_send_fn send;
    pm3_nfc_emit_fn emit;
    void *ctx;
    ipp_rx rx;
    double t0, now;
    uint8_t seq;
    /* setup */
    int stage, fn_idx, fn_tries, after_startup;
    double next_tx, setup_started, ready_at, resync_until;
    unsigned setups, startups, fn_rejects;
    /* request correlation */
    struct { uint8_t type, seq, arg, open; double at; } pend[PM3_NFC_PEND];
    unsigned pend_i;
    double last_status_tx, last_status_ok, last_valid;
    unsigned status_ok, status_late, status_missing, status_miss_run, status_malformed, uncorrelated;
    unsigned status_code;
    int have_status_code, link_silent;
    /* presence */
    int present, present_kind, removal_pending, released, abort_armed;
    double last_seen, removed_at, last_accept, present_since, phone_last, abort_last;
    unsigned accepted[2], cooldown, held, resynced, phone_rejected, absent_removal, absent_timeout;
    unsigned release_acks, aborts, heartbeats, reader_msgs, tx_frames, tx_refused;
    double health_next;
} pm3_nfc;

void pm3_nfc_init(pm3_nfc *z, const pm3_nfc_cfg *cfg, pm3_nfc_send_fn send, pm3_nfc_emit_fn emit,
                  void *ctx, double now);
void pm3_nfc_bytes(pm3_nfc *z, const uint8_t *b, size_t n, double now);
void pm3_nfc_tick(pm3_nfc *z, double now);
void pm3_nfc_health(pm3_nfc *z, double now);
/* Shutdown, in two steps about 0.2 s apart: step 0 aborts card handling, step 1
 * releases the card slot, so the next program starts clean. */
void pm3_nfc_release(pm3_nfc *z, int step, double now);

/* Internal, exposed for tests: send one frame if its type is on the allowlist. 1 sent, 0 refused. */
int pm3_nfc_send(pm3_nfc *z, uint8_t type, uint8_t reply_to, const uint8_t *pay, size_t n);

#endif
