/* wt32-r5.1.3: the retained loop() phase record (shared/nori_sense/src/nori_phase.h) and the NLG_SLOW event.
 *   - take: garbage and damaged records are refused, a valid one is handed over intact and restarted;
 *   - enter: durations, the longest phase and when it began, the slow count at exactly NPH_SLOW_MS, clock wrap;
 *   - the question it exists for: a reset during normal work (brownout) vs loop() stuck in one call (watchdog);
 *   - DIAG p=ph and /diag JSON: worst-case lengths within NPH_DIAG_MAX / NPH_JSON_MAX, every name defined. */
#include "nori_evlog.h"
#include "nori_phase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks, fails;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static int balanced(const char *s)
{
    int depth = 0, q = 0;
    for (; *s; s++) {
        if (*s == '"') q = !q;
        else if (!q && *s == '{') depth++;
        else if (!q && *s == '}' && --depth < 0) return 0;
    }
    return depth == 0 && !q;
}

static void test_take(void)
{
    nph_rec rtc, prev;
    srand(7);
    int refused = 0;
    for (int i = 0; i < 2000; i++) {                 /* power-on RTC memory: garbage */
        unsigned char *p = (unsigned char *)&rtc;
        for (size_t k = 0; k < sizeof rtc; k++) p[k] = (unsigned char)rand();
        refused += nph_take(&rtc, &prev, 0x1000u + (unsigned)i, 5) == 0;
    }
    check("random RTC contents are never taken for a previous boot", refused == 2000);
    check("refused: the previous record reads as zero", prev.magic == 0 && prev.ph == 0 && prev.alive_ms == 0);
    check("restarted: valid, this boot, in setup", nph_valid(&rtc) && rtc.boot_id == 0x1000u + 1999 && rtc.ph == NPH_SETUP && rtc.ph_ms == 5);

    nph_take(&rtc, &prev, 0xabcdef01u, 0);
    nph_enter(&rtc, NPH_JOIN, 1200, NULL);
    nph_alive(&rtc, 1300);
    rtc.passes = 42;
    rtc.wdt = 1;
    nph_rec copy = rtc;
    check("a valid record is taken", nph_take(&rtc, &prev, 0x22u, 7) == 1);
    check("handed over intact", !memcmp(&prev, &copy, sizeof prev));
    check("and the new one starts clean", rtc.boot_id == 0x22u && rtc.passes == 0 && rtc.worst_ms == 0 && rtc.slow == 0 && rtc.wdt == 0 &&
          rtc.guard_ms == 0 && rtc.ph == NPH_SETUP);

    nph_rec bad = rtc;
    bad.boot_inv ^= 1;
    check("boot id complement wrong: refused", !nph_valid(&bad));
    bad = rtc; bad.ph = NPH_COUNT;
    check("phase out of range: refused", !nph_valid(&bad));
    bad = rtc; bad.worst_ph = 200;
    check("longest phase out of range: refused", !nph_valid(&bad));
    bad = rtc; bad.wdt = 3;
    check("watchdog state out of range: refused", !nph_valid(&bad));
    bad = rtc; bad.magic = 0x4e525431u;
    check("another record's magic: refused", !nph_valid(&bad));
}

