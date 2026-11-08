/* wt32-r5.1.3: loop() on the task watchdog (shared/nori_sense/src/nori_lwdt.h), against a model of the ESP-IDF 4.4 task
 * watchdog (5 s, one subscriber: the loop task) and of the esp_timer one-shot guard.
 *
 * The model is strict where the real API is: subscribing twice, unsubscribing or feeding an unsubscribed task are
 * counted as misuse.  After start it checks, at every step, that loop() is covered: subscribed, or the guard armed.
 *
 * Timelines follow the firmware: ordinary passes; the bounded HTTP waits (3 s head + 3 s body, a 300 s upload, the
 * 30 s drain of a refused upload, a 10 s reply), which feed; every way the pinned ArduinoOTA _runUpdate() can end
 * (success, connect error reported twice, receive timeout, end error, a break without a callback, a refused auth that
 * never started), its 4 x 1 s waits between progress callbacks, a transfer that wedges, and one that trickles forever.
 * The guard model re-arms the way the firmware does (stop, then a start that can fail and leave no timer), and a
 * re-arm failing mid-transfer must fail closed: watched again at once, or reset. */
#include "nori_lwdt.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

#define TWDT_MS 5000u
enum { FIRED_NONE, FIRED_TWDT, FIRED_GUARD, FIRED_ABORT };

static struct {
    uint32_t now, last_feed, deadline;
    int sub, armed, fired, started;
    int misuse, uncovered;
    int fail_add, fail_del, fail_arm;            /* make the next call fail */
    int adds, dels, arms, disarms, fails_called;
    nwd *w;
} M;

static void covered(void) { if (M.started && !M.fired && !M.sub && !M.armed) M.uncovered++; }   /* fired: the board resets */
static int m_add(void *c)
{
    (void)c;
    if (M.fail_add) { M.fail_add--; return -1; }
    if (M.sub) { M.misuse++; return -1; }       /* ESP_ERR_INVALID_ARG: already subscribed */
    M.sub = 1; M.last_feed = M.now; M.adds++;
    covered();
    return 0;
}
static int m_del(void *c)
{
    (void)c;
    if (M.fail_del) { M.fail_del--; return -1; }
    if (!M.sub) { M.misuse++; return -1; }
    M.sub = 0; M.dels++;
    covered();
    return 0;
}
static void m_reset(void *c) { (void)c; if (!M.sub) M.misuse++; else M.last_feed = M.now; }
/* As the firmware's wd_arm(): esp_timer_stop() first, then esp_timer_start_once(), which may fail.  A failure leaves
 * NO guard running: the earlier deadline is gone.  (Coverage is checked by the caller once the call sequence is done:
 * between the failed start and the recovery no time passes, a few instructions on the device.) */
static int m_arm(void *c, uint32_t ms)
{
    (void)c;
    M.armed = 0;                                 /* esp_timer_stop() */
    if (M.fail_arm) { M.fail_arm--; return -1; }
    M.armed = 1; M.deadline = M.now + ms; M.arms++;
    return 0;
}
static void m_disarm(void *c) { (void)c; M.armed = 0; M.disarms++; covered(); }
static void m_fail(void *c) { (void)c; M.fails_called++; if (!M.fired) M.fired = FIRED_ABORT; }   /* esp_system_abort() */
static const nwd_hal HAL = { NULL, m_add, m_del, m_reset, m_arm, m_disarm, m_fail };

/* Advance the clock 1 ms at a time; the watchdog or the guard may fire. */
static void advance(uint32_t ms)
{
    for (uint32_t i = 0; i < ms && !M.fired; i++) {
        M.now++;
        if (M.sub && M.now - M.last_feed >= TWDT_MS) M.fired = FIRED_TWDT;
        else if (M.armed && (int32_t)(M.now - M.deadline) >= 0) {
            M.armed = 0;
            if (nwd_guard_due(M.w)) M.fired = FIRED_GUARD;   /* guard_cb: abort only for an open hole */
        }
        covered();
    }
}
/* loop() passes of 1 ms for ms, each starting with a feed. */
static void passes(uint32_t ms) { for (uint32_t i = 0; i < ms && !M.fired; i++) { nwd_feed(M.w); advance(1); } }
/* A bounded wait loop that feeds every `every` ms for ms. */
static void fed_wait(uint32_t ms, uint32_t every) { for (uint32_t t = 0; t < ms && !M.fired; t += every) { nwd_feed(M.w); advance(every); } }

static nwd W;
static void fresh(uint32_t t0)
{
    memset(&M, 0, sizeof M);
    M.now = t0;
    M.w = &W;
    nwd_init(&W, &HAL);
    check("start subscribes", nwd_start(&W) == 0 && W.state == NWD_ON && M.sub);
    M.started = 1;
}
static int clean(void) { return !M.misuse && !M.uncovered; }

