/* wt32-r3 portable sensor / diagnostics modules on the host (shared/nori_sense):
 *   nori_presence  debounce, hysteresis, zone validity, invalid data, bad frames, stale / fault release,
 *                  re-arm on departure, configuration, and a randomised "invalid data never triggers" run;
 *   nori_tofsm     the VL53L5CX supervisor: start delay, absent sensor backoff, happy path, frame timeout,
 *                  bad-frame restarts, init failures with bus clear and doubling backoff, backoff reset,
 *                  pause (OTA), disable, restart, and a 24 h dead-sensor bound on bus traffic;
 *   nori_evlog     ring order and wrap, compact and JSON formatting within their buffers;
 *   nori_rst       the previous-boot trace (take / validate / corruption / event order);
 *   nori_i2c_chunk transaction splitting for the ULD platform layer. */
#include "nori_evlog.h"
#include "nori_i2c_chunk.h"
#include "nori_imgvalid.h"
#include "nori_link.h"
#include "nori_presence.h"
#include "nori_rst.h"
#include "nori_tofsm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static int tokens_ok(const char *s)
{
    const char *p = s;
    while (*p) {
        const char *e = strchr(p, ' ');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!n || !nl_token_ok(p, n)) return 0;
        p += n + (e ? 1 : 0);
    }
    return 1;
}

/* ---------------------------------------------------------------- presence */
static int16_t D[16];
static uint8_t ST[16], NB[16];
static np_frame F = { 16, D, ST, NB };

/* k zones at mm with status st (1 target), the rest "no target" (status 255, 0 targets) */
static const np_frame *scene(int k, int mm, int st)
{
    for (int i = 0; i < 16; i++) {
        int on = i < k;
        D[i] = on ? (int16_t)mm : 0;
        ST[i] = on ? (uint8_t)st : 255;
        NB[i] = on ? 1 : 0;
    }
    return &F;
}