static void test_enter(void)
{
    nph_rec r, prev;
    uint32_t d = 99;
    nph_take(&r, &prev, 1, 100);
    check("enter returns the phase left and its length", nph_enter(&r, NPH_UART, 150, &d) == NPH_SETUP && d == 50);
    nph_enter(&r, NPH_INPUT, 151, &d);
    nph_enter(&r, NPH_HTTP, 160, &d);
    nph_enter(&r, NPH_LINK, 160 + NPH_SLOW_MS - 1, &d);
    check("just under the slow threshold: not slow", r.slow == 0 && d == NPH_SLOW_MS - 1);
    check("the longest so far, with its phase and start", r.worst_ph == NPH_HTTP && r.worst_ms == NPH_SLOW_MS - 1 && r.worst_at_ms == 160);
    uint32_t t = 160 + NPH_SLOW_MS - 1;
    nph_enter(&r, NPH_IDLE, t + NPH_SLOW_MS, &d);
    check("exactly the threshold: slow", r.slow == 1 && r.worst_ph == NPH_LINK && r.worst_ms == NPH_SLOW_MS);
    nph_enter(&r, NPH_UART, t + NPH_SLOW_MS + 3, &d);
    check("a short one does not replace the longest", r.worst_ph == NPH_LINK && r.worst_ms == NPH_SLOW_MS && r.slow == 1);
    nph_enter(&r, 77, t + NPH_SLOW_MS + 4, &d);
    check("an unknown code becomes none, never an out-of-range index", r.ph == NPH_NONE && nph_valid(&r));
    check("NULL left_ms is allowed", nph_enter(&r, NPH_TICK, t + NPH_SLOW_MS + 5, NULL) == NPH_NONE);

    nph_take(&r, &prev, 2, 0xFFFFFF00u);              /* the millisecond clock wraps after 49 days */
    nph_enter(&r, NPH_NET, 0xFFFFFFF0u, &d);
    nph_enter(&r, NPH_NAT, 0x00000010u, &d);
    check("a phase across the clock wrap", d == 0x20 && r.slow == 0);
    nph_alive(&r, 0x00000030u);
    check("time in the phase across the wrap", nph_in_ms(&r, r.alive_ms) == 0x20);
    check("an alive stamp older than the phase entry counts as 0", nph_in_ms(&r, 0x00000005u) == 0);
}

/* The firmware: ~12 phases per 1 ms pass, alive_ms every 100 ms from an independent timer. */
static void run_passes(nph_rec *r, uint32_t *now, uint32_t ms)
{
    static const uint8_t pass[] = { NPH_UART, NPH_INPUT, NPH_TOF, NPH_NET, NPH_NAT, NPH_OTA_SVC, NPH_OTA_POLL, NPH_HTTP,
                                    NPH_LINK, NPH_TICK, NPH_IDLE };
    for (uint32_t e = 0; e < ms; e++, (*now)++) {
        r->passes++;
        for (size_t i = 0; i < sizeof pass; i++) nph_enter(r, pass[i], *now, NULL);
        if (*now % NPH_ALIVE_MS == 0) nph_alive(r, *now);
    }
}

static void test_discriminates(void)
{
    nph_rec r, prev;
    uint32_t now = 1000;

    /* a brownout during normal work: the board resets mid-pass, the record is whatever it was */
    nph_take(&r, &prev, 10, now);
    run_passes(&r, &now, 60000);
    nph_enter(&r, NPH_JOIN, now, NULL);               /* WiFi.begin() of a join, then the rail collapses */
    nph_take(&r, &prev, 11, 0);
    check("reset during normal work: previous record valid", nph_valid(&prev));
    check("  its last phase is the one it was in", prev.ph == NPH_JOIN);
    check("  and loop() had been in it for less than one alive period", nph_in_ms(&prev, prev.alive_ms) < NPH_ALIVE_MS);
    check("  nothing was slow", prev.slow == 0 && prev.worst_ms <= 1);

    /* loop() stuck in one call: phases stop, the alive timer goes on until the watchdog resets the board */
    nph_take(&r, &prev, 12, now = 1000);
    run_passes(&r, &now, 30000);
    nph_enter(&r, NPH_OTA_STOP, now, NULL);           /* e.g. ArduinoOTA end() -> MDNS.end() never returning */
    for (uint32_t e = 0; e < 5000; e++, now++) if (now % NPH_ALIVE_MS == 0) nph_alive(&r, now);
    nph_take(&r, &prev, 13, 0);
    uint32_t in = nph_in_ms(&prev, prev.alive_ms);
    check("stuck: previous record names the call", prev.ph == NPH_OTA_STOP);
    check("  and shows ~5 s in it (the watchdog timeout)", in >= 4900 && in <= 5000);
    check("  the stuck phase never completed, so it is not the longest phase", prev.worst_ph != NPH_OTA_STOP);

    /* a slow but bounded phase: counted, the longest, and the record moves on */
    nph_take(&r, &prev, 14, now = 1000);
    run_passes(&r, &now, 1000);
    nph_enter(&r, NPH_HTTP, now, NULL);
    now += 3000;                                      /* an idle client holding the 3 s head wait */
    run_passes(&r, &now, 1000);
    check("a 3 s HTTP head wait is counted slow and is the longest", r.slow == 1 && r.worst_ph == NPH_HTTP && r.worst_ms == 3000);
}

