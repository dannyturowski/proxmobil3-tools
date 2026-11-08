/* Accessory-side link core: everything the ESP32 says and hears on the UART.
 *
 * Hardware is reached only through nd_hal, so the identical logic runs in the
 * firmware (Arduino HAL) and in the native simulator used by the integration
 * tests (POSIX HAL: a pty instead of the UART, a localhost listener instead of
 * Wi-Fi).
 *
 * Device -> host frames
 *   HELLO boot=<hex8> proto=1 fw=<ver> sensor=<kind> btn=<n> wifi=<state> tun=<0|1>
 *   HB    boot=<hex8> up=<ms> wifi=<state> [ip=<a.b.c.d> rssi=<dBm>] tun=<idle|open> evdrop=<n>
 *   EV    <kind> t=<ms>                       kind: tap dtap b1d b1u b2d b2u (tilt reserved)
 *         wt32-r5: trk5 trk1, a move of the three-way track selector.  Sent once per move, only while a
 *         host is talking: never for the position at boot, never re-sent on a new host session, never
 *         for the centre.  Unlike pon/poff this is an action, not a level: a lost one is not replayed.
 *         wt32-r5.1.1: tsh5 tsh1 tlg5 tlg1, what that move turned out to be (ni_gesture).  Only after its own
 *         trk5/trk1 was queued in the same host session: a host that goes away or restarts ends the gesture.
 *   OK    <what> [k=v...]  /  ERR <what> <reason>     replies to commands
 *   TOPEN id=<n>  TDATA id=<n> o=<offset> d=<base64>  TACK id=<n> n=<bytes>  TCLOSE id=<n> why=<reason>
 *   (o = bytes already sent in that direction; a lost line is detected, never silently skipped)
 * Host -> device frames
 *   HOST  proto=1 role=<app|linkd> tun=<0|1>  (answered with HELLO)
 *   HHB                                        (host heartbeat)
 *   WIFISET s=<base64 ssid> p=<base64 psk>     (never echoed or logged)  -> OK WIFISET / ERR WIFISET <nowifi|format|psklen|store|full>
 *         wt32-r5.1: makes this network the primary and keeps every other saved one; "full" = 8 saved, see nori_wnet.h.
 *         WIFICLR forgets all of them.  Saved networks in general: WNBEGIN..., shared/nori_wifi/nori_wnprov.h.
 *   WIFIEN on=<0|1>   WIFICLR   CFG k=v...   DIAG
 *   CFG keys: ths=1..255 wait=0..1000 refr=40..5000; r5.1 builds with ND_TAP_AXIS also
 *         axis=0..3 (0 any, 1 x, 2 y, 3 z) axr=50..100 (percent); elsewhere those keys are
 *         refused like any unknown key (ERR CFG range).  OK CFG ths=.. wait=.. refr=..[ axis=.. axr=..]
 *   IICSCAN [hz=100|400]                       (r5.1, builds with ND_I2CSCAN; elsewhere ignored like any
 *         unknown command.  "I2CSCAN" is not a legal frame type: types are 2-8 letters A-Z)
 *         -> OK IICSCAN sda=<pd>/<pu> scl=<pd>/<pu> ... found=<hex,..|none|skipped>  / ERR IICSCAN <range|nobus>
 *   MODE g=0|1                                 (r5.2, builds with ND_GAME; elsewhere ignored like any unknown
 *         command, i.e. no reply).  Volatile game profile: g=1 selects the CFG gths/grefr/gq/gaxis tuning
 *         until g=0, ND_GAME_TTL_MS without a refresh, host silence (NL_PEER_TIMEOUT_MS), a new HOST
 *         handshake or a reboot.  -> OK MODE g=<0|1>  /  ERR MODE range
 *   CFG keys (r5.2, ND_GAME): gths=1..255 (x32 mg) grefr=15..1000 (ms) gq=5..40 (ms) gaxis=0..3
 *         -> OK CFG ths=.. wait=.. refr=.. axis=.. axr=.. gths=.. grefr=.. gq=.. gaxis=..  (10 args: the maximum)
 *   Every frame, replies included, carries at most NL_ARGS_MAX (10) args or nl_parse refuses it.
 *   OTAKEY h=<32 hex>                          (r2: MD5 of the OTA password; never echoed or logged)
 *         -> OK OTAKEY  /  ERR OTAKEY <noota|format|weak|store>
 *   wt32-r3, builds with ND_PRESENCE:
 *     EV pon t=<ms> / EV poff t=<ms>           debounced presence level changes.  The device re-sends the
 *         current level whenever a host (re)starts talking (HOST, or any frame after host silence), and
 *         retries a level the event bucket refused, so a host that missed an edge converges to the level.
 *         Nothing is sent before the sensor has produced its first good frame this boot.
 *   wt32-r3, builds with ND_EXT:
 *     DIAG p=<page>                            extra diagnostic pages (firmware-defined, e.g. tof net rst ev zg):
 *         -> OK DIAG rx=.. noise=.. crc=.. bad=.. long=.. pg=<page>;<tokens ';'-joined>  /  ERR DIAG page
 *     any other command the core does not know is offered to nd_hal.ext_cmd (e.g. PRESCFG, TOF); a command
 *     it does not take either gets no reply, as before.
 *   TOK id=<n>  TDATA ...  TACK ...  TCLOSE ...
 */