static void test_presence(void)
{
    np_cfg c;
    np_state s;
    np_defaults(&c);
    check("np defaults valid", np_cfg_valid(&c));
    char b[96];
    np_cfg_format(&c, b, sizeof b);
    check("np cfg format", !strcmp(b, "near=1000 far=1300 min=60 zones=2 on=3 off=8 stale=1500") && tokens_ok(b));
    np_cfg x = c;
    check("np set near", np_cfg_set(&x, "near", 900) == 0 && x.near_mm == 900);
    check("np set far below near -> invalid together", np_cfg_set(&x, "far", 800) == 0 && !np_cfg_valid(&x));
    x = c;
    check("np set out of range", np_cfg_set(&x, "near", 99) && np_cfg_set(&x, "zones", 0) && np_cfg_set(&x, "zones", 17) &&
                                     np_cfg_set(&x, "on", 0) && np_cfg_set(&x, "off", 51) && np_cfg_set(&x, "stale", 499) &&
                                     np_cfg_set(&x, "min", 501) && np_cfg_set(&x, "bogus", 1));
    check("np failed sets leave cfg unchanged", !memcmp(&x, &c, sizeof c));
    x = c; x.min_mm = 1000;
    check("np min >= near invalid", !np_cfg_valid(&x));

    /* zone validity */
    check("zone status 5 valid", np_zone_valid(&c, 500, 5, 1));
    check("zone status 9 valid", np_zone_valid(&c, 500, 9, 1));
    int others_bad = 1;
    for (int st = 0; st < 256; st++) if (st != 5 && st != 9 && np_zone_valid(&c, 500, (uint8_t)st, 1)) others_bad = 0;
    check("every other status invalid (0 4 6 10 12 255 ...)", others_bad);
    check("zero targets invalid", !np_zone_valid(&c, 500, 5, 0));
    check("targets unknown (-1) allowed", np_zone_valid(&c, 500, 5, -1));
    check("below min_mm invalid (cover glass)", !np_zone_valid(&c, 59, 5, 1) && np_zone_valid(&c, 60, 5, 1));
    check("beyond 4000 invalid", !np_zone_valid(&c, 4001, 5, 1) && !np_zone_valid(&c, -5, 5, 1));

    /* rise needs on_frames consecutive near frames */
    np_init(&s);
    uint32_t t = 1000;
    int r1 = np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    int r2 = np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    check("2 near frames: not yet", r1 == 0 && r2 == 0 && s.level == 0);
    check("3rd near frame: rise", np_frame_update(&s, &c, scene(16, 600, 5), t += 200) == 1 && s.level == 1 && s.rises == 1);
    check("nearest / zones reported", s.nearest_mm == 600 && s.valid_zones == 16 && s.near_zones == 16 && s.held_zones == 16);
    check("held: no repeat rise", np_frame_update(&s, &c, scene(16, 600, 5), t += 200) == 0 && s.rises == 1);

    /* hysteresis band: between near (1000) and far (1300) the level is held indefinitely */
    for (int i = 0; i < 50; i++) np_frame_update(&s, &c, scene(16, 1200, 5), t += 200);
    check("in the hysteresis band: held", s.level == 1 && s.falls == 0);
    /* departure: off_frames frames with nobody within far_mm */
    for (int i = 0; i < 7; i++) np_frame_update(&s, &c, scene(0, 0, 255), t += 200);
    check("7 empty frames: still held", s.level == 1);
    np_frame_update(&s, &c, scene(16, 1250, 5), t += 200);
    for (int i = 0; i < 7; i++) np_frame_update(&s, &c, scene(16, 1400, 5), t += 200);
    check("a held frame restarts the off run; 7 far frames: held", s.level == 1);
    check("8th far frame: fall (departure)", np_frame_update(&s, &c, scene(16, 1400, 5), t += 200) == -1 && s.level == 0 && s.falls == 1);
    /* re-arm: a new presence needs on_frames again */
    np_frame_update(&s, &c, scene(16, 900, 5), t += 200);
    np_frame_update(&s, &c, scene(16, 900, 5), t += 200);
    check("re-arm: 2 frames not enough", s.level == 0);
    check("re-arm: 3rd frame rises again", np_frame_update(&s, &c, scene(16, 900, 5), t += 200) == 1 && s.rises == 2);
    /* band from below does not start a presence */
    np_init(&s);
    for (int i = 0; i < 30; i++) np_frame_update(&s, &c, scene(16, 1100, 5), t += 200);
    check("in the band from absent: never rises", s.level == 0 && s.rises == 0);

    /* invalid evidence never triggers */
    np_init(&s);
    for (int i = 0; i < 50; i++) np_frame_update(&s, &c, scene(1, 400, 5), t += 200);
    check("one near zone (fleck) never rises", s.level == 0);
    for (int i = 0; i < 50; i++) np_frame_update(&s, &c, scene(16, 400, 4), t += 200);
    check("16 near zones with invalid status never rise", s.level == 0 && s.zones_rejected >= 16 * 50);
    for (int i = 0; i < 50; i++) np_frame_update(&s, &c, scene(16, 30, 5), t += 200);
    check("16 zones closer than min_mm never rise", s.level == 0);
    for (int i = 0; i < 50; i++) np_frame_update(&s, &c, scene(16, 400, 6), t += 200);
    check("status 6 (wrap-around not performed) never rises", s.level == 0);
    np_frame zf = { 0, D, ST, NB }, nf = { 16, NULL, ST, NB }, bigf = { 65, D, ST, NB };
    uint32_t bf = s.bad_frames;
    np_frame_update(&s, &c, &zf, t += 200);
    np_frame_update(&s, &c, &nf, t += 200);
    np_frame_update(&s, &c, &bigf, t += 200);
    np_frame_update(&s, &c, NULL, t += 200);
    check("malformed frames are bad frames", s.bad_frames == bf + 4 && s.level == 0);

    /* bad frames break a rising run */
    np_init(&s);
    np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    np_bad_frame(&s);
    np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    check("near, near, BAD, near: no rise", s.level == 0);
    np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    check("... near, near: rise", np_frame_update(&s, &c, scene(16, 600, 5), t += 200) == 1 && s.level == 1);
    /* bad frames do not advance a falling run and never change the level by themselves */
    for (int i = 0; i < 5; i++) np_bad_frame(&s);
    for (int i = 0; i < 7; i++) np_frame_update(&s, &c, scene(0, 0, 255), t += 100);
    for (int i = 0; i < 5; i++) np_bad_frame(&s);
    check("bad frames between far frames: still held", s.level == 1);

    /* staleness */
    np_init(&s);
    t = 50000;
    for (int i = 0; i < 3; i++) np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    uint32_t last = t;
    check("level 1", s.level == 1);
    check("tick before stale: held", np_tick(&s, &c, last + 1499) == 0 && s.level == 1);
    check("effective just after stale (inside margin) still 1", np_effective(&s, &c, last + 1600, 500));
    check("effective after stale + margin is 0", !np_effective(&s, &c, last + 2000, 500));
    check("tick at stale: fall", np_tick(&s, &c, last + 1500) == -1 && s.level == 0 && s.stale_falls == 1);
    check("effective 0 after a stale fall", !np_effective(&s, &c, last + 1501, 500));
    /* a frame arriving after a long gap: the held level ends first, and a new rise needs fresh frames */
    np_init(&s);
    t = 60000;
    for (int i = 0; i < 3; i++) np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    check("gap then near frame: fall reported", np_frame_update(&s, &c, scene(16, 600, 5), t += 5000) == -1 && s.level == 0);
    np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    check("gap: needs on_frames fresh frames (2 so far)", s.level == 0);
    check("gap: 3rd fresh frame rises", np_frame_update(&s, &c, scene(16, 600, 5), t += 200) == 1);
    /* a partial rising run interrupted by a gap starts over */
    np_init(&s);
    np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    np_frame_update(&s, &c, scene(16, 600, 5), t += 4000);
    check("rising run broken by a gap", s.level == 0);

    /* fault */
    np_init(&s);
    for (int i = 0; i < 3; i++) np_frame_update(&s, &c, scene(16, 600, 5), t += 200);
    check("fault while present: fall", np_fault(&s, t) == -1 && s.level == 0 && s.fault_falls == 1 && !s.have_frame &&
                                           s.nearest_mm == -1);
    check("fault while absent: nothing", np_fault(&s, t) == 0 && s.fault_falls == 1);
    check("effective 0 without frames", !np_effective(&s, &c, t, 500));
    check("counters survive a fault", s.rises == 1 && s.frames == 3);

    /* tuned config: zones=1 on=1 off=1 */
    x = c; x.min_zones = 1; x.on_frames = 1; x.off_frames = 1;
    np_init(&s);
    check("zones=1 on=1: one zone rises at once", np_frame_update(&s, &x, scene(1, 500, 9), t += 200) == 1);
    check("off=1: one empty frame falls", np_frame_update(&s, &x, scene(0, 0, 255), t += 200) == -1);

    /* randomised: frames without valid near evidence (invalid statuses, zero targets, a single zone,
     * too close, beyond near) never produce presence */
    srand(12345);
    np_init(&s);
    int ever = 0;
    for (int i = 0; i < 20000; i++) {
        int kind = rand() % 5;
        for (int z = 0; z < 16; z++) {
            int st = rand() % 256;
            if (st == 5 || st == 9) st = 4;
            D[z] = (int16_t)(rand() % 5000 - 200);
            ST[z] = (uint8_t)st;
            NB[z] = (uint8_t)(rand() % 3);
            if (kind == 1) { ST[z] = 5; NB[z] = 0; }                                 /* valid status, no target */
            if (kind == 2) { ST[z] = z == 3 ? 5 : 255; NB[z] = z == 3; D[z] = 300; } /* a single zone */
            if (kind == 3) { ST[z] = 5; NB[z] = 1; D[z] = (int16_t)(rand() % 60); }  /* below min */
            if (kind == 4) { ST[z] = 9; NB[z] = 1; D[z] = (int16_t)(1001 + rand() % 3000); } /* valid, not near */
        }
        if (rand() % 7 == 0) np_bad_frame(&s);
        else np_frame_update(&s, &c, &F, t += 200);
        ever |= s.level;
    }
    check("20000 random frames without valid near evidence: never present", !ever && s.rises == 0);
}

/* ---------------------------------------------------------------- supervisor */
typedef struct { nt_sm s; nt_cfg c; uint32_t t; } SM;

