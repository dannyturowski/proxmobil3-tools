/* Native unit tests for shared/nori_link: framing, input conditioning,
 * device core and host session.  Build+run: tests/run_tests.sh */
#include "nori_dev.h"
#include "nori_host.h"
#include "nori_input.h"
#include "nori_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ------------------------------------------------------------------ framing */
typedef struct { int n; char type[8][12]; uint32_t seq[8]; char raw[8][NL_LINE_MAX + 1]; } Got;
static void got_cb(void *ctx, const nl_frame *f, const char *raw, size_t len)
{
    Got *g = (Got *)ctx;
    if (g->n < 8) {
        snprintf(g->type[g->n], sizeof g->type[0], "%s", f->type);
        g->seq[g->n] = f->seq;
        memcpy(g->raw[g->n], raw, len); g->raw[g->n][len] = 0;
    }
    g->n++;
}
static int feed_str(nl_rx *r, Got *g, const char *s) { return (int)nl_rx_feed(r, (const uint8_t *)s, strlen(s), got_cb, g, 0); }

static void test_framing(void)
{
    CHECK(nl_crc16("123456789", 9) == 0x29B1);          /* CRC-16/CCITT-FALSE check value */

    char b[64];
    CHECK(nl_b64_encode((const uint8_t *)"Ma", 2, b, sizeof b) == 4 && !strcmp(b, "TWE="));
    CHECK(nl_b64_encode((const uint8_t *)"", 0, b, sizeof b) == 0 || b[0] == 0);
    uint8_t raw[256], back[256];
    for (int i = 0; i < 256; i++) raw[i] = (uint8_t)(i * 37 + 11);
    for (int len = 1; len <= NL_TUN_CHUNK; len += 7) {
        char e[NL_TUN_CHUNK / 3 * 4 + 8];
        CHECK(nl_b64_encode(raw, (size_t)len, e, sizeof e) > 0);
        CHECK(nl_b64_decode(e, back, sizeof back) == len && !memcmp(raw, back, (size_t)len));
    }
    CHECK(nl_b64_decode("TWE", back, sizeof back) == -1);
    CHECK(nl_b64_decode("T=E=", back, sizeof back) == -1);
    CHECK(nl_b64_decode("TW?=", back, sizeof back) == -1);
    CHECK(nl_b64_decode("TWFu", back, 2) == -1);        /* output too small */

    char line[NL_LINE_MAX + 2];
    int n = nl_format(line, sizeof line, 7, "EV", "tap t=1234");
    CHECK(n > 0 && line[n - 1] == '\n' && !strncmp(line, "$NL1 7 EV tap t=1234*", 21));
    nl_frame f;
    CHECK(nl_parse(line, (size_t)n - 1, &f) == NL_OK && f.seq == 7 && !strcmp(f.type, "EV") && f.argc == 2);
    uint32_t v;
    CHECK(nl_get_u32(&f, "t", &v) == 0 && v == 1234 && nl_get(&f, "x") == NULL);
    /* invalid inputs */
    CHECK(nl_format(line, sizeof line, 1, "ev", NULL) < 0);
    CHECK(nl_format(line, sizeof line, 1, "EV", "a  b") < 0);
    CHECK(nl_format(line, sizeof line, 1, "EV", "a$b") < 0);
    CHECK(nl_format(line, sizeof line, 1, "EV", "a*b") < 0);
    CHECK(nl_format(line, sizeof line, 1, "EV", "tail ") < 0);
    char big[NL_LINE_MAX + 10];
    memset(big, 'a', sizeof big); big[sizeof big - 1] = 0;
    CHECK(nl_format(line, sizeof line, 1, "EV", big) < 0);
    CHECK(nl_format(line, 10, 1, "EV", NULL) < 0);
    /* shortest frame */
    n = nl_format(line, sizeof line, 0, "HB", NULL);
    CHECK(n == 15 && nl_parse(line, 14, &f) == NL_OK);
    /* corrupted CRC / magic / seq */
    n = nl_format(line, sizeof line, 3, "HB", "up=5");
    line[6] = '9';
    CHECK(nl_parse(line, (size_t)n - 1, &f) == NL_E_CRC);
    CHECK(nl_parse("$XX1 1 HB*0000", 14, &f) != NL_OK);
    CHECK(nl_parse("hello", 5, &f) == NL_E_NOISE);
    CHECK(nl_parse_u32("4294967295", &v) == 0 && v == 4294967295u);
    CHECK(nl_parse_u32("4294967296", &v) == -1 && nl_parse_u32("01", &v) == -1 && nl_parse_u32("", &v) == -1);

    /* stream assembly: ESP32 ROM noise, split packets, CRLF, garbage, oversize */
    nl_rx r; Got g; memset(&g, 0, sizeof g); nl_rx_init(&r);
    feed_str(&r, &g, "ets Jun  8 2016 00:22:57\r\n\r\nrst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\r\n");
    CHECK(g.n == 0 && r.noise == 2);
    n = nl_format(line, sizeof line, 1, "HELLO", "boot=0000abcd proto=1");
    for (int i = 0; i < n; i++) nl_rx_feed(&r, (const uint8_t *)line + i, 1, got_cb, &g, 0);   /* byte by byte */
    CHECK(g.n == 1 && !strcmp(g.type[0], "HELLO") && strlen(g.raw[0]) == (size_t)n - 1);
    char crlf[NL_LINE_MAX + 4];
    n = nl_format(crlf, sizeof crlf, 2, "HB", "boot=0000abcd up=1");
    crlf[n - 1] = '\r'; crlf[n] = '\n'; crlf[n + 1] = 0;
    feed_str(&r, &g, crlf);
    CHECK(g.n == 2);
    feed_str(&r, &g, "garbage$NL1 junk\n");          /* '$' restarts, but the frame is bad */
    CHECK(g.n == 2 && r.bad_syntax == 1);
    n = nl_format(line, sizeof line, 3, "EV", "tap t=9");
    char glued[2 * NL_LINE_MAX];
    snprintf(glued, sizeof glued, "\x01\xff noise %s", line);      /* binary junk then a frame, same line */
    feed_str(&r, &g, glued);
    CHECK(g.n == 3 && g.seq[2] == 3);
    char huge[NL_LINE_MAX * 2 + 4];
    huge[0] = '$'; memset(huge + 1, 'A', sizeof huge - 3); huge[sizeof huge - 2] = '\n'; huge[sizeof huge - 1] = 0;
    feed_str(&r, &g, huge);
    CHECK(g.n == 3 && r.too_long == 1);
    feed_str(&r, &g, line);                          /* recovers after oversize */
    CHECK(g.n == 4);
    char ctl[64];
    snprintf(ctl, sizeof ctl, "$NL1 4 EV t\x07p*0000\n");
    feed_str(&r, &g, ctl);
    CHECK(g.n == 4);
    /* bounded feed: max_frames */
    char two[2 * NL_LINE_MAX];
    int a = nl_format(two, sizeof two, 10, "HB", NULL);
    nl_format(two + a, sizeof two - (size_t)a, 11, "HB", NULL);
    Got g2; memset(&g2, 0, sizeof g2);
    size_t used = nl_rx_feed(&r, (const uint8_t *)two, strlen(two), got_cb, &g2, 1);
    CHECK(g2.n == 1 && used == (size_t)a);
    nl_rx_feed(&r, (const uint8_t *)two + used, strlen(two) - used, got_cb, &g2, 1);
    CHECK(g2.n == 2 && g2.seq[1] == 11);
}