#ifndef NORI_DEV_H
#define NORI_DEV_H

#include "nori_input.h"
#include "nori_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/* r5.1 build options.  Both change nd_cfg / nd_hal, so every file of one build must see the
 * same values (they are global build flags); builds without them are unchanged from r5. */
#ifndef ND_TAP_AXIS
#define ND_TAP_AXIS 0              /* 1: CFG axis=/axr= (ni_motion axis gate) */
#endif
#ifndef ND_I2CSCAN
#define ND_I2CSCAN 0               /* 1: I2CSCAN command (nd_hal.i2c_scan) */
#endif
#ifndef ND_GAME
#define ND_GAME 0                  /* r5.2, 1: MODE command + game tap profile (CFG gths/grefr/gq/gaxis) */
#endif
/* wt32-r1: join the firmware's DIAG tokens into one ';'-joined token (as r5.1 builds do) without
 * the r5.1 sensor features, and use the full 296-char reply.  Layout of nd_cfg/nd_hal is unchanged. */
#ifndef ND_DIAG_JOIN
#define ND_DIAG_JOIN 0
#endif
/* wt32-r3 (both change nd_dev / nd_hal, so all files of one build need the same values) */
#ifndef ND_PRESENCE
#define ND_PRESENCE 0              /* 1: nd_presence() -> EV pon/poff with host resync */
#endif
#ifndef ND_EXT
#define ND_EXT 0                   /* 1: DIAG p=<page> (nd_hal.diag_page) and nd_hal.ext_cmd */
#endif
#if ND_EXT && !ND_DIAG_JOIN
#error "ND_EXT needs ND_DIAG_JOIN (pages are one ';'-joined token)"
#endif
#if ND_GAME && !ND_TAP_AXIS
#error "ND_GAME needs ND_TAP_AXIS (the game profile has its own axis gate setting)"
#endif
#if ND_TAP_AXIS || ND_I2CSCAN || ND_GAME || ND_PRESENCE || ND_EXT
/* Link-time layout check: only a nori_dev.c compiled with the same flags defines this
 * name, so a library built without them fails to link instead of disagreeing on nd_cfg. */
#define ND_ABI_CAT2(a, b, c, d, e) nd_abi_axis##a##_scan##b##_game##c##_pres##d##_ext##e
#define ND_ABI_CAT(a, b, c, d, e) ND_ABI_CAT2(a, b, c, d, e)
#define ND_ABI_CHECK ND_ABI_CAT(ND_TAP_AXIS, ND_I2CSCAN, ND_GAME, ND_PRESENCE, ND_EXT)
void ND_ABI_CHECK(void);
#endif
#define ND_AXIS_DEFAULT 3          /* z: normal to the faces with the breakout flat in the back cover */
#define ND_AXR_DEFAULT 70
#define ND_AXR_MIN 50
#define ND_AXR_MAX 100
#if ND_GAME
/* r5.2 game profile (see GAME-MODE.md for why these values) */
#define ND_GTHS_DEFAULT 3          /* x 32 mg = 96 mg */
#define ND_GREFR_DEFAULT 25        /* ms, ni_tap refractory (and axis-gate reject hold) in the game */
#define ND_GREFR_MIN 15
#define ND_GREFR_MAX 1000
#define ND_GQ_DEFAULT 12           /* ms of quiet (< 40 % of the threshold) before the detector re-arms */
#define ND_GQ_MIN 5
#define ND_GQ_MAX 40
#define ND_GAXIS_DEFAULT 0         /* gate off: a tap is reported at the threshold crossing */
#define ND_GAME_TTL_MS 60000u      /* game mode ends this long after the last MODE g=1 */
#define ND_GAME_WARMUP_MS 40       /* detector warm-up after a gap (normal profile: 160 ms) */
#ifndef ND_GAME_FREEZE_MS
#define ND_GAME_FREEZE_MS 80       /* ni_motion baseline hold after a crossing (normal profile: off) */
#endif
#ifndef ND_GAME_RETRIG_MS
#define ND_GAME_RETRIG_MS 20       /* ni_motion onset re-trigger window (normal profile: off) */
#endif
#define ND_EV_PER_SEC_GAME 20      /* event bucket in game mode (normal: 12/s, burst 6) */
#define ND_EV_BURST_GAME 10
#endif