static void test_format(void)
{
    nph_rec cur, prev;
    char s[512];
    memset(&cur, 0, sizeof cur);
    memset(&prev, 0, sizeof prev);
    int longest = 0;
    for (int i = 0; i < NPH_COUNT; i++) {
        check("every phase has a name", NPH_NAME[i] && NPH_NAME[i][0] && strcmp(nph_name((uint32_t)i), "?"));
        if ((int)strlen(NPH_NAME[i]) > (int)strlen(NPH_NAME[longest])) longest = i;
        for (int j = 0; j < i; j++) if (!strcmp(NPH_NAME[i], NPH_NAME[j])) check("phase names are unique", 0);
    }
    check("out of range: ?", !strcmp(nph_name(NPH_COUNT), "?") && !strcmp(nph_wdt_name(3), "?"));
    /* worst case: every number at its widest, the longest names */
    cur.ph = cur.worst_ph = prev.ph = prev.worst_ph = (uint32_t)longest;
    cur.ph_ms = 0; cur.worst_ms = cur.worst_at_ms = cur.slow = cur.passes = 0xFFFFFFFFu; cur.wdt = 2;
    prev.ph_ms = 0; prev.alive_ms = prev.worst_ms = prev.worst_at_ms = prev.slow = prev.passes = prev.guard_ms = 0xFFFFFFFFu; prev.wdt = 2;
    int n = nph_diag(&cur, 0xFFFFFFFFu, &prev, 1, s, sizeof s);
    check("DIAG p=ph worst case within NPH_DIAG_MAX", n > 0 && n < NPH_DIAG_MAX && (int)strlen(s) == n);
    printf("  DIAG p=ph worst case %d of %d: %s\n", n, NPH_DIAG_MAX, s);
    char small[NPH_DIAG_MAX];
    n = nph_diag(&cur, 0xFFFFFFFFu, &prev, 1, small, sizeof small);
    check("DIAG p=ph fits the firmware's buffer", n < (int)sizeof small && !strcmp(small, s));
    n = nph_json(&cur, 0xFFFFFFFFu, &prev, 1, s, sizeof s);
    check("JSON worst case within NPH_JSON_MAX", n > 0 && n < NPH_JSON_MAX);
    printf("  JSON worst case %d of %d\n", n, NPH_JSON_MAX);
    check("JSON balanced", balanced(s) && !strncmp(s, "\"phase\":{", 9) && strstr(s, ",\"phase_prev\":{\"valid\":1,"));

    nph_take(&cur, &prev, 5, 0);
    nph_enter(&cur, NPH_LINK, 40, NULL);
    cur.wdt = 1;
    nph_diag(&cur, 45, &prev, 0, s, sizeof s);
    check("DIAG p=ph, no previous record", !strcmp(s, "ph=link pms=5 wph=setup wms=40 wat=0 slow=0 wdt=on prev=none"));
    nph_json(&cur, 45, &prev, 0, s, sizeof s);
    check("JSON, no previous record", balanced(s) && strstr(s, "\"phase_prev\":{\"valid\":0}") && strstr(s, "\"wdt\":\"on\""));

    nph_rec p2;
    memset(&p2, 0, sizeof p2);
    p2.ph = NPH_JOIN; p2.ph_ms = 807000; p2.alive_ms = 811998; p2.worst_ph = NPH_HTTP; p2.worst_ms = 2981; p2.wdt = 1;
    nph_diag(&cur, 45, &p2, 1, s, sizeof s);
    check("DIAG p=ph, previous boot stuck 4998 ms in join",
          strstr(s, " prev=ok pph=join pin=4998 pal=811 pwdt=on pgd=0 pwph=http pwms=2981 pslow=0") != NULL);
    check("a truncating buffer never overflows", nph_diag(&cur, 45, &p2, 1, small, 20) >= 20 && strlen(small) == 19);

    nlg_ev e = { 12345, NLG_SLOW, NPH_HTTP, 2981 };
    nlg_fmt(&e, s, sizeof s);
    check("event log names NLG_SLOW", !strcmp(NLG_NAME[NLG_SLOW], "slow") && strstr(s, "slow") != NULL);
    check("NLG_SLOW appended after the r5.1 codes", NLG_SLOW == NLG_WNET + 1 && NLG_COUNT == NLG_SLOW + 1);
}

int main(void)
{
    test_take();
    test_enter();
    test_discriminates();
    test_format();
    printf("test_phase: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
