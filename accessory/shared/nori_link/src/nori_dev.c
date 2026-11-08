/* Accessory-side link core; see nori_dev.h. */
#include "nori_dev.h"

#include <stdio.h>
#include <string.h>

const char *const ND_WIFI_NAME[] = { "off", "nocfg", "joining", "up" };
#if ND_TAP_AXIS || ND_I2CSCAN || ND_GAME || ND_PRESENCE || ND_EXT
void ND_ABI_CHECK(void) {}
#endif

#define HELLO_ANNOUNCE_MS 3000     /* until a host talks to us, say HELLO this often (and nothing else) */
/* r4: 12/s, burst 6 (was 5/s, burst 3): a game needs ~6 taps/s, and a button press is two
 * events (down+up).  Still a hard bound on what a rattling sensor can push. */
#define EV_PER_SEC 12
#define EV_BURST 6

void nd_cfg_defaults(nd_cfg *c)
{
    ni_tap_cfg t;
    ni_tap_defaults(&t);
    c->tap_ths = 0;                /* 0 = firmware's per-sensor default */
    c->dtap_wait_ms = t.dtap_wait_ms;
    c->refractory_ms = t.refractory_ms;
    c->wifi_on = 0;
#if ND_TAP_AXIS
    c->tap_axis = ND_AXIS_DEFAULT;
    c->axis_pct = ND_AXR_DEFAULT;
#endif
#if ND_GAME
    c->g_ths = ND_GTHS_DEFAULT;
    c->g_refr_ms = ND_GREFR_DEFAULT;
    c->g_quiet_ms = ND_GQ_DEFAULT;
    c->g_axis = ND_GAXIS_DEFAULT;
#endif
}

#if ND_TAP_AXIS
void nd_axis_load(nd_cfg *c, const nd_kv *kv)
{
    uint8_t v;
    if (kv->get_u8(kv->ctx, ND_K_AXIS, &v)) c->tap_axis = v <= 3 ? v : ND_AXIS_DEFAULT;
    if (kv->get_u8(kv->ctx, ND_K_AXR, &v)) c->axis_pct = v >= ND_AXR_MIN && v <= ND_AXR_MAX ? v : ND_AXR_DEFAULT;
}

void nd_axis_save(const nd_cfg *old, const nd_cfg *c, const nd_kv *kv)
{
    if (c->tap_axis != old->tap_axis) kv->put_u8(kv->ctx, ND_K_AXIS, c->tap_axis);
    if (c->axis_pct != old->axis_pct) kv->put_u8(kv->ctx, ND_K_AXR, c->axis_pct);
}
#endif

#if ND_GAME
void nd_game_load(nd_cfg *c, const nd_kv *kv, const nd_kv16 *kv16)
{
    uint8_t v;
    uint16_t w;
    if (kv->get_u8(kv->ctx, ND_K_GTHS, &v)) c->g_ths = v >= 1 ? v : ND_GTHS_DEFAULT;
    if (kv16->get_u16(kv16->ctx, ND_K_GREFR, &w)) c->g_refr_ms = w >= ND_GREFR_MIN && w <= ND_GREFR_MAX ? w : ND_GREFR_DEFAULT;
    if (kv->get_u8(kv->ctx, ND_K_GQ, &v)) c->g_quiet_ms = v >= ND_GQ_MIN && v <= ND_GQ_MAX ? v : ND_GQ_DEFAULT;
    if (kv->get_u8(kv->ctx, ND_K_GAXIS, &v)) c->g_axis = v <= 3 ? v : ND_GAXIS_DEFAULT;
}

void nd_game_save(const nd_cfg *old, const nd_cfg *c, const nd_kv *kv, const nd_kv16 *kv16)
{
    if (c->g_ths != old->g_ths) kv->put_u8(kv->ctx, ND_K_GTHS, c->g_ths);
    if (c->g_refr_ms != old->g_refr_ms) kv16->put_u16(kv16->ctx, ND_K_GREFR, c->g_refr_ms);
    if (c->g_quiet_ms != old->g_quiet_ms) kv->put_u8(kv->ctx, ND_K_GQ, c->g_quiet_ms);
    if (c->g_axis != old->g_axis) kv->put_u8(kv->ctx, ND_K_GAXIS, c->g_axis);
}

int nd_game(const nd_dev *d) { return d->game; }

static uint16_t ms_to_samples(uint32_t ms, uint16_t odr, int round_up)
{
    uint32_t n = (ms * odr + (round_up ? 999u : 0u)) / 1000u;
    return (uint16_t)(n > 65535u ? 65535u : n);
}