enum { ND_WIFI_OFF, ND_WIFI_NOCFG, ND_WIFI_JOINING, ND_WIFI_UP };
extern const char *const ND_WIFI_NAME[];

typedef struct {
    uint8_t tap_ths;           /* sensor-specific raw threshold (see firmware README) */
    uint16_t dtap_wait_ms;
    uint16_t refractory_ms;
    uint8_t wifi_on;           /* persisted opt-in: Wi-Fi station + SSH tunnel (R1 uses Wi-Fi for nothing else) */
#if ND_TAP_AXIS
    uint8_t tap_axis;          /* r5.1: 0 any (gate off), 1 x, 2 y, 3 z */
    uint8_t axis_pct;          /* r5.1: ND_AXR_MIN..ND_AXR_MAX */
#endif
#if ND_GAME
    uint8_t g_ths;             /* r5.2 game profile: 1..255, x 32 mg */
    uint16_t g_refr_ms;        /* ND_GREFR_MIN..ND_GREFR_MAX */
    uint8_t g_quiet_ms;        /* ND_GQ_MIN..ND_GQ_MAX */
    uint8_t g_axis;            /* 0 any (gate off), 1 x, 2 y, 3 z; ratio = axis_pct */
#endif
} nd_cfg;

typedef struct {
    void *ctx;
    size_t (*uart_room)(void *ctx);                 /* bytes writable now without blocking */
    size_t (*uart_queued)(void *ctx);               /* bytes written but not yet sent */
    void (*uart_write)(void *ctx, const char *s, size_t n);
    /* Wi-Fi; all may be NULL in a build without Wi-Fi */
    int (*wifi_status)(void *ctx, char *ip, size_t ipcap, int *rssi);
    int (*wifi_set)(void *ctx, const char *ssid, const char *psk);   /* persist + (re)join; 0 ok, -2 no room, else store error */
    int (*wifi_clear)(void *ctx);
    void (*wifi_power)(void *ctx, int on);
    /* exactly one TCP client at a time on the tunnel port */
    void (*tcp_listen)(void *ctx, int on);
    int (*tcp_accept)(void *ctx);                    /* 1: a new client is now held */
    int (*tcp_read)(void *ctx, uint8_t *b, size_t max);        /* >0, 0 = nothing, -1 = closed */
    int (*tcp_write)(void *ctx, const uint8_t *b, size_t n);   /* accepted bytes, -1 = closed */
    void (*tcp_close)(void *ctx);
    int (*cfg_save)(void *ctx, const nd_cfg *c);
    void (*diag)(void *ctx, char *out, size_t cap);  /* extra DIAG tokens, e.g. "i2c=53" */
    /* r2, appended so older positional initializers still compile (NULL = no OTA):
     * persist the OTA password hash (32 lowercase hex chars, already validated); 0 ok */
    int (*ota_key_set)(void *ctx, const char *md5hex);
#if ND_I2CSCAN
    /* r5.1: bus/wiring diagnostic; khz 0 = the build's bus speed.  Writes the reply tokens
     * (after "IICSCAN ") into out; must be bounded in time.  NULL: ERR IICSCAN nobus. */
    void (*i2c_scan)(void *ctx, unsigned khz, char *out, size_t cap);
#endif
#if ND_EXT
    /* wt32-r3: DIAG p=<page>.  Write the page's tokens (space separated; the core joins them) into out
     * and return 0, or -1 for an unknown page (ERR DIAG page). */
    int (*diag_page)(void *ctx, const char *page, char *out, size_t cap);
    /* wt32-r3: a command the core does not handle.  Return 1 and write the reply ("OK ..." / "ERR ...",
     * at most NL_ARGS_MAX args) into reply if the firmware takes it, 0 to ignore it (no reply). */
    int (*ext_cmd)(void *ctx, const nl_frame *f, char *reply, size_t cap);
#endif
} nd_hal;