static int step(SM *m, int res_if_act)    /* run one nt_next; report res_if_act for an action; return the action */
{
    uint32_t w;
    int a = nt_next(&m->s, &m->c, m->t, &w);
    if (a == NT_A_WAIT) { m->t += w ? w : 1; return a; }
    nt_result(&m->s, &m->c, res_if_act, m->t);
    return a;
}
/* advance to the next non-WAIT action (without reporting it) */
static int next_act(SM *m, uint32_t *waited)
{
    uint32_t w, sum = 0;
    for (int i = 0; i < 1000; i++) {
        int a = nt_next(&m->s, &m->c, m->t, &w);
        if (a != NT_A_WAIT) { if (waited) *waited = sum; return a; }
        if (m->s.paused || !m->s.enabled) { if (waited) *waited = sum; return a; }
        m->t += w ? w : 1;
        sum += w ? w : 1;
    }
    return -1;
}
static void bring_up(SM *m)
{
    int seq[] = { NT_A_PROBE, NT_A_IDCHK, NT_A_INIT, NT_A_CONFIG, NT_A_START };
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        int a = next_act(m, NULL);
        if (a == NT_A_BUSCLR) { nt_result(&m->s, &m->c, NT_R_OK, m->t); a = next_act(m, NULL); }
        if (a != seq[i]) { printf("  bring_up: expected %s got %d\n", NT_ACTION_NAME[seq[i]], a); check("bring_up order", 0); return; }
        if (a == NT_A_INIT) m->t += 2500;     /* firmware upload time */
        nt_result(&m->s, &m->c, NT_R_OK, m->t);
    }
}

