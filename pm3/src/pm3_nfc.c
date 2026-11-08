/* Anonymous NFC presence engine; see pm3_nfc.h. */
#include "pm3_nfc.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LISTEN_S     1.2    /* listen before setup: a booting module announces itself */
#define STEP_GAP_S   0.2    /* between abort, release and the probes */
#define ENABLE_RETRY 1.0
#define ENABLE_SLOW  4.0    /* after 10 tries; a cold module can take a minute */
#define ABORT_GRACE  1.5    /* 0xD1 right after our own abort is ignored */
#define ABORT_MIN    2.0    /* at most one rearm abort per this */
#define PHONE_QUIET  0.40   /* 0xD1 bursts closer than this are one presence */
#define STATUS_LEN   2

static const char *const STAGE_NAME[] = { "listen", "abort", "release", "probe", "enable", "ready" };
static const char *const KIND_NAME[] = { "card", "phone_or_payment" };

void pm3_nfc_cfg_defaults(pm3_nfc_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->accept_gap = 0.35;
    c->remove_confirm = 0.30;
    c->presence_timeout = 3.0;
    c->status_every = 5.0;
    c->status_deadline = 3.0;
    c->startup_grace = 3.0;
    c->resync = 3.0;
    c->health_every = 30.0;
    c->silent_after = 10.0;
    c->phone_presence = 1;
    c->release_cards = 1;
    c->rearm_abort = 1;
    c->funcs[0] = IPP_FN_ISO; c->funcs[1] = IPP_FN_DESFIRE; c->funcs[2] = IPP_FN_ULTRALIGHT;
    c->nfuncs = 3;
}

/* ---- output ---------------------------------------------------------------------------- */
static void emitf(pm3_nfc *z, const char *event, const char *fmt, ...)
{
    char body[400], line[512];
    body[0] = 0;
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(body, sizeof body, fmt, ap);
        va_end(ap);
    }
    snprintf(line, sizeof line, "{\"t\":%.3f,\"event\":\"%s\"%s%s}", z->now - z->t0, event, body[0] ? "," : "", body);
    if (z->emit) z->emit(z->ctx, line);
}

static const char *type_label(uint8_t type, char *buf, size_t cap)
{
    const char *n = ipp_type_name(type);
    if (n) return n;
    snprintf(buf, cap, "unknown");
    return buf;
}

/* ---- transmit ---------------------------------------------------------------------------- */
static int tx_allowed(uint8_t type)
{
    return type == IPP_VERSION || type == IPP_STATUS || type == IPP_CARD_RELEASE ||
           type == IPP_ABORT_HANDLING || type == IPP_FUNC_ENABLE;
}

static void pend_add(pm3_nfc *z, uint8_t type, uint8_t seq, uint8_t arg)
{
    if (type != IPP_VERSION && type != IPP_STATUS && type != IPP_FUNC_ENABLE) return;
    unsigned i = z->pend_i++ % PM3_NFC_PEND;
    z->pend[i].type = type; z->pend[i].seq = seq; z->pend[i].arg = arg;
    z->pend[i].at = z->now; z->pend[i].open = 1;
}

/* 0: answers nothing we asked, 1: in time, 2: late.  Each request is answered once. */
static int pend_take(pm3_nfc *z, uint8_t type, uint8_t reply_to, uint8_t *arg)
{
    for (int i = 0; i < PM3_NFC_PEND; i++) {
        if (z->pend[i].type != type || z->pend[i].seq != reply_to) continue;   /* type 0 = empty slot */
        if (!z->pend[i].open && type != IPP_STATUS) continue;
        int late = !z->pend[i].open || z->now - z->pend[i].at > z->cfg.status_deadline;
        if (arg) *arg = z->pend[i].arg;
        memset(&z->pend[i], 0, sizeof z->pend[i]);
        return late ? 2 : 1;
    }
    return 0;
}

int pm3_nfc_send(pm3_nfc *z, uint8_t type, uint8_t reply_to, const uint8_t *pay, size_t n)
{
    uint8_t b[IPP_HDR + 16 + 4];
    if (!tx_allowed(type) || n > 16) { z->tx_refused++; return 0; }
    if (++z->seq == 0) z->seq = 1;
    size_t len = ipp_build(b, sizeof b, z->seq, reply_to, type, pay, n);
    if (!len) { z->tx_refused++; return 0; }
    pend_add(z, type, z->seq, n > 1 ? pay[1] : 0);
    z->tx_frames++;
    if (z->send) z->send(z->ctx, b, len);
    return 1;
}