enum { NT_IDLE, NT_OPENING, NT_OPEN };

#define ND_EVQ 8
#define ND_TUN_OPEN_TIMEOUT_MS 5000
#define ND_TUN_IDLE_MS (2u * 3600u * 1000u)
#define ND_DATA_TXQ_MAX 768        /* keep UART backlog short so events are never stuck behind data */

typedef struct {
    const nd_hal *hal;
    nd_cfg cfg;
    int cfg_changed;               /* set when CFG changed sensor settings; firmware clears it */
    uint32_t boot_id, seq;
    char fw[24], sensor[24];
    int buttons;

    nl_rx rx;
    uint32_t rx_now;               /* time of the input being parsed */
    uint32_t host_seen;            /* last valid host frame (ms) */
    int host_alive, host_tun;
    uint32_t host_gen;             /* wt32-r5.1.1: changes when a host session ends or starts */
    int hello_req;                 /* host asked for HELLO */
    uint32_t next_hb, next_hello;  /* next_hello: unsolicited announcements while no host is talking */

    uint8_t evk[ND_EVQ];
    uint32_t evt[ND_EVQ];
    int evn;
    ni_bucket bucket;
    uint32_t ev_dropped;

    char reply[NL_LINE_MAX + 2];   /* one pending command reply */
    int reply_len;

    /* tunnel */
    int tun, wifi_last;
    uint32_t tun_id, tun_since, tun_activity;
    uint32_t tx_inflight;          /* bytes sent to host, not yet acknowledged */
    uint32_t tx_off, rx_off;       /* stream offsets: every TDATA carries o=<offset>, a gap closes the tunnel */
    uint32_t rx_unacked;           /* bytes delivered to the TCP client, not yet acknowledged */
    uint32_t last_ack;
    uint8_t rxq[NL_TUN_WINDOW];    /* host->client bytes the client has not accepted yet */
    uint32_t rxq_head, rxq_n;
    int listening;
    char close_why[16];            /* pending TCLOSE reason */
    int close_pending;
    uint32_t close_id;
#if ND_GAME
    /* r5.2 game mode: volatile (never saved), see MODE above */
    int game;                      /* 1: the game profile is in use */
    uint32_t game_until;           /* while game: ends when now reaches this */
    uint32_t game_ttl_ms;          /* ND_GAME_TTL_MS (the simulator may shorten it for tests) */
    uint32_t game_on_n, game_ttl_n, game_lost_n;   /* diagnostics: entered, expired, ended by host loss/HOST */
#endif
#if ND_PRESENCE
    int pres_known;                /* the sensor produced a good frame this boot: levels are meaningful */
    int pres_level;                /* current debounced level from the firmware */
    int pres_sent;                 /* level last queued to the current host session, -1: none/resync */
    int pres_was_alive;            /* host_alive at the previous nd_poll (resync on 0 -> 1) */
    int pres_blocked;              /* the current level is waiting for the bucket/queue */
    uint32_t pres_tx, pres_retry;  /* diagnostics: pon/poff queued; levels delayed by the bucket/queue */
#endif
} nd_dev;

void nd_cfg_defaults(nd_cfg *c);
#if ND_TAP_AXIS
/* r5.1 persistence of the axis settings (NVS keys ND_K_AXIS, ND_K_AXR, one byte each).
 * get returns 1 and sets *v if the key exists.  Load keeps c's value for an absent key and
 * replaces an out-of-range stored value with the default; save writes only what differs
 * from *old (r4 rule: never freeze defaults into NVS). */