/* ------------------------------------------------------------------ input */
static void test_buttons(void)
{
    ni_button b;
    ni_button_init(&b, 0, 0);
    CHECK(ni_button_update(&b, 1, 10, 30) == 0);
    CHECK(ni_button_update(&b, 0, 15, 30) == 0);       /* bounce */
    CHECK(ni_button_update(&b, 1, 20, 30) == 0);
    CHECK(ni_button_update(&b, 1, 45, 30) == 0);       /* only 25 ms stable */
    CHECK(ni_button_update(&b, 1, 50, 30) == +1);
    int ev = 0;
    for (uint32_t t = 55; t < 20000; t += 5) ev |= ni_button_update(&b, 1, t, 30);
    CHECK(ev == 0);                                    /* holding never repeats */
    CHECK(ni_button_update(&b, 0, 20000, 30) == 0);
    CHECK(ni_button_update(&b, 0, 20031, 30) == -1);
    /* pressed (or shorted) at boot: silent until released once */
    ni_button_init(&b, 1, 0);
    int any = 0;
    for (uint32_t t = 0; t < 5000; t += 5) any |= ni_button_update(&b, 1, t, 30);
    CHECK(any == 0);
    for (uint32_t t = 5000; t < 5100; t += 5) any |= ni_button_update(&b, 0, t, 30);
    CHECK(any == 0 && b.armed);
    int got = 0;
    for (uint32_t t = 5100; t < 5200; t += 5) { int e = ni_button_update(&b, 1, t, 30); if (e) got = e; }
    CHECK(got == +1);
}

static void test_taps(void)
{
    ni_tap_cfg c; ni_tap t;
    ni_tap_defaults(&c);
    ni_tap_init(&t);
    /* single: reported only after the double-tap wait */
    CHECK(ni_tap_update(&t, &c, 1, 0, 1000) == NI_NONE);
    CHECK(ni_tap_update(&t, &c, 0, 0, 1000 + c.dtap_wait_ms - 1) == NI_NONE);
    CHECK(ni_tap_update(&t, &c, 0, 0, 1000 + c.dtap_wait_ms) == NI_TAP);
    /* refractory: a tap right after is ignored */
    CHECK(ni_tap_update(&t, &c, 1, 0, 1500) == NI_NONE);
    CHECK(ni_tap_update(&t, &c, 0, 0, 2500) == NI_NONE);
    /* hardware double tap */
    CHECK(ni_tap_update(&t, &c, 1, 0, 5000) == NI_NONE);
    CHECK(ni_tap_update(&t, &c, 0, 1, 5200) == NI_DTAP);
    CHECK(ni_tap_update(&t, &c, 0, 0, 5000 + c.dtap_wait_ms + 5) == NI_NONE);  /* no trailing single */
    /* two singles inside the wait also count as a double */
    CHECK(ni_tap_update(&t, &c, 1, 0, 8000) == NI_NONE);
    CHECK(ni_tap_update(&t, &c, 1, 0, 8250) == NI_DTAP);
    /* fast mode */
    ni_tap_cfg fast = c; fast.dtap_wait_ms = 0;
    ni_tap_init(&t);
    CHECK(ni_tap_update(&t, &fast, 1, 0, 100) == NI_TAP);
    /* shaking: a burst of hits is suppressed entirely, then quiet re-arms */
    ni_tap_init(&t);
    int out = 0;
    for (uint32_t k = 0; k < 12; k++) {
        int e = ni_tap_update(&t, &c, 1, k % 3 == 0, 10000 + k * 150);
        if (k >= 3 && e) out++;                         /* the first couple may report before shake is detected */
    }
    CHECK(out == 0 && t.muted);
    CHECK(ni_tap_update(&t, &c, 0, 0, 10000 + 11 * 150 + c.shake_quiet_ms - 10) == NI_NONE && t.muted);
    CHECK(ni_tap_update(&t, &c, 0, 0, 10000 + 11 * 150 + c.shake_quiet_ms + 1) == NI_NONE && !t.muted);
    CHECK(ni_tap_update(&t, &c, 1, 0, 20000) == NI_NONE);
    CHECK(ni_tap_update(&t, &c, 0, 0, 20000 + c.dtap_wait_ms) == NI_TAP);
    /* 32-bit millisecond wrap */
    ni_tap_init(&t);
    uint32_t w = 0xFFFFFF00u;
    CHECK(ni_tap_update(&t, &c, 1, 0, w) == NI_NONE);
    CHECK(ni_tap_update(&t, &c, 0, 0, w + c.dtap_wait_ms) == NI_TAP);
    CHECK(ni_tap_update(&t, &c, 1, 0, w + c.dtap_wait_ms + 10) == NI_NONE);   /* refractory across wrap */
    /* token bucket */
    ni_bucket bk; memset(&bk, 0, sizeof bk);
    int ok = 0;
    for (int i = 0; i < 10; i++) ok += ni_bucket_take(&bk, 1000, 5, 3);
    CHECK(ok == 3);
    CHECK(ni_bucket_take(&bk, 1100, 5, 3) == 0 && ni_bucket_take(&bk, 1250, 5, 3) == 1);
}

