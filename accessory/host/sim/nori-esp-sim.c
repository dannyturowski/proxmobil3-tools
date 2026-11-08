/* nori-esp-sim: the accessory firmware's link core (shared/nori_link) on Linux.
 *
 * The UART is a pty (its slave path is published as a symlink), "Wi-Fi" is a
 * listener on 127.0.0.1, NVS is a small file.  Test scripts drive buttons and
 * taps through stdin so the same debounce/tap/link code as the ESP32 build is
 * exercised end to end.  Test tool only: never installed on the reader.
 *
 * stdin commands:
 *   b1 down|up, b2 down|up   raw button level (debounced like the firmware)
 *   tap | dtap               raw sensor single/double tap flag
 *   tog five|one|center|both raw track selector position (wt32-r5; debounced like the firmware)
 *  with --motion (r4, LIS3DSH-style polled accelerometer through ni_motion, 400 Hz in 10 ms batches):
 *   knock [MG]               one knock (default 3000 mg peak) with a decaying ring
 *   turn                     rotate gravity through 180 degrees in 0.5 s (must not tap)
 *   shake MS                 +-1500 mg vibration for MS (no taps once it is going)
 *   noise                    ESP32 ROM boot text on the UART
 *   garbage                  binary junk and a corrupt frame
 *   dupe                     resend the last EV line verbatim (duplicate seq)
 *   reboot                   firmware restart: new boot id, sequence restarts
 *   unplug MS                USB unplug: pty vanishes, device powers off, replug after MS reboots
 *   hangup MS                pty vanishes for MS but the device keeps running
 *   quit
 *  r5.2 (built with -DND_GAME=1): MODE g=0|1 from the host selects the game profile through the same
 *  nd_tuning() as the firmware; each change is logged on stderr as "SIM mode game|normal (why)".
 *  --game-ttl-ms N shortens the 60 s MODE TTL for tests.
 *  wt32-r3 (built with -DND_PRESENCE=1 -DND_EXT=1 and shared/nori_sense/src/nori_presence.c): a synthetic
 *  VL53L5CX scene, 4x4 zones at --tof-hz (default 5), through the firmware's presence detector
 *  (nori_presence) into nd_presence, exactly as the firmware's loop does:
 *   tof near MM              all 16 zones: a valid target (status 5) at MM
 *   tof one MM               one zone at MM, the rest no target (a fleck: must never count)
 *   tof far                  no target in any zone (status 255, 0 targets)
 *   tof invalid MM           targets at MM with an invalid status (4): must never count
 *   tof bad                  every frame a driver error (np_bad_frame)
 *   tof stall                no frames at all (the sensor stopped answering)
 *   tof fault                sensor lost (np_fault, as the firmware does when ranging ends)
 *  Before the first "tof" command the sensor is absent (known=0: nothing is ever sent).
 *  DIAG p=tof|rst|net|ev|prev, PRESCFG and SENSOR are answered like the firmware (rst/net/ev/prev minimal).
 */
#define _GNU_SOURCE
#include "nori_dev.h"
#if ND_PRESENCE
#include "nori_presence.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TXCAP 4096                    /* like the firmware's Serial TX buffer */

static struct {
    const char *link, *nvs;
    int port, baud, join_ms;
    int master, hold;                 /* hold: our own slave fd, so the master never sees POLLHUP */
    char tx[TXCAP]; size_t txn;
    double tx_budget, last_flush;
    int lfd, cfd;                     /* listener, single client */
    char ssid[40], psk[70];
    nd_cfg cfg;
    uint32_t wifi_since;
    char last_ev[NL_LINE_MAX + 2]; size_t last_ev_n;
    int motion;                       /* r4: --motion */
} S = { .master = -1, .hold = -1, .lfd = -1, .cfd = -1 };

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