static void test_ordinary(void)
{
    fresh(1000);
    passes(10 * 60 * 1000);
    check("10 minutes of ordinary passes: never fires, always covered, no misuse", !M.fired && clean() && W.state == NWD_ON);
    nwd_start(&W);
    check("a second start changes nothing", M.adds == 1 && !M.misuse);

    fresh(1000);
    passes(100);
    advance(TWDT_MS);
    check("loop() stuck in a call: the watchdog fires at 5 s", M.fired == FIRED_TWDT);

    fresh(1000);
    passes(10);
    fed_wait(3000, 2); fed_wait(3000, 2);            /* HTTP head 3 s, then a sealed Wi-Fi body 3 s, one pass */
    fed_wait(30000, 2);                               /* a refused upload's drain */
    fed_wait(10000, 2);                               /* a /diag reply over a slow link */
    fed_wait(300000, 1);                              /* an HTTP upload at its total limit */
    passes(10);
    check("bounded HTTP waits that feed: never fire, no hole", !M.fired && clean() && M.dels == 0 && W.holes == 0);

    fresh(1000);
    passes(10);
    advance(3000); advance(3000);                     /* the same two waits without the feeds r5.1.3 added */
    check("(why they feed: 3 s + 3 s in one pass would fire)", M.fired == FIRED_TWDT);
}

/* ArduinoOTA _runUpdate() after the auth: Update.begin, onStart, progress(0), connect, chunks with progress. */
static void ota_begin(void) { nwd_hole_begin(&W, M.now); nwd_progress(&W, M.now); }
static void chunks(uint32_t ms, uint32_t gap) { for (uint32_t t = 0; t < ms && !M.fired; t += gap) { advance(gap); nwd_progress(&W, M.now); covered(); } }

static void test_ota(void)
{
    /* success: 1.0 MB at 20 KB/s with the worst gaps the library allows, then ESP.restart() */
    fresh(5000);
    passes(10);
    ota_begin();
    check("onStart: unsubscribed, guard armed first", W.state == NWD_OTA && !M.sub && M.armed && M.arms >= 1 && clean());
    advance(3000);                                    /* connect: up to WIFI_CLIENT_DEF_CONN_TIMEOUT_MS */
    chunks(50000, 4600);                              /* 4 x 1 s waits + a sector erase between two progress calls */
    check("a slow transfer with 4.6 s gaps: no reset, still guarded", !M.fired && W.state == NWD_OTA && clean());

    /* receive failure: the library waits 4 x 1 s, calls onError(OTA_RECEIVE_ERROR) and returns */
    fresh(5000);
    passes(10);
    ota_begin();
    chunks(20000, 400);
    advance(4000);
    nwd_hole_end(&W, M.now);                          /* ota_on_error */
    nwd_hole_end(&W, M.now);                          /* loop(), after handle() returns */
    check("receive error: subscribed again, guard disarmed, once", W.state == NWD_ON && M.sub && !M.armed && M.adds == 2 && clean());
    passes(60000);
    check("and ordinary operation after it", !M.fired && clean());
    check("the hole's length is reported", W.holes == 1 && W.hole_max_ms == 24000);

    /* connect failure: onError(OTA_CONNECT_ERROR), then (no return in the library) onError(OTA_END_ERROR) */
    fresh(5000);
    ota_begin();
    advance(3000);
    nwd_hole_end(&W, M.now);
    nwd_hole_end(&W, M.now);
    nwd_hole_end(&W, M.now);
    passes(10000);
    check("connect error, reported twice, then handle() returns: idempotent", W.state == NWD_ON && M.adds == 2 && !M.fired && clean());

    /* end error (MD5, image check) and a break without any callback ("No Data", "failed to respond"): handle() returns */
    fresh(5000);
    ota_begin();
    chunks(10000, 500);
    advance(800);                                     /* Update.end() verifying */
    nwd_hole_end(&W, M.now);                          /* only loop() closes it */
    passes(10000);
    check("a transfer that ends without a callback is closed by loop()", W.state == NWD_ON && !M.fired && clean());

    /* auth refused, or Update.begin failed: onError before any onStart */
    fresh(5000);
    nwd_hole_end(&W, M.now);
    passes(10000);
    check("an error before onStart touches nothing", W.state == NWD_ON && M.adds == 1 && M.dels == 0 && !M.fired && clean());

    /* wedged: no progress and no return (a socket call that never comes back) */
    fresh(5000);
    ota_begin();
    chunks(30000, 1000);
    uint32_t last = M.now;
    advance(NWD_STALL_MS + 100);
    check("a wedged transfer: the guard fires 15 s after the last progress", M.fired == FIRED_GUARD && M.now - last == NWD_STALL_MS && clean());

    /* a trickle that never ends: progress every second, forever */
    fresh(5000);
    uint32_t t0 = M.now;
    ota_begin();
    chunks(NWD_HOLE_MAX_MS + 30000, 1000);
    check("a transfer that never ends: the guard fires at the cap", M.fired == FIRED_GUARD && M.now - t0 == NWD_HOLE_MAX_MS && clean());

    /* the clock wraps during a transfer */
    fresh(0xFFFFFFFFu - 20000);
    ota_begin();
    chunks(60000, 1000);
    nwd_hole_end(&W, M.now);
    check("a transfer across the clock wrap", !M.fired && W.state == NWD_ON && clean() && W.hole_max_ms == 60000);

    /* the guard firing just as the hole closed (callback already dispatched) */
    fresh(5000);
    ota_begin();
    nwd_hole_end(&W, M.now);
    check("a guard callback after the hole closed does nothing", !nwd_guard_due(&W));
}