/* ------------------------------------------------------------------ R4: tap policy + polled motion detector */
static void test_r4_tap_policy(void)
{
    ni_tap_cfg g; ni_tap t;
    ni_tap_game_defaults(&g);
    CHECK(g.dtap_wait_ms == 0 && g.refractory_ms >= 60 && g.refractory_ms <= 100);
    ni_tap_init(&t);
    /* immediate single, then the refractory swallows the ringing of the same knock */
    CHECK(ni_tap_update(&t, &g, 1, 0, 1000) == NI_TAP);
    CHECK(ni_tap_update(&t, &g, 1, 0, 1000 + g.refractory_ms - 1) == NI_NONE);
    /* a chip double flag after the first tap was reported: one more TAP, never a DTAP */
    CHECK(ni_tap_update(&t, &g, 0, 1, 1250) == NI_TAP);
    CHECK(ni_tap_update(&t, &g, 0, 0, 2000) == NI_NONE);
    /* both flags before anything was reported: the whole gesture, once */
    ni_tap_init(&t);
    CHECK(ni_tap_update(&t, &g, 1, 1, 3000) == NI_DTAP);
    CHECK(ni_tap_update(&t, &g, 0, 0, 3500) == NI_NONE);
    /* fast player: 6 taps/s all reported */
    ni_tap_init(&t);
    int n = 0;
    for (uint32_t k = 0; k < 4; k++) n += ni_tap_update(&t, &g, 1, 0, 5000 + k * 166) == NI_TAP;
    CHECK(n == 4);
    /* delayed mode (legacy 420 ms): a matured single followed by the chip's late double flag
     * is a second TAP, not TAP + DTAP for one gesture */
    ni_tap_cfg c; ni_tap_defaults(&c);
    ni_tap_init(&t);
    CHECK(ni_tap_update(&t, &c, 1, 0, 10000) == NI_NONE);
    CHECK(ni_tap_update(&t, &c, 0, 0, 10000 + c.dtap_wait_ms) == NI_TAP);
    uint32_t late = 10000 + c.dtap_wait_ms + c.refractory_ms + 10;
    CHECK(ni_tap_update(&t, &c, 0, 1, late) == NI_NONE);                  /* a new single, waiting like any other */
    CHECK(ni_tap_update(&t, &c, 0, 0, late + c.dtap_wait_ms) == NI_TAP);
    /* a fast player keeps going: 7 taps/s for 3 s, every one reported */
    ni_tap_init(&t);
    n = 0;
    for (uint32_t k = 0; k < 21; k++) n += ni_tap_update(&t, &g, 1, 0, 30000 + k * 143) == NI_TAP;
    CHECK(n == 21 && !t.muted);
    /* shake suppression still bounds a rattling sensor in game mode */
    ni_tap_init(&t);
    int out = 0;
    for (uint32_t k = 0; k < 20; k++) out += ni_tap_update(&t, &g, 1, 0, 20000 + k * 100) != NI_NONE;
    CHECK(out <= g.shake_hits && t.muted);
}

/* 400 Hz stream helpers: gravity g (mg) along a unit-ish direction plus an added pulse */
static int feed(ni_motion *m, const ni_motion_cfg *c, int n, int32_t x, int32_t y, int32_t z)
{
    int hits = 0;
    for (int i = 0; i < n; i++) hits += ni_motion_sample(m, c, x, y, z);
    return hits;
}
/* one knock: a 3-sample spike of amp on z, then a decaying ring (half amplitude, alternating) */
static int knock(ni_motion *m, const ni_motion_cfg *c, int32_t gx, int32_t gy, int32_t gz, int32_t amp, int ring)
{
    int hits = 0;
    hits += ni_motion_sample(m, c, gx, gy, gz + amp / 2);
    hits += ni_motion_sample(m, c, gx, gy, gz + amp);
    hits += ni_motion_sample(m, c, gx, gy, gz + amp / 2);
    int32_t a = amp / 2;
    for (int i = 0; i < ring; i++) {
        hits += ni_motion_sample(m, c, gx + a / 3, gy, gz + ((i & 1) ? a : -a));
        a = a * 7 / 8;
    }
    return hits;
}

static void test_motion(void)
{
    ni_motion_cfg c; ni_motion m;
    ni_motion_defaults(&c, 400);
    CHECK(c.ths_mg == 1000 && c.rearm_mg < c.ths_mg && c.quiet_samples == 16 && c.warmup_samples == 64 && c.hp_shift == 4);
    memset(&m, 0, sizeof m);
    /* start-up: the first samples are a 1 g step from nothing; warm-up must not call it a tap */
    CHECK(feed(&m, &c, 200, 0, 0, 1000) == 0 && m.armed);
    /* gravity in any orientation, held still: nothing */
    memset(&m, 0, sizeof m);
    CHECK(feed(&m, &c, 400, 707, 0, -707) == 0);
    memset(&m, 0, sizeof m);
    CHECK(feed(&m, &c, 200, 0, 0, 1000) == 0);
    /* gravity rejection: rotate 1 g through 180 degrees in 0.5 s (200 samples), a brisk hand turn */
    int hits = 0;
    for (int i = 0; i <= 200; i++) {
        double a = 3.14159265 * i / 200;
        hits += ni_motion_sample(&m, &c, (int32_t)(1000 * __builtin_sin(a)), 0, (int32_t)(1000 * __builtin_cos(a)));
    }
    hits += feed(&m, &c, 100, 0, 0, -1000);
    CHECK(hits == 0);
    /* a slow tilt with sensor noise (+-30 mg): nothing */
    uint32_t seed = 1;
    hits = 0;
    for (int i = 0; i < 2000; i++) {
        seed = seed * 1103515245u + 12345u;
        int32_t nz = (int32_t)((seed >> 16) % 61) - 30;
        hits += ni_motion_sample(&m, &c, i / 4, nz, -1000 + nz);
    }
    CHECK(hits == 0);
    /* one knock with ringing: exactly one hit */
    memset(&m, 0, sizeof m);
    feed(&m, &c, 100, 0, 0, 1000);
    CHECK(knock(&m, &c, 0, 0, 1000, 3000, 30) == 1 && m.hits == 1 && m.peak_mg >= 2500);
    /* quiet re-arm: a knock while the last one still rings is not a new hit ... */
    feed(&m, &c, 50, 0, 0, 1000);
    CHECK(knock(&m, &c, 0, 0, 1000, 3000, 6) == 1);
    CHECK(knock(&m, &c, 0, 0, 1000, 3000, 0) == 0);
    /* ... once it has been calm for the quiet time, the next knock is */
    feed(&m, &c, c.quiet_samples + 8, 0, 0, 1000);
    CHECK(knock(&m, &c, 0, 0, 1000, 3000, 10) == 1);
    /* re-arm latency is short enough for ~6 taps/s: knocks every 160 ms all count */
    feed(&m, &c, 64, 0, 0, 1000);
    hits = 0;
    for (int k = 0; k < 6; k++) { hits += knock(&m, &c, 0, 0, 1000, 2500, 12); hits += feed(&m, &c, 64 - 15, 0, 0, 1000); }
    CHECK(hits == 6);
    /* below threshold: a soft touch is ignored */
    feed(&m, &c, 50, 0, 0, 1000);
    CHECK(knock(&m, &c, 0, 0, 1000, 700, 4) == 0);
    /* sustained vibration (motor, rattle) never re-arms: at most the onset counts */
    hits = 0;
    for (int i = 0; i < 800; i++) hits += ni_motion_sample(&m, &c, 0, 0, 1000 + ((i & 1) ? 1500 : -1500));
    CHECK(hits <= 1);
    feed(&m, &c, 40, 0, 0, 1000);
    CHECK(m.armed);                                  /* and it re-arms once the vibration stops */
    /* reset after a gap: warm-up again, the jump to a new orientation is not a tap */
    ni_motion_reset(&m);
    CHECK(feed(&m, &c, 100, 1000, 0, 0) == 0 && m.hits >= 9);
    /* threshold is configurable */
    ni_motion_cfg hard = c; hard.ths_mg = 4000; hard.rearm_mg = 1600;
    feed(&m, &hard, 50, 1000, 0, 0);
    CHECK(knock(&m, &hard, 1000, 0, 0, 3000, 4) == 0);
    /* clipped full-scale samples (+-8 g on LIS3DSH) are still one hit */
    feed(&m, &c, 50, 1000, 0, 0);
    CHECK(knock(&m, &c, 1000, 0, 0, 7800, 20) == 1);
}