void nd_tuning(const nd_cfg *c, int game, uint16_t odr_hz, ni_motion_cfg *m, ni_tap_cfg *t)
{
    if (!odr_hz) odr_hz = 400;
    ni_motion_defaults(m, odr_hz);
    ni_tap_game_defaults(t);                     /* r4/r5 motion builds: shake guard tuned for game tapping */
    uint8_t ths = game ? c->g_ths : c->tap_ths;
    if (ths) {                                   /* N * 32 mg (32 .. 8160); 0 (normal only) = the 1000 mg default */
        m->ths_mg = (uint16_t)(ths * 32u);
        m->rearm_mg = (uint16_t)(m->ths_mg * 2u / 5u);
    }
    if (!game) {                                 /* exactly r5.1's motion_tuning() and loop() tap settings */
        m->axis = c->tap_axis;
        m->axis_pct = c->axis_pct;
        m->reject_hold_samples = ms_to_samples(c->refractory_ms, odr_hz, 0);
        t->dtap_wait_ms = c->dtap_wait_ms;
        t->refractory_ms = c->refractory_ms;
        return;
    }
    /* game: nothing waits.  Singles at once, the short refractory, the short quiet re-arm, no
     * shake mute (a fast player must never be muted; the event bucket still bounds a rattle). */
    uint16_t q = ms_to_samples(c->g_quiet_ms, odr_hz, 1);
    m->quiet_samples = q ? q : 1;
    uint16_t w = ms_to_samples(ND_GAME_WARMUP_MS, odr_hz, 1);
    m->warmup_samples = w ? w : 1;
    m->freeze_samples = ms_to_samples(ND_GAME_FREEZE_MS, odr_hz, 1);   /* the knock does not drag the baseline */
    uint16_t rw = ms_to_samples(ND_GAME_RETRIG_MS, odr_hz, 1);
    m->retrig_window = (uint8_t)(rw < 1 ? 1 : rw > NI_RETRIG_MAX ? NI_RETRIG_MAX : rw);
    m->retrig_min = ms_to_samples(c->g_refr_ms, odr_hz, 0);   /* never inside the knock's own refractory */
    m->axis = c->g_axis;                         /* 0: hit at the crossing, no peak-window latency */
    m->axis_pct = c->axis_pct;
    m->reject_hold_samples = ms_to_samples(c->g_refr_ms, odr_hz, 0);
    t->dtap_wait_ms = 0;
    t->refractory_ms = c->g_refr_ms;
    t->shake_hits = 0;
}

void nd_tap_reprofile(ni_tap *t)
{
    t->muted = 0;
    t->nhits = 0;
    t->hpos = 0;
}

int nd_game_diag(const nd_dev *d, char *out, size_t cap)
{
    const nd_cfg *c = &d->cfg;
    return snprintf(out, cap, "mode=%s gtap=t%u/r%u/q%u/a%u", d->game ? "game" : "normal", c->g_ths, c->g_refr_ms,
                    c->g_quiet_ms, c->g_axis);
}

static void game_end(nd_dev *d)
{
    d->game = 0;
}
#endif

static void wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

/* Send one frame if the UART can take all of it now. Returns 0 if sent. */
static int send(nd_dev *d, const char *type, const char *args)
{
    char line[NL_LINE_MAX + 2];
    int n = nl_format(line, sizeof line, d->seq + 1, type, args);
    if (n < 0) return -2;
    if (d->hal->uart_room(d->hal->ctx) < (size_t)n) return -1;
    d->seq++;
    d->hal->uart_write(d->hal->ctx, line, (size_t)n);
    return 0;
}

static void set_reply(nd_dev *d, const char *type, const char *args)
{
    snprintf(d->reply, sizeof d->reply, "%s%s%s", type, args && *args ? " " : "", args ? args : "");
    d->reply_len = (int)strlen(d->reply);
}

static int wifi_state(nd_dev *d, char *ip, size_t ipcap, int *rssi)
{
    if (ip && ipcap) ip[0] = 0;
    if (rssi) *rssi = 0;
    if (!d->hal->wifi_status) return ND_WIFI_OFF;
    return d->hal->wifi_status(d->hal->ctx, ip, ipcap, rssi);
}

void nd_init(nd_dev *d, const nd_hal *hal, uint32_t boot_id, const char *fw, const char *sensor,
             int buttons, const nd_cfg *cfg, uint32_t now)
{
    memset(d, 0, sizeof *d);
    d->hal = hal;
    if (cfg) d->cfg = *cfg; else nd_cfg_defaults(&d->cfg);
    d->boot_id = boot_id ? boot_id : 1;
    snprintf(d->fw, sizeof d->fw, "%s", fw);
    snprintf(d->sensor, sizeof d->sensor, "%s", sensor);
    d->buttons = buttons;
    nl_rx_init(&d->rx);
    d->next_hello = now;
    d->next_hb = now;
#if ND_PRESENCE
    d->pres_sent = -1;
#endif
#if ND_GAME
    d->game_ttl_ms = ND_GAME_TTL_MS;   /* game mode itself is off: never persisted, a reboot ends it */
#endif
    if (hal->wifi_power) hal->wifi_power(hal->ctx, d->cfg.wifi_on);
}

