/* wt32-r3 VL53L5CX supervisor; see nori_tofsm.h. */
#include "nori_tofsm.h"

#include <stdio.h>
#include <string.h>

const char *const NT_STAGE_NAME[NT_S_COUNT] = { "off", "probe", "idchk", "init", "config", "start", "ranging", "stop",
                                                "backoff", "paused" };
const char *const NT_ACTION_NAME[] = { "wait", "probe", "idchk", "init", "config", "start", "poll", "stop", "busclr" };

#define START_DELAY_MS 3000u        /* after boot: let Ethernet, the UART link and HELLO come up first */

void nt_defaults(nt_cfg *c)
{
    c->poll_ms = 40;
    c->frame_timeout_ms = 1500;
    c->max_bad = 5;
    c->absent_min_ms = 2000;
    c->absent_max_ms = 60000;
    c->fail_min_ms = 5000;
    c->fail_max_ms = 60000;
    c->stable_ms = 60000;
}

void nt_init(nt_sm *s, int enabled, uint32_t now)
{
    memset(s, 0, sizeof *s);
    s->enabled = enabled;
    s->stage = enabled ? NT_S_PROBE : NT_S_OFF;
    s->act = -1;
    s->wait_until = now + START_DELAY_MS;
    s->last_fail_act = s->last_fail_res = -1;
}

static int dmin(uint32_t a, uint32_t b) { return (int)(a < b ? a : b); }

static void reset_backoff(nt_sm *s)
{
    s->backoff_ms = 0;
    s->absent_ms = 0;
}

static uint32_t next_backoff(uint32_t *cur, uint32_t lo, uint32_t hi)
{
    uint32_t b = *cur ? *cur : lo;
    *cur = b >= hi / 2 ? hi : b * 2;
    return b > hi ? hi : b;
}

/* A failure: STOP first if ranging may be on, then PROBE after the backoff. */
static void fail(nt_sm *s, const nt_cfg *c, int act, int res, uint32_t now)
{
    s->last_fail_act = act;
    s->last_fail_res = res;
    s->last_fail_ms = now;
    uint32_t b = next_backoff(&s->backoff_ms, c->fail_min_ms, c->fail_max_ms);
    s->resume_at = now + b;
    s->stage = s->need_stop ? NT_S_STOP : NT_S_BACKOFF;
    s->wait_until = s->stage == NT_S_STOP ? now : s->resume_at;   /* stop at once; the backoff follows the stop */
    s->bad_run = 0;
}

void nt_enable(nt_sm *s, int on, uint32_t now)
{
    if (on == s->enabled) return;
    s->enabled = on;
    if (on) {
        s->stage = NT_S_PROBE;
        s->wait_until = s->resume_at = now;
        reset_backoff(s);
    } else s->stage = s->need_stop ? NT_S_STOP : NT_S_OFF;
}

void nt_pause(nt_sm *s, int on, uint32_t now)
{
    if (on == s->paused) return;
    s->paused = on;
    if (!on && s->stage == NT_S_RANGING) {         /* the gap is not the sensor's fault */
        s->last_frame = now;
        s->next_poll = now;
        s->bad_run = 0;
    }
}

void nt_restart(nt_sm *s, uint32_t now)
{
    if (!s->enabled) return;
    s->restarts++;
    s->stage = s->need_stop ? NT_S_STOP : NT_S_PROBE;
    s->wait_until = s->resume_at = now;
    s->bus_dirty = 1;
    reset_backoff(s);
}

int nt_ranging(const nt_sm *s) { return s->enabled && !s->paused && s->stage == NT_S_RANGING; }

static int give(nt_sm *s, int act, uint32_t now)
{
    s->act = act;
    s->act_started = now;
    return act;
}

int nt_next(nt_sm *s, const nt_cfg *c, uint32_t now, uint32_t *wait_ms)
{
    *wait_ms = 0;
    if (s->act >= 0) return s->act;                 /* the glue never got to report: hand it out again */
    if (s->paused) { *wait_ms = 100; return NT_A_WAIT; }
    if (!s->enabled) {
        if (s->stage == NT_S_STOP && s->need_stop) return give(s, NT_A_STOP, now);
        s->stage = NT_S_OFF;
        *wait_ms = 1000;
        return NT_A_WAIT;
    }
    if ((int32_t)(now - s->wait_until) < 0) {
        *wait_ms = s->wait_until - now;
        return NT_A_WAIT;
    }
    switch (s->stage) {
    case NT_S_OFF:
    case NT_S_BACKOFF:
        s->stage = NT_S_PROBE;
        /* fall through */
    case NT_S_PROBE:
        return give(s, s->bus_dirty ? NT_A_BUSCLR : NT_A_PROBE, now);
    case NT_S_IDCHK: return give(s, NT_A_IDCHK, now);
    case NT_S_INIT: return give(s, s->bus_dirty ? NT_A_BUSCLR : NT_A_INIT, now);
    case NT_S_CONFIG: return give(s, NT_A_CONFIG, now);
    case NT_S_START: return give(s, NT_A_START, now);
    case NT_S_STOP: return give(s, NT_A_STOP, now);
    case NT_S_RANGING:
        if (now - s->last_frame >= c->frame_timeout_ms) {
            s->timeouts++;
            s->restarts++;
            fail(s, c, NT_A_POLL, NT_R_NOTREADY, now);
            return nt_next(s, c, now, wait_ms);
        }
        if ((int32_t)(now - s->next_poll) < 0) {
            *wait_ms = (uint32_t)dmin(s->next_poll - now, c->frame_timeout_ms);
            return NT_A_WAIT;
        }
        return give(s, NT_A_POLL, now);
    default:
        s->stage = NT_S_PROBE;
        *wait_ms = 100;
        return NT_A_WAIT;
    }
}