#define ND_K_AXIS "m_axis"
#define ND_K_AXR "m_axr"
typedef struct {
    void *ctx;
    int (*get_u8)(void *ctx, const char *key, uint8_t *v);
    void (*put_u8)(void *ctx, const char *key, uint8_t v);
} nd_kv;
void nd_axis_load(nd_cfg *c, const nd_kv *kv);
void nd_axis_save(const nd_cfg *old, const nd_cfg *c, const nd_kv *kv);
#endif
#if ND_GAME
/* r5.2 persistence of the game profile, same rules as nd_axis_load/save. */
#define ND_K_GTHS "m_gths"
#define ND_K_GREFR "m_grefr"       /* u16 */
#define ND_K_GQ "m_gq"
#define ND_K_GAXIS "m_gaxis"
typedef struct {
    void *ctx;
    int (*get_u16)(void *ctx, const char *key, uint16_t *v);
    void (*put_u16)(void *ctx, const char *key, uint16_t v);
} nd_kv16;
void nd_game_load(nd_cfg *c, const nd_kv *kv, const nd_kv16 *kv16);
void nd_game_save(const nd_cfg *old, const nd_cfg *c, const nd_kv *kv, const nd_kv16 *kv16);
/* 1 while the game profile is in use (after nd_poll has applied the TTL / host-loss rules). */
int nd_game(const nd_dev *d);
/* The detector and tap settings of one profile at a sample rate, exactly what the firmware applies:
 * game = 0 -> normal profile (CFG ths/wait/refr/axis/axr; tap guard = ni_tap_game_defaults);
 * game = 1 -> game profile (gths/grefr/gq/gaxis, axr shared; wait 0, shake guard off, 40 ms warm-up). */
void nd_tuning(const nd_cfg *c, int game, uint16_t odr_hz, ni_motion_cfg *m, ni_tap_cfg *t);
/* Call when the profile changes: drops a shake mute and the shake history (the game profile has
 * no shake guard, and a mute begun in the normal profile must not outlast the switch). */
void nd_tap_reprofile(ni_tap *t);
/* DIAG tokens "mode=<game|normal> gtap=t<gths>/r<grefr>/q<gq>/a<gaxis>"; snprintf-style (never writes past cap). */
int nd_game_diag(const nd_dev *d, char *out, size_t cap);
#endif
void nd_init(nd_dev *d, const nd_hal *hal, uint32_t boot_id, const char *fw, const char *sensor,
             int buttons, const nd_cfg *cfg, uint32_t now);
void nd_uart_input(nd_dev *d, const uint8_t *b, size_t n, uint32_t now);
void nd_event(nd_dev *d, int kind, uint32_t now);   /* queue an input event (rate limited) */
void nd_button(nd_dev *d, int index, int edge, uint32_t now);   /* index 0/1, edge from ni_button_update */
void nd_tap(nd_dev *d, int tap, uint32_t now);                  /* tap from ni_tap_update */
/* wt32-r5: pos from ni_toggle_update.  1: queued as EV trk5/trk1; 0: nothing to send, or dropped (no
 * host, full queue, rate bucket; a drop counts in ev_dropped like any event). */
int nd_toggle(nd_dev *d, int pos, uint32_t now);
/* wt32-r5.1.1: ges from ni_gesture_update.  1: queued as EV tsh5/tsh1/tlg5/tlg1; 0: nothing, or dropped.  Not
 * charged to the rate bucket: at most one follows each trk, which paid, and a game's fast flips must not
 * lose moves to it. */
int nd_gesture(nd_dev *d, int ges, uint32_t now);
/* wt32-r5.1.1: one selector sample, the firmware's and the simulator's: ni_toggle_update, ni_gesture_update
 * and both events in wire order, with a gesture ended by a new host session or a move that was not sent.
 * Returns ni_toggle_update's result; *queued = nd_toggle's. */
int nd_selector(nd_dev *d, ni_toggle *t, ni_gesture *g, int raw_five, int raw_one, uint32_t now, uint16_t debounce_ms, int *queued);
void nd_poll(nd_dev *d, uint32_t now);              /* send what is due; pump the tunnel */
#if ND_PRESENCE
/* wt32-r3: the firmware's current presence state, every loop pass.  known = 0 (no good frame yet this
 * boot): nothing is sent.  Level changes and host (re)connections are turned into EV pon/poff by nd_poll. */
void nd_presence(nd_dev *d, int known, int level);
#endif

#ifdef __cplusplus
}
#endif
#endif