/* ---- tunnel ---- */
static void tun_close(nd_dev *d, const char *why, int tell_host)
{
    if (d->tun == NT_IDLE) return;
    if (d->hal->tcp_close) d->hal->tcp_close(d->hal->ctx);
    if (tell_host) {
        d->close_pending = 1;
        d->close_id = d->tun_id;
        snprintf(d->close_why, sizeof d->close_why, "%s", why);
    }
    d->tun = NT_IDLE;
    d->tx_inflight = d->rx_unacked = 0;
    d->tx_off = d->rx_off = 0;
    d->rxq_head = d->rxq_n = 0;
}

static void rxq_push(nd_dev *d, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) d->rxq[(d->rxq_head + d->rxq_n++) % NL_TUN_WINDOW] = b[i];
}

static int frame_id(const nl_frame *f, uint32_t *id) { return nl_get_u32(f, "id", id); }

static void on_tunnel_frame(nd_dev *d, const nl_frame *f, uint32_t now)
{
    uint32_t id;
    if (frame_id(f, &id) != 0 || d->tun == NT_IDLE || id != d->tun_id) return;   /* stale or foreign */
    if (!strcmp(f->type, "TOK")) {
        if (d->tun == NT_OPENING) { d->tun = NT_OPEN; d->tun_activity = now; d->last_ack = now; }
    } else if (!strcmp(f->type, "TDATA")) {
        uint8_t buf[NL_TUN_CHUNK + 3];
        const char *s = nl_get(f, "d");
        uint32_t off;
        int n = s ? nl_b64_decode(s, buf, sizeof buf) : -1;
        if (d->tun != NT_OPEN || n <= 0 || nl_get_u32(f, "o", &off) != 0) { tun_close(d, "proto", 1); return; }
        if (off != d->rx_off) { tun_close(d, "gap", 1); return; }      /* a line was lost: never corrupt the stream */
        d->rx_off += (uint32_t)n;
        if (d->rxq_n + (uint32_t)n + d->rx_unacked > NL_TUN_WINDOW) { tun_close(d, "overrun", 1); return; }
        rxq_push(d, buf, (size_t)n);
        d->tun_activity = now;
    } else if (!strcmp(f->type, "TACK")) {
        uint32_t n;
        if (nl_get_u32(f, "n", &n) == 0) d->tx_inflight -= n < d->tx_inflight ? n : d->tx_inflight;
    } else if (!strcmp(f->type, "TCLOSE")) {
        tun_close(d, "host", 0);
    }
}

/* ---- commands ---- */
static void cmd_wifiset(nd_dev *d, const nl_frame *f)
{
    char ssid[33], psk[65];
    const char *s = nl_get(f, "s"), *p = nl_get(f, "p");
    int ns = s ? nl_b64_decode(s, (uint8_t *)ssid, 32) : -1;
    int np = p ? nl_b64_decode(p, (uint8_t *)psk, 64) : -1;
    const char *err = NULL;
    if (!d->hal->wifi_set) err = "nowifi";
    else if (ns < 1 || np < 0) err = "format";
    else if (np != 0 && (np < 8 || np > 63)) err = "psklen";
    else {
        ssid[ns] = 0; psk[np] = 0;
        if (memchr(ssid, 0, (size_t)ns) || memchr(psk, 0, (size_t)np)) err = "format";
        else {
            int r = d->hal->wifi_set(d->hal->ctx, ssid, psk);
            if (r) err = r == -2 ? "full" : "store";
        }
    }
    wipe(ssid, sizeof ssid);
    wipe(psk, sizeof psk);
    wipe(d->rx.frame.store, sizeof d->rx.frame.store);   /* wt32-r5.1: as OTAKEY; f and the raw line live here, unused after this */
    wipe(d->rx.buf, sizeof d->rx.buf);
    if (err) { char a[24]; snprintf(a, sizeof a, "WIFISET %s", err); set_reply(d, "ERR", a); }
    else set_reply(d, "OK", "WIFISET");
}

/* OTAKEY h=<32 hex>: the ArduinoOTA password hash.  It is the effective OTA
 * secret, so it is wiped from every buffer this core owns and never echoed. */
static void cmd_otakey(nd_dev *d, const nl_frame *f)
{
    static const char empty_md5[] = "d41d8cd98f00b204e9800998ecf8427e";   /* MD5("") */
    char h[33];
    const char *s = nl_get(f, "h");
    const char *err = NULL;
    size_t i = 0;
    if (!d->hal->ota_key_set) err = "noota";
    else if (f->argc != 1 || !s || strlen(s) != 32) err = "format";
    else {
        for (; i < 32; i++) {
            char c = s[i];
            if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) break;
            h[i] = c;
        }
        h[32] = 0;
        if (i != 32) err = "format";
        else if (!strcmp(h, empty_md5)) err = "weak";
        else if (d->hal->ota_key_set(d->hal->ctx, h) != 0) err = "store";
    }
    wipe(h, sizeof h);
    wipe(d->rx.frame.store, sizeof d->rx.frame.store);   /* f and the raw line live here; unused after this */
    wipe(d->rx.buf, sizeof d->rx.buf);
    if (err) { char a[24]; snprintf(a, sizeof a, "OTAKEY %s", err); set_reply(d, "ERR", a); }
    else set_reply(d, "OK", "OTAKEY");
}