/* ---- setup ------------------------------------------------------------------------------- */
static void stage_set(pm3_nfc *z, int stage)
{
    if (z->stage == stage) return;
    z->stage = stage;
    if (stage != PM3_ST_READY) emitf(z, "setup", "\"stage\":\"%s\"", STAGE_NAME[stage]);
}

static void setup_begin(pm3_nfc *z, int after_startup)
{
    z->setups++;
    z->after_startup = after_startup;
    z->fn_idx = 0; z->fn_tries = 0;
    z->setup_started = z->now;
    z->next_tx = z->now + LISTEN_S;
    z->stage = -1;
    stage_set(z, PM3_ST_LISTEN);
}

static void setup_ready(pm3_nfc *z)
{
    stage_set(z, PM3_ST_READY);
    z->ready_at = z->now;
    if (z->after_startup) z->resync_until = z->now + z->cfg.resync;
    emitf(z, "ready", "\"elapsed\":%.2f,\"setups\":%u,\"resync\":%s", z->now - z->setup_started, z->setups,
          z->after_startup ? "true" : "false");
}

/* ---- presence ---------------------------------------------------------------------------- */
static void end_presence(pm3_nfc *z, const char *why)
{
    z->present = 0; z->removal_pending = 0;
    emitf(z, "absent", "\"kind\":\"%s\",\"reason\":\"%s\",\"present_s\":%.2f", KIND_NAME[z->present_kind], why,
          z->last_seen - z->present_since);
}

static void presence_update(pm3_nfc *z)
{
    if (!z->present) return;
    if (z->removal_pending && z->now - z->removed_at >= z->cfg.remove_confirm) { z->absent_removal++; end_presence(z, "removal"); }
    else if (z->now - z->last_seen > z->cfg.presence_timeout) { z->absent_timeout++; end_presence(z, "timeout"); }
}

static void card_report(pm3_nfc *z, const ipp_frame *f)
{
    char tb[16], since[24];
    presence_update(z);
    if (z->present) {
        z->held++;
        z->last_seen = z->now; z->removal_pending = 0;
    } else {
        const char *oc;
        double gap = z->now - z->last_accept;
        if (z->last_accept < -1e8) snprintf(since, sizeof since, "null");
        else snprintf(since, sizeof since, "%.2f", gap);
        z->present = 1; z->present_kind = PM3_KIND_CARD; z->released = 0; z->present_since = z->now;
        if (z->now < z->resync_until) { oc = "resync"; z->resynced++; }
        else if (gap >= z->cfg.accept_gap) { oc = "accepted"; z->accepted[PM3_KIND_CARD]++; z->last_accept = z->now; }
        else { oc = "cooldown"; z->cooldown++; }
        if (z->cfg.rearm_abort && f->type != IPP_ULTRALIGHT && strcmp(oc, "resync")) z->abort_armed = 1;
        emitf(z, "presence", "\"kind\":\"card\",\"frame\":\"%s\",\"type\":%u,\"outcome\":\"%s\",\"since_last_s\":%s",
              type_label(f->type, tb, sizeof tb), f->type, oc, since);
        z->last_seen = z->now; z->removal_pending = 0;
    }
    /* One release per presence, answering the report, as the stock flow does.  Ultralight
     * field reports are not a hand-off and are never released. */
    if (z->cfg.release_cards && !z->released && f->type != IPP_ULTRALIGHT)
        z->released = pm3_nfc_send(z, IPP_CARD_RELEASE, f->seq, NULL, 0);
}