/* ------------------------------------------------------------------ device core + host session */
typedef struct {
    char out[16384]; size_t n;       /* UART bytes written by the device */
    size_t room;
    int wifi, listening, client, accepted_pending;
    uint8_t cl_in[8192]; size_t cl_in_n;      /* bytes the TCP client sends (device reads) */
    uint8_t cl_out[16384]; size_t cl_out_n;   /* bytes the TCP client received */
    size_t cl_write_cap;                      /* how many bytes the client accepts per call */
    char ssid[40], psk[70]; int sets, saves;
    nd_cfg saved;
    char otah[40]; int ota_sets, ota_fail;    /* OTAKEY: last stored hash; ota_fail makes the store fail */
} FakeHal;
static FakeHal F;
static size_t f_room(void *c) { (void)c; return F.room; }
static size_t f_queued(void *c) { (void)c; return 0; }
static void f_write(void *c, const char *s, size_t n) { (void)c; if (F.n + n < sizeof F.out) { memcpy(F.out + F.n, s, n); F.n += n; } }
static int f_wifi(void *c, char *ip, size_t cap, int *rssi) { (void)c; if (F.wifi == ND_WIFI_UP) { snprintf(ip, cap, "10.0.0.7"); *rssi = -55; } return F.wifi; }
static int f_set(void *c, const char *s, const char *p) { (void)c; snprintf(F.ssid, sizeof F.ssid, "%s", s); snprintf(F.psk, sizeof F.psk, "%s", p); F.sets++; return 0; }
static int f_clear(void *c) { (void)c; F.ssid[0] = F.psk[0] = 0; return 0; }
static void f_power(void *c, int on) { (void)c; if (!on) F.wifi = ND_WIFI_OFF; }
static void f_listen(void *c, int on) { (void)c; F.listening = on; }
static int f_accept(void *c) { (void)c; if (F.accepted_pending) { F.accepted_pending = 0; F.client = 1; return 1; } return 0; }
static int f_read(void *c, uint8_t *b, size_t max) { (void)c; if (!F.client) return -1; size_t k = F.cl_in_n < max ? F.cl_in_n : max; memcpy(b, F.cl_in, k); memmove(F.cl_in, F.cl_in + k, F.cl_in_n - k); F.cl_in_n -= k; return (int)k; }
static int f_twrite(void *c, const uint8_t *b, size_t n) { (void)c; if (!F.client) return -1; size_t k = n < F.cl_write_cap ? n : F.cl_write_cap; memcpy(F.cl_out + F.cl_out_n, b, k); F.cl_out_n += k; return (int)k; }
static void f_close(void *c) { (void)c; F.client = 0; }
static int f_save(void *c, const nd_cfg *cfg) { (void)c; F.saved = *cfg; F.saves++; return 0; }
static void f_diag(void *c, char *out, size_t cap) { (void)c; snprintf(out, cap, "ota=%s", F.otah[0] ? "ready" : "nokey"); }
static int f_otakey(void *c, const char *h) { (void)c; if (F.ota_fail) return -1; snprintf(F.otah, sizeof F.otah, "%s", h); F.ota_sets++; return 0; }
static const nd_hal HAL = { NULL, f_room, f_queued, f_write, f_wifi, f_set, f_clear, f_power, f_listen, f_accept, f_read, f_twrite, f_close, f_save, f_diag, f_otakey };
/* an R1-style HAL: positional initializer without the r2 member (zero-filled) */
static const nd_hal HAL_R1 = { NULL, f_room, f_queued, f_write, f_wifi, f_set, f_clear, f_power, f_listen, f_accept, f_read, f_twrite, f_close, f_save, NULL, NULL };

static nl_rx HRX;
static void copy_frame(nl_frame *dst, const nl_frame *src)   /* argv/type point into store: rebase them */
{
    *dst = *src;
    dst->type = dst->store + (src->type - src->store);
    for (int i = 0; i < src->argc; i++) dst->argv[i] = dst->store + (src->argv[i] - src->store);
}
/* host side of the test: parse what the device wrote */
typedef struct { nh_session s; int hello, reboot, hb, events, other; int last_ev; nl_frame last; char last_raw[NL_LINE_MAX + 1]; double now; } Host;
static void host_cb(void *ctx, const nl_frame *f, const char *raw, size_t len)
{
    Host *h = (Host *)ctx;
    nh_event ev;
    int r = nh_frame(&h->s, f, raw, len, h->now, &ev);
    if (getenv("TRACE")) fprintf(stderr, "  dev> %.*s  [%d]\n", (int)len, raw, r);
    if (r == NH_F_HELLO) h->hello++;
    else if (r == NH_F_REBOOT) h->reboot++;
    else if (r == NH_F_HB) h->hb++;
    else if (r == NH_F_EVENT) { h->events++; h->last_ev = ev.kind; }
    else if (r == NH_F_OTHER) { h->other++; copy_frame(&h->last, f); memcpy(h->last_raw, raw, len); h->last_raw[len] = 0; }
}
static void host_drain(Host *h) { nl_rx_feed(&HRX, (const uint8_t *)F.out, F.n, host_cb, h, 0); F.n = 0; }
static void to_dev(nd_dev *d, uint32_t now, const char *type, const char *args)
{
    char line[NL_LINE_MAX + 2];
    static uint32_t hseq;
    int n = nl_format(line, sizeof line, ++hseq, type, args);
    CHECK(n > 0);
    nd_uart_input(d, (const uint8_t *)line, (size_t)n, now);
}