static void cmd_cfg(nd_dev *d, const nl_frame *f)
{
    nd_cfg c = d->cfg;
    uint32_t v;
    int bad = 0;
    for (int i = 0; i < f->argc; i++) {
        const char *a = f->argv[i];
        const char *eq = strchr(a, '=');
        if (!eq || nl_parse_u32(eq + 1, &v) != 0) { bad = 1; break; }
        size_t kl = (size_t)(eq - a);
        if (kl == 3 && !strncmp(a, "ths", 3) && v >= 1 && v <= 255) c.tap_ths = (uint8_t)v;
        else if (kl == 4 && !strncmp(a, "wait", 4) && v <= 1000) c.dtap_wait_ms = (uint16_t)v;
        else if (kl == 4 && !strncmp(a, "refr", 4) && v >= 40 && v <= 5000) c.refractory_ms = (uint16_t)v;   /* r4: floor 40 (was 100) for game taps */
#if ND_TAP_AXIS
        else if (kl == 4 && !strncmp(a, "axis", 4) && v <= 3) c.tap_axis = (uint8_t)v;
        else if (kl == 3 && !strncmp(a, "axr", 3) && v >= ND_AXR_MIN && v <= ND_AXR_MAX) c.axis_pct = (uint8_t)v;
#endif
#if ND_GAME
        else if (kl == 4 && !strncmp(a, "gths", 4) && v >= 1 && v <= 255) c.g_ths = (uint8_t)v;
        else if (kl == 5 && !strncmp(a, "grefr", 5) && v >= ND_GREFR_MIN && v <= ND_GREFR_MAX) c.g_refr_ms = (uint16_t)v;
        else if (kl == 2 && !strncmp(a, "gq", 2) && v >= ND_GQ_MIN && v <= ND_GQ_MAX) c.g_quiet_ms = (uint8_t)v;
        else if (kl == 5 && !strncmp(a, "gaxis", 5) && v <= 3) c.g_axis = (uint8_t)v;
#endif
        else { bad = 1; break; }
    }
    if (bad) { set_reply(d, "ERR", "CFG range"); return; }
    d->cfg = c;
    d->cfg_changed = 1;
    if (d->hal->cfg_save) d->hal->cfg_save(d->hal->ctx, &d->cfg);
#if ND_GAME
    char a[128];                       /* 10 args: NL_ARGS_MAX, so no further key fits in this reply */
    snprintf(a, sizeof a, "CFG ths=%u wait=%u refr=%u axis=%u axr=%u gths=%u grefr=%u gq=%u gaxis=%u", c.tap_ths,
             c.dtap_wait_ms, c.refractory_ms, c.tap_axis, c.axis_pct, c.g_ths, c.g_refr_ms, c.g_quiet_ms, c.g_axis);
#elif ND_TAP_AXIS
    char a[96];
    snprintf(a, sizeof a, "CFG ths=%u wait=%u refr=%u axis=%u axr=%u", c.tap_ths, c.dtap_wait_ms, c.refractory_ms,
             c.tap_axis, c.axis_pct);
#else
    char a[64];
    snprintf(a, sizeof a, "CFG ths=%u wait=%u refr=%u", c.tap_ths, c.dtap_wait_ms, c.refractory_ms);
#endif
    set_reply(d, "OK", a);
}

#if ND_I2CSCAN
/* r5.1 IICSCAN [hz=100|400]: the firmware's wiring diagnostic (see nori_i2cscan.h). */
static void cmd_i2cscan(nd_dev *d, const nl_frame *f)
{
    uint32_t khz = 0;
    const char *hz = nl_get(f, "hz");
    if (f->argc > 1 || (f->argc == 1 && (!hz || nl_get_u32(f, "hz", &khz) != 0 || (khz != 100 && khz != 400)))) {
        set_reply(d, "ERR", "IICSCAN range");
        return;
    }
    if (!d->hal->i2c_scan) { set_reply(d, "ERR", "IICSCAN nobus"); return; }
    char a[NL_LINE_MAX - 40];
    int n = snprintf(a, sizeof a, "IICSCAN ");
    a[n] = 0;
    d->hal->i2c_scan(d->hal->ctx, (unsigned)khz, a + n, sizeof a - (size_t)n);
    a[sizeof a - 1] = 0;
    set_reply(d, "OK", a);
}
#endif