static void phone_report(pm3_nfc *z)
{
    const char *reason = z->stage != PM3_ST_READY ? "setup" :
                         z->now < z->resync_until ? "resync" :
                         z->now < z->ready_at + z->cfg.startup_grace ? "startup-grace" :
                         z->now - z->abort_last < ABORT_GRACE ? "abort-grace" : NULL;
    double gap = z->now - z->phone_last;
    if (!reason) {
        z->phone_last = z->now;
        reason = gap < PHONE_QUIET ? "burst" :
                 z->now - z->last_seen < PHONE_QUIET ? "recent-card" :
                 z->now - z->last_accept < PHONE_QUIET ? "cooldown" : NULL;
    }
    if (reason) {
        z->phone_rejected++;
        if (z->cfg.frames || strcmp(reason, "burst"))
            emitf(z, "phone_ignored", "\"reason\":\"%s\"", reason);
        return;
    }
    presence_update(z);
    z->accepted[PM3_KIND_PHONE]++;
    z->present = 1; z->present_kind = PM3_KIND_PHONE; z->released = 1; z->removal_pending = 0;
    z->present_since = z->last_seen = z->last_accept = z->now;
    z->abort_armed = 0;
    if (gap > 1e8) emitf(z, "presence", "\"kind\":\"phone_or_payment\",\"frame\":\"emv-status\",\"type\":209,\"outcome\":\"accepted\",\"quiet_s\":null");
    else emitf(z, "presence", "\"kind\":\"phone_or_payment\",\"frame\":\"emv-status\",\"type\":209,\"outcome\":\"accepted\",\"quiet_s\":%.2f", gap);
}

/* ---- receive ----------------------------------------------------------------------------- */
static void on_frame(void *ctx, const ipp_frame *f)
{
    pm3_nfc *z = ctx;
    const uint8_t *p = f->payload;
    char tb[16];
    z->last_valid = z->now;
    if (z->link_silent) { z->link_silent = 0; emitf(z, "link", "\"state\":\"alive\""); }
    if (z->cfg.frames)
        emitf(z, "frame", "\"type\":%u,\"name\":\"%s\",\"len\":%u,\"seq\":%u,\"reply_to\":%u",
              f->type, type_label(f->type, tb, sizeof tb), f->len, f->seq, f->reply_to);

    switch (f->type) {
    case IPP_HEARTBEAT: z->heartbeats++; return;
    case IPP_VERSION_REPLY:
        if (pend_take(z, IPP_VERSION, f->reply_to, NULL)) emitf(z, "version_reply", "\"len\":%u", f->len);
        return;
    case IPP_STATUS_REPLY: {
        if (f->len != STATUS_LEN) { z->status_malformed++; emitf(z, "status_malformed", "\"len\":%u", f->len); return; }
        int r = pend_take(z, IPP_STATUS, f->reply_to, NULL);
        if (!r) { z->uncorrelated++; return; }
        if (z->status_miss_run >= 3) emitf(z, "status_restored", "\"missed\":%u", z->status_miss_run);
        z->status_miss_run = 0;
        z->status_ok++;
        if (r == 2) z->status_late++;
        z->last_status_ok = z->now;
        unsigned code = (unsigned)p[0] << 8 | p[1];
        if (!z->have_status_code || code != z->status_code)
            emitf(z, "status", "\"code\":\"%04x\",\"late\":%s", code, r == 2 ? "true" : "false");
        z->status_code = code; z->have_status_code = 1;
        return;
    }
    case IPP_STARTUP:
        z->startups++;
        emitf(z, "module_startup", "\"count\":%u", z->startups);
        if (z->present) end_presence(z, "module-restart");
        z->abort_armed = 0;
        setup_begin(z, 1);
        return;
    case IPP_FUNC_ENABLE_REPLY: {
        uint8_t fn = 0;
        if (z->stage != PM3_ST_ENABLE || f->len < 4 || p[0] != 0 || p[1] != z->cfg.funcs[z->fn_idx]) { z->uncorrelated++; return; }
        /* A retry's answer counts; an unasked one, or one for another function, does not. */
        if (!pend_take(z, IPP_FUNC_ENABLE, f->reply_to, &fn) || fn != p[1]) { z->uncorrelated++; return; }
        if (p[2] == 0 && p[3] == 0) {
            emitf(z, "function_enabled", "\"function\":%u,\"tries\":%d", p[1], z->fn_tries);
            z->fn_idx++; z->fn_tries = 0; z->next_tx = z->now + 0.15;
            if (z->fn_idx >= z->cfg.nfuncs) setup_ready(z);
        } else {
            z->fn_rejects++;
            emitf(z, "function_rejected", "\"function\":%u,\"status\":\"%02x%02x\"", p[1], p[2], p[3]);
            z->next_tx = z->now + 5.0;
        }
        return;
    }
    case IPP_READER_MESSAGE: {
        int text = f->len > 1;
        for (unsigned i = 1; i < f->len && p[i]; i++)
            if (!(p[i] >= 0x20 && p[i] < 0x7F) && p[i] != '\n' && p[i] != '\r' && p[i] != '\t') text = 0;
        z->reader_msgs++;
        emitf(z, "reader_message", "\"level\":%d,\"len\":%u,\"format\":\"%s\",\"text\":\"omitted\"",
              f->len ? p[0] : -1, f->len, text ? "text" : "binary");
        if (f->len && p[0] >= 2 && z->stage == PM3_ST_READY && z->now - z->last_status_tx >= 1.0) z->last_status_tx = -1e9;
        return;
    }
    case IPP_ISO_RELEASED: z->release_acks++; return;
    case IPP_EMV_STATUS:
        if (f->reply_to != 0 || f->len != 63) { z->phone_rejected++; return; }
        if (z->cfg.phone_presence) phone_report(z);
        return;
    default: break;
    }
    int arrival = f->type == IPP_ISO_READ || f->type == IPP_DESFIRE_READ || f->type == IPP_PICC_READ ||
                  (f->type == IPP_ULTRALIGHT && f->reply_to == 0 && f->len >= 1 && p[0] == 0);
    int removal = f->type == IPP_DESFIRE_REMOVED ||
                  (f->type == IPP_ULTRALIGHT && f->reply_to == 0 && f->len >= 1 && p[0] == 1);
    if (arrival) card_report(z, f);
    else if (removal && z->present && z->present_kind == PM3_KIND_CARD) {
        z->removal_pending = 1; z->removed_at = z->now;
    }
}