static void test_tofsm(void)
{
    SM m;
    uint32_t w;
    char d[200];
    nt_defaults(&m.c);
    m.t = 0;
    nt_init(&m.s, 1, 0);
    check("start delay: WAIT ~3 s", nt_next(&m.s, &m.c, 0, &w) == NT_A_WAIT && w == 3000);
    check("not ranging at start", !nt_ranging(&m.s));
    m.t = 3000;
    check("then PROBE", nt_next(&m.s, &m.c, m.t, &w) == NT_A_PROBE);
    check("an outstanding action is handed out again", nt_next(&m.s, &m.c, m.t, &w) == NT_A_PROBE);

    /* absent: NACK backoff 2, 4, 8, 16, 32, 60, 60 s */
    uint32_t expect[] = { 2000, 4000, 8000, 16000, 32000, 60000, 60000 };
    int ok = 1;
    for (unsigned i = 0; i < 7; i++) {
        nt_result(&m.s, &m.c, NT_R_NACK, m.t);
        uint32_t waited = 0;
        int a = next_act(&m, &waited);
        if (a != NT_A_PROBE || waited != expect[i]) { ok = 0; printf("  absent backoff %u: act %d waited %u\n", i, a, (unsigned)waited); }
    }
    check("absent: backoff 2 s doubling to 60 s", ok);
    nt_diag(&m.s, d, sizeof d);
    check("absent: diag says tof=absent, counts NACKs", !strncmp(d, "tof=absent ", 11) && strstr(d, " nk=7 ") && tokens_ok(d));
    /* the sensor appears */
    nt_result(&m.s, &m.c, NT_R_OK, m.t);
    check("probe ACK -> IDCHK", next_act(&m, NULL) == NT_A_IDCHK && m.s.present);

    /* 24 h of a dead sensor: bounded bus traffic */
    SM z;
    z.c = m.c; z.t = 0;
    nt_init(&z.s, 1, 0);
    while (z.t < 24u * 3600u * 1000u) step(&z, NT_R_NACK);
    check("24 h absent: <= 1460 probes (one a minute + ramp)", z.s.probes <= 1460 && z.s.probes > 1400);

    /* happy path */
    SM h;
    nt_defaults(&h.c);
    h.t = 0;
    nt_init(&h.s, 1, 0);
    bring_up(&h);
    check("happy path: ranging", nt_ranging(&h.s) && h.s.inits_ok == 1 && h.s.init_ms_last == 2500);
    check("first POLL after poll_ms", next_act(&h, &w) == NT_A_POLL && w == h.c.poll_ms);
    nt_result(&h.s, &h.c, NT_R_NOTREADY, h.t);
    check("NOTREADY: poll again after poll_ms", next_act(&h, &w) == NT_A_POLL && w == h.c.poll_ms);
    nt_result(&h.s, &h.c, NT_R_OK, h.t);
    check("frame counted", h.s.frames == 1);
    /* frames at 5 Hz for a while */
    for (int i = 0; i < 200; i++) {
        int a = next_act(&h, NULL);
        nt_result(&h.s, &h.c, (i % 5) == 4 ? NT_R_OK : NT_R_NOTREADY, h.t);
        (void)a;
    }
    check("steady ranging: no timeouts", nt_ranging(&h.s) && h.s.timeouts == 0 && h.s.frames == 41);

    /* frame timeout: data never ready -> STOP -> backoff 5 s -> PROBE -> ... */
    uint32_t t0 = h.t;
    int a;
    do { a = next_act(&h, NULL); if (a == NT_A_POLL) nt_result(&h.s, &h.c, NT_R_NOTREADY, h.t); } while (a == NT_A_POLL && h.t - t0 < 10000);
    check("timeout -> STOP", a == NT_A_STOP && h.s.timeouts == 1 && h.s.restarts == 1 && !nt_ranging(&h.s));
    check("timeout after frame_timeout_ms", h.t - t0 >= (uint32_t)(h.c.frame_timeout_ms - h.c.poll_ms) && h.t - t0 <= (uint32_t)(h.c.frame_timeout_ms + h.c.poll_ms));
    nt_result(&h.s, &h.c, NT_R_OK, h.t);
    a = next_act(&h, &w);
    check("after STOP: backoff 5 s then PROBE", a == NT_A_PROBE && w == 5000);
    check("stop cleared need_stop", !h.s.need_stop);

    /* consecutive bad frames */
    SM b;
    nt_defaults(&b.c);
    b.t = 0;
    nt_init(&b.s, 1, 0);
    bring_up(&b);
    for (int i = 0; i < 4; i++) { next_act(&b, NULL); nt_result(&b.s, &b.c, NT_R_BADFRAME, b.t); }
    check("4 bad frames: still ranging", nt_ranging(&b.s) && b.s.bad_frames == 4);
    next_act(&b, NULL); nt_result(&b.s, &b.c, NT_R_OK, b.t);
    for (int i = 0; i < 4; i++) { next_act(&b, NULL); nt_result(&b.s, &b.c, NT_R_BADFRAME, b.t); }
    check("a good frame resets the bad run", nt_ranging(&b.s));
    next_act(&b, NULL); nt_result(&b.s, &b.c, NT_R_BUS, b.t);
    check("5th consecutive bad (bus): restart via STOP", !nt_ranging(&b.s) && b.s.stage == NT_S_STOP && b.s.bus_dirty);
    check("STOP next", next_act(&b, NULL) == NT_A_STOP);
    nt_result(&b.s, &b.c, NT_R_ERR, b.t);
    check("failed stop counted", b.s.stops_fail == 1);
    a = next_act(&b, &w);
    check("bus dirty: BUSCLR before PROBE", a == NT_A_BUSCLR && w == 5000);
    nt_result(&b.s, &b.c, NT_R_OK, b.t);
    check("then PROBE", next_act(&b, NULL) == NT_A_PROBE && b.s.busclrs == 1);

    /* INIT failures: bus clear + doubling backoff 5 10 20 40 60 60 */
    SM f;
    nt_defaults(&f.c);
    f.t = 0;
    nt_init(&f.s, 1, 0);
    uint32_t fexp[] = { 5000, 10000, 20000, 40000, 60000, 60000 };
    ok = 1;
    for (unsigned i = 0; i < 6; i++) {
        a = next_act(&f, NULL);
        if (a == NT_A_BUSCLR) { nt_result(&f.s, &f.c, NT_R_OK, f.t); a = next_act(&f, NULL); }
        if (a != NT_A_PROBE) { ok = 0; break; }
        nt_result(&f.s, &f.c, NT_R_OK, f.t);
        if (next_act(&f, NULL) != NT_A_IDCHK) { ok = 0; break; }
        nt_result(&f.s, &f.c, NT_R_OK, f.t);
        if (next_act(&f, NULL) != NT_A_INIT) { ok = 0; break; }
        nt_result(&f.s, &f.c, NT_R_ERR, f.t);
        uint32_t waited;
        a = next_act(&f, &waited);
        if (a != NT_A_BUSCLR || waited != fexp[i]) { ok = 0; printf("  init fail %u: act %d waited %u\n", i, a, (unsigned)waited); }
    }
    check("INIT failure: BUSCLR first, backoff 5 s doubling to 60 s", ok && f.s.inits_fail == 6);
    nt_diag(&f.s, d, sizeof d);
    check("diag shows last failure init/5", strstr(d, " lf=init/5") != NULL && strstr(d, " ini=0/6 ") != NULL && tokens_ok(d));
    /* CONFIG and START failures need a STOP only once START ran */
    nt_result(&f.s, &f.c, NT_R_OK, f.t);
    next_act(&f, NULL); nt_result(&f.s, &f.c, NT_R_OK, f.t);    /* PROBE */
    next_act(&f, NULL); nt_result(&f.s, &f.c, NT_R_OK, f.t);    /* IDCHK */
    next_act(&f, NULL); nt_result(&f.s, &f.c, NT_R_OK, f.t);    /* INIT */
    check("CONFIG", next_act(&f, NULL) == NT_A_CONFIG);
    nt_result(&f.s, &f.c, NT_R_ERR, f.t);
    check("CONFIG failure: no STOP (nothing started), backoff", f.s.stage == NT_S_BACKOFF && f.s.cfg_fail == 1);
    bring_up(&f);
    check("recovers to ranging", nt_ranging(&f.s));
    /* backoff resets after stable_ms of good ranging */
    uint32_t since = f.t;
    while (f.t - since < f.c.stable_ms + 1000) { next_act(&f, NULL); nt_result(&f.s, &f.c, NT_R_OK, f.t); }
    check("stable ranging resets the backoff", f.s.backoff_ms == 0);

    /* START failure goes through STOP */
    SM s2;
    nt_defaults(&s2.c);
    s2.t = 0;
    nt_init(&s2.s, 1, 0);
    for (int i = 0; i < 4; i++) { next_act(&s2, NULL); nt_result(&s2.s, &s2.c, NT_R_OK, s2.t); }
    check("START", next_act(&s2, NULL) == NT_A_START);
    nt_result(&s2.s, &s2.c, NT_R_ERR, s2.t);
    check("START failure: STOP at once", next_act(&s2, &w) == NT_A_STOP && w == 0 && s2.s.start_fail == 1);
    nt_result(&s2.s, &s2.c, NT_R_OK, s2.t);
    check("then the backoff", next_act(&s2, &w) == NT_A_PROBE && w == 5000);

    /* pause (OTA): no I2C, no timeout afterwards */
    SM p;
    nt_defaults(&p.c);
    p.t = 0;
    nt_init(&p.s, 1, 0);
    bring_up(&p);
    nt_pause(&p.s, 1, p.t);
    check("paused: WAIT, not ranging", nt_next(&p.s, &p.c, p.t, &w) == NT_A_WAIT && !nt_ranging(&p.s));
    nt_diag(&p.s, d, sizeof d);
    check("paused: diag tof=paused", !strncmp(d, "tof=paused ", 11));
    p.t += 120000;
    check("still WAIT after 2 minutes", nt_next(&p.s, &p.c, p.t, &w) == NT_A_WAIT);
    nt_pause(&p.s, 0, p.t);
    check("resume: POLL at once, no timeout", nt_next(&p.s, &p.c, p.t, &w) == NT_A_POLL && p.s.timeouts == 0 && nt_ranging(&p.s));
    nt_result(&p.s, &p.c, NT_R_NOTREADY, p.t);

    /* disable while ranging: STOP, then OFF; enable: PROBE */
    nt_enable(&p.s, 0, p.t);
    check("disable: STOP", next_act(&p, NULL) == NT_A_STOP);
    nt_result(&p.s, &p.c, NT_R_OK, p.t);
    check("disabled: off, WAIT", nt_next(&p.s, &p.c, p.t, &w) == NT_A_WAIT && p.s.stage == NT_S_OFF && !nt_ranging(&p.s));
    p.t += 600000;
    check("disabled: no I2C for 10 min", nt_next(&p.s, &p.c, p.t, &w) == NT_A_WAIT && p.s.probes == 1);
    nt_enable(&p.s, 1, p.t);
    check("enable: PROBE now", nt_next(&p.s, &p.c, p.t, &w) == NT_A_PROBE);
    nt_result(&p.s, &p.c, NT_R_OK, p.t);
    /* restart */
    SM r;
    nt_defaults(&r.c);
    r.t = 0;
    nt_init(&r.s, 1, 0);
    bring_up(&r);
    nt_restart(&r.s, r.t);
    check("restart while ranging: STOP", next_act(&r, NULL) == NT_A_STOP && r.s.restarts == 1);
    nt_result(&r.s, &r.c, NT_R_OK, r.t);
    check("restart: BUSCLR without backoff", next_act(&r, &w) == NT_A_BUSCLR && w == 0);
    /* disabled from boot: never touches the bus */
    SM o;
    nt_defaults(&o.c);
    o.t = 0;
    nt_init(&o.s, 0, 0);
    int any = 0;
    for (int i = 0; i < 10000; i++) any |= step(&o, NT_R_OK) != NT_A_WAIT;
    check("disabled at boot: only WAITs", !any && o.s.probes == 0);
    nt_diag(&o.s, d, sizeof d);
    check("disabled: diag tof=off", !strncmp(d, "tof=off ", 8));
}

