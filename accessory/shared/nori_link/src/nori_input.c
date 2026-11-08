/* Accessory input conditioning; see nori_input.h. */
#include "nori_input.h"

#include <string.h>

/* wrap-safe "a is at or after b" for 32-bit millisecond clocks */
static int after_eq(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

void ni_button_init(ni_button *b, int raw_pressed, uint32_t now)
{
    b->stable = raw_pressed ? 1 : 0;
    b->raw = b->stable;
    b->armed = !raw_pressed;
    b->raw_since = now;
}

int ni_button_update(ni_button *b, int raw_pressed, uint32_t now, uint16_t debounce_ms)
{
    uint8_t r = raw_pressed ? 1 : 0;
    if (r != b->raw) { b->raw = r; b->raw_since = now; return 0; }
    if (r == b->stable || now - b->raw_since < debounce_ms) return 0;
    b->stable = r;
    if (!b->armed) {                     /* held since boot: the first release only arms it */
        if (!r) b->armed = 1;
        return 0;
    }
    return r ? +1 : -1;
}

static uint8_t tog_pos(int raw_five, int raw_one)
{
    return (uint8_t)((raw_five ? NI_TOG_FIVE : 0) | (raw_one ? NI_TOG_ONE : 0));   /* both: NI_TOG_BOTH */
}

void ni_toggle_init(ni_toggle *t, int raw_five, int raw_one, uint32_t now)
{
    memset(t, 0, sizeof *t);
    t->stable = t->raw = tog_pos(raw_five, raw_one);
    t->locked = t->stable == NI_TOG_BOTH;
    t->raw_since = now;
}

int ni_toggle_update(ni_toggle *t, int raw_five, int raw_one, uint32_t now, uint16_t debounce_ms)
{
    uint8_t r = tog_pos(raw_five, raw_one);
    if (r != t->raw) { t->raw = r; t->raw_since = now; return NI_TOG_CENTER; }
    if (r == t->stable || now - t->raw_since < debounce_ms) return NI_TOG_CENTER;
    t->stable = r;
    if (r == NI_TOG_CENTER) { t->locked = 0; t->n_center++; return NI_TOG_CENTER; }
    if (r == NI_TOG_BOTH) { t->locked = 1; t->n_both++; return NI_TOG_CENTER; }
    if (t->locked) { t->n_locked++; return NI_TOG_CENTER; }
    if (r == NI_TOG_FIVE) t->n_five++; else t->n_one++;
    return r;
}

void ni_gesture_init(ni_gesture *g, const ni_toggle *t)
{
    memset(g, 0, sizeof *g);
    g->blocked = t->stable != NI_TOG_CENTER;
}

void ni_gesture_cancel(ni_gesture *g)
{
    if (g->pos && !g->fired) g->n_cancel++;
    g->pos = NI_TOG_CENTER;
    g->blocked = 1;
}

/* The gesture ended with its contact open since `end`.  Long if the loop missed the threshold. */
static int ges_end(ni_gesture *g, uint32_t end)
{
    int out = NI_GES_NONE;
    if (!g->fired) {
        int five = g->pos == NI_TOG_FIVE;
        if (end - g->since >= NI_GES_HOLD_MS) { out = five ? NI_GES_LONG_FIVE : NI_GES_LONG_ONE; g->n_long++; }
        else { out = five ? NI_GES_SHORT_FIVE : NI_GES_SHORT_ONE; g->n_short++; }
    }
    g->pos = NI_TOG_CENTER;
    return out;
}

int ni_gesture_update(ni_gesture *g, const ni_toggle *t, int pos, uint32_t now)
{
    int out = NI_GES_NONE;
    if (t->stable == NI_TOG_BOTH || t->locked) { ni_gesture_cancel(g); return NI_GES_NONE; }
    if (g->blocked) {
        if (t->stable != NI_TOG_CENTER) return NI_GES_NONE;
        g->blocked = 0;
    }
    if (g->pos && pos && pos != g->pos) {
        out = ges_end(g, t->raw_since);
    } else if (g->pos && t->stable == NI_TOG_CENTER) {
        if (t->raw == NI_TOG_CENTER && now - t->raw_since >= NI_GES_RELEASE_MS) out = ges_end(g, t->raw_since);
    } else if (g->pos && !g->fired && t->raw == g->pos && now - g->since >= NI_GES_HOLD_MS) {
        g->fired = 1;              /* raw: a contact already opening is a release, not a hold */
        g->n_long++;
        out = g->pos == NI_TOG_FIVE ? NI_GES_LONG_FIVE : NI_GES_LONG_ONE;
    }
    if (pos && !g->pos) {
        g->pos = (uint8_t)pos;
        g->fired = 0;
        g->since = t->raw_since;
    }
    return out;
}

void ni_tap_defaults(ni_tap_cfg *c)
{
    c->dtap_wait_ms = 420;
    c->refractory_ms = 700;
    c->shake_hits = 5;
    c->shake_window_ms = 2000;
    c->shake_quiet_ms = 1500;
}

void ni_tap_game_defaults(ni_tap_cfg *c)
{
    ni_tap_defaults(c);
    c->dtap_wait_ms = 0;
    c->refractory_ms = 80;
    /* Shake = 8 hits inside 700 ms (~11/s, only reachable just above the refractory):
     * a fast player (~7 taps/s) never trips it; the R1 5-in-2 s would mute at 3 taps/s. */
    c->shake_hits = 8;
    c->shake_window_ms = 700;
    c->shake_quiet_ms = 1000;
}

void ni_tap_init(ni_tap *t) { memset(t, 0, sizeof *t); }

static int count_recent(const ni_tap *t, uint32_t now, uint16_t window)
{
    int n = 0;
    for (int i = 0; i < t->nhits; i++)
        if (now - t->hits[i] <= window) n++;
    return n;
}

static void add_hit(ni_tap *t, uint32_t now)
{
    t->hits[t->hpos] = now;
    t->hpos = (uint8_t)((t->hpos + 1) % NI_SHAKE_HIST);
    if (t->nhits < NI_SHAKE_HIST) t->nhits++;
}

int ni_tap_update(ni_tap *t, const ni_tap_cfg *c, int single, int dbl, uint32_t now)
{
    int hits = (single ? 1 : 0) + (dbl ? 1 : 0);
    for (int i = 0; i < hits; i++) add_hit(t, now);

    if (hits && c->shake_hits && count_recent(t, now, c->shake_window_ms) >= c->shake_hits) {
        t->muted = 1;                    /* shaking or rattling: say nothing until it settles */
        t->pending = 0;
    }
    if (t->muted) {
        if (hits) { t->muted_until = now + c->shake_quiet_ms; t->suppressed += (uint32_t)hits; return NI_NONE; }
        if (!after_eq(now, t->muted_until)) return NI_NONE;
        t->muted = 0;
        t->nhits = 0;
    }

    if (t->refr && after_eq(now, t->refractory_until)) t->refr = 0;
    if (hits && t->refr) { t->suppressed += (uint32_t)hits; single = dbl = 0; }

    /* R4: a double flag with nothing pending belongs to a gesture whose first tap
     * was already reported (wait 0, or the wait ran out first): its second tap is
     * one more TAP.  Reporting a DTAP here would repeat the first tap. */
    if (dbl && !single && !t->pending) { single = 1; dbl = 0; }

    int out = NI_NONE;
    if (dbl || (single && t->pending)) {
        out = NI_DTAP;                   /* the chip saw two taps, or we saw two singles inside the wait */
        t->pending = 0;
    } else if (t->pending && now - t->pending_at >= c->dtap_wait_ms) {
        out = NI_TAP;
        t->pending = 0;
    } else if (single) {
        if (c->dtap_wait_ms == 0) out = NI_TAP;
        else { t->pending = 1; t->pending_at = now; }
    }
    if (out != NI_NONE) { t->refr = 1; t->refractory_until = now + c->refractory_ms; }
    return out;
}

int ni_bucket_take(ni_bucket *b, uint32_t now, uint16_t per_sec, uint8_t burst)
{
    uint32_t cap = (uint32_t)burst * 1000u;
    if (!b->init) { b->init = 1; b->milli = cap; b->last = now; }
    uint32_t dt = now - b->last;
    b->last = now;
    uint64_t m = (uint64_t)b->milli + (uint64_t)dt * per_sec;
    b->milli = m > cap ? cap : (uint32_t)m;
    if (b->milli < 1000u) return 0;
    b->milli -= 1000u;
    return 1;
}