#if ND_GAME
/* r5.2 MODE g=0|1: exactly one arg.  g=1 (re)starts the TTL; g=0 ends game mode at once. */
static void cmd_mode(nd_dev *d, const nl_frame *f, uint32_t now)
{
    uint32_t g;
    if (f->argc != 1 || nl_get_u32(f, "g", &g) != 0 || g > 1) { set_reply(d, "ERR", "MODE range"); return; }
    if (g) {
        if (!d->game) d->game_on_n++;
        d->game = 1;
        d->game_until = now + d->game_ttl_ms;
    } else game_end(d);
    set_reply(d, "OK", g ? "MODE g=1" : "MODE g=0");
}
#endif

static void on_host_frame(void *ctx, const nl_frame *f, const char *raw, size_t rawlen)
{
    (void)raw; (void)rawlen;
    nd_dev *d = (nd_dev *)ctx;
    uint32_t now = d->rx_now;
    d->host_seen = now;
    if (!d->host_alive) d->host_gen++;
    d->host_alive = 1;
    const char *t = f->type;
    if (!strcmp(t, "HOST")) {
        d->host_gen++;                 /* a (re)starting host, even if the last one was never timed out */
        uint32_t proto, tun = 0;
        if (nl_get_u32(f, "proto", &proto) != 0 || proto != NL_PROTO) { set_reply(d, "ERR", "HOST proto"); return; }
        nl_get_u32(f, "tun", &tun);
        d->host_tun = tun == 1;
        tun_close(d, "host", 1);       /* a (re)starting host knows nothing of an older tunnel */
#if ND_GAME
        if (d->game) { game_end(d); d->game_lost_n++; }   /* r5.2: a new host session starts in the normal profile */
#endif
        d->hello_req = 1;              /* answer with HELLO right away */
        d->next_hb = now + NL_HB_MS;
#if ND_PRESENCE
        d->pres_sent = -1;             /* wt32-r3: a new host session gets the current presence level */
#endif
    } else if (!strcmp(t, "HHB")) {
        /* liveness only */
    } else if (!strcmp(t, "WIFISET")) {
        cmd_wifiset(d, f);
    } else if (!strcmp(t, "WIFIEN")) {
        uint32_t on;
        if (nl_get_u32(f, "on", &on) != 0 || on > 1) { set_reply(d, "ERR", "WIFIEN range"); return; }
        d->cfg.wifi_on = (uint8_t)on;
        if (d->hal->cfg_save) d->hal->cfg_save(d->hal->ctx, &d->cfg);
        if (d->hal->wifi_power) d->hal->wifi_power(d->hal->ctx, (int)on);
        set_reply(d, "OK", on ? "WIFIEN on=1" : "WIFIEN on=0");
    } else if (!strcmp(t, "WIFICLR")) {
        int r = d->hal->wifi_clear ? d->hal->wifi_clear(d->hal->ctx) : -1;
        set_reply(d, r == 0 ? "OK" : "ERR", r == 0 ? "WIFICLR" : "WIFICLR nowifi");
    } else if (!strcmp(t, "OTAKEY")) {
        cmd_otakey(d, f);
    } else if (!strcmp(t, "CFG")) {
        cmd_cfg(d, f);
#if ND_I2CSCAN
    } else if (!strcmp(t, "IICSCAN")) {
        cmd_i2cscan(d, f);
#endif
#if ND_GAME
    } else if (!strcmp(t, "MODE")) {
        cmd_mode(d, f, now);
#endif
    } else if (!strcmp(t, "DIAG")) {
#if ND_GAME || ND_DIAG_JOIN
        /* r5.2: the longest reply nl_format accepts: "$NL1 <10-digit seq> OK " + 296 + "*XXXX" = 320 */
        char a[NL_LINE_MAX - 23], extra[300] = "";
#if ND_EXT
        const char *pg = nl_get(f, "p");
        if (pg || f->argc) {               /* wt32-r3: DIAG p=<page>; a page name is 1..8 of [a-z0-9] */
            size_t pl = pg ? strlen(pg) : 0;
            int ok = f->argc == 1 && pl >= 1 && pl <= 8 && d->hal->diag_page;
            for (size_t i = 0; ok && i < pl; i++) ok = (pg[i] >= 'a' && pg[i] <= 'z') || (pg[i] >= '0' && pg[i] <= '9');
            int hn = ok ? snprintf(extra, sizeof extra, "pg=%s ", pg) : 0;
            if (!ok || d->hal->diag_page(d->hal->ctx, pg, extra + hn, sizeof extra - (size_t)hn) != 0) {
                set_reply(d, "ERR", "DIAG page");
                return;
            }
        } else
#endif
#elif ND_TAP_AXIS || ND_I2CSCAN
        char a[NL_LINE_MAX - 40], extra[256] = "";      /* r5.1: room for ax=/rej=/pk=/dom=/cs= */
#else
        char a[NL_LINE_MAX - 40], extra[200] = "";
#endif
        if (d->hal->diag) d->hal->diag(d->hal->ctx, extra, sizeof extra);
#if ND_TAP_AXIS || ND_I2CSCAN || ND_DIAG_JOIN
        { size_t el = strlen(extra); while (el && extra[el - 1] == ' ') extra[--el] = 0; }   /* never a trailing ';' */
        /* r5.1: the firmware's ~20 tokens become one ';'-joined token.  nl_parse (linkd, app)
         * refuses a frame with more than NL_ARGS_MAX args, so r4/r5 DIAG replies from real
         * hardware never reached `nori-linkd --diag`; with this they do (7 args).  It also means
         * a truncated reply can never end in a space (nl_format refuses that). */
        for (char *p = extra; *p; p++) if (*p == ' ') *p = ';';
#endif
#if ND_TAP_AXIS || ND_I2CSCAN || ND_DIAG_JOIN
        int dn = snprintf(a, sizeof a, "DIAG rx=%lu noise=%lu crc=%lu bad=%lu long=%lu",
                          (unsigned long)d->rx.frames, (unsigned long)d->rx.noise, (unsigned long)d->rx.bad_crc,
                          (unsigned long)d->rx.bad_syntax, (unsigned long)d->rx.too_long);   /* <= 89 chars */
        if (extra[0] && dn > 0 && (size_t)dn + 2 < sizeof a) {   /* cut to fit: still one valid token */
            size_t k = strlen(extra), room = sizeof a - 2 - (size_t)dn;
            a[dn++] = ' ';
            if (k > room) k = room;
            memcpy(a + dn, extra, k);
            a[(size_t)dn + k] = 0;
        }
#else
        snprintf(a, sizeof a, "DIAG rx=%lu noise=%lu crc=%lu bad=%lu long=%lu%s%s",
                 (unsigned long)d->rx.frames, (unsigned long)d->rx.noise, (unsigned long)d->rx.bad_crc,
                 (unsigned long)d->rx.bad_syntax, (unsigned long)d->rx.too_long, extra[0] ? " " : "", extra);
#endif
        set_reply(d, "OK", a);
    } else if (t[0] == 'T' && (!strcmp(t, "TOK") || !strcmp(t, "TDATA") || !strcmp(t, "TACK") || !strcmp(t, "TCLOSE"))) {
        on_tunnel_frame(d, f, now);
    }
#if ND_EXT
    else if (d->hal->ext_cmd) {            /* wt32-r3: PRESCFG, SENSOR, ... (no reply if not taken) */
        char r[NL_LINE_MAX - 23];
        r[0] = 0;
        if (d->hal->ext_cmd(d->hal->ctx, f, r, sizeof r) == 1) {
            char *sp = strchr(r, ' ');
            if (sp) { *sp = 0; set_reply(d, r, sp + 1); }
            else set_reply(d, r[0] ? r : "ERR", r[0] ? NULL : "EXT");
        }
    }
#endif
    else if (t[0] == 'T') {
        on_tunnel_frame(d, f, now);
    }
}

