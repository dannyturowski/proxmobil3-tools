/* wt32-r5.1.3: loop() under the ESP-IDF task watchdog (CONFIG_ESP_TASK_WDT_TIMEOUT_S=5, panic, unchanged sdkconfig).
 *
 * The Arduino core leaves its loop task off the task watchdog, so a loop() blocked in a call that never returns
 * kept the board alive but deaf (no buttons, no selector, no heartbeat) until a power cycle.  With this, such a
 * block ends in a task_wdt reset (reason 6) and the retained phase record (nori_phase.h) names the call.
 *
 *   OFF  --nwd_start-->  ON  --nwd_hole_begin-->  OTA  --nwd_hole_end-->  ON
 *
 * ON: loop() is subscribed.  nwd_feed() at the top of every pass and inside every bounded wait loop (HTTP head,
 *   body, drain, reply).  This is ordinary operation, and it has no hole.
 * OTA: an authenticated ArduinoOTA transfer.  The pinned ArduinoOTA's _runUpdate() blocks loop() and can wait
 *   4 x 1 s for data between two progress callbacks (plus a 3 s connect), too close to 5 s to feed from there.  For
 *   that transfer only, loop() is unsubscribed and a one-shot guard timer takes its place: armed BEFORE the
 *   unsubscribe, re-armed by each progress callback (NWD_STALL_MS), never past NWD_HOLE_MAX_MS from the start.  When
 *   it fires the firmware aborts (a panic reset), so the transfer is bounded either way.  On the way out (any error
 *   callback, or handle() returning) loop() is subscribed again BEFORE the guard is disarmed: never uncovered.
 *   The HTTP upload has no such hole: its loop is ours and feeds.
 * If the guard cannot be armed, the hole is not opened (the transfer then runs under the 5 s watchdog).
 * Re-arming STOPS the running guard before starting it again (esp_timer in IDF 4.4 has no restart), so a failed re-arm
 * leaves no guard at all.  It fails closed: loop() is subscribed again at once (state ON: progress feeds, a gap over
 * 5 s is a task_wdt reset), and if even that fails the HAL's fail() resets the board.  Counted in rearm_fails.
 * If subscribing fails at start, the state stays OFF and is reported; nothing else changes.
 * Calls are idempotent: ArduinoOTA calls its error callback twice on a failed connect.
 * Portable C, no time source of its own, loop() context only: tests/test_lwdt.c drives it with a model watchdog. */
#ifndef NORI_LWDT_H
#define NORI_LWDT_H

#include <stdint.h>

#define NWD_STALL_MS 15000u      /* ArduinoOTA: no progress for this long ends the transfer */
#define NWD_HOLE_MAX_MS 330000u  /* ArduinoOTA: whole transfer, progress or not (the HTTP upload's own limit is 300 s) */

enum { NWD_OFF, NWD_ON, NWD_OTA };

typedef struct {
    void *ctx;
    int (*add)(void *ctx);                  /* subscribe loop(): 0 ok */
    int (*del)(void *ctx);                  /* unsubscribe loop(): 0 ok */
    void (*reset)(void *ctx);               /* feed */
    int (*arm)(void *ctx, uint32_t ms);     /* (re)arm the one-shot guard: 0 ok; on failure NO guard is left running */
    void (*disarm)(void *ctx);
    void (*fail)(void *ctx);                /* unguarded and unwatched: reset now (does not return on the device) */
} nwd_hal;

typedef struct {
    const nwd_hal *h;
    volatile int state;                     /* read by the guard callback */
    uint32_t t0, holes, hole_max_ms, add_fails, del_fails, arm_fails, rearm_fails;
} nwd;

static inline void nwd_init(nwd *w, const nwd_hal *h)
{
    w->h = h;
    w->state = NWD_OFF;
    w->t0 = w->holes = w->hole_max_ms = w->add_fails = w->del_fails = w->arm_fails = w->rearm_fails = 0;
}

static inline int nwd_start(nwd *w)
{
    if (w->state != NWD_OFF) return 0;
    if (w->h->add(w->h->ctx) != 0) { w->add_fails++; return -1; }
    w->state = NWD_ON;
    w->h->reset(w->h->ctx);
    return 0;
}

static inline void nwd_feed(nwd *w)
{
    if (w->state == NWD_ON) w->h->reset(w->h->ctx);
}

static inline void nwd_hole_begin(nwd *w, uint32_t now)
{
    if (w->state != NWD_ON) return;
    w->h->reset(w->h->ctx);
    if (w->h->arm(w->h->ctx, NWD_STALL_MS) != 0) { w->arm_fails++; return; }   /* no guard: no hole */
    if (w->h->del(w->h->ctx) != 0) { w->del_fails++; w->h->disarm(w->h->ctx); return; }
    w->t0 = now;
    w->holes++;
    w->state = NWD_OTA;
}

/* ArduinoOTA progress (and the HTTP-free path: when ON this is a feed). */
static inline void nwd_progress(nwd *w, uint32_t now)
{
    if (w->state == NWD_ON) { w->h->reset(w->h->ctx); return; }
    if (w->state != NWD_OTA) return;
    uint32_t used = now - w->t0;
    if (used >= NWD_HOLE_MAX_MS) return;                /* past the cap: the armed guard fires as it is */
    uint32_t left = NWD_HOLE_MAX_MS - used;
    if (w->h->arm(w->h->ctx, left < NWD_STALL_MS ? left : NWD_STALL_MS) == 0) return;
    w->rearm_fails++;                                   /* the old deadline was stopped: no guard now.  Fail closed: */
    if (w->h->add(w->h->ctx) == 0) {                    /* back under the watchdog for the rest of the transfer */
        w->h->reset(w->h->ctx);
        w->state = NWD_ON;
        if (used > w->hole_max_ms) w->hole_max_ms = used;
        return;
    }
    w->add_fails++;
    w->h->fail(w->h->ctx);                              /* neither guard nor watchdog: reset now */
}

static inline void nwd_hole_end(nwd *w, uint32_t now)
{
    if (w->state != NWD_OTA) return;
    if (w->h->add(w->h->ctx) != 0) { w->add_fails++; return; }   /* stay guarded; the guard ends it */
    w->h->reset(w->h->ctx);
    w->state = NWD_ON;
    w->h->disarm(w->h->ctx);
    if (now - w->t0 > w->hole_max_ms) w->hole_max_ms = now - w->t0;
}

/* For the guard callback: fire only for a hole that is still open. */
static inline int nwd_guard_due(const nwd *w) { return w->state == NWD_OTA; }

#endif
