/* See nori_wnsm.h. */
#include "nori_wnsm.h"

#include <string.h>

const char *const NWS_STATE_NAME[NWS_STATES] = { "off", "scan", "join", "up", "bgscan", "leave", "wait" };
const char *const NWF_NAME[NWF_COUNT] = { "none", "noap", "auth", "assoc", "dhcp", "lost", "begin" };
const char *const NWW_NAME[NWW_COUNT] = { "none", "boot", "lost", "failed", "upgrade", "config" };

enum { K_NORMAL, K_FAST, K_UPGRADE };
enum { A_NEXT, A_SCAN, A_JOIN };
#define R_ASSOC_LEAVE 8        /* our own disconnect (or the core's first_connect retry): says nothing about this attempt */

static int due(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
static void note(nws *s, int what, int i, int cls, int reason)
{
    if (s->hal->note) s->hal->note(s->hal->ctx, what, i, cls, reason);
}

int nws_class(uint8_t r)
{
    switch (r) {
    case 201: return NWF_NOAP;                                   /* NO_AP_FOUND */
    case 2: case 15: case 23: case 202: case 204: return NWF_AUTH;   /* AUTH_EXPIRE, 4WAY_HANDSHAKE_TIMEOUT, 802_1X, AUTH_FAIL, HANDSHAKE_TIMEOUT */
    default: return NWF_ASSOC;
    }
}

void nws_init(nws *s, const nws_hal *hal)
{
    memset(s, 0, sizeof *s);
    s->hal = hal;
    s->state = NWS_OFF;
    s->cur = s->prev = s->cand = -1;
    s->rescan_ms = NWS_RESCAN_MIN_MS;
}

static uint32_t backoff(uint32_t base, uint32_t max, unsigned n)
{
    uint32_t v = base << (n < 8 ? n : 8);
    return v < max ? v : max;
}

static void go_leave(nws *s, int after, int idx, int kind, uint32_t now)
{
    s->hal->leave(s->hal->ctx);
    s->state = NWS_LEAVE;
    s->t_leave = now;
    s->after = after;
    s->after_idx = idx;
    s->after_kind = kind;
}

static void fail(nws *s, int i, int cls, uint8_t reason, uint32_t now)
{
    nws_prof *p = &s->p[i];
    p->fails[cls]++;
    p->last_cls = s->last_cls = (uint8_t)cls;
    p->last_reason = s->last_reason = reason;
    note(s, NWE_FAIL, i, cls, reason);
    if (cls == NWF_NOAP) {
        if (s->kind == K_FAST) { /* the access point went away: the next scan decides, no penalty */ }
        else if (p->vis && !s->blind) { p->nb = now + NWS_STALE_MS; p->nb_on = 1; }
        else { p->bnb = now + backoff(NWS_BLIND_MS, NWS_BLIND_MAX_MS, p->bfc); p->bnb_on = 1; if (p->bfc < 255) p->bfc++; }
    } else {
        p->nb = now + backoff(NWS_BACKOFF_MS, NWS_BACKOFF_MAX_MS, p->fc);
        p->nb_on = 1;
        if (p->fc < 255) p->fc++;
    }
    s->cur = -1;
    if (s->kind == K_UPGRADE) {
        s->upgrade_fails++;
        uint32_t hold = now + NWS_UPGRADE_HOLD_MS;
        if (!p->nb_on || (int32_t)(hold - p->nb) > 0) p->nb = hold;
        p->nb_on = 1;
        go_leave(s, A_JOIN, s->prev, K_FAST, now);
    } else {
        s->why = NWW_FAILED;
        go_leave(s, s->kind == K_FAST ? A_SCAN : A_NEXT, 0, K_NORMAL, now);
    }
}

static void start_join(nws *s, int i, int kind, const nws_in *in, uint32_t now)
{
    s->kind = kind;
    s->cur = i;
    s->p[i].tries++;
    s->p[i].tried = 1;
    s->joins++;
    s->disc_base = in->disc_seq;
    s->assoc_seen = 0;
    s->t_join = now;
    s->state = NWS_JOIN;
    note(s, NWE_JOIN, i, 0, s->why);
    if (s->hal->join(s->hal->ctx, i) != 0) fail(s, i, NWF_BEGIN, 0, now);
}

static int eligible(const nws *s, const nw_table *t, int i)
{
    const nws_prof *p = &s->p[i];
    if (p->tried || p->nb_on) return 0;
    if (p->vis && !s->blind) return 1;
    return ((t->net[i].flags & NW_F_HIDDEN) || s->blind) && !p->bnb_on;
}

static void next_attempt(nws *s, const nw_table *t, const nws_in *in, uint32_t now)
{
    for (int i = 0; i < t->n; i++) {
        if (eligible(s, t, i)) { start_join(s, i, K_NORMAL, in, now); return; }
    }
    s->rounds++;
    s->cur = -1;
    s->state = NWS_WAIT;
    s->wait_until = now + s->rescan_ms;
    s->rescan_ms = s->rescan_ms * 2 < NWS_RESCAN_MAX_MS ? s->rescan_ms * 2 : NWS_RESCAN_MAX_MS;
}

static void round_begin(nws *s, const nw_table *t, const nws_in *in, uint32_t now, int blind)
{
    s->blind = blind;
    for (int i = 0; i < NW_MAX; i++) s->p[i].tried = 0;
    next_attempt(s, t, in, now);
}

static void start_scan(nws *s, const nw_table *t, const nws_in *in, uint32_t now)
{
    s->cur = -1;
    s->scans++;
    if (s->hal->scan_start(s->hal->ctx, 0) != 0) {
        s->scan_fails++;
        note(s, NWE_SCANFAIL, 0, 0, 0);
        round_begin(s, t, in, now, 1);
        return;
    }
    s->state = NWS_SCAN;
    s->t_scan = now;
}

static void store_seen(nws *s, const nws_seen *seen)
{
    for (int i = 0; i < NW_MAX; i++) { s->p[i].vis = seen[i].vis; s->p[i].rssi = seen[i].rssi; }
}

static void link_up(nws *s, uint32_t now)
{
    nws_prof *p = &s->p[s->cur];
    p->oks++;
    if ((uint32_t)s->cur + 1 != s->last_up && s->last_up) s->switches++;
    s->last_up = (uint32_t)s->cur + 1;
    if (s->kind == K_UPGRADE) s->upgrades++;
    s->state = NWS_UP;
    s->t_up = now;
    s->stable = s->ipl_on = 0;
    s->cand = -1;
    s->cand_n = 0;
    s->rescan_ms = NWS_RESCAN_MIN_MS;
    s->next_bg = now + NWS_BG_FIRST_MS;
    note(s, NWE_UP, s->cur, 0, s->why);
}

static void link_lost(nws *s, const nws_in *in, uint32_t now)
{
    if (s->state == NWS_BG) s->hal->scan_stop(s->hal->ctx);
    s->state = NWS_UP;
    if (s->stable) {
        s->why = NWW_LOST;
        start_join(s, s->cur, K_FAST, in, now);
    } else {
        s->kind = K_NORMAL;
        fail(s, s->cur, NWF_LOST, in->reason, now);
        s->after = A_SCAN;
    }
}

static int any_better(const nws *s)
{
    for (int i = 0; i < s->cur; i++) if (!s->p[i].nb_on) return 1;
    return 0;
}

static void bg_result(nws *s, const nws_in *in, uint32_t now)
{
    int best = -1;
    for (int i = 0; i < s->cur && best < 0; i++)
        if (s->p[i].vis && s->p[i].rssi >= NWS_UPGRADE_RSSI && !s->p[i].nb_on) best = i;
    s->state = NWS_UP;
    if (best < 0) {
        s->cand = -1;
        s->cand_n = 0;
        s->next_bg = now + NWS_BG_PERIOD_MS;
        return;
    }
    s->cand_n = best == s->cand ? s->cand_n + 1 : 1;
    s->cand = best;
    s->next_bg = now + NWS_BG_CONFIRM_MS;
    if (s->cand_n >= 2 && !in->no_bg) {
        s->prev = s->cur;
        s->why = NWW_UPGRADE;
        note(s, NWE_UPGRADE, best, 0, s->cur);
        s->cur = -1;
        go_leave(s, A_JOIN, best, K_UPGRADE, now);
    }
}

static void do_kick(nws *s, uint32_t now)
{
    s->kick = 0;
    if (s->state == NWS_SCAN || s->state == NWS_BG) s->hal->scan_stop(s->hal->ctx);
    s->why = NWW_CONFIG;
    s->rescan_ms = NWS_RESCAN_MIN_MS;
    s->cur = -1;
    go_leave(s, A_SCAN, 0, K_NORMAL, now);
}

void nws_tick(nws *s, const nw_table *t, const nws_in *in, uint32_t now)
{
    for (int i = 0; i < NW_MAX; i++) {             /* cleared here so a clock wrap can never revive an old time */
        if (s->p[i].nb_on && due(now, s->p[i].nb)) s->p[i].nb_on = 0;
        if (s->p[i].bnb_on && due(now, s->p[i].bnb)) s->p[i].bnb_on = 0;
    }
    if (!in->wanted) {
        if (s->state == NWS_OFF) return;
        if (s->state == NWS_SCAN || s->state == NWS_BG) s->hal->scan_stop(s->hal->ctx);
        s->hal->radio_off(s->hal->ctx);
        s->state = NWS_OFF;
        s->cur = -1;
        s->kick = s->held = 0;
        return;
    }
    if (s->state == NWS_OFF) {
        s->hal->radio_on(s->hal->ctx);
        s->why = NWW_BOOT;
        s->rescan_ms = NWS_RESCAN_MIN_MS;
        for (int i = 0; i < NW_MAX; i++) s->p[i].nb_on = s->p[i].bnb_on = s->p[i].fc = s->p[i].bfc = 0;
        start_scan(s, t, in, now);
        return;
    }
    if (in->hold) {
        if (!s->held) s->holds++;
        s->held = 1;
        if (s->state == NWS_BG) {
            s->hal->scan_stop(s->hal->ctx);
            s->state = NWS_UP;
        }
        return;
    }
    if (s->held) {                                 /* time under hold does not count against anything */
        s->held = 0;
        s->t_join = s->t_assoc = s->t_ipl = s->t_scan = s->t_leave = now;
        if (s->state == NWS_UP) s->next_bg = now + NWS_BG_FIRST_MS;
    }
    if (s->kick) { do_kick(s, now); return; }

    switch (s->state) {
    case NWS_SCAN: {
        nws_seen seen[NW_MAX];
        memset(seen, 0, sizeof seen);
        int r = s->hal->scan_poll(s->hal->ctx, seen);
        if (r == 0 && !due(now, s->t_scan + NWS_SCAN_MAX_MS)) break;
        if (r == 1) {
            store_seen(s, seen);
            round_begin(s, t, in, now, 0);
        } else {
            if (r == 0) s->hal->scan_stop(s->hal->ctx);
            s->scan_fails++;
            note(s, NWE_SCANFAIL, 0, 0, 0);
            round_begin(s, t, in, now, 1);
        }
        break;
    }
    case NWS_JOIN:
        if (in->assoc && in->ip) { link_up(s, now); break; }
        if (in->disc_seq != s->disc_base) {
            s->disc_base = in->disc_seq;
            if (!in->assoc && in->reason != R_ASSOC_LEAVE) { fail(s, s->cur, nws_class(in->reason), in->reason, now); break; }
        }
        if (in->assoc) {
            if (!s->assoc_seen) { s->assoc_seen = 1; s->t_assoc = now; }
            if (due(now, s->t_assoc + NWS_DHCP_MS)) fail(s, s->cur, NWF_DHCP, 0, now);
        } else if (due(now, s->t_join + NWS_ASSOC_MS)) {
            fail(s, s->cur, NWF_ASSOC, in->reason, now);
        }
        break;
    case NWS_UP:
    case NWS_BG:
        if (!in->assoc) { link_lost(s, in, now); break; }
        if (!in->ip) {
            if (!s->ipl_on) { s->ipl_on = 1; s->t_ipl = now; }
            else if (due(now, s->t_ipl + NWS_IP_LOSS_MS)) {
                if (s->state == NWS_BG) s->hal->scan_stop(s->hal->ctx);
                s->state = NWS_UP;
                s->kind = K_NORMAL;
                fail(s, s->cur, NWF_DHCP, 0, now);
                s->after = A_SCAN;
            }
            break;
        }
        s->ipl_on = 0;
        if (!s->stable && due(now, s->t_up + NWS_STABLE_MS)) {
            s->stable = 1;
            s->p[s->cur].fc = s->p[s->cur].bfc = 0;
        }
        if (s->state == NWS_BG) {
            nws_seen seen[NW_MAX];
            memset(seen, 0, sizeof seen);
            int r = s->hal->scan_poll(s->hal->ctx, seen);
            if (r == 1) {
                store_seen(s, seen);
                bg_result(s, in, now);
            } else if (r < 0 || due(now, s->t_scan + NWS_BG_MAX_MS)) {
                if (r == 0) s->hal->scan_stop(s->hal->ctx);
                s->scan_fails++;
                s->state = NWS_UP;
                s->next_bg = now + NWS_BG_PERIOD_MS;
            }
        } else if (s->cur > 0 && s->stable && due(now, s->next_bg)) {
            if (in->no_bg) { s->cand_n = 0; s->next_bg = now + NWS_BG_CONFIRM_MS; }
            else if (!any_better(s)) s->next_bg = now + NWS_BG_PERIOD_MS;
            else if (s->hal->scan_start(s->hal->ctx, 1) != 0) { s->scan_fails++; s->next_bg = now + NWS_BG_PERIOD_MS; }
            else { s->state = NWS_BG; s->t_scan = now; s->bg_scans++; }
        }
        break;
    case NWS_LEAVE:
        if ((in->assoc || !due(now, s->t_leave + NWS_SETTLE_MS)) && !due(now, s->t_leave + NWS_LEAVE_MAX_MS)) break;
        if (s->after == A_NEXT) next_attempt(s, t, in, now);
        else if (s->after == A_SCAN) start_scan(s, t, in, now);
        else start_join(s, s->after_idx, s->after_kind, in, now);
        break;
    case NWS_WAIT:
        if (due(now, s->wait_until)) start_scan(s, t, in, now);
        break;
    default:
        break;
    }
}

void nws_remap(nws *s, const nw_table *old, const nw_table *now_t, uint32_t now)
{
    static nws_prof np[NW_MAX];
    int cur = -1, was_up = s->state == NWS_UP || s->state == NWS_BG;
    memset(np, 0, sizeof np);
    for (int i = 0; i < now_t->n; i++) {
        const nw_net *e = &now_t->net[i];
        int j = nw_find(old, e->ssid, e->ssid_len);
        if (j < 0) continue;
        if (nw_same_login(&old->net[j], e)) {
            np[i] = s->p[j];
            if (j == s->cur) cur = i;
        } else {                                   /* a new password is a new chance; the counters stay */
            np[i].tries = s->p[j].tries;
            np[i].oks = s->p[j].oks;
            memcpy(np[i].fails, s->p[j].fails, sizeof np[i].fails);
        }
    }
    memcpy(s->p, np, sizeof np);
    s->cand = -1;
    s->cand_n = 0;
    if (s->state == NWS_OFF) { s->cur = -1; return; }
    if (was_up && cur >= 0) {                      /* still on a saved login: the link stays exactly as it is */
        if (s->state == NWS_BG) { s->hal->scan_stop(s->hal->ctx); s->state = NWS_UP; }
        s->cur = cur;
        s->last_up = (uint32_t)cur + 1;
        s->next_bg = now + NWS_RESCAN_MIN_MS;
    } else {
        s->kick = 1;                               /* removed or changed under us, or between links: reselect */
    }
}

void nws_kick(nws *s)
{
    if ((s->state == NWS_UP || s->state == NWS_BG) && s->cur == 0) return;
    if (s->state != NWS_OFF) s->kick = 1;
}

int nws_active(const nws *s) { return s->state == NWS_UP || s->state == NWS_BG ? s->cur : -1; }