/* ---- NVS file ---- */
static void nvs_save(void)
{
    if (!S.nvs) return;
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", S.nvs);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    char b1[64] = "", b2[100] = "";
    nl_b64_encode((const uint8_t *)S.ssid, strlen(S.ssid), b1, sizeof b1);
    nl_b64_encode((const uint8_t *)S.psk, strlen(S.psk), b2, sizeof b2);
    fprintf(f, "ssid=%s\npsk=%s\nwifi_on=%u\nths=%u\nwait=%u\nrefr=%u\n", b1, b2, S.cfg.wifi_on, S.cfg.tap_ths,
            S.cfg.dtap_wait_ms, S.cfg.refractory_ms);
#if ND_TAP_AXIS
    fprintf(f, "axis=%u\naxr=%u\n", S.cfg.tap_axis, S.cfg.axis_pct);
#endif
#if ND_GAME
    fprintf(f, "gths=%u\ngrefr=%u\ngq=%u\ngaxis=%u\n", S.cfg.g_ths, S.cfg.g_refr_ms, S.cfg.g_quiet_ms, S.cfg.g_axis);
#endif
    fclose(f);
    chmod(tmp, 0600);
    rename(tmp, S.nvs);
}
static void nvs_load(void)
{
    nd_cfg_defaults(&S.cfg);
    if (S.motion) {                   /* r4 motion build defaults, as the LIS3DSH firmware */
        ni_tap_cfg g;
        ni_tap_game_defaults(&g);
        S.cfg.dtap_wait_ms = g.dtap_wait_ms;
        S.cfg.refractory_ms = g.refractory_ms;
    }
    S.ssid[0] = S.psk[0] = 0;
    FILE *f = S.nvs ? fopen(S.nvs, "r") : NULL;
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        int n;
        if (!strcmp(line, "ssid") && (n = nl_b64_decode(v, (uint8_t *)S.ssid, 32)) >= 0) S.ssid[n] = 0;
        else if (!strcmp(line, "psk") && (n = nl_b64_decode(v, (uint8_t *)S.psk, 64)) >= 0) S.psk[n] = 0;
        else if (!strcmp(line, "wifi_on")) S.cfg.wifi_on = (uint8_t)atoi(v);
        else if (!strcmp(line, "ths")) S.cfg.tap_ths = (uint8_t)atoi(v);
        else if (!strcmp(line, "wait")) S.cfg.dtap_wait_ms = (uint16_t)atoi(v);
        else if (!strcmp(line, "refr")) S.cfg.refractory_ms = (uint16_t)atoi(v);
#if ND_TAP_AXIS
        else if (!strcmp(line, "axis")) S.cfg.tap_axis = (uint8_t)atoi(v);
        else if (!strcmp(line, "axr")) S.cfg.axis_pct = (uint8_t)atoi(v);
#endif
#if ND_GAME
        else if (!strcmp(line, "gths")) S.cfg.g_ths = (uint8_t)atoi(v);
        else if (!strcmp(line, "grefr")) S.cfg.g_refr_ms = (uint16_t)atoi(v);
        else if (!strcmp(line, "gq")) S.cfg.g_quiet_ms = (uint8_t)atoi(v);
        else if (!strcmp(line, "gaxis")) S.cfg.g_axis = (uint8_t)atoi(v);
#endif
    }
    fclose(f);
}

