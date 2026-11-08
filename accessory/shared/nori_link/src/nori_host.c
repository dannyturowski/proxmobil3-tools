/* Host-side accessory session; see nori_host.h. */
#include "nori_host.h"

#include <stdio.h>
#include <string.h>

void nh_open(nh_session *s, const char *role, int want_tun, double now)
{
    nh_session keep = *s;            /* our sequence and the diagnostics survive a reopen */
    memset(s, 0, sizeof *s);
    s->seq_tx = keep.seq_tx;
    s->n_dup = keep.n_dup; s->n_stale = keep.n_stale; s->n_early = keep.n_early;
    s->n_reboot = keep.n_reboot; s->n_badproto = keep.n_badproto; s->n_events = keep.n_events;
    s->state = NH_WAIT_HELLO;
    s->role = role;
    s->want_tun = want_tun;
    s->opened = s->last_rx = s->hello_wait_since = now;
    s->next_tx = now;
}

int nh_format(nh_session *s, char *out, size_t cap, const char *type, const char *args)
{
    int n = nl_format(out, cap, s->seq_tx + 1, type, args);
    if (n > 0) s->seq_tx++;
    return n;
}

static void copy_tok(char *dst, size_t cap, const char *v)
{
    snprintf(dst, cap, "%s", v ? v : "?");
}

static void enter_wait(nh_session *s, double now)
{
    s->state = NH_WAIT_HELLO;
    s->hello_wait_since = now;
    s->next_tx = now;
}

int nh_frame(nh_session *s, const nl_frame *f, const char *raw, size_t rawlen, double now, nh_event *ev)
{
    uint32_t boot = 0, v;
    if (!strcmp(f->type, "HELLO")) {
        if (nl_get_u32(f, "proto", &v) != 0 || v != NL_PROTO || nl_get_hex32(f, "boot", &boot) != 0) {
            s->n_badproto++;
            return NH_F_IGNORED;
        }
        int reboot = s->have_boot && boot != s->boot;
        if (!s->have_boot || reboot) { s->last_seq = f->seq; s->dev_ms = 0; }
        else if (f->seq > s->last_seq) s->last_seq = f->seq;
        s->boot = boot;
        s->have_boot = 1;
        s->state = NH_READY;
        s->last_rx = now;
        copy_tok(s->fw, sizeof s->fw, nl_get(f, "fw"));
        copy_tok(s->sensor, sizeof s->sensor, nl_get(f, "sensor"));
        copy_tok(s->wifi, sizeof s->wifi, nl_get(f, "wifi"));
        s->tun_cap = nl_get_u32(f, "tun", &v) == 0 && v == 1;
        if (rawlen < sizeof s->hello_raw) { memcpy(s->hello_raw, raw, rawlen); s->hello_len = rawlen; }
        if (reboot) s->n_reboot++;
        return reboot ? NH_F_REBOOT : NH_F_HELLO;
    }
    if (s->state != NH_READY) { s->n_early++; return NH_F_IGNORED; }

    if (!strcmp(f->type, "HB")) {
        if (nl_get_hex32(f, "boot", &boot) != 0) return NH_F_IGNORED;
        if (boot != s->boot) {       /* rebooted and we missed its HELLO: insist on a fresh one */
            s->n_reboot++;
            enter_wait(s, now);
            return NH_F_REBOOT;
        }
    }
    if (f->seq <= s->last_seq) { s->n_dup++; return NH_F_DROPPED; }
    s->last_seq = f->seq;
    s->last_rx = now;

    if (!strcmp(f->type, "HB")) {
        if (nl_get_u32(f, "up", &v) == 0 && (int32_t)(v - s->dev_ms) > 0) s->dev_ms = v;
        copy_tok(s->wifi, sizeof s->wifi, nl_get(f, "wifi"));
        return NH_F_HB;
    }
    if (!strcmp(f->type, "EV")) {
        int kind = f->argc >= 1 ? nl_ev_from_name(f->argv[0]) : NL_EV_NONE;
        if (kind == NL_EV_NONE || nl_get_u32(f, "t", &v) != 0) return NH_F_IGNORED;
        if (s->dev_ms && (int32_t)(s->dev_ms - v) > NL_EV_STALE_MS) { s->n_stale++; return NH_F_DROPPED; }
        if ((int32_t)(v - s->dev_ms) > 0) s->dev_ms = v;
        s->n_events++;
        if (ev) { ev->kind = kind; ev->t = v; }
        return NH_F_EVENT;
    }
    return NH_F_OTHER;               /* OK/ERR replies, tunnel frames: the caller decides */
}

int nh_tick(nh_session *s, double now, char *out, size_t cap, int *close_now)
{
    *close_now = 0;
    if (s->state == NH_READY && now - s->last_rx > NL_PEER_TIMEOUT_MS / 1000.0) enter_wait(s, now);
    if (s->state == NH_WAIT_HELLO && now - s->hello_wait_since > NH_HELLO_GIVEUP &&
        now - s->last_rx > NH_SILENT_CLOSE - NH_HELLO_GIVEUP) {
        *close_now = 1;
        return 0;
    }
    if (now < s->next_tx) return 0;
    s->next_tx = now + NL_HB_MS / 1000.0;
    if (s->state == NH_WAIT_HELLO) {
        char a[48];
        snprintf(a, sizeof a, "proto=%d role=%s tun=%d", NL_PROTO, s->role ? s->role : "app", s->want_tun ? 1 : 0);
        return nh_format(s, out, cap, "HOST", a);
    }
    return nh_format(s, out, cap, "HHB", NULL);
}