static void test_device_core(void)
{
    static nd_dev d;
    static Host h;
    memset(&F, 0, sizeof F); F.room = 4096; F.cl_write_cap = 100000;
    memset(&h, 0, sizeof h); nl_rx_init(&HRX);
    nd_cfg cfg; nd_cfg_defaults(&cfg);
    nd_init(&d, &HAL, 0x1234abcd, "r2.0.0", "none", 2, &cfg, 0);
    nh_open(&h.s, "app", 0, 0);

    /* announce: HELLO only, every 3 s, no HB, no events */
    nd_poll(&d, 0); nd_event(&d, NL_EV_TAP, 10); nd_poll(&d, 10);
    for (uint32_t t = 100; t < 7000; t += 100) nd_poll(&d, t);
    host_drain(&h);
    CHECK(h.hello == 3 && h.hb == 0 && h.events == 0);     /* t=0, 3 s, 6 s announcements only */
    CHECK(HRX.frames == 3);
    CHECK(d.ev_dropped == 1);

    /* host handshake: HOST -> HELLO, then HB every second, events flow */
    to_dev(&d, 7000, "HOST", "proto=1 role=app tun=0");
    nd_poll(&d, 7000);
    nd_poll(&d, 8000);
    nd_event(&d, NL_EV_TAP, 8100); nd_event(&d, NL_EV_B2D, 8150);
    nd_poll(&d, 8200);
    h.now = 8.2; host_drain(&h);
    CHECK(h.hello == 4 && h.hb == 1 && h.events == 2 && h.last_ev == NL_EV_B2D);

    /* wrong protocol version is refused */
    to_dev(&d, 8300, "HOST", "proto=2");
    nd_poll(&d, 8300);
    host_drain(&h);
    CHECK(h.other == 1 && !strcmp(h.last.type, "ERR"));

    /* event flood is rate limited, queue bounded */
    for (int i = 0; i < 50; i++) nd_event(&d, NL_EV_TAP, 9000 + (uint32_t)i);
    nd_poll(&d, 9100);
    h.now = 9.1; host_drain(&h);
    CHECK(h.events <= 2 + 6 + 1);                    /* r4: burst 6 */

    /* UART back-pressure: nothing is half-written; queued events go out later */
    F.room = 10;
    nd_event(&d, NL_EV_DTAP, 12000);
    to_dev(&d, 12000, "HHB", NULL);
    nd_poll(&d, 12000);
    CHECK(F.n == 0);
    F.room = 4096;
    nd_poll(&d, 12001);
    int before = h.events;
    h.now = 12.0; host_drain(&h);
    CHECK(h.events == before + 1 && h.last_ev == NL_EV_DTAP);

    /* host silence => device stops HB/events and goes back to announcing */
    nd_poll(&d, 12000 + NL_PEER_TIMEOUT_MS + 10);
    CHECK(!d.host_alive);
    nd_event(&d, NL_EV_TAP, 16000);
    CHECK(d.evn == 0);

    /* provisioning: secrets go to the HAL, never back on the wire */
    char s64[64], p64[100], args[200];
    nl_b64_encode((const uint8_t *)"Home Net", 8, s64, sizeof s64);
    nl_b64_encode((const uint8_t *)"correct horse battery", 21, p64, sizeof p64);
    snprintf(args, sizeof args, "s=%s p=%s", s64, p64);
    to_dev(&d, 17000, "WIFISET", args);
    nd_poll(&d, 17000);
    CHECK(F.sets == 1 && !strcmp(F.ssid, "Home Net") && !strcmp(F.psk, "correct horse battery"));
    CHECK(strstr(F.out, p64) == NULL && strstr(F.out, "horse") == NULL && strstr(F.out, s64) == NULL);
    host_drain(&h);
    CHECK(!strcmp(h.last.type, "OK") && !strcmp(h.last.argv[0], "WIFISET"));
    nl_b64_encode((const uint8_t *)"short", 5, p64, sizeof p64);
    snprintf(args, sizeof args, "s=%s p=%s", s64, p64);
    to_dev(&d, 17100, "WIFISET", args); nd_poll(&d, 17100); host_drain(&h);
    CHECK(!strcmp(h.last.type, "ERR") && F.sets == 1);
    to_dev(&d, 17200, "CFG", "ths=12 wait=300 refr=900"); nd_poll(&d, 17200); host_drain(&h);
    CHECK(!strcmp(h.last.type, "OK") && d.cfg.tap_ths == 12 && d.cfg.dtap_wait_ms == 300 && d.cfg_changed && F.saved.refractory_ms == 900);
    to_dev(&d, 17300, "CFG", "refr=1"); nd_poll(&d, 17300); host_drain(&h);
    CHECK(!strcmp(h.last.type, "ERR") && d.cfg.refractory_ms == 900);
    to_dev(&d, 17400, "DIAG", NULL); nd_poll(&d, 17400); host_drain(&h);
    CHECK(!strcmp(h.last.type, "OK") && !strcmp(h.last.argv[0], "DIAG"));
#if !ND_TAP_AXIS
    /* r5.1: builds without the axis gate refuse its keys like any unknown key, and change nothing */
    to_dev(&d, 17410, "CFG", "axis=3"); nd_poll(&d, 17410); host_drain(&h);
    CHECK(!strcmp(h.last.type, "ERR") && !strcmp(h.last.argv[1], "range") && d.cfg.refractory_ms == 900);
    to_dev(&d, 17420, "CFG", "ths=12 axr=70"); nd_poll(&d, 17420); host_drain(&h);
    CHECK(!strcmp(h.last.type, "ERR") && d.cfg.tap_ths == 12);
    to_dev(&d, 17425, "CFG", "ths=12 wait=300 refr=900"); nd_poll(&d, 17425); host_drain(&h);
    CHECK(strstr(h.last_raw, " OK CFG ths=12 wait=300 refr=900*") != NULL);   /* r5 reply text, unchanged */
#endif
#if !ND_I2CSCAN
    {   /* r5.1: ... and ignore IICSCAN like any unknown command (no reply at all) */
        int other = h.other;
        to_dev(&d, 17430, "IICSCAN", NULL); nd_poll(&d, 17430); host_drain(&h);
        CHECK(h.other == other);
    }
#endif

    /* tunnel: needs opt-in on the device, a tunnel-capable host and Wi-Fi up */
    to_dev(&d, 18000, "HOST", "proto=1 role=linkd tun=1"); nd_poll(&d, 18000);
    CHECK(!F.listening);                                   /* wifi_on still 0 */
    to_dev(&d, 18100, "WIFIEN", "on=1"); nd_poll(&d, 18100);
    CHECK(!F.listening && F.saved.wifi_on == 1);           /* Wi-Fi not up yet */
    F.wifi = ND_WIFI_UP; nd_poll(&d, 18200);
    CHECK(F.listening);
    F.accepted_pending = 1; nd_poll(&d, 18300);
    h.now = 18.3; host_drain(&h);
    CHECK(d.tun == NT_OPENING && !strcmp(h.last.type, "TOPEN"));
    char idarg[32]; uint32_t id; nl_get_u32(&h.last, "id", &id);
    snprintf(idarg, sizeof idarg, "id=%lu", (unsigned long)id);
    /* client data waits until host confirms */
    memcpy(F.cl_in, "SSH-2.0-client\r\n", 16); F.cl_in_n = 16;
    nd_poll(&d, 18400); host_drain(&h);
    CHECK(strcmp(h.last.type, "TDATA") != 0);
    to_dev(&d, 18500, "TOK", idarg); nd_poll(&d, 18500); host_drain(&h);
    CHECK(d.tun == NT_OPEN && !strcmp(h.last.type, "TDATA"));
    uint8_t dec[256]; int dn = nl_b64_decode(nl_get(&h.last, "d"), dec, sizeof dec);
    CHECK(dn == 16 && !memcmp(dec, "SSH-2.0-client\r\n", 16));
    CHECK(nl_get(&h.last, "o") && !strcmp(nl_get(&h.last, "o"), "0"));       /* first chunk starts the stream at 0 */
    CHECK(d.tx_inflight == 16);
    /* window: client sends lots, device stops at NL_TUN_WINDOW unacked */
    for (int i = 0; i < 8000; i++) F.cl_in[i] = (uint8_t)i;
    F.cl_in_n = 8000;
    for (uint32_t t = 18600; t < 19000; t += 10) nd_poll(&d, t);
    CHECK(d.tx_inflight == NL_TUN_WINDOW && F.cl_in_n == 8000 - (NL_TUN_WINDOW - 16));
    char ack[48]; snprintf(ack, sizeof ack, "%s n=%d", idarg, NL_TUN_WINDOW);
    to_dev(&d, 19000, "TACK", ack); nd_poll(&d, 19000);
    CHECK(d.tx_inflight > 0 && F.cl_in_n < 8000 - (NL_TUN_WINDOW - 16));
    host_drain(&h);
    /* host -> client, with a slow client: bytes queue, acks follow delivery */
    F.cl_write_cap = 50;
    char d64[300], targ[400];
    uint8_t payload[NL_TUN_CHUNK];
    for (int i = 0; i < NL_TUN_CHUNK; i++) payload[i] = (uint8_t)(255 - i);
    nl_b64_encode(payload, NL_TUN_CHUNK, d64, sizeof d64);
    uint32_t hoff = 0;                                 /* host->device stream offset */
    snprintf(targ, sizeof targ, "%s o=%lu d=%s", idarg, (unsigned long)hoff, d64); hoff += NL_TUN_CHUNK;
    to_dev(&d, 19100, "TDATA", targ);
    nd_poll(&d, 19100);
    CHECK(F.cl_out_n == 50 * 4 || F.cl_out_n == NL_TUN_CHUNK);
    for (uint32_t t = 19110; t < 19300; t += 10) nd_poll(&d, t);
    CHECK(F.cl_out_n == NL_TUN_CHUNK && !memcmp(F.cl_out, payload, NL_TUN_CHUNK));
    host_drain(&h);
    CHECK(d.tx_off > NL_TUN_WINDOW);                   /* the device->host offset advances with every chunk */
    /* a host that ignores the window is cut off */
    F.cl_write_cap = 0;
    for (int i = 0; i < 30 && d.tun == NT_OPEN; i++) {
        snprintf(targ, sizeof targ, "%s o=%lu d=%s", idarg, (unsigned long)hoff, d64); hoff += NL_TUN_CHUNK;
        to_dev(&d, 19400, "TDATA", targ);
    }
    nd_poll(&d, 19400); host_drain(&h);
    CHECK(d.tun == NT_IDLE && !F.client && !strcmp(h.last.type, "TCLOSE") && !strcmp(nl_get(&h.last, "why"), "overrun"));
    /* a lost line (offset gap) closes the tunnel instead of corrupting the stream */
    F.cl_write_cap = 100000; F.accepted_pending = 1;
    to_dev(&d, 19410, "HHB", NULL); nd_poll(&d, 19410); host_drain(&h);
    nl_get_u32(&h.last, "id", &id); snprintf(idarg, sizeof idarg, "id=%lu", (unsigned long)id);
    to_dev(&d, 19420, "TOK", idarg); nd_poll(&d, 19420);
    snprintf(targ, sizeof targ, "%s o=0 d=%s", idarg, d64); to_dev(&d, 19430, "TDATA", targ); nd_poll(&d, 19430);
    snprintf(targ, sizeof targ, "%s o=%d d=%s", idarg, 2 * NL_TUN_CHUNK, d64); to_dev(&d, 19440, "TDATA", targ);
    nd_poll(&d, 19440); host_drain(&h);
    CHECK(d.tun == NT_IDLE && F.cl_out_n == NL_TUN_CHUNK * 2 && !strcmp(nl_get(&h.last, "why"), "gap"));
    snprintf(targ, sizeof targ, "%s d=%s", idarg, d64);    /* no offset at all: protocol error */
    F.accepted_pending = 1; to_dev(&d, 19450, "HHB", NULL); nd_poll(&d, 19450); host_drain(&h);
    nl_get_u32(&h.last, "id", &id); snprintf(idarg, sizeof idarg, "id=%lu", (unsigned long)id);
    to_dev(&d, 19460, "TOK", idarg); snprintf(targ, sizeof targ, "%s d=%s", idarg, d64); to_dev(&d, 19470, "TDATA", targ);
    nd_poll(&d, 19470); host_drain(&h);
    CHECK(d.tun == NT_IDLE && !strcmp(nl_get(&h.last, "why"), "proto"));
    /* a (re)starting host resets any tunnel it cannot know about */
    F.accepted_pending = 1; to_dev(&d, 19480, "HHB", NULL); nd_poll(&d, 19480); host_drain(&h);
    nl_get_u32(&h.last, "id", &id); snprintf(idarg, sizeof idarg, "id=%lu", (unsigned long)id);
    to_dev(&d, 19490, "TOK", idarg); nd_poll(&d, 19490);
    CHECK(d.tun == NT_OPEN);
    to_dev(&d, 19495, "HOST", "proto=1 role=linkd tun=1"); nd_poll(&d, 19495); host_drain(&h);
    CHECK(d.tun == NT_IDLE && !F.client);
    /* new connection, then Wi-Fi drops: tunnel closes, listener stops */
    F.cl_write_cap = 100000; F.accepted_pending = 1;
    to_dev(&d, 19500, "HHB", NULL); nd_poll(&d, 19500);
    CHECK(d.tun == NT_OPENING && d.tun_id == id + 1);
    uint32_t base_id = id;
    F.wifi = ND_WIFI_JOINING; nd_poll(&d, 19600); host_drain(&h);
    CHECK(d.tun == NT_IDLE && !F.listening && !strcmp(nl_get(&h.last, "why"), "wifi"));
    /* opening timeout */
    F.wifi = ND_WIFI_UP; F.accepted_pending = 1;
    to_dev(&d, 19700, "HHB", NULL); nd_poll(&d, 19700); nd_poll(&d, 19701);
    CHECK(d.tun == NT_OPENING);
    to_dev(&d, 22000, "HHB", NULL); nd_poll(&d, 22000);
    to_dev(&d, 24800, "HHB", NULL); nd_poll(&d, 24800);
    host_drain(&h);
    CHECK(d.tun == NT_IDLE && !strcmp(nl_get(&h.last, "why"), "timeout"));
    /* stale TOK for an old id is ignored */
    CHECK(d.tun_id == base_id + 2);
    to_dev(&d, 24900, "TOK", idarg); nd_poll(&d, 24900);
    CHECK(d.tun == NT_IDLE);
    /* turning Wi-Fi off persists and stops listening */
    to_dev(&d, 25000, "WIFIEN", "on=0"); nd_poll(&d, 25000);
    CHECK(!F.listening && F.saved.wifi_on == 0);
}

