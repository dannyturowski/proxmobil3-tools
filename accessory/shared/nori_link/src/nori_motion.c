/* Polled-accelerometer tap detector (ni_motion, r4) with the r5.1 optional axis gate; see
 * nori_input.h.  r5.1 moved it out of nori_input.c: the Xtensa assembler's branch-target
 * padding depends on what else is in a translation unit, so keeping the grown detector apart
 * leaves the button/tap code of the builds that never use it assembled exactly as in r5. */
#include "nori_input.h"

#include <stdio.h>
#include <string.h>

static uint32_t isqrt64(int64_t v)
{
    uint64_t x = (uint64_t)v, r = 0, bit = 1ull << 62;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

void ni_motion_defaults(ni_motion_cfg *c, uint16_t odr_hz)
{
    if (!odr_hz) odr_hz = 400;
    c->ths_mg = 1000;
    c->rearm_mg = 400;
    c->quiet_samples = (uint16_t)(odr_hz / 25 ? odr_hz / 25 : 1);      /* 40 ms */
    c->warmup_samples = (uint16_t)(odr_hz * 4 / 25 ? odr_hz * 4 / 25 : 4);   /* 160 ms */
    c->hp_shift = odr_hz >= 800 ? 5 : odr_hz >= 400 ? 4 : odr_hz >= 200 ? 3 : 2;   /* ~40 ms */
    c->axis = NI_AXIS_ANY;                                            /* r5.1: gate off unless chosen */
    c->axis_pct = 70;
    c->peak_samples = (uint8_t)(odr_hz / 100 ? (odr_hz / 100 > 255 ? 255 : odr_hz / 100) : 1);   /* 10 ms */
    c->reject_hold_samples = (uint16_t)(odr_hz * 80u / 1000u);        /* 80 ms, the game refractory */
    c->freeze_samples = 0;                                            /* r5.2: baseline hold off */
    c->retrig_window = 0;                                             /* r5.2: onset re-trigger off */
    c->retrig_min = 0;
}

void ni_motion_reset(ni_motion *m)
{
    uint32_t hits = m->hits, peak = m->peak_mg, rej = m->rejects, held = m->held, retrigs = m->retrigs;
    uint32_t dom[3] = { m->dom[0], m->dom[1], m->dom[2] };
    ni_peak last = m->last;
    memset(m, 0, sizeof *m);          /* r5.1: a pending candidate is dropped: never decided across a gap */
    m->hits = hits;
    m->peak_mg = peak;
    m->rejects = rej;
    m->held = held;
    m->retrigs = retrigs;
    memcpy(m->dom, dom, sizeof dom);
    m->last = last;
}

/* r5.1: the candidate's peak is known; record it and, for a gated candidate, decide. */
static int cand_finish(ni_motion *m, const ni_motion_cfg *c)
{
    int out = 0;
    uint32_t mag = isqrt64(m->cand_r2);
    int dom = 0;
    for (int i = 0; i < 3; i++) {
        int64_t a = m->cand_v[i] < 0 ? -(int64_t)m->cand_v[i] : m->cand_v[i];
        m->last.share[i] = (uint8_t)(mag ? (a * 100 / mag > 100 ? 100 : a * 100 / mag) : 0);
        int64_t b = m->cand_v[dom] < 0 ? -(int64_t)m->cand_v[dom] : m->cand_v[dom];
        if (a > b) dom = i;
    }
    m->last.mg = mag;
    m->dom[dom]++;
    if (m->cand == NI_CAND_ACCEPTED) m->last.verdict = 'A';
    else if (m->cand == NI_CAND_HELD) m->last.verdict = 'H';
    else {
        int ax = c->axis >= 1 && c->axis <= 3 ? c->axis - 1 : 2;
        int64_t a = m->cand_v[ax], pct = c->axis_pct;
        if (a * a * 10000 >= pct * pct * m->cand_r2) {        /* |a_axis| / |a| >= pct / 100, exact */
            m->hits++;
            m->last.verdict = 'A';
            out = 1;
        } else {
            m->rejects++;
            m->hold = c->reject_hold_samples;
            m->last.verdict = 'R';
        }
    }
    m->cand = NI_CAND_NONE;
    return out;
}

int ni_motion_sample(ni_motion *m, const ni_motion_cfg *c, int32_t x, int32_t y, int32_t z)
{
    const int32_t v[3] = { x, y, z };
    if (m->n == 0)
        for (int i = 0; i < 3; i++) m->base[i] = v[i] * 256;
    int64_t r2 = 0;
    int32_t div = 1 << (m->n < c->warmup_samples ? 2 : c->hp_shift);   /* settle fast while warming up */
    int32_t res[3], dv[3];
    for (int i = 0; i < 3; i++) {
        int32_t d = v[i] * 256 - m->base[i];                /* residual before this sample moves the baseline */
        int64_t dm = d / 256;
        res[i] = (int32_t)dm;
        dv[i] = d;
        r2 += dm * dm;
    }
    int64_t ths = c->ths_mg, rearm = c->rearm_mg < c->ths_mg ? c->rearm_mg : c->ths_mg;
    /* r5.2 onset re-trigger: decided against the history before this sample joins it */
    int onset = 0;
    uint8_t w = c->retrig_window > NI_RETRIG_MAX ? NI_RETRIG_MAX : c->retrig_window;
    if (w && !m->armed && m->n >= c->warmup_samples && m->since >= c->retrig_min && m->since >= w && r2 > ths * ths) {
        int64_t mx = 0;
        for (uint8_t i = 0; i < w; i++) if (m->win[i] > mx) mx = m->win[i];
        onset = r2 > 9 * mx;                                /* |r| > 3 x the recent peak */
    }
    /* r5.2 baseline hold: from the crossing (this sample, if it is one) while loud, at most freeze_samples */
    int hold = c->freeze_samples && m->n >= c->warmup_samples && r2 >= rearm * rearm &&
               (m->frz || ((m->armed || onset) && r2 > ths * ths));
    if (m->frz) m->frz--;
    if (!hold)
        for (int i = 0; i < 3; i++) m->base[i] += dv[i] / div;
    if (m->n < UINT32_MAX) m->n++;
    if (r2 > (int64_t)m->peak_mg * m->peak_mg) m->peak_mg = isqrt64(r2);
    if (m->n <= c->warmup_samples) { m->armed = 0; m->quiet = 0; return 0; }

    /* r5.1: follow a candidate to its peak (r5 arming below is untouched by this) */
    int out = 0;
    if (m->hold) m->hold--;
    if (m->cand) {
        if (r2 > m->cand_r2) { m->cand_r2 = r2; memcpy(m->cand_v, res, sizeof res); }
        if (++m->cand_n >= c->peak_samples) out = cand_finish(m, c);
    }

    if (w) {                                                /* r5.2: history for the onset re-trigger */
        m->win[m->wpos] = r2;
        m->wpos = (uint8_t)((m->wpos + 1) % w);
        if (m->since < UINT16_MAX) m->since++;
        if (onset) { m->retrigs++; m->armed = 1; }
    }
    if (m->armed) {
        if (r2 <= ths * ths) return out;
        m->since = 0;
        m->armed = 0;
        m->quiet = 0;
        m->frz = c->freeze_samples;                         /* r5.2 (0 unless the hold is on) */
        if (m->cand) out |= cand_finish(m, c);              /* only if quiet_samples < peak_samples */
        m->cand_n = 1;
        m->cand_r2 = r2;
        memcpy(m->cand_v, res, sizeof res);
        if (m->hold) { m->cand = NI_CAND_HELD; m->held++; }
        else if (c->axis == NI_AXIS_ANY) {                  /* gate off: the r5 hit, at the crossing */
            m->cand = NI_CAND_ACCEPTED;
            m->hits++;
            out = 1;
        } else m->cand = NI_CAND_PENDING;
        if (c->peak_samples <= 1) out |= cand_finish(m, c);
        return out;
    }
    if (r2 < rearm * rearm) {
        if (++m->quiet >= c->quiet_samples) { m->armed = 1; m->frz = 0; }
    } else m->quiet = 0;
    return out;
}

int ni_motion_diag(ni_motion *m, const ni_motion_cfg *c, char *out, size_t cap)
{
    static const char *const AX[] = { "any", "x", "y", "z" };
    const ni_peak *p = &m->last;
    char pk[48];
    if (p->verdict) snprintf(pk, sizeof pk, "%c/x%u/y%u/z%u/%lu", p->verdict, p->share[0], p->share[1], p->share[2], (unsigned long)p->mg);
    else snprintf(pk, sizeof pk, "-");
    int n = snprintf(out, cap, "ax=%s/%u rej=%lu held=%lu pk=%s dom=%lu/%lu/%lu", AX[c->axis <= 3 ? c->axis : 0], c->axis_pct,
                     (unsigned long)m->rejects, (unsigned long)m->held, pk,
                     (unsigned long)m->dom[0], (unsigned long)m->dom[1], (unsigned long)m->dom[2]);
    memset(m->dom, 0, sizeof m->dom);
    return n;
}
