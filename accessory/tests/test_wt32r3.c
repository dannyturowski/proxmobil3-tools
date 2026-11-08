/* wt32-r3: the device core as the r3 firmware builds it (-DND_DIAG_JOIN=1 -DND_PRESENCE=1 -DND_EXT=1).
 *   - presence: nothing before the sensor is known; one EV per level change; resync on HOST and when a
 *     host returns after silence; nothing while no host; a level the rate bucket refused is retried and
 *     never counted as a dropped event; presence and buttons share the bucket;
 *   - the host side (nori_host) accepts EV pon/poff and still ignores unknown event names (what a
 *     pre-r3 linkd does with pon/poff);
 *   - DIAG p=<page>: one joined token "pg=<page>;...", <= 10 args, unknown or malformed pages refused;
 *     plain DIAG unchanged, never a trailing ';';
 *   - extension commands (PRESCFG/SENSOR) reach nd_hal.ext_cmd and their reply goes out as one frame;
 *     unknown commands get no reply; tunnel frames still reach the tunnel. */
#include "nori_dev.h"
#include "nori_host.h"

#include <stdio.h>
#include <string.h>

#if !ND_DIAG_JOIN || !ND_PRESENCE || !ND_EXT || ND_TAP_AXIS || ND_I2CSCAN || ND_GAME
#error "build with -DND_DIAG_JOIN=1 -DND_PRESENCE=1 -DND_EXT=1 (the wt32-r3 firmware's flags)"
#endif

static int checks, fails;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static char uart[65536];
static size_t uart_n;
static const char *diag_text = "sensor=vl53l5cx tof=ranging pr=0";
static const char *page_text = "tof=ranging f=10";
static char ext_seen[64];
static int ext_take = 1;

static size_t h_room(void *c) { (void)c; return sizeof uart - uart_n; }
static size_t h_queued(void *c) { (void)c; return 0; }
static void h_write(void *c, const char *s, size_t n) { (void)c; memcpy(uart + uart_n, s, n); uart_n += n; }
static int h_status(void *c, char *ip, size_t cap, int *rssi) { (void)c; snprintf(ip, cap, "10.0.0.2"); *rssi = 0; return ND_WIFI_UP; }
static void h_power(void *c, int on) { (void)c; (void)on; }
static void h_diag(void *c, char *out, size_t cap) { (void)c; snprintf(out, cap, "%s", diag_text); }
static int h_page(void *c, const char *page, char *out, size_t cap)
{
    (void)c;
    if (!strcmp(page, "zg")) {                      /* wt32-r5.1.2: the longest the firmware writes */
        snprintf(out, cap, "tof=ranging nm=4000 zf=4294967295 za=999 zg=fa0fa0fa0fa0fa0fa0fa0fa0fa0fa0fa0fa0fa0fa0fa0fa0");
        return 0;
    }
    if (strcmp(page, "tof") && strcmp(page, "net")) return -1;
    snprintf(out, cap, "%s ", page_text);           /* a trailing space must not become a trailing ';' */
    return 0;
}
static int h_ext(void *c, const nl_frame *f, char *reply, size_t cap)
{
    (void)c;
    snprintf(ext_seen, sizeof ext_seen, "%s/%d", f->type, f->argc);
    if (!ext_take) return 0;
    if (!strcmp(f->type, "PRESCFG")) { snprintf(reply, cap, "OK PRESCFG near=1000 far=1300 min=60 zones=2 on=3 off=8 stale=1500"); return 1; }
    if (!strcmp(f->type, "SENSOR")) { snprintf(reply, cap, "OK SENSOR en=1 st=ranging"); return 1; }
    if (!strcmp(f->type, "TOFX")) { snprintf(reply, cap, "ERR TOFX nope"); return 1; }
    return 0;
}
static const nd_hal HAL = { NULL, h_room, h_queued, h_write, h_status, NULL, NULL, h_power,
                            NULL, NULL, NULL, NULL, NULL, NULL, h_diag, NULL, h_page, h_ext };

static uint32_t hseq;
static void host_send(nd_dev *d, const char *type, const char *args, uint32_t now)
{
    char line[NL_LINE_MAX + 2];
    int n = nl_format(line, sizeof line, ++hseq, type, args);
    check("host frame formats", n > 0);
    if (n > 0) nd_uart_input(d, (const uint8_t *)line, (size_t)n, now);
}