/* OTAKEY h=<32 hex>: validated, normalized, stored via the HAL, never echoed */
static char TXLOG[65536]; static size_t TXLOG_N;      /* every byte the device wrote during test_otakey */
static void log_drain(Host *h)
{
    if (TXLOG_N + F.n < sizeof TXLOG) { memcpy(TXLOG + TXLOG_N, F.out, F.n); TXLOG_N += F.n; }
    host_drain(h);
}
static int txlog_has(const char *needle)              /* case-insensitive search */
{
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= TXLOG_N; i++) {
        size_t j = 0;
        while (j < k && (TXLOG[i + j] | 0x20) == (needle[j] | 0x20)) j++;
        if (j == k) return 1;
    }
    return 0;
}
static const char *otakey(nd_dev *d, Host *h, uint32_t now, const char *args)
{
    static char reply[64];
    to_dev(d, now, "OTAKEY", args);
    nd_poll(d, now);
    CHECK(strstr(F.out, "OTAKEY") != NULL);            /* the reply went out */
    log_drain(h);
    snprintf(reply, sizeof reply, "%s %s%s%s", h->last.type, h->last.argc > 0 ? h->last.argv[0] : "",
             h->last.argc > 1 ? " " : "", h->last.argc > 1 ? h->last.argv[1] : "");
    return reply;
}