void nd_uart_input(nd_dev *d, const uint8_t *b, size_t n, uint32_t now)
{
    d->rx_now = now;
    nl_rx_feed(&d->rx, b, n, on_host_frame, d, 0);
}

static void ev_push(nd_dev *d, int kind, uint32_t now)
{
    d->evk[d->evn] = (uint8_t)kind;
    d->evt[d->evn] = now;
    d->evn++;
}

/* Queue one event if the host is alive, the queue has room and the rate bucket allows it. */
static int ev_queue(nd_dev *d, int kind, uint32_t now)
{
#if ND_GAME
    uint16_t per_sec = d->game ? ND_EV_PER_SEC_GAME : EV_PER_SEC;
    uint8_t burst = d->game ? ND_EV_BURST_GAME : EV_BURST;
#else
    const uint16_t per_sec = EV_PER_SEC;
    const uint8_t burst = EV_BURST;
#endif
    if (!d->host_alive || d->evn >= ND_EVQ || !ni_bucket_take(&d->bucket, now, per_sec, burst)) return 0;
    ev_push(d, kind, now);
    return 1;
}

void nd_event(nd_dev *d, int kind, uint32_t now)
{
    if (!ev_queue(d, kind, now)) d->ev_dropped++;
}

#if ND_PRESENCE
void nd_presence(nd_dev *d, int known, int level)
{
    d->pres_known = known ? 1 : 0;
    d->pres_level = level ? 1 : 0;
}

/* wt32-r3: the presence level is state, not a one-off event: whatever the current host session has not
 * been told yet is (re)queued here; a refusal (bucket, full queue) is simply tried again next poll. */
static void presence_poll(nd_dev *d, uint32_t now)
{
    if (d->host_alive && !d->pres_was_alive) d->pres_sent = -1;   /* host back after silence: resync */
    d->pres_was_alive = d->host_alive;
    if (!d->host_alive || !d->pres_known || d->pres_level == d->pres_sent) return;
    if (d->hello_req) return;          /* a host in WAIT_HELLO drops events as "early": HELLO goes out first */
    if (ev_queue(d, d->pres_level ? NL_EV_PON : NL_EV_POFF, now)) {
        d->pres_sent = d->pres_level;
        d->pres_tx++;
        d->pres_blocked = 0;
    } else if (!d->pres_blocked) {     /* count each delayed level once, not every poll */
        d->pres_blocked = 1;
        d->pres_retry++;
    }
}
#endif