/* Parse everything sent since the last call.  Counts EV pon/poff, returns the last frame of `type`. */
static int npon, npoff, nother_ev, nbad;
static int scan(const char *type, nl_frame *out)
{
    static nl_frame f;
    char *hit = NULL;
    size_t hit_n = 0;
    npon = npoff = nother_ev = nbad = 0;
    char *p = uart, *end = uart + uart_n;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) { nbad++; break; }
        if (nl_parse(p, (size_t)(nl - p), &f) != NL_OK) nbad++;
        else {
            if (!strcmp(f.type, "EV")) {
                if (!strcmp(f.argv[0], "pon")) npon++;
                else if (!strcmp(f.argv[0], "poff")) npoff++;
                else nother_ev++;
            }
            if (type && !strcmp(f.type, type)) { hit = p; hit_n = (size_t)(nl - p); }
        }
        p = nl + 1;
    }
    uart_n = 0;
    return hit && out && nl_parse(hit, hit_n, out) == NL_OK;
}

int main(void)
{
    static nd_dev d;
    nd_cfg cfg;
    nl_frame f;
    nd_cfg_defaults(&cfg);
    nd_init(&d, &HAL, 0x0badf00du, "wt32-r3.0.0", "vl53l5cx", 2, &cfg, 0);

    /* no host yet: nothing but HELLO, whatever the sensor says */
    nd_presence(&d, 1, 1);
    nd_poll(&d, 0);
    scan("HELLO", &f);
    check("no host: no presence event", npon == 0 && npoff == 0);
    check("HELLO sensor=vl53l5cx", !strcmp(nl_get(&f, "sensor"), "vl53l5cx"));

    /* host arrives, sensor not known yet */
    nd_presence(&d, 0, 0);
    host_send(&d, "HOST", "proto=1 role=linkd tun=1", 10);
    nd_poll(&d, 10);
    scan(NULL, NULL);
    check("sensor unknown: nothing sent", npon == 0 && npoff == 0 && !nbad);
    for (uint32_t t = 20; t < 3000; t += 100) { host_send(&d, "HHB", NULL, t); nd_poll(&d, t); }
    scan(NULL, NULL);
    check("sensor unknown for 3 s: still nothing", npon == 0 && npoff == 0);

    /* first good frame: absent -> poff once (the host learns the level) */
    nd_presence(&d, 1, 0);
    nd_poll(&d, 3000);
    scan(NULL, NULL);
    check("known, absent: one poff", npoff == 1 && npon == 0);
    for (uint32_t t = 3000; t < 4000; t += 50) { host_send(&d, "HHB", NULL, t); nd_poll(&d, t); }
    scan(NULL, NULL);
    check("level unchanged: nothing more", npon == 0 && npoff == 0);

    /* level changes */
    nd_presence(&d, 1, 1);
    nd_poll(&d, 4000);
    check("rise: pon", scan("EV", &f) && npon == 1 && !strcmp(nl_get(&f, "t"), "4000"));
    nd_poll(&d, 4001);
    scan(NULL, NULL);
    check("pon once", npon == 0);
    nd_presence(&d, 1, 0);
    nd_poll(&d, 4100);
    scan(NULL, NULL);
    check("fall: poff", npoff == 1 && npon == 0);
    check("counters", d.pres_tx == 3 && d.ev_dropped == 0);

    /* HOST again (linkd restarted): resync the current level */
    nd_presence(&d, 1, 1);
    nd_poll(&d, 4200);
    scan(NULL, NULL);
    host_send(&d, "HOST", "proto=1 role=linkd tun=1", 4300);
    nd_poll(&d, 4300);
    {   /* order matters: a host waiting for HELLO drops any EV before it */
        char *h = strstr(uart, " HELLO "), *e = strstr(uart, " EV pon ");
        check("HOST: HELLO goes out before the resync EV", h && (!e || h < e));
    }
    nd_poll(&d, 4301);
    scan(NULL, NULL);
    check("HOST: current level re-sent (pon) after HELLO", npon == 1 && npoff == 0);
    /* and through the real host session: a fresh nh_session accepts it */
    {
        nh_session hs;
        nh_event ev;
        memset(&hs, 0, sizeof hs);
        nh_open(&hs, "linkd", 0, 0.0);
        host_send(&d, "HOST", "proto=1 role=linkd tun=1", 4400);
        nd_poll(&d, 4400);
        nd_poll(&d, 4401);
        int got_pon = 0, early = 0;
        char *p = uart, *end = uart + uart_n;
        static nl_frame g;
        while (p < end) {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            if (nl_parse(p, (size_t)(nl - p), &g) == NL_OK) {
                int r = nh_frame(&hs, &g, p, (size_t)(nl - p) + 1, 1.0, &ev);
                if (r == NH_F_EVENT && ev.kind == NL_EV_PON) got_pon = 1;
                if (r == NH_F_IGNORED && !strcmp(g.type, "EV")) early = 1;
            }
            p = nl + 1;
        }
        uart_n = 0;
        check("fresh host session: resync pon accepted, not dropped as early", got_pon && !early);
    }

    /* host silent > NL_PEER_TIMEOUT_MS: nothing sent; changes are remembered as the level */
    nd_poll(&d, 4400 + NL_PEER_TIMEOUT_MS + 10);
    check("host considered gone", !d.host_alive);
    nd_presence(&d, 1, 0);
    nd_poll(&d, 9000);
    nd_presence(&d, 1, 1);
    nd_poll(&d, 9100);
    nd_presence(&d, 1, 0);
    nd_poll(&d, 9200);
    scan(NULL, NULL);
    check("no host: nothing sent", npon == 0 && npoff == 0);
    host_send(&d, "HHB", NULL, 9300);              /* host back without a new HOST */
    nd_poll(&d, 9300);
    scan(NULL, NULL);
    check("host back after silence: current level (poff) re-sent once", npoff == 1 && npon == 0);

    /* the event bucket: buttons flood it, presence waits and is retried, never counted as dropped */
    uint32_t t = 10000;
    host_send(&d, "HHB", NULL, t);
    for (int i = 0; i < 12; i++) nd_button(&d, 0, (i & 1) ? -1 : 1, t);
    uint32_t dropped = d.ev_dropped;
    nd_presence(&d, 1, 1);
    nd_poll(&d, t);
    scan(NULL, NULL);
    check("bucket empty: presence not sent yet", npon == 0 && d.pres_retry >= 1);
    check("presence refusal is not an event drop", d.ev_dropped == dropped);
    int sent_at = -1;
    for (uint32_t k = 1; k <= 1000 && sent_at < 0; k++) {
        if (k % 100 == 0) host_send(&d, "HHB", NULL, t + k);
        nd_poll(&d, t + k);
        scan(NULL, NULL);
        if (npon) sent_at = (int)k;
    }
    check("presence sent once the bucket refills (< 200 ms)", sent_at > 0 && sent_at < 200);

    /* host side: nh_frame accepts pon/poff; unknown names stay ignored (a pre-r3 linkd ignores pon/poff) */
    check("name table", nl_ev_from_name("pon") == NL_EV_PON && nl_ev_from_name("poff") == NL_EV_POFF &&
                            nl_ev_from_name("tilt") == NL_EV_TILT && nl_ev_from_name("zzz") == NL_EV_NONE);
    {
        nh_session s;
        nh_event ev;
        nl_frame g;
        char line[NL_LINE_MAX + 2];
        memset(&s, 0, sizeof s);
        nh_open(&s, "linkd", 0, 0.0);
        int n = nl_format(line, sizeof line, 1, "HELLO", "boot=0badf00d proto=1 fw=wt32-r3.0.0 sensor=vl53l5cx btn=2 wifi=up tun=1");
        nl_parse(line, (size_t)n - 1, &g);
        check("host: HELLO", nh_frame(&s, &g, line, (size_t)n, 0.1, &ev) == NH_F_HELLO);
        n = nl_format(line, sizeof line, 2, "EV", "pon t=100");
        nl_parse(line, (size_t)n - 1, &g);
        check("host: EV pon accepted", nh_frame(&s, &g, line, (size_t)n, 0.2, &ev) == NH_F_EVENT && ev.kind == NL_EV_PON);
        n = nl_format(line, sizeof line, 3, "EV", "poff t=200");
        nl_parse(line, (size_t)n - 1, &g);
        check("host: EV poff accepted", nh_frame(&s, &g, line, (size_t)n, 0.3, &ev) == NH_F_EVENT && ev.kind == NL_EV_POFF);
        n = nl_format(line, sizeof line, 4, "EV", "zzz t=300");
        nl_parse(line, (size_t)n - 1, &g);
        check("host: unknown event name ignored (old-linkd behaviour for new names)", nh_frame(&s, &g, line, (size_t)n, 0.4, &ev) == NH_F_IGNORED);
    }

    /* DIAG pages */
    t = 20000;
    host_send(&d, "HOST", "proto=1 role=linkd tun=1", t);
    nd_poll(&d, t);
    scan(NULL, NULL);
    host_send(&d, "DIAG", NULL, t);
    nd_poll(&d, t);
    check("plain DIAG", scan("OK", &f) && f.argc == 7 && !strcmp(f.argv[0], "DIAG") &&
                            !strcmp(f.argv[6], "sensor=vl53l5cx;tof=ranging;pr=0"));
    host_send(&d, "DIAG", "p=tof", t);
    nd_poll(&d, t);
    check("DIAG p=tof: one joined token, no trailing ';'", scan("OK", &f) && f.argc == 7 && !strcmp(f.argv[6], "pg=tof;tof=ranging;f=10"));
    d.rx.frames = d.rx.noise = d.rx.bad_crc = d.rx.bad_syntax = d.rx.too_long = 0xfffffff0u;   /* the widest DIAG header */
    host_send(&d, "DIAG", "p=zg", t);
    nd_poll(&d, t);
    check("DIAG p=zg: whole in the widest reply", scan("OK", &f) && f.argc == 7 && strlen(f.argv[6]) == 98 && strstr(f.argv[6], "pg=zg;tof=ranging;nm=4000;zf=") == f.argv[6] &&
                                                 !strcmp(f.argv[6] + strlen(f.argv[6]) - 3, "fa0"));
    d.rx.frames = d.rx.noise = d.rx.bad_crc = d.rx.bad_syntax = d.rx.too_long = 0;
    host_send(&d, "DIAG", "p=bogus", t);
    nd_poll(&d, t);
    check("unknown page: ERR DIAG page", scan("ERR", &f) && f.argc == 2 && !strcmp(f.argv[0], "DIAG") && !strcmp(f.argv[1], "page"));
    const char *badp[] = { "p=TOF", "p=toolongname", "p=", "x=1", "p=tof q=1", "p=a;b" };
    int allbad = 1;
    for (unsigned i = 0; i < sizeof badp / sizeof badp[0]; i++) {
        host_send(&d, "DIAG", badp[i], t);
        nd_poll(&d, t);
        if (!scan("ERR", &f) || strcmp(f.argv[1], "page")) { allbad = 0; printf("  page arg %s accepted\n", badp[i]); }
    }
    check("malformed page arguments refused", allbad);
    /* a long page is cut to one valid frame */
    static char longp[600];
    memset(longp, 0, sizeof longp);
    for (int i = 0; i < 45; i++) strcat(longp, "abcdefg=123 ");   /* 540 chars: longer than any reply */
    page_text = longp;
    host_send(&d, "DIAG", "p=net", t);
    nd_poll(&d, t);
    check("long page: one valid frame, cut, <= 10 args", scan("OK", &f) && !nbad && f.argc == 7 && !strncmp(f.argv[6], "pg=net;abcdefg=123;", 19));

    /* extension commands */
    host_send(&d, "PRESCFG", "near=900", t);
    nd_poll(&d, t);
    check("PRESCFG reaches ext_cmd", !strcmp(ext_seen, "PRESCFG/1"));
    check("PRESCFG reply is one frame <= 10 args", scan("OK", &f) && f.argc == 8 && !strcmp(f.argv[0], "PRESCFG"));
    host_send(&d, "SENSOR", "en=1", t);
    nd_poll(&d, t);
    check("SENSOR reply", scan("OK", &f) && f.argc == 3 && !strcmp(f.argv[1], "en=1"));
    host_send(&d, "TOFX", NULL, t);
    nd_poll(&d, t);
    check("a T-command that is not a tunnel frame reaches ext_cmd", scan("ERR", &f) && !strcmp(f.argv[0], "TOFX"));
    ext_take = 0;
    host_send(&d, "XYZZY", "a=1", t);
    nd_poll(&d, t);
    scan(NULL, NULL);
    check("unknown command: offered, no reply", !strcmp(ext_seen, "XYZZY/1") && uart_n == 0);
    ext_seen[0] = 0;
    host_send(&d, "TCLOSE", "id=99", t);
    host_send(&d, "TACK", "id=99 n=5", t);
    nd_poll(&d, t);
    scan(NULL, NULL);
    check("tunnel frames are not offered to ext_cmd", ext_seen[0] == 0);
    host_send(&d, "OTAKEY", "h=0123456789abcdef0123456789abcdef", t);
    nd_poll(&d, t);
    check("OTAKEY without a HAL: ERR OTAKEY noota (unchanged)", scan("ERR", &f) && !strcmp(f.argv[1], "noota"));

    printf("test_wt32r3: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