/* ---------------------------------------------------------------- event log + trace */
static int json_shape_ok(const char *j)
{
    size_t n = strlen(j);
    if (n < 2 || j[0] != '[' || j[n - 1] != ']') return 0;
    int depth = 0, inq = 0;
    for (size_t i = 0; i < n; i++) {
        char c = j[i];
        if (c == '"') inq = !inq;
        else if (!inq && c == '{') depth++;
        else if (!inq && c == '}') { if (--depth < 0) return 0; }
    }
    return depth == 0 && !inq;
}

static void test_evlog(void)
{
    nlg_ring r;
    nlg_ev e;
    char b[4096];
    nlg_init(&r);
    check("empty", nlg_kept(&r) == 0 && nlg_get(&r, 0, &e) == -1);
    check("empty json", nlg_json(&r, b, sizeof b) == 2 && !strcmp(b, "[]"));
    check("empty tail", nlg_tail(&r, 5, b, sizeof b) == 3 && !strcmp(b, "ev="));
    nlg_add(&r, 12345, NLG_WIFI_DISC, 201, 0);
    nlg_add(&r, 13000, NLG_ETH_UP, 1, 100);
    check("two kept", nlg_kept(&r) == 2 && nlg_get(&r, 0, &e) == 0 && e.code == NLG_WIFI_DISC && e.t_ms == 12345);
    nlg_fmt(&e, b, sizeof b);
    check("fmt", !strcmp(b, "12.3:wifi_disc:201"));
    nlg_get(&r, 1, &e);
    nlg_fmt(&e, b, sizeof b);
    check("fmt with b", !strcmp(b, "13.0:eth_up:1:100"));
    nlg_tail(&r, 10, b, sizeof b);
    check("tail", !strcmp(b, "ev=12.3:wifi_disc:201,13.0:eth_up:1:100") && tokens_ok(b));
    nlg_json(&r, b, sizeof b);
    check("json", !strcmp(b, "[{\"t\":12345,\"e\":\"wifi_disc\",\"a\":201,\"b\":0},{\"t\":13000,\"e\":\"eth_up\",\"a\":1,\"b\":100}]"));
    nlg_add(&r, 1, 200, 0, 0);
    nlg_get(&r, 2, &e);
    check("unknown code stored as none", e.code == NLG_NONE);
    for (int i = 0; i < 100; i++) nlg_add(&r, 20000u + (uint32_t)i, NLG_TOF, (uint8_t)i, 0);
    check("ring keeps NLG_N, total counts all", nlg_kept(&r) == NLG_N && r.total == 103 && r.count[NLG_TOF] == 100);
    nlg_get(&r, 0, &e);
    check("oldest kept is #55", e.code == NLG_TOF && e.a == 52);
    nlg_get(&r, NLG_N - 1, &e);
    check("newest", e.a == 99 && nlg_get(&r, NLG_N, &e) == -1);
    /* bounded outputs */
    int ok = 1;
    for (size_t cap = 0; cap < 400; cap++) {
        char small[400];
        memset(small, 'X', sizeof small);
        int n = nlg_tail(&r, 16, small, cap);
        if (cap && (strlen(small) != (size_t)n || (size_t)n >= cap)) ok = 0;
        if (cap > (size_t)n + 1 && small[n + 1] != 'X' && cap < sizeof small) { /* untouched beyond NUL is not required */ }
        if (n && !tokens_ok(small)) ok = 0;
        if (cap >= 3) {
            memset(small, 'X', sizeof small);
            int m = nlg_json(&r, small, cap);
            if ((size_t)m >= cap || strlen(small) != (size_t)m || !json_shape_ok(small)) ok = 0;
        }
    }
    check("tail / json never exceed cap, always well-formed", ok);
    nlg_tail(&r, 16, b, 60);
    check("short tail keeps the NEWEST entries", strstr(b, "tof:99") != NULL);
    /* worst case 48 entries, 10-digit times, longest name, 3-digit a, 5-digit b */
    nlg_init(&r);
    for (int i = 0; i < NLG_N; i++) nlg_add(&r, 4294967295u, NLG_WIFI_NOIP, 255, 65535);
    int jn = nlg_json(&r, b, sizeof b);
    check("worst-case 48-event JSON fits the firmware's 3072-byte buffer", jn > 0 && jn < 48 * 64 && json_shape_ok(b));
    printf("  worst-case event JSON %d bytes\n", jn);
    int tn = nlg_tail(&r, 16, b, 296 - 10);
    check("DIAG p=ev tail fits the DIAG token", tn > 0 && tn < 286 && tokens_ok(b));

    /* previous-boot trace */
    nr_trace rtc, prev;
    memset(&rtc, 0x5a, sizeof rtc);
    check("garbage trace: none", nr_trace_take(&rtc, &prev, 0x11111111u) == 0 && prev.magic == 0);
    check("fresh trace valid", nr_trace_valid(&rtc) && rtc.boot_id == 0x11111111u && rtc.ev_total == 0);
    for (int i = 0; i < 20; i++) {
        nlg_ev x = { (uint32_t)i * 1000u, NLG_WIFI_DISC, (uint8_t)i, 0 };
        nr_trace_ev(&rtc, &x);
    }
    nr_trace_tick(&rtc, 77, 120000);
    nr_trace_tick(&rtc, 78, 90000);
    nr_trace_tick(&rtc, 79, 110000);
    check("trace valid after updates", nr_trace_valid(&rtc));
    check("next boot: previous trace taken", nr_trace_take(&rtc, &prev, 0x22222222u) == 1 && prev.boot_id == 0x11111111u &&
                                                 prev.up_s == 79 && prev.heap_min == 90000 && prev.ev_total == 20);
    nlg_ev pe[NR_TRACE_EV];
    unsigned k = nr_trace_events(&prev, pe);
    check("previous events: last 16, oldest first", k == 16 && pe[0].a == 4 && pe[15].a == 19);
    check("trace restarted for this boot", rtc.boot_id == 0x22222222u && rtc.ev_total == 0 && nr_trace_valid(&rtc));
    nr_trace c = rtc;
    c.up_s ^= 4;
    check("bit flip: invalid", !nr_trace_valid(&c));
    c = rtc;
    c.magic = NR_MAGIC;
    c.check = nr_trace_sum(&c);
    check("other magic: invalid", !nr_trace_valid(&c));
    memset(&c, 0, sizeof c);
    check("all zero: invalid", !nr_trace_valid(&c));
    nlg_json_arr(pe, k, b, sizeof b);
    check("previous events JSON", json_shape_ok(b));
    check("record sizes (RTC slow memory is small)", sizeof(nr_trace) <= 160 && sizeof(nr_rtc) <= 24);
}