/* ---- HAL ---- */
static size_t h_room(void *c) { (void)c; return S.master < 0 ? TXCAP : TXCAP - S.txn; }
static size_t h_queued(void *c) { (void)c; return S.txn; }
static void h_write(void *c, const char *s, size_t n)
{
    (void)c;
    if (S.master < 0) return;          /* USB gone: bytes fall on the floor, like a real UART with no host */
    if (S.txn + n > TXCAP) return;
    memcpy(S.tx + S.txn, s, n);
    S.txn += n;
    if (n > 4 && !strncmp(s, "$NL1 ", 5) && strstr(s, " EV ") && n < sizeof S.last_ev) { memcpy(S.last_ev, s, n); S.last_ev_n = n; }
}
static int h_wifi(void *c, char *ip, size_t cap, int *rssi)
{
    (void)c;
    if (!S.cfg.wifi_on) return ND_WIFI_OFF;
    if (!S.ssid[0]) return ND_WIFI_NOCFG;
    if (now_ms() - S.wifi_since < (uint32_t)S.join_ms) return ND_WIFI_JOINING;
    snprintf(ip, cap, "127.0.0.1");
    *rssi = -50;
    return ND_WIFI_UP;
}
static int h_wifi_set(void *c, const char *ssid, const char *psk)
{
    (void)c;
    snprintf(S.ssid, sizeof S.ssid, "%s", ssid);
    snprintf(S.psk, sizeof S.psk, "%s", psk);
    S.wifi_since = now_ms();
    nvs_save();
    return 0;
}
static int h_wifi_clear(void *c) { (void)c; S.ssid[0] = S.psk[0] = 0; nvs_save(); return 0; }
static void h_wifi_power(void *c, int on) { (void)c; S.cfg.wifi_on = (uint8_t)on; S.wifi_since = now_ms(); }
static void h_listen(void *c, int on)
{
    (void)c;
    if (!on) { if (S.lfd >= 0) close(S.lfd); S.lfd = -1; fprintf(stderr, "SIM listen off\n"); return; }
    if (S.lfd >= 0 || S.port <= 0) return;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)S.port) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(fd, 2) != 0) { close(fd); fprintf(stderr, "SIM listen failed\n"); return; }
    S.lfd = fd;
    fprintf(stderr, "SIM listen on %d\n", S.port);
}
static int h_accept(void *c)
{
    (void)c;
    if (S.lfd < 0) return 0;
    int fd = accept4(S.lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return 0;
    if (S.cfd >= 0) { close(fd); return 0; }    /* one tunnel at a time */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    S.cfd = fd;
    return 1;
}
static int h_tread(void *c, uint8_t *b, size_t max)
{
    (void)c;
    if (S.cfd < 0) return -1;
    ssize_t n = read(S.cfd, b, max);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return -1;
}
static int h_twrite(void *c, const uint8_t *b, size_t n)
{
    (void)c;
    if (S.cfd < 0) return -1;
    ssize_t w = send(S.cfd, b, n, MSG_NOSIGNAL);
    if (w >= 0) return (int)w;
    return errno == EAGAIN ? 0 : -1;
}
static void h_tclose(void *c) { (void)c; if (S.cfd >= 0) close(S.cfd); S.cfd = -1; }
static int h_save(void *c, const nd_cfg *cfg) { (void)c; S.cfg = *cfg; nvs_save(); return 0; }
#if ND_PRESENCE
/* ---- wt32-r3: synthetic VL53L5CX scene -> nori_presence -> nd_presence ---- */
enum { SC_ABSENT, SC_NEAR, SC_ONE, SC_FAR, SC_INVALID, SC_BAD, SC_STALL, SC_RAMP };
static struct {
    int scene, mm, known, en, no_zg;   /* no_zg: answer DIAG p=zg as r5.1.1 firmware does */
    uint32_t next, period;
    np_state np;
    np_cfg cfg;
} P = { .scene = SC_ABSENT, .en = 1, .period = 200 };
static nd_dev *p_dev;

static void tof_step(uint32_t t)
{
    if ((int32_t)(t - P.next) < 0) return;
    P.next = t + P.period;
    if (!P.en || P.scene == SC_ABSENT) return;
    if (P.scene == SC_STALL) { np_tick(&P.np, &P.cfg, t); return; }
    if (P.scene == SC_BAD) { np_bad_frame(&P.np); np_tick(&P.np, &P.cfg, t); return; }
    int16_t d[16];
    uint8_t st[16], nb[16];
    for (int i = 0; i < 16; i++) {
        int on = P.scene == SC_NEAR || P.scene == SC_INVALID || (P.scene == SC_ONE && i == 5);
        d[i] = on ? (int16_t)P.mm : 0;
        st[i] = !on ? 255 : P.scene == SC_INVALID ? 4 : 5;
        nb[i] = on ? 1 : 0;
        if (P.scene == SC_RAMP && i != 15) { d[i] = (int16_t)(P.mm + i * 160); st[i] = 5; nb[i] = 1; }   /* zone 15 stays empty */
    }
    np_frame f = { 16, d, st, nb };
    int r = np_frame_update(&P.np, &P.cfg, &f, t);
    P.known = 1;
    if (r) fprintf(stderr, "SIM presence %d\n", P.np.level);
}
static int tof_cmd(const char *cmd, uint32_t t)
{
    int mm = 0;
    int old = P.np.level;
    if (sscanf(cmd, "tof near %d", &mm) == 1) { P.scene = SC_NEAR; P.mm = mm; }
    else if (sscanf(cmd, "tof one %d", &mm) == 1) { P.scene = SC_ONE; P.mm = mm; }
    else if (sscanf(cmd, "tof ramp %d", &mm) == 1) { P.scene = SC_RAMP; P.mm = mm; }
    else if (!strcmp(cmd, "tof far")) P.scene = SC_FAR;
    else if (sscanf(cmd, "tof invalid %d", &mm) == 1) { P.scene = SC_INVALID; P.mm = mm; }
    else if (!strcmp(cmd, "tof bad")) P.scene = SC_BAD;
    else if (!strcmp(cmd, "tof stall")) P.scene = SC_STALL;
    else if (!strcmp(cmd, "tof fault")) { np_fault(&P.np, t); P.scene = SC_STALL; }
    else return 0;
    if (old != P.np.level) fprintf(stderr, "SIM presence %d\n", P.np.level);
    return 1;
}
/* tof= and nm= as the firmware's DIAG and DIAG p=zg have them */
static int tof_tokens(char *out, size_t cap)
{
    int ranging = P.en && P.np.have_frame;
    if (ranging && P.known && P.np.nearest_mm >= 0) return snprintf(out, cap, "tof=ranging nm=%d", P.np.nearest_mm);
    return snprintf(out, cap, "tof=%s nm=-", ranging ? "ranging" : "wait");
}
static int h_diag_page(void *c, const char *page, char *out, size_t cap)
{
    (void)c;
    if (!strcmp(page, "zg") && !P.no_zg) {
        char grid[NP_GRID_TEXT_MAX];
        int n = tof_tokens(out, cap);
        np_grid_format(&P.np, now_ms(), grid, sizeof grid);
        snprintf(out + n, cap - (size_t)n, " %s", grid);
        return 0;
    }
    if (!strcmp(page, "tof")) {
        snprintf(out, cap, "tof=sim pr=%s lvl=%d nm=%d vz=%u nz=%u ri=%lu fa=%lu sf=%lu ff=%lu gf=%lu nbf=%lu ptx=%lu prt=%lu",
                 !P.known ? "-" : np_effective(&P.np, &P.cfg, now_ms(), 500) ? "1" : "0", P.np.level, P.np.nearest_mm,
                 P.np.valid_zones, P.np.near_zones, (unsigned long)P.np.rises, (unsigned long)P.np.falls,
                 (unsigned long)P.np.stale_falls, (unsigned long)P.np.fault_falls, (unsigned long)P.np.frames,
                 (unsigned long)P.np.bad_frames, (unsigned long)p_dev->pres_tx, (unsigned long)p_dev->pres_retry);
        return 0;
    }
    if (!strcmp(page, "rst") || !strcmp(page, "net") || !strcmp(page, "ev") || !strcmp(page, "prev")) {
        snprintf(out, cap, "sim=1 page=%s", page);
        return 0;
    }
    return -1;
}
static int h_ext(void *c, const nl_frame *f, char *reply, size_t cap)
{
    (void)c;
    if (!strcmp(f->type, "PRESCFG")) {
        np_cfg n = P.cfg;
        for (int i = 0; i < f->argc; i++) {
            char key[12];
            const char *eq = strchr(f->argv[i], '=');
            uint32_t v;
            size_t kl = eq ? (size_t)(eq - f->argv[i]) : 0;
            if (!eq || !kl || kl >= sizeof key || nl_parse_u32(eq + 1, &v) != 0) { snprintf(reply, cap, "ERR PRESCFG format"); return 1; }
            memcpy(key, f->argv[i], kl);
            key[kl] = 0;
            if (np_cfg_set(&n, key, v) != 0) { snprintf(reply, cap, "ERR PRESCFG range"); return 1; }
        }
        if (!np_cfg_valid(&n)) { snprintf(reply, cap, "ERR PRESCFG range"); return 1; }
        P.cfg = n;
        char a[96];
        np_cfg_format(&P.cfg, a, sizeof a);
        snprintf(reply, cap, "OK PRESCFG %s", a);
        return 1;
    }
    if (!strcmp(f->type, "SENSOR")) {
        for (int i = 0; i < f->argc; i++) {
            if (!strcmp(f->argv[i], "en=0")) { P.en = 0; np_fault(&P.np, now_ms()); }
            else if (!strcmp(f->argv[i], "en=1")) P.en = 1;
            else if (!strcmp(f->argv[i], "re=1")) np_fault(&P.np, now_ms());
            else { snprintf(reply, cap, "ERR SENSOR range"); return 1; }
        }
        snprintf(reply, cap, "OK SENSOR en=%d st=%s", P.en, P.en ? "ranging" : "off");
        return 1;
    }
    return 0;
}
#endif
#if ND_GAME
static nd_dev *g_dev;
static void h_diag(void *c, char *out, size_t cap)
{
    (void)c;
    int n = snprintf(out, cap, "sim=1 i2c=none ");
    if (n > 0 && (size_t)n < cap) nd_game_diag(g_dev, out + n, cap - (size_t)n);
}
#elif ND_PRESENCE
static void h_diag(void *c, char *out, size_t cap)
{
    (void)c;
    int n = snprintf(out, cap, "sim=1 i2c=none ");
    n += tof_tokens(out + n, cap - (size_t)n);
    snprintf(out + n, cap - (size_t)n, " up=1");     /* never last: linkd wants a token after nm= */
}
#else
static void h_diag(void *c, char *out, size_t cap) { (void)c; snprintf(out, cap, "sim=1 i2c=none"); }
#endif
#if ND_EXT
static const nd_hal HAL = { NULL, h_room, h_queued, h_write, h_wifi, h_wifi_set, h_wifi_clear, h_wifi_power, h_listen,
                            h_accept, h_tread, h_twrite, h_tclose, h_save, h_diag, NULL, h_diag_page, h_ext };
#else
static const nd_hal HAL = { NULL, h_room, h_queued, h_write, h_wifi, h_wifi_set, h_wifi_clear, h_wifi_power, h_listen,
                            h_accept, h_tread, h_twrite, h_tclose, h_save, h_diag, NULL };   /* no OTA in the sim */
#endif

/* ---- pty "USB" ---- */
static int plug(void)
{
    int m = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (m < 0 || grantpt(m) != 0 || unlockpt(m) != 0) return -1;
    const char *slave = ptsname(m);
    struct termios t;
    int s = open(slave, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (s >= 0 && tcgetattr(s, &t) == 0) { cfmakeraw(&t); tcsetattr(s, TCSANOW, &t); }
    S.hold = s;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", S.link);
    unlink(tmp);
    if (symlink(slave, tmp) != 0 || rename(tmp, S.link) != 0) { close(m); return -1; }
    S.master = m;
    S.txn = 0;
    fprintf(stderr, "SIM plugged %s\n", slave);
    return 0;
}
static void unplug(void)
{
    if (S.master >= 0) close(S.master);
    if (S.hold >= 0) close(S.hold);
    S.master = S.hold = -1;
    S.txn = 0;
    unlink(S.link);
    fprintf(stderr, "SIM unplugged\n");
}
static void raw_uart(const char *s, size_t n)    /* bypasses the link core: boot text, junk */
{
    if (S.master >= 0 && S.txn + n <= TXCAP) { memcpy(S.tx + S.txn, s, n); S.txn += n; }
}
static void flush_tx(void)
{
    if (S.master < 0 || !S.txn) return;
    double t = now_s();
    size_t allow = S.txn;
    if (S.baud > 0) {                  /* pace like a real UART: 10 bits per byte */
        S.tx_budget += (t - S.last_flush) * S.baud / 10.0;
        if (S.tx_budget > 512) S.tx_budget = 512;
        if ((double)allow > S.tx_budget) allow = (size_t)S.tx_budget;
    }
    S.last_flush = t;
    if (!allow) return;
    ssize_t w = write(S.master, S.tx, allow);
    if (w < 0 && errno == EAGAIN && S.txn == TXCAP) { S.txn = 0; return; }   /* nobody reading: a UART just loses it */
    if (w > 0) {
        memmove(S.tx, S.tx + w, S.txn - (size_t)w);
        S.txn -= (size_t)w;
        if (S.baud > 0) S.tx_budget -= (double)w;
    }
}

static uint32_t new_boot_id(void)
{
    uint32_t v = 0;
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { if (read(fd, &v, sizeof v) != sizeof v) v = 0; close(fd); }
    return v ? v : (uint32_t)time(NULL);
}

static const char ROM_BOOT[] =
    "ets Jun  8 2016 00:22:57\r\n\r\nrst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\r\n"
    "configsip: 0, SPIWP:0xee\r\nclk_drv:0x00,q_drv:0x00,d_drv:0x00,cs0_drv:0x00,hd_drv:0x00,wp_drv:0x00\r\n"
    "mode:DIO, clock div:1\r\nload:0x3fff0030,len:1184\r\nentry 0x400805e4\r\n";

/* ---- r4: synthetic accelerometer (mg), fed to ni_motion like the LIS3DSH firmware ---- */
static struct {
    ni_motion m;
    ni_motion_cfg c;
    uint32_t next, seed;
    int knock_i, knock_amp;           /* knock_i < 0: none */
    int turn_i;                       /* < 0: none; else 0..200 */
    double angle;                     /* gravity direction in the x-z plane */
    uint32_t shake_until;
    uint32_t n;
} M = { .knock_i = -1, .turn_i = -1 };

static int motion_batch(uint32_t t)   /* 4 samples per 10 ms; returns 1 if the detector saw a hit */
{
    int hit = 0;
    if (!S.motion || (int32_t)(t - M.next) < 0) return 0;
    M.next = t + 10;
    for (int k = 0; k < 4; k++, M.n++) {
        if (M.turn_i >= 0) { M.angle += 3.14159265 / 200; if (++M.turn_i > 200) M.turn_i = -1; }
        int32_t x = (int32_t)(1000 * __builtin_sin(M.angle)), y = 0, z = (int32_t)(1000 * __builtin_cos(M.angle));
        M.seed = M.seed * 1103515245u + 12345u;
        int32_t noise = (int32_t)((M.seed >> 16) % 41) - 20;
        x += noise; y -= noise / 2; z += noise / 3;
        if (M.knock_i >= 0) {
            int i = M.knock_i++, a = M.knock_amp;
            if (i < 3) z += i == 1 ? a : a / 2;
            else { int r = a / 2; for (int j = 3; j < i; j++) r = r * 7 / 8; z += (i & 1) ? r : -r; x += r / 3; }
            if (M.knock_i > 33) M.knock_i = -1;
        }
        if ((int32_t)(M.shake_until - t) > 0) z += (M.n & 1) ? 1500 : -1500;
        hit |= ni_motion_sample(&M.m, &M.c, x, y, z);
    }
    return hit;
}

static volatile sig_atomic_t g_run = 1;
static void on_sig(int s) { (void)s; g_run = 0; }

int main(int argc, char **argv)
{
    const char *sensor = "none";
#if ND_GAME
    uint32_t game_ttl = 0;
    int sim_game = 0;
    uint32_t seen_ttl = 0, seen_lost = 0;
#endif
    S.baud = 115200;
    S.join_ms = 300;
    for (int i = 1; i < argc; i++) {
        int more = i + 1 < argc;
        if (!strcmp(argv[i], "--pty-link") && more) S.link = argv[++i];
        else if (!strcmp(argv[i], "--wifi-port") && more) S.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--nvs") && more) S.nvs = argv[++i];
        else if (!strcmp(argv[i], "--baud") && more) S.baud = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sensor") && more) sensor = argv[++i];
        else if (!strcmp(argv[i], "--join-ms") && more) S.join_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--motion")) S.motion = 1;
#if ND_GAME
        else if (!strcmp(argv[i], "--game-ttl-ms") && more) game_ttl = (uint32_t)atoi(argv[++i]);
#endif
#if ND_PRESENCE
        else if (!strcmp(argv[i], "--tof-hz") && more) P.period = 1000u / (uint32_t)(atoi(argv[++i]) > 0 ? atoi(argv[i]) : 5);
        else if (!strcmp(argv[i], "--no-zg")) P.no_zg = 1;
#endif
        else { fprintf(stderr, "usage: nori-esp-sim --pty-link PATH [--wifi-port N] [--nvs FILE] [--baud N|0] [--sensor NAME] [--motion]\n"); return 2; }
    }
    if (!S.link) { fprintf(stderr, "--pty-link required\n"); return 2; }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
    static nd_dev dev;
    nvs_load();
    if (plug() != 0) { perror("pty"); return 1; }
    raw_uart(ROM_BOOT, sizeof ROM_BOOT - 1);
    uint32_t t = now_ms();
    nd_init(&dev, &HAL, new_boot_id(), "r1.0.0-sim", sensor, 2, &S.cfg, t);
#if ND_GAME
    g_dev = &dev;
    if (game_ttl) dev.game_ttl_ms = game_ttl;
#endif
#if ND_PRESENCE
    p_dev = &dev;
    np_defaults(&P.cfg);
    np_init(&P.np);
#endif
    ni_button btn[2];
    ni_button_init(&btn[0], 0, t); ni_button_init(&btn[1], 0, t);
    int raw[2] = { 0, 0 };
    int tog_raw = NI_TOG_CENTER;
    ni_toggle tog; ni_toggle_init(&tog, 0, 0, t);
    ni_gesture ges; ni_gesture_init(&ges, &tog);
    ni_tap tap; ni_tap_init(&tap);
    ni_motion_defaults(&M.c, 400);
    M.seed = 1;
    ni_tap_cfg tcfg;
    if (S.motion) ni_tap_game_defaults(&tcfg); else ni_tap_defaults(&tcfg);
    uint32_t replug_at = 0; int replug_reboot = 0;
    char cmd[128]; size_t cn = 0;
    S.last_flush = now_s();
    fprintf(stderr, "SIM ready boot=%08lx\n", (unsigned long)dev.boot_id);

    while (g_run) {
        struct pollfd pf[3];
        int np = 0, im = -1;
        pf[np++] = (struct pollfd){ 0, POLLIN, 0 };
        if (S.master >= 0) { im = np; pf[np++] = (struct pollfd){ S.master, POLLIN, 0 }; }
        poll(pf, (nfds_t)np, 3);
        t = now_ms();
        int single = 0, dbl = 0;
        if (pf[0].revents & POLLIN) {
            char b[256];
            ssize_t n = read(0, b, sizeof b);
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; i++) {
                if (b[i] != '\n') { if (cn < sizeof cmd - 1) cmd[cn++] = b[i]; continue; }
                cmd[cn] = 0; cn = 0;
                int ms;
                if (!strcmp(cmd, "b1 down")) raw[0] = 1;
                else if (!strcmp(cmd, "b1 up")) raw[0] = 0;
                else if (!strcmp(cmd, "b2 down")) raw[1] = 1;
                else if (!strcmp(cmd, "b2 up")) raw[1] = 0;
                else if (!strcmp(cmd, "tog five")) tog_raw = NI_TOG_FIVE;
                else if (!strcmp(cmd, "tog one")) tog_raw = NI_TOG_ONE;
                else if (!strcmp(cmd, "tog center")) tog_raw = NI_TOG_CENTER;
                else if (!strcmp(cmd, "tog both")) tog_raw = NI_TOG_BOTH;
                else if (!strcmp(cmd, "tap")) single = 1;
                else if (!strcmp(cmd, "dtap")) { single = 1; dbl = 1; }
                else if (S.motion && (!strcmp(cmd, "knock") || sscanf(cmd, "knock %d", &ms) == 1)) {
                    M.knock_amp = !strcmp(cmd, "knock") ? 3000 : ms;
                    M.knock_i = 0;
                } else if (S.motion && !strcmp(cmd, "turn")) M.turn_i = 0;
                else if (S.motion && sscanf(cmd, "shake %d", &ms) == 1) M.shake_until = t + (uint32_t)ms;
                else if (!strcmp(cmd, "noise")) raw_uart(ROM_BOOT, sizeof ROM_BOOT - 1);
                else if (!strcmp(cmd, "garbage")) {
                    static const char junk[] = "\x00\xff\xfe\x13junk$NL1 99 EV tap t=1*0000\r\n$NL1 x\n\x01\x02";
                    raw_uart(junk, sizeof junk - 1);
                } else if (!strcmp(cmd, "dupe")) { if (S.last_ev_n) raw_uart(S.last_ev, S.last_ev_n); }
                else if (!strcmp(cmd, "reboot")) {
                    h_tclose(NULL); h_listen(NULL, 0);
                    raw_uart(ROM_BOOT, sizeof ROM_BOOT - 1);
                    nvs_load();
                    nd_init(&dev, &HAL, new_boot_id(), "r1.0.0-sim", sensor, 2, &S.cfg, t);
                    ni_toggle_init(&tog, tog_raw & NI_TOG_FIVE, tog_raw & NI_TOG_ONE, t);   /* the boot position is not a move */
                    ni_gesture_init(&ges, &tog);
#if ND_GAME
                    if (game_ttl) dev.game_ttl_ms = game_ttl;
                    seen_ttl = seen_lost = 0;
#endif
#if ND_PRESENCE
                    P.known = 0; np_init(&P.np); P.next = t + 3000;   /* as the firmware: probing starts ~3 s after boot */
#endif
                    fprintf(stderr, "SIM rebooted boot=%08lx\n", (unsigned long)dev.boot_id);
                } else if (sscanf(cmd, "unplug %d", &ms) == 1 || sscanf(cmd, "hangup %d", &ms) == 1) {
                    replug_reboot = cmd[0] == 'u';
                    if (replug_reboot) { h_tclose(NULL); h_listen(NULL, 0); }
                    unplug();
                    replug_at = t + (uint32_t)ms;
                } else if (!strcmp(cmd, "quit")) g_run = 0;
#if ND_PRESENCE
                else if (tof_cmd(cmd, t)) {}
#endif
                else if (cmd[0]) fprintf(stderr, "SIM unknown command\n");
            }
        }
        if (replug_at && (int32_t)(t - replug_at) >= 0) {
            replug_at = 0;
            if (plug() == 0 && replug_reboot) {
                raw_uart(ROM_BOOT, sizeof ROM_BOOT - 1);
                nvs_load();
                nd_init(&dev, &HAL, new_boot_id(), "r1.0.0-sim", sensor, 2, &S.cfg, t);
#if ND_GAME
                if (game_ttl) dev.game_ttl_ms = game_ttl;
                seen_ttl = seen_lost = 0;
#endif
#if ND_PRESENCE
                P.known = 0; np_init(&P.np); P.next = t + 3000;
#endif
                fprintf(stderr, "SIM rebooted boot=%08lx\n", (unsigned long)dev.boot_id);
            }
        }
        if (im >= 0 && (pf[im].revents & POLLIN)) {
            uint8_t b[512];
            ssize_t n = read(S.master, b, sizeof b);
            if (n > 0) nd_uart_input(&dev, b, (size_t)n, t);
        }
        if (S.cfd >= 0 && S.lfd >= 0) {          /* one tunnel at a time, as the firmware does */
            int extra = accept4(S.lfd, NULL, NULL, SOCK_CLOEXEC);
            if (extra >= 0) close(extra);
        }
        for (int k = 0; k < 2; k++) nd_button(&dev, k, ni_button_update(&btn[k], raw[k], t, 30), t);
        nd_selector(&dev, &tog, &ges, tog_raw & NI_TOG_FIVE, tog_raw & NI_TOG_ONE, t, 30, NULL);
#if ND_PRESENCE
        tof_step(t);
        nd_presence(&dev, P.known, np_effective(&P.np, &P.cfg, t, 500));   /* as the firmware's loop() */
#endif
#if ND_GAME
        if (nd_game(&dev) != sim_game) {        /* r5.2, as the firmware's loop(): the next sample uses the new profile */
            const char *why = nd_game(&dev) ? "MODE g=1" : dev.game_ttl_n != seen_ttl ? "ttl" : dev.game_lost_n != seen_lost ? "host lost" : "MODE g=0";
            sim_game = nd_game(&dev);
            nd_tap_reprofile(&tap);
            fprintf(stderr, "SIM mode %s (%s)\n", sim_game ? "game" : "normal", why);
        }
        seen_ttl = dev.game_ttl_n; seen_lost = dev.game_lost_n;
        if (S.motion) nd_tuning(&dev.cfg, sim_game, 400, &M.c, &tcfg);
        else {
            tcfg.dtap_wait_ms = dev.cfg.dtap_wait_ms;
            tcfg.refractory_ms = dev.cfg.refractory_ms;
        }
        if (motion_batch(t)) single = 1;
#else
#if ND_TAP_AXIS
        M.c.axis = dev.cfg.tap_axis;  /* r5.1, as the firmware's motion_tuning(): CFG axis=/axr=/refr */
        M.c.axis_pct = dev.cfg.axis_pct;
        M.c.reject_hold_samples = (uint16_t)(dev.cfg.refractory_ms * 400u / 1000u);
#endif
        if (motion_batch(t)) single = 1;
        tcfg.dtap_wait_ms = dev.cfg.dtap_wait_ms;
        tcfg.refractory_ms = dev.cfg.refractory_ms;
#endif
        nd_tap(&dev, ni_tap_update(&tap, &tcfg, single, dbl, t), t);
        nd_poll(&dev, t);
        flush_tx();
    }
    unplug();
    h_tclose(NULL);
    h_listen(NULL, 0);
    return 0;
}