void nd_button(nd_dev *d, int index, int edge, uint32_t now)
{
    static const int down[2] = { NL_EV_B1D, NL_EV_B2D }, up[2] = { NL_EV_B1U, NL_EV_B2U };
    if (index < 0 || index > 1 || !edge) return;
    nd_event(d, edge > 0 ? down[index] : up[index], now);
}

void nd_tap(nd_dev *d, int tap, uint32_t now)
{
    if (tap == NI_TAP) nd_event(d, NL_EV_TAP, now);
    else if (tap == NI_DTAP) nd_event(d, NL_EV_DTAP, now);
}

int nd_toggle(nd_dev *d, int pos, uint32_t now)
{
    if (pos != NI_TOG_FIVE && pos != NI_TOG_ONE) return 0;
    if (ev_queue(d, pos == NI_TOG_FIVE ? NL_EV_TRK5 : NL_EV_TRK1, now)) return 1;
    d->ev_dropped++;
    return 0;
}

int nd_gesture(nd_dev *d, int ges, uint32_t now)
{
    static const uint8_t kind[] = { NL_EV_NONE, NL_EV_TSH5, NL_EV_TSH1, NL_EV_TLG5, NL_EV_TLG1 };
    if (ges <= NI_GES_NONE || ges > NI_GES_LONG_ONE) return 0;
    if (!d->host_alive || d->evn >= ND_EVQ) { d->ev_dropped++; return 0; }
    ev_push(d, kind[ges], now);
    return 1;
}

int nd_selector(nd_dev *d, ni_toggle *t, ni_gesture *g, int raw_five, int raw_one, uint32_t now, uint16_t debounce_ms, int *queued)
{
    if (g->session != d->host_gen) {
        g->session = d->host_gen;
        ni_gesture_cancel(g);
    }
    int pos = ni_toggle_update(t, raw_five, raw_one, now, debounce_ms);
    nd_gesture(d, ni_gesture_update(g, t, pos, now), now);
    int ok = nd_toggle(d, pos, now);
    if (pos && !ok) ni_gesture_cancel(g);
    if (queued) *queued = ok;
    return pos;
}

static int send_close(nd_dev *d)
{
    if (!d->close_pending) return 0;
    char a[40];
    snprintf(a, sizeof a, "id=%lu why=%s", (unsigned long)d->close_id, d->close_why);
    if (send(d, "TCLOSE", a) != 0) return -1;
    d->close_pending = 0;
    return 0;
}

static void pump_tunnel(nd_dev *d, uint32_t now)
{
    const nd_hal *h = d->hal;
    char ip[16];
    int rssi;
    int wifi = wifi_state(d, ip, sizeof ip, &rssi);
    int want = h->tcp_listen && d->cfg.wifi_on && d->host_alive && d->host_tun && wifi == ND_WIFI_UP;
    if (!want && d->tun != NT_IDLE) tun_close(d, wifi != ND_WIFI_UP ? "wifi" : "host", d->host_alive);
    if (want != d->listening && h->tcp_listen) { h->tcp_listen(h->ctx, want); d->listening = want; }

    if (send_close(d) != 0) return;
    if (!d->listening) return;

    char a[NL_LINE_MAX];
    if (d->tun == NT_IDLE) {
        if (h->tcp_accept(h->ctx) != 1) return;
        d->tun_id++;
        d->tun = NT_OPENING;
        d->tun_since = d->tun_activity = now;
        d->tx_inflight = d->rx_unacked = 0;
        d->tx_off = d->rx_off = 0;
        d->rxq_head = d->rxq_n = 0;
        snprintf(a, sizeof a, "id=%lu", (unsigned long)d->tun_id);
        if (send(d, "TOPEN", a) != 0) { tun_close(d, "uart", 0); }
        return;
    }
    if (d->tun == NT_OPENING) {
        if (now - d->tun_since > ND_TUN_OPEN_TIMEOUT_MS) tun_close(d, "timeout", 1);
        return;
    }
    /* OPEN: host -> client */
    while (d->rxq_n) {
        uint32_t chunk = d->rxq_n;
        if (d->rxq_head + chunk > NL_TUN_WINDOW) chunk = NL_TUN_WINDOW - d->rxq_head;
        int w = h->tcp_write(h->ctx, d->rxq + d->rxq_head, chunk);
        if (w < 0) { tun_close(d, "peer", 1); return; }
        if (w == 0) break;
        d->rxq_head = (d->rxq_head + (uint32_t)w) % NL_TUN_WINDOW;
        d->rxq_n -= (uint32_t)w;
        d->rx_unacked += (uint32_t)w;
        d->tun_activity = now;
    }
    if (d->rx_unacked && (d->rx_unacked >= NL_TUN_WINDOW / 4 || now - d->last_ack >= 50)) {
        snprintf(a, sizeof a, "id=%lu n=%lu", (unsigned long)d->tun_id, (unsigned long)d->rx_unacked);
        if (send(d, "TACK", a) == 0) { d->rx_unacked = 0; d->last_ack = now; }
    }
    /* client -> host, while the window and a short UART backlog allow */
    while (d->tx_inflight < NL_TUN_WINDOW && h->uart_queued(h->ctx) < ND_DATA_TXQ_MAX && h->uart_room(h->ctx) >= NL_LINE_MAX + 1) {
        uint8_t buf[NL_TUN_CHUNK];
        uint32_t room = NL_TUN_WINDOW - d->tx_inflight;
        int r = h->tcp_read(h->ctx, buf, room < NL_TUN_CHUNK ? room : NL_TUN_CHUNK);
        if (r < 0) { tun_close(d, "peer", 1); return; }
        if (r == 0) break;
        char b64[NL_TUN_CHUNK / 3 * 4 + 8];
        nl_b64_encode(buf, (size_t)r, b64, sizeof b64);
        snprintf(a, sizeof a, "id=%lu o=%lu d=%s", (unsigned long)d->tun_id, (unsigned long)d->tx_off, b64);
        if (send(d, "TDATA", a) != 0) { tun_close(d, "uart", 1); return; }   /* cannot happen: room checked */
        d->tx_inflight += (uint32_t)r;
        d->tx_off += (uint32_t)r;
        d->tun_activity = now;
    }
    if (now - d->tun_activity > ND_TUN_IDLE_MS) tun_close(d, "idle", 1);
}