/* ---------------------------------------------------------------- I2C chunking */
static struct { uint16_t reg[1024]; uint32_t n[1024]; int calls, fail_at; } X;
static int xfer(void *ctx, uint16_t reg, uint8_t *buf, uint32_t n)
{
    (void)ctx; (void)buf;
    if (X.calls == X.fail_at) { X.calls++; return 7; }
    X.reg[X.calls] = reg;
    X.n[X.calls] = n;
    X.calls++;
    return 0;
}
static void test_chunk(void)
{
    static uint8_t buf[0x8000];
    memset(&X, 0, sizeof X); X.fail_at = -1;
    check("32 KB in 256-byte transactions", ni2c_chunked(NULL, 0, buf, 0x8000, 256, xfer) == 0 && X.calls == 128 &&
                                               X.reg[0] == 0 && X.reg[127] == 0x7f00 && X.n[127] == 256);
    memset(&X, 0, sizeof X); X.fail_at = -1;
    check("0x5000 + odd sizes", ni2c_chunked(NULL, 0x2c34, buf, 1000, 256, xfer) == 0 && X.calls == 4 && X.n[3] == 232 &&
                                    X.reg[3] == 0x2c34 + 768);
    memset(&X, 0, sizeof X); X.fail_at = -1;
    check("size 0: no transaction", ni2c_chunked(NULL, 0, buf, 0, 256, xfer) == 0 && X.calls == 0);
    memset(&X, 0, sizeof X); X.fail_at = 2;
    check("first failure stops and is returned", ni2c_chunked(NULL, 0, buf, 2000, 256, xfer) == 7 && X.calls == 3);
    memset(&X, 0, sizeof X); X.fail_at = -1;
    check("past index 0xFFFF refused", ni2c_chunked(NULL, 0xff00, buf, 0x200, 256, xfer) == -1 && X.calls == 0);
    check("chunk 0 refused", ni2c_chunked(NULL, 0, buf, 10, 0, xfer) == -1);
    check("exactly to 0xFFFF ok", ni2c_chunked(NULL, 0xff00, buf, 0x100, 256, xfer) == 0);
}