int nt_result(nt_sm *s, const nt_cfg *c, int res, uint32_t now)
{
    int act = s->act, was_ranging = s->stage == NT_S_RANGING;
    s->act = -1;
    if (res == NT_R_BUS) { s->bus_errs++; s->bus_dirty = 1; }
    switch (act) {
    case NT_A_BUSCLR:
        s->busclrs++;
        if (res == NT_R_OK) { s->bus_dirty = 0; break; }
        /* review fix: a failed clear (driver reinstall failed) stays dirty and is retried only after a
         * backoff, never in a tight loop and never followed by INIT on a bus without a driver */
        s->busclr_fail++;
        s->bus_dirty = 1;
        s->last_fail_act = act; s->last_fail_res = res; s->last_fail_ms = now;
        s->wait_until = now + next_backoff(&s->backoff_ms, c->fail_min_ms, c->fail_max_ms);
        break;
    case NT_A_PROBE:
        s->probes++;
        if (res == NT_R_OK) {
            s->present = 1;
            s->absent_ms = 0;
            s->stage = NT_S_IDCHK;
        } else {
            if (res == NT_R_NACK) s->nacks++;
            s->last_fail_act = act; s->last_fail_res = res; s->last_fail_ms = now;
            s->wait_until = now + next_backoff(&s->absent_ms, c->absent_min_ms, c->absent_max_ms);
        }
        break;
    case NT_A_IDCHK:
        if (res == NT_R_OK) s->stage = NT_S_INIT;
        else { s->id_fail++; fail(s, c, act, res, now); }
        break;
    case NT_A_INIT:
        if (res == NT_R_OK) {
            s->inits_ok++;
            s->init_ms_last = now - s->act_started;
            s->stage = NT_S_CONFIG;
        } else {
            s->inits_fail++;
            s->bus_dirty = 1;                       /* a half-done upload: clear the bus before the next try */
            fail(s, c, act, res, now);
        }
        break;
    case NT_A_CONFIG:
        if (res == NT_R_OK) s->stage = NT_S_START;
        else { s->cfg_fail++; fail(s, c, act, res, now); }
        break;
    case NT_A_START:
        s->need_stop = 1;                          /* even a failed start may have started the session */
        if (res == NT_R_OK) {
            s->stage = NT_S_RANGING;
            s->ranging_since = s->last_frame = now;
            s->next_poll = now + c->poll_ms;
            s->bad_run = 0;
        } else { s->start_fail++; fail(s, c, act, res, now); }
        break;
    case NT_A_POLL:
        s->next_poll = now + c->poll_ms;
        if (res == NT_R_OK) {
            s->frames++;
            s->last_frame = now;
            s->bad_run = 0;
            if (now - s->ranging_since >= c->stable_ms) reset_backoff(s);
        } else if (res != NT_R_NOTREADY) {
            s->bad_frames++;
            if (res == NT_R_BADFRAME) s->last_frame = now;   /* the sensor is talking, the data is not usable */
            if (++s->bad_run >= c->max_bad) { s->restarts++; fail(s, c, act, res, now); }
        }
        break;
    case NT_A_STOP:
        if (res != NT_R_OK) s->stops_fail++;
        s->need_stop = 0;
        if (!s->enabled) s->stage = NT_S_OFF;
        else {
            s->stage = NT_S_BACKOFF;               /* then PROBE once the backoff fail() chose is over */
            s->wait_until = (int32_t)(s->resume_at - now) > 0 ? s->resume_at : now;
            s->bus_dirty = s->bus_dirty || res != NT_R_OK;
        }
        break;
    default:
        break;
    }
    return was_ranging && s->stage != NT_S_RANGING;
}

int nt_diag(const nt_sm *s, char *out, size_t cap)
{
    const char *st = s->paused ? "paused" : (s->stage == NT_S_PROBE || s->stage == NT_S_BACKOFF) && !s->present && s->nacks ? "absent"
                                                                                                                         : NT_STAGE_NAME[s->stage];
    char lf[24] = "-";
    if (s->last_fail_act >= 0) snprintf(lf, sizeof lf, "%s/%d", NT_ACTION_NAME[s->last_fail_act], s->last_fail_res);
    return snprintf(out, cap, "tof=%s f=%lu bf=%lu to=%lu rs=%lu ini=%lu/%lu ims=%lu pr=%lu nk=%lu be=%lu bc=%lu lf=%s",
                    st, (unsigned long)s->frames, (unsigned long)s->bad_frames, (unsigned long)s->timeouts,
                    (unsigned long)s->restarts, (unsigned long)s->inits_ok, (unsigned long)s->inits_fail,
                    (unsigned long)s->init_ms_last, (unsigned long)s->probes, (unsigned long)s->nacks,
                    (unsigned long)s->bus_errs, (unsigned long)s->busclrs, lf);
}