/* r5.1 finding: a real-hardware DIAG reply carries ~24 firmware tokens.  nl_parse (linkd, app)
 * refuses frames with more than NL_ARGS_MAX args, so r4/r5 builds' DIAG never reached a C host;
 * r5.1 motion builds (ND_TAP_AXIS/ND_I2CSCAN) join those tokens and it does. */
static void f_diag_hw(void *c, char *out, size_t cap)
{
    (void)c;
    snprintf(out, cap, "sensor=lis3dh ws=6 wr=201 begin=0 ap=0 sig=-127 auth=-1 fw=r5.0.0 ota=off otalast=none http=off "
                       "httplast=none part=app0 img=valid tap=w0/r80/t0 mot=400/0/12/1840 i2c=19");
}
typedef struct { int ok; } DiagSeen;
static void diag_cb(void *ctx, const nl_frame *f, const char *raw, size_t len)
{
    (void)raw; (void)len;
    if (!strcmp(f->type, "OK") && f->argc && !strcmp(f->argv[0], "DIAG")) ((DiagSeen *)ctx)->ok++;
}
static void test_diag_args(void)
{
    static nd_dev d;
    nd_hal hal = HAL;
    hal.diag = f_diag_hw;
    memset(&F, 0, sizeof F); F.room = 4096;
    nd_cfg cfg; nd_cfg_defaults(&cfg);
    nd_init(&d, &hal, 1, "r5.0.0", "lis3dh", 2, &cfg, 0);
    char line[64];
    int n = nl_format(line, sizeof line, 1, "DIAG", NULL);
    nd_uart_input(&d, (const uint8_t *)line, (size_t)n, 0);
    F.n = 0;
    nd_poll(&d, 0);
    CHECK(F.n > 0 && strstr(F.out, " OK DIAG ") != NULL);   /* the device does send it ... */
    static nl_rx rx; nl_rx_init(&rx);
    DiagSeen seen = { 0 };
    nl_rx_feed(&rx, (const uint8_t *)F.out, F.n, diag_cb, &seen, 0);
#if ND_TAP_AXIS || ND_I2CSCAN
    CHECK(seen.ok == 1 && rx.bad_syntax == 0);               /* r5.1 motion builds: a host can read it */
#else
    CHECK(seen.ok == 0 && rx.bad_syntax == 1);               /* r5 and the other builds: > 10 args, dropped */
#endif
}

static void test_otakey(void)
{
    static nd_dev d;
    static Host h;
    static const char *K = "0123456789abcdef0123456789ABCDEF", *KL = "0123456789abcdef0123456789abcdef";
    char args[NL_LINE_MAX];
    memset(&F, 0, sizeof F); F.room = 4096; TXLOG_N = 0;
    memset(&h, 0, sizeof h); nl_rx_init(&HRX);
    nd_init(&d, &HAL, 0x0badf00d, "r2.0.0", "none", 2, NULL, 0);
    nh_open(&h.s, "app", 0, 0);
    to_dev(&d, 0, "HOST", "proto=1 role=app tun=0"); nd_poll(&d, 0); host_drain(&h);
    CHECK(h.hello == 1);
#define OTAKEY(args) otakey(&d, &h, 100, args)
    /* invalid input: nothing reaches the HAL, short sanitized reason only */
    CHECK(!strcmp(OTAKEY(NULL), "ERR OTAKEY format"));
    CHECK(!strcmp(OTAKEY("h="), "ERR OTAKEY format"));
    CHECK(!strcmp(OTAKEY("h=0123456789abcdef0123456789abcde"), "ERR OTAKEY format"));     /* 31 */
    CHECK(!strcmp(OTAKEY("h=0123456789abcdef0123456789abcdef0"), "ERR OTAKEY format"));   /* 33 */
    CHECK(!strcmp(OTAKEY("h=0123456789abcdef0123456789abcdeg"), "ERR OTAKEY format"));    /* non-hex */
    CHECK(!strcmp(OTAKEY("h=0x23456789abcdef0123456789abcdef"), "ERR OTAKEY format"));
    CHECK(!strcmp(OTAKEY("k=0123456789abcdef0123456789abcdef"), "ERR OTAKEY format"));    /* wrong key */
    CHECK(!strcmp(OTAKEY("hh=0123456789abcdef0123456789abcdef"), "ERR OTAKEY format"));
    CHECK(!strcmp(OTAKEY("h=0123456789abcdef0123456789abcdef x=1"), "ERR OTAKEY format")); /* extra token */
    CHECK(!strcmp(OTAKEY("h=0123456789abcdef0123456789abcdef h=0123456789abcdef0123456789abcdef"), "ERR OTAKEY format"));
    CHECK(!strcmp(OTAKEY("h=d41d8cd98f00b204e9800998ecf8427e"), "ERR OTAKEY weak"));      /* MD5("") */
    CHECK(!strcmp(OTAKEY("h=D41D8CD98F00B204E9800998ECF8427E"), "ERR OTAKEY weak"));
    CHECK(F.ota_sets == 0 && F.otah[0] == 0);
    /* a CRC-damaged OTAKEY line is dropped by the framing layer: no reply, no store */
    char line[NL_LINE_MAX + 2];
    int n = nl_format(line, sizeof line, 900, "OTAKEY", "h=0123456789abcdef0123456789abcdef");
    line[20] ^= 1;
    uint32_t bad = d.rx.bad_crc;
    nd_uart_input(&d, (const uint8_t *)line, (size_t)n, 150); nd_poll(&d, 150);
    CHECK(d.rx.bad_crc == bad + 1 && F.n == 0 && F.ota_sets == 0);
    /* valid: stored lowercase, plain OK, hash never on the wire nor left in the receive buffers */
    snprintf(args, sizeof args, "h=%s", K);
    CHECK(!strcmp(OTAKEY(args), "OK OTAKEY"));
    CHECK(F.ota_sets == 1 && !strcmp(F.otah, KL));
    CHECK(memmem(d.rx.buf, sizeof d.rx.buf, "0123456789", 10) == NULL);
    CHECK(memmem(d.rx.frame.store, sizeof d.rx.frame.store, "0123456789", 10) == NULL);
    /* rotation overwrites; a failing store is reported, not hidden */
    CHECK(!strcmp(OTAKEY("h=ffffffffffffffffffffffffffffffff"), "OK OTAKEY") && F.ota_sets == 2);
    F.ota_fail = 1;
    CHECK(!strcmp(OTAKEY("h=eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"), "ERR OTAKEY store"));
    F.ota_fail = 0;
    CHECK(!strcmp(F.otah, "ffffffffffffffffffffffffffffffff"));
    /* DIAG reports readiness only */
    to_dev(&d, 200, "DIAG", NULL); nd_poll(&d, 200);
    CHECK(strstr(F.out, " ota=ready") != NULL);
    log_drain(&h);
    /* the link keeps working afterwards: events and heartbeats still flow */
    nd_event(&d, NL_EV_B1D, 300); nd_poll(&d, 1100);
    h.now = 1.1; log_drain(&h);
    CHECK(h.events == 1 && h.last_ev == NL_EV_B1D && h.hb >= 1);
    /* across everything the device sent, no hash (valid, rejected or weak, any case) ever appeared */
    CHECK(TXLOG_N > 500 && txlog_has("ERR OTAKEY format") && txlog_has("DIAG"));
    CHECK(!txlog_has("0123456789") && !txlog_has("ffffffff") && !txlog_has("eeeeeeee") && !txlog_has("d41d8cd9"));
#undef OTAKEY

    /* an R1-style HAL (no OTA support) refuses cleanly */
    static nd_dev d1;
    memset(&F, 0, sizeof F); F.room = 4096; nl_rx_init(&HRX);
    nd_init(&d1, &HAL_R1, 0x0badf00e, "r1.0.0", "none", 2, NULL, 0);
    to_dev(&d1, 0, "HOST", "proto=1 role=app tun=0"); nd_poll(&d1, 0); host_drain(&h);
    snprintf(args, sizeof args, "h=%s", KL);
    CHECK(!strcmp(otakey(&d1, &h, 10, args), "ERR OTAKEY noota") && F.ota_sets == 0);
}