/* ---------------------------------------------------------------- review regressions */
static void test_review(void)
{
    /* failed BUSCLR: stays dirty, backs off (no tight loop), never runs INIT without a driver; recovers */
    SM f;
    uint32_t w;
    nt_defaults(&f.c);
    f.t = 0;
    nt_init(&f.s, 1, 0);
    next_act(&f, NULL); nt_result(&f.s, &f.c, NT_R_OK, f.t);    /* PROBE */
    next_act(&f, NULL); nt_result(&f.s, &f.c, NT_R_OK, f.t);    /* IDCHK */
    next_act(&f, NULL); nt_result(&f.s, &f.c, NT_R_ERR, f.t);   /* INIT fails -> bus dirty */
    int ok = 1, inits = 0;
    uint32_t exp[] = { 10000, 20000, 40000, 60000, 60000 };
    int a = next_act(&f, NULL);
    for (unsigned i = 0; i < 5; i++) {
        if (a != NT_A_BUSCLR) { ok = 0; break; }
        nt_result(&f.s, &f.c, NT_R_BUS, f.t);
        uint32_t waited;
        a = next_act(&f, &waited);
        if (a == NT_A_INIT) inits++;
        if (a != NT_A_BUSCLR || waited != exp[i]) { ok = 0; printf("  busclr fail %u: act %d waited %u\n", i, a, (unsigned)waited); }
    }
    check("failed BUSCLR: retried only after a growing backoff, still dirty, no INIT", ok && inits == 0 && f.s.bus_dirty && f.s.busclr_fail == 5);
    uint32_t t0 = f.t;
    for (int i = 0; i < 1000; i++) { uint32_t ww; if (nt_next(&f.s, &f.c, f.t, &ww) != NT_A_WAIT && f.t == t0) break; f.t += 1; }
    nt_result(&f.s, &f.c, NT_R_OK, f.t);                          /* the clear works this time */
    check("successful BUSCLR clears dirty", !f.s.bus_dirty);
    a = next_act(&f, &w);
    check("then PROBE", a == NT_A_PROBE);

    /* mark-valid: the "now | 1" sentinel bug -- even timestamp > min, no authenticated request */
    niv_state v;
    const char *why;
    memset(&v, 0, sizeof v);
    check("before min uptime: wait", niv_step(&v, 1000, 30000, 60000, 1, 1, 1, 1, 0, &why) == NIV_WAIT && !strcmp(why, "min-uptime"));
    memset(&v, 0, sizeof v);
    check("health first seen at an even time > min, no auth: NOT valid at once",
          niv_step(&v, 40000, 30000, 60000, 1, 1, 1, 1, 0, &why) == NIV_WAIT && !strcmp(why, "health-wait"));
    check("still not valid 59.999 s later", niv_step(&v, 99999, 30000, 60000, 1, 1, 1, 1, 0, &why) == NIV_WAIT);
    check("valid after 60 s of continuous health", niv_step(&v, 100000, 30000, 60000, 1, 1, 1, 1, 0, &why) == NIV_SUSTAINED &&
                                                         !strcmp(why, "sustained-health"));
    memset(&v, 0, sizeof v);
    niv_step(&v, 40000, 30000, 60000, 1, 1, 1, 1, 0, &why);
    check("network drop resets the health period", niv_step(&v, 70000, 30000, 60000, 0, 1, 0, 1, 0, &why) == NIV_WAIT &&
                                                       !strcmp(why, "no-network"));
    niv_step(&v, 80000, 30000, 60000, 1, 1, 1, 1, 0, &why);
    check("... not valid 60 s after the FIRST period began", niv_step(&v, 100001, 30000, 60000, 1, 1, 1, 1, 0, &why) == NIV_WAIT);
    check("... valid 60 s after recovery", niv_step(&v, 140000, 30000, 60000, 1, 1, 1, 1, 0, &why) == NIV_SUSTAINED);
    memset(&v, 0, sizeof v);
    niv_step(&v, 40000, 30000, 60000, 1, 1, 0, 1, 0, &why);
    check("ArduinoOTA down: ota-down", !strcmp(why, "ota-down"));
    check("no key: no-otakey", niv_step(&v, 40001, 30000, 60000, 1, 0, 0, 0, 0, &why) == NIV_WAIT && !strcmp(why, "no-otakey"));
    check("authenticated request + health: valid", niv_step(&v, 41000, 30000, 60000, 1, 1, 1, 1, 1, &why) == NIV_AUTH);
    check("authenticated request but OTA down: wait", niv_step(&v, 41001, 30000, 60000, 1, 1, 1, 0, 1, &why) == NIV_WAIT);
    /* millis() wrap: health starting just before the wrap */
    memset(&v, 0, sizeof v);
    niv_step(&v, 50000, 30000, 60000, 0, 1, 0, 0, 0, &why);           /* min latched */
    niv_step(&v, 0xFFFFF000u, 30000, 60000, 1, 1, 1, 1, 0, &why);
    check("wrap: 4 s after the start, still waiting", niv_step(&v, 0xFFFFF000u + 4000u, 30000, 60000, 1, 1, 1, 1, 0, &why) == NIV_WAIT);
    check("wrap: 60 s after the start (past 2^32), valid", niv_step(&v, 0xFFFFF000u + 60000u, 30000, 60000, 1, 1, 1, 1, 0, &why) == NIV_SUSTAINED);
    memset(&v, 0, sizeof v);
    niv_step(&v, 50000, 30000, 60000, 0, 1, 0, 0, 0, &why);
    check("wrap: min-uptime latch survives millis() wrapping to small values",
          niv_step(&v, 100, 30000, 60000, 1, 1, 1, 1, 0, &why) == NIV_WAIT && !strcmp(why, "health-wait"));
    ok = 1;
    for (uint32_t t = 30000; t < 200000; t += 2) {               /* every even and odd first-health time */
        memset(&v, 0, sizeof v);
        if (niv_step(&v, t, 30000, 60000, 1, 1, 1, 1, 0, &why) != NIV_WAIT) { ok = 0; break; }
        if (niv_step(&v, t + 1, 30000, 60000, 1, 1, 1, 1, 0, &why) != NIV_WAIT) { ok = 0; break; }
    }
    check("no first-health timestamp (even or odd) is ever valid within 2 ms", ok);

    /* invalid presence configs are refused by np_cfg_valid (the task applies only valid ones) */
    np_cfg c;
    np_defaults(&c);
    c.on_frames = 0; c.off_frames = 0;
    check("on=0/off=0 is invalid", !np_cfg_valid(&c));
    /* net return value: a stale fall followed by a rise in the same call is 0 */
    np_state s;
    np_defaults(&c);
    c.on_frames = 1;
    np_init(&s);
    check("rise", np_frame_update(&s, &c, scene(16, 500, 5), 1000) == 1);
    check("stale fall + immediate rise in one call: net 0", np_frame_update(&s, &c, scene(16, 500, 5), 9000) == 0 && s.level == 1 &&
                                                                  s.stale_falls == 1 && s.rises == 2);
}

/* wt32-r4: mark-valid for the router image = Wi-Fi management AND routing ok AND the PM3's lease, composed
 * with niv_step exactly as firmware/src/main.cpp ota_service() does. */
static int r4_step(niv_state *v, uint32_t now, int wifi, int rt, int lease, uint32_t auth_wifi, const char **why)
{
    const char *nw;
    int net = niv_router_net(wifi, rt, lease, &nw);
    int how = niv_step(v, now, 30000, 60000, net, 1, 1, 1, auth_wifi, why);
    if (how == NIV_WAIT && !strcmp(*why, "no-network")) *why = nw;
    return how;
}
static void test_r4_gate(void)
{
    niv_state v;
    const char *why;
    memset(&v, 0, sizeof v);
    check("r4 gate: Wi-Fi down -> no-network", r4_step(&v, 40000, 0, 1, 1, 5, &why) == NIV_WAIT && !strcmp(why, "no-network"));
    check("r4 gate: Wi-Fi + authenticated request but routing not ok -> route-wait, NOT valid",
          r4_step(&v, 41000, 1, 0, 1, 5, &why) == NIV_WAIT && !strcmp(why, "route-wait"));
    check("r4 gate: routing ok but the PM3 has no lease -> lease-wait, NOT valid",
          r4_step(&v, 42000, 1, 1, 0, 5, &why) == NIV_WAIT && !strcmp(why, "lease-wait"));
    check("r4 gate: everything up + authenticated Wi-Fi request -> valid (auth)", r4_step(&v, 43000, 1, 1, 1, 5, &why) == NIV_AUTH);
    memset(&v, 0, sizeof v);
    check("r4 gate: sustained path starts", r4_step(&v, 40000, 1, 1, 1, 0, &why) == NIV_WAIT && !strcmp(why, "health-wait"));
    check("r4 gate: lease lost at 90 s resets the 60 s period", r4_step(&v, 90000, 1, 1, 0, 0, &why) == NIV_WAIT && !strcmp(why, "lease-wait"));
    check("r4 gate: lease back at 91 s, not valid at 150.999 s", r4_step(&v, 91000, 1, 1, 1, 0, &why) == NIV_WAIT &&
          r4_step(&v, 150999, 1, 1, 1, 0, &why) == NIV_WAIT);
    check("r4 gate: valid after 60 s of Wi-Fi + routing + lease", r4_step(&v, 151000, 1, 1, 1, 0, &why) == NIV_SUSTAINED);
    memset(&v, 0, sizeof v);
    check("r4 gate: before 30 s uptime -> min-uptime even with everything up", r4_step(&v, 1000, 1, 1, 1, 5, &why) == NIV_WAIT &&
          !strcmp(why, "min-uptime"));
    const char *w;
    check("niv_router_net all up -> 1/ok", niv_router_net(1, 1, 1, &w) == 1 && !strcmp(w, "ok"));
}