static void test_failures(void)
{
    /* the guard timer cannot be armed (esp_timer_create failed): no hole, the transfer runs watched */
    fresh(5000);
    M.fail_arm = 1;
    ota_begin();
    check("no guard: no hole", W.state == NWD_ON && M.sub && W.arm_fails == 1 && M.dels == 0 && clean());
    chunks(20000, 1000);
    check("  progress callbacks feed instead", !M.fired && clean());
    advance(TWDT_MS);
    check("  and a wedge is still caught by the watchdog", M.fired == FIRED_TWDT);

    /* unsubscribing fails: the guard is disarmed again, nothing else changes */
    fresh(5000);
    M.fail_del = 1;
    ota_begin();
    check("unsubscribe failed: stays on, guard disarmed", W.state == NWD_ON && M.sub && !M.armed && W.del_fails == 1 && clean());

    /* subscribing again fails after a transfer: stays guarded, the guard ends it */
    fresh(5000);
    ota_begin();
    chunks(5000, 1000);
    M.fail_add = 1;
    nwd_hole_end(&W, M.now);
    check("resubscribe failed: still guarded", W.state == NWD_OTA && M.armed && W.add_fails == 1 && clean());
    advance(NWD_STALL_MS);
    check("  and the guard resets the board", M.fired == FIRED_GUARD);

    /* subscribing fails at start: reported, nothing fed, no hole */
    memset(&M, 0, sizeof M);
    M.w = &W;
    M.now = 100;
    nwd_init(&W, &HAL);
    M.fail_add = 1;
    check("start fails: reported", nwd_start(&W) == -1 && W.state == NWD_OFF && W.add_fails == 1);
    nwd_feed(&W);
    nwd_hole_begin(&W, M.now);
    nwd_progress(&W, M.now);
    nwd_hole_end(&W, M.now);
    check("  and nothing touches the watchdog after it", !M.misuse && M.adds == 0 && M.arms == 0 && M.dels == 0);
    check("  a later start can still succeed", nwd_start(&W) == 0 && W.state == NWD_ON);
}

/* Re-arming fails mid-transfer.  The HAL has already stopped the old timer, so nothing guards the hole any more. */
static void test_rearm(void)
{
    /* the watchdog can be restored: the rest of the transfer runs watched */
    fresh(5000);
    ota_begin();
    chunks(20000, 1000);
    uint32_t t_fail = M.now;
    M.fail_arm = 1;
    advance(1000);
    nwd_progress(&W, M.now);
    check("the model's arm really stops first: no timer after the failed start", !M.armed);
    check("re-arm failed: subscribed again at once, counted, no unguarded moment",
          W.state == NWD_ON && M.sub && W.rearm_fails == 1 && M.adds == 2 && clean() && !M.fired);
    check("  the hole's length is recorded", W.hole_max_ms == M.now - t_fail + 20000);
    chunks(30000, 4000);
    check("  progress callbacks feed from then on", !M.fired && clean() && W.state == NWD_ON);
    advance(TWDT_MS);
    check("  and a transfer that then stalls 5 s is a task_wdt reset", M.fired == FIRED_TWDT);

    fresh(5000);
    ota_begin();
    chunks(5000, 1000);
    M.fail_arm = 1;
    advance(1000);
    nwd_progress(&W, M.now);
    nwd_hole_end(&W, M.now);                          /* ota_on_error / loop() after handle(): already closed */
    nwd_hole_end(&W, M.now);
    passes(20000);
    check("  closing it afterwards is a no-op, ordinary operation goes on", W.state == NWD_ON && M.adds == 2 && !M.fired && clean());

    /* neither the guard nor the watchdog: reset at once */
    fresh(5000);
    ota_begin();
    chunks(10000, 1000);
    M.fail_arm = 1;
    M.fail_add = 1;
    advance(1000);
    nwd_progress(&W, M.now);
    check("re-arm and resubscribe both failed: the HAL resets the board at once", M.fired == FIRED_ABORT && M.fails_called == 1 &&
          W.rearm_fails == 1 && W.add_fails == 1 && !M.misuse && !M.uncovered);

    /* the first arm failing while loop() is still subscribed: the hole is simply refused (stop-then-fail is harmless) */
    fresh(5000);
    M.fail_arm = 1;
    ota_begin();
    check("first arm failed: still subscribed, no hole, no reset", W.state == NWD_ON && M.sub && !M.armed && M.fails_called == 0 && clean());
}

int main(void)
{
    test_ordinary();
    test_ota();
    test_failures();
    test_rearm();
    printf("test_lwdt: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