/* ---- public ------------------------------------------------------------------------------ */
void pm3_nfc_init(pm3_nfc *z, const pm3_nfc_cfg *cfg, pm3_nfc_send_fn send, pm3_nfc_emit_fn emit,
                  void *ctx, double now)
{
    memset(z, 0, sizeof *z);
    if (cfg) z->cfg = *cfg; else pm3_nfc_cfg_defaults(&z->cfg);
    if (z->cfg.nfuncs < 1 || z->cfg.nfuncs > PM3_NFC_MAX_FUNCS) pm3_nfc_cfg_defaults(&z->cfg);
    z->send = send; z->emit = emit; z->ctx = ctx;
    z->t0 = z->now = now;
    ipp_rx_init(&z->rx);
    z->last_accept = z->phone_last = z->abort_last = -1e9;
    z->last_valid = now;
    z->last_status_ok = -1e9;
    z->health_next = now + z->cfg.health_every;
    setup_begin(z, 0);
}

void pm3_nfc_bytes(pm3_nfc *z, const uint8_t *b, size_t n, double now)
{
    unsigned hdr = z->rx.bad_hdr, crc = z->rx.bad_crc, big = z->rx.oversize;
    z->now = now;
    ipp_rx_feed(&z->rx, b, n, on_frame, z);
    if (z->rx.bad_hdr != hdr || z->rx.bad_crc != crc || z->rx.oversize != big)
        emitf(z, "parse_error", "\"bad_header\":%u,\"bad_crc\":%u,\"oversize\":%u", z->rx.bad_hdr, z->rx.bad_crc, z->rx.oversize);
}

static void status_expire(pm3_nfc *z)
{
    for (int i = 0; i < PM3_NFC_PEND; i++) {
        if (!z->pend[i].open || z->pend[i].type != IPP_STATUS || z->now - z->pend[i].at <= z->cfg.status_deadline) continue;
        z->pend[i].open = 0;                         /* a late reply still counts, as late */
        z->status_missing++;
        if (++z->status_miss_run == 3)
            emitf(z, "status_missing", "\"consecutive\":%u,\"heartbeats\":%u", z->status_miss_run, z->heartbeats);
    }
}