/* wt32-r5.1.2 depth grid: raw zone order, np_zone_valid, and nothing old is ever offered as current */
static void test_grid(void)
{
    np_cfg c;
    np_state s;
    np_defaults(&c);
    np_init(&s);
    char b[NP_GRID_TEXT_MAX];
    int16_t d[16];
    uint8_t st[16], nb[16];
    for (int i = 0; i < 16; i++) { d[i] = (int16_t)(100 + i * 10); st[i] = i % 2 ? 9 : 5; nb[i] = 1; }
    st[3] = 4;                                      /* a status np_zone_valid refuses */
    nb[4] = 0;                                      /* no target */
    d[5] = 59;                                      /* under min_mm */
    d[6] = 4001;                                    /* over NP_MAX_MM */
    d[7] = 4000; d[8] = 60;                         /* the two limits, valid */
    np_frame f = { 16, d, st, nb };
    check("grid: nothing before the first frame", np_grid_format(&s, 500, b, sizeof b) > 0 && !strcmp(b, "zf=0 za=- zg=-") && tokens_ok(b));
    uint32_t t = 1000;
    np_frame_update(&s, &c, &f, t);
    int same = 1;
    for (int i = 0; i < 16; i++) {
        int valid = np_zone_valid(&c, d[i], st[i], nb[i]);
        same &= s.grid_mm[i] == (valid ? d[i] : -1);
    }
    check("grid: zone i is sensor zone i, by the np_zone_valid rule", same && s.grid_ok && s.grid_mm[0] == 100 && s.grid_mm[15] == 250 &&
                                                                    s.grid_mm[7] == 4000 && s.grid_mm[8] == 60);
    check("grid: invalid zones", s.grid_mm[3] == -1 && s.grid_mm[4] == -1 && s.grid_mm[5] == -1 && s.grid_mm[6] == -1);
    check("grid: the summary is the same frame", s.valid_zones == 12 && s.nearest_mm == 60);
    np_grid_format(&s, t + 40, b, sizeof b);
    check("grid text: exact", !strcmp(b, "zf=1 za=40 zg=06406e078fffffffffffffa003c0be0c80d20dc0e60f00fa") && tokens_ok(b));
    const char *z = strstr(b, "zg=") + 3;
    check("grid text: length and first, invalid, limit and last zones", strlen(z) == 48 && !strncmp(z, "064", 3) && !strncmp(z + 3 * 3, "fff", 3) &&
                                                                       !strncmp(z + 7 * 3, "fa0", 3) && !strncmp(z + 8 * 3, "03c", 3) && !strcmp(z + 15 * 3, "0fa"));
    /* freshness */
    check("grid fresh just under the limit", np_grid_format(&s, t + NP_GRID_STALE_MS - 1, b, sizeof b) > 0 && strstr(b, "za=999 "));
    np_grid_format(&s, t + NP_GRID_STALE_MS, b, sizeof b);
    check("grid withheld at the limit, frame id kept", !strcmp(b, "zf=1 za=- zg=-"));
    uint32_t wrap = 0xfffffff0u;
    np_frame_update(&s, &c, &f, wrap);
    check("grid age across the clock wrap", np_grid_format(&s, wrap + 0x20, b, sizeof b) > 0 && strstr(b, "zf=2 za=32 "));
    /* a bad frame, a fault, then a good frame */
    np_frame_update(&s, &c, &f, t);
    np_bad_frame(&s);
    np_grid_format(&s, t + 1, b, sizeof b);
    check("grid cleared by a bad frame", !s.grid_ok && !strcmp(b, "zf=3 za=- zg=-"));
    np_frame_update(&s, &c, &f, t + 200);
    check("grid back with the next good frame", s.grid_ok && np_grid_format(&s, t + 200, b, sizeof b) > 0 && strstr(b, "zf=4 za=0 "));
    np_fault(&s, t + 201);                          /* the task: disable, pause for OTA, restart, sensor lost */
    np_grid_format(&s, t + 202, b, sizeof b);
    check("grid cleared by a fault", !s.grid_ok && !strcmp(b, "zf=4 za=- zg=-"));
    /* other frame shapes never leave a half-written grid */
    np_frame e = { 15, d, st, nb };
    np_frame_update(&s, &c, &f, t + 300);
    np_frame_update(&s, &c, &e, t + 400);
    check("a frame of another size is no grid", !s.grid_ok);
    np_frame_update(&s, &c, &f, t + 500);
    np_frame_update(&s, &c, NULL, t + 600);
    check("a refused frame clears it", !s.grid_ok);
    /* limits */
    for (int i = 0; i < 16; i++) { d[i] = 4000; st[i] = 5; nb[i] = 1; }
    np_frame_update(&s, &c, &f, t);
    s.frames = 0xffffffffu;
    int n = np_grid_format(&s, t + 999, b, sizeof b);
    check("grid text: longest case fits", n == 72 && n < NP_GRID_TEXT_MAX && tokens_ok(b));
    char small[NP_GRID_TEXT_MAX - 1] = "x";
    check("grid text: short buffer refused", np_grid_format(&s, t, small, sizeof small) == -1 && !small[0]);
}

int main(void)
{
    test_review();
    test_r4_gate();
    test_presence();
    test_grid();
    test_tofsm();
    test_evlog();
    test_chunk();
    printf("test_sense: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