void nd_poll(nd_dev *d, uint32_t now)
{
    if (d->host_alive && now - d->host_seen > NL_PEER_TIMEOUT_MS) {
        d->host_alive = 0;            /* host gone: stop talking except for slow HELLO announcements */
        d->host_gen++;
        d->host_tun = 0;
        d->evn = 0;
        d->next_hello = now;
#if ND_GAME
        if (d->game) { game_end(d); d->game_lost_n++; }   /* r5.2: no host, no game */
#endif
    }
#if ND_GAME
    if (d->game && (int32_t)(now - d->game_until) >= 0) { game_end(d); d->game_ttl_n++; }   /* no MODE g=1 refresh */
#endif
    if (d->reply_len) {
        char *sp = strchr(d->reply, ' ');
        if (sp) *sp = 0;
        int r = send(d, d->reply, sp ? sp + 1 : NULL);
        if (sp) *sp = ' ';
        if (r == -1) return;          /* UART full: try again next poll, keep order */
        d->reply_len = 0;
    }
#if ND_PRESENCE
    presence_poll(d, now);
#endif
    while (d->evn) {
        char a[32];
        snprintf(a, sizeof a, "%s t=%lu", NL_EV_NAME[d->evk[0]], (unsigned long)d->evt[0]);
        if (send(d, "EV", a) == -1) return;
        memmove(d->evk, d->evk + 1, (size_t)(d->evn - 1));
        memmove(d->evt, d->evt + 1, (size_t)(d->evn - 1) * sizeof d->evt[0]);
        d->evn--;
    }
    char ip[16], a[NL_LINE_MAX];
    int rssi;
    int wifi = wifi_state(d, ip, sizeof ip, &rssi);
    if (d->hello_req || (!d->host_alive && (int32_t)(now - d->next_hello) >= 0)) {
        snprintf(a, sizeof a, "boot=%08lx proto=%d fw=%s sensor=%s btn=%d wifi=%s tun=%d",
                 (unsigned long)d->boot_id, NL_PROTO, d->fw, d->sensor, d->buttons, ND_WIFI_NAME[wifi], d->cfg.wifi_on);
        if (send(d, "HELLO", a) == -1) return;
        d->hello_req = 0;
        d->next_hello = now + HELLO_ANNOUNCE_MS;
    }
    if (d->host_alive && (int32_t)(now - d->next_hb) >= 0) {
        int n = snprintf(a, sizeof a, "boot=%08lx up=%lu wifi=%s", (unsigned long)d->boot_id, (unsigned long)now, ND_WIFI_NAME[wifi]);
        if (wifi == ND_WIFI_UP && ip[0]) n += snprintf(a + n, sizeof a - (size_t)n, " ip=%s rssi=%d", ip, rssi);
        snprintf(a + n, sizeof a - (size_t)n, " tun=%s evdrop=%lu", d->tun == NT_OPEN ? "open" : d->tun ? "opening" : "idle",
                 (unsigned long)d->ev_dropped);
        if (send(d, "HB", a) == -1) return;
        d->next_hb = now + NL_HB_MS;
    }
    pump_tunnel(d, now);
    send_close(d);                    /* a close decided during this pump goes out now */
}