void pm3_nfc_tick(pm3_nfc *z, double now)
{
    z->now = now;
    switch (z->stage) {
    case PM3_ST_LISTEN:
        if (now < z->next_tx) break;
        stage_set(z, PM3_ST_ABORT);
        /* fall through */
    case PM3_ST_ABORT:
        if (now >= z->next_tx) {
            pm3_nfc_send(z, IPP_ABORT_HANDLING, 0, NULL, 0);
            z->abort_last = now;
            stage_set(z, PM3_ST_RELEASE); z->next_tx = now + STEP_GAP_S;
        }
        break;
    case PM3_ST_RELEASE:
        if (now >= z->next_tx) {
            pm3_nfc_send(z, IPP_CARD_RELEASE, 0, NULL, 0);
            stage_set(z, PM3_ST_PROBE); z->next_tx = now + STEP_GAP_S;
        }
        break;
    case PM3_ST_PROBE:
        if (now >= z->next_tx) {
            pm3_nfc_send(z, IPP_VERSION, 0, NULL, 0);
            pm3_nfc_send(z, IPP_STATUS, 0, NULL, 0);
            z->last_status_tx = now;
            stage_set(z, PM3_ST_ENABLE); z->next_tx = now + 0.3;
        }
        break;
    case PM3_ST_ENABLE:
        if (now >= z->next_tx) {
            uint8_t pay[4] = { 0x00, z->cfg.funcs[z->fn_idx], 0x01, 0x01 };
            pm3_nfc_send(z, IPP_FUNC_ENABLE, 0, pay, 4);
            z->fn_tries++;
            z->next_tx = now + (z->fn_tries < 10 ? ENABLE_RETRY : ENABLE_SLOW);
        }
        break;
    default: break;
    }
    if (z->stage > PM3_ST_PROBE && now - z->last_status_tx >= z->cfg.status_every) {
        pm3_nfc_send(z, IPP_STATUS, 0, NULL, 0);
        z->last_status_tx = now;
    }
    status_expire(z);
    presence_update(z);
    if (z->abort_armed && now - z->last_seen > z->cfg.presence_timeout) {
        z->abort_armed = 0;
        if (z->stage == PM3_ST_READY && now - z->abort_last >= ABORT_MIN) {
            pm3_nfc_send(z, IPP_ABORT_HANDLING, 0, NULL, 0);
            z->abort_last = now; z->aborts++;
            emitf(z, "rearm_abort", "\"count\":%u", z->aborts);
        }
    }
    if (!z->link_silent && now - z->last_valid > z->cfg.silent_after) {
        z->link_silent = 1;
        emitf(z, "link", "\"state\":\"silent\",\"since_s\":%.1f", now - z->last_valid);
    }
    if (z->cfg.health_every > 0 && now >= z->health_next) {
        z->health_next = now + z->cfg.health_every;
        pm3_nfc_health(z, now);
    }
}

void pm3_nfc_health(pm3_nfc *z, double now)
{
    z->now = now;
    const char *cmd = z->status_ok && now - z->last_status_ok < 15.0 ? "verified" : z->status_ok ? "stale" : "unknown";
    emitf(z, "health",
          "\"link\":\"%s\",\"configured\":%s,\"stage\":\"%s\",\"cmd\":\"%s\",\"card_accepted\":%u,\"phone_accepted\":%u,"
          "\"held\":%u,\"cooldown\":%u,\"resync\":%u,\"phone_ignored\":%u,\"absent_removal\":%u,\"absent_timeout\":%u,"
          "\"heartbeats\":%u,\"status_ok\":%u,\"status_late\":%u,\"status_missing\":%u,\"startups\":%u,"
          "\"reader_messages\":%u,\"bad_header\":%u,\"bad_crc\":%u",
          z->link_silent ? "silent" : "alive", z->stage == PM3_ST_READY ? "true" : "false", STAGE_NAME[z->stage], cmd,
          z->accepted[PM3_KIND_CARD], z->accepted[PM3_KIND_PHONE], z->held, z->cooldown, z->resynced, z->phone_rejected,
          z->absent_removal, z->absent_timeout, z->heartbeats, z->status_ok, z->status_late, z->status_missing,
          z->startups, z->reader_msgs, z->rx.bad_hdr, z->rx.bad_crc);
}

void pm3_nfc_release(pm3_nfc *z, int step, double now)
{
    z->now = now;
    if (step == 0) pm3_nfc_send(z, IPP_ABORT_HANDLING, 0, NULL, 0);
    else pm3_nfc_send(z, IPP_CARD_RELEASE, 0, NULL, 0);
}