static void test_host_session(void)
{
    nh_session s;
    memset(&s, 0, sizeof s);
    nh_open(&s, "app", 0, 100.0);
    char out[NL_LINE_MAX + 2], line[NL_LINE_MAX + 2];
    int cl;
    CHECK(nh_tick(&s, 100.0, out, sizeof out, &cl) > 0 && strstr(out, " HOST proto=1 role=app tun=0*"));
    CHECK(nh_tick(&s, 100.5, out, sizeof out, &cl) == 0);
    nl_frame f; nh_event ev;
#define FEED(seq, type, args, now) (nl_format(line, sizeof line, seq, type, args), nl_parse(line, strlen(line) - 1, &f), \
                                    nh_frame(&s, &f, line, strlen(line) - 1, now, &ev))
    CHECK(FEED(5, "EV", "tap t=10", 100.6) == NH_F_IGNORED && s.n_early == 1);   /* before HELLO */
    CHECK(FEED(6, "HELLO", "boot=0000beef proto=9", 100.7) == NH_F_IGNORED);     /* future protocol */
    CHECK(FEED(7, "HELLO", "boot=0000beef proto=1 fw=r1.0.0 sensor=adxl345 tun=0", 100.8) == NH_F_HELLO);
    CHECK(s.state == NH_READY && !strcmp(s.sensor, "adxl345"));
    CHECK(FEED(8, "HB", "boot=0000beef up=5000", 101.0) == NH_F_HB && s.dev_ms == 5000);
    CHECK(FEED(9, "EV", "tap t=5100", 101.1) == NH_F_EVENT && ev.kind == NL_EV_TAP);
    CHECK(FEED(9, "EV", "tap t=5100", 101.1) == NH_F_DROPPED && s.n_dup == 1);  /* duplicate seq */
    CHECK(FEED(8, "EV", "dtap t=5150", 101.1) == NH_F_DROPPED);                  /* out of order */
    CHECK(FEED(10, "EV", "tap t=2000", 101.2) == NH_F_DROPPED && s.n_stale == 1); /* stale */
    CHECK(FEED(11, "EV", "bogus t=5200", 101.2) == NH_F_IGNORED);
    CHECK(FEED(12, "EV", "b1d", 101.2) == NH_F_IGNORED);
    CHECK(nh_tick(&s, 101.9, out, sizeof out, &cl) > 0 && strstr(out, " HHB*"));
    /* reboot seen via HB: back to waiting, events refused until HELLO */
    CHECK(FEED(2, "HB", "boot=0000cafe up=300", 102.0) == NH_F_REBOOT && s.state == NH_WAIT_HELLO);
    CHECK(FEED(3, "EV", "tap t=310", 102.0) == NH_F_IGNORED);
    CHECK(FEED(4, "HELLO", "boot=0000cafe proto=1", 102.1) == NH_F_REBOOT && s.state == NH_READY);
    CHECK(FEED(5, "EV", "dtap t=400", 102.2) == NH_F_EVENT && ev.kind == NL_EV_DTAP);  /* low seq ok: new boot */
    /* silence: READY -> WAIT_HELLO after 3.5 s, close after the give-up */
    nh_tick(&s, 106.0, out, sizeof out, &cl);
    CHECK(s.state == NH_WAIT_HELLO && !cl);
    nh_tick(&s, 111.5, out, sizeof out, &cl);
    CHECK(cl == 1);
    /* the session's own frames carry increasing seq across reopen */
    uint32_t before = s.seq_tx, dups = s.n_dup, reboots = s.n_reboot;
    nh_open(&s, "linkd", 1, 200.0);
    CHECK(nh_tick(&s, 200.0, out, sizeof out, &cl) > 0 && s.seq_tx == before + 1 && strstr(out, "role=linkd tun=1"));
    CHECK(s.n_dup == dups && dups == 2 && s.n_reboot == reboots && reboots == 2 && s.state == NH_WAIT_HELLO && !s.have_boot);
}

int main(void)
{
    test_framing();
    test_buttons();
    test_taps();
    test_r4_tap_policy();
    test_motion();
    test_device_core();
    test_diag_args();
    test_otakey();
    test_host_session();
    printf("test_shared: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
