/* wt32-r3 transition log; see nori_evlog.h. */
#include "nori_evlog.h"

#include <stdio.h>
#include <string.h>

const char *const NLG_NAME[NLG_COUNT] = { "none", "boot", "eth_start", "eth_up", "eth_down", "eth_ip", "eth_stop",
                                          "wifi_on", "wifi_off", "wifi_conn", "wifi_disc", "wifi_ip", "wifi_noip",
                                          "net", "host", "ota", "http", "img_ok", "tof", "pres", "tun", "heap",
                                          "dhcp", "lan", "route", "tog", "wnet", "slow" };

void nlg_init(nlg_ring *r) { memset(r, 0, sizeof *r); }

void nlg_add(nlg_ring *r, uint32_t t_ms, uint8_t code, uint8_t a, uint16_t b)
{
    if (code >= NLG_COUNT) code = NLG_NONE;
    nlg_ev *e = &r->ev[r->total % NLG_N];
    e->t_ms = t_ms;
    e->code = code;
    e->a = a;
    e->b = b;
    r->total++;
    if (r->count[code] != UINT32_MAX) r->count[code]++;
}

uint32_t nlg_kept(const nlg_ring *r) { return r->total < NLG_N ? r->total : NLG_N; }

int nlg_get(const nlg_ring *r, uint32_t i, nlg_ev *out)
{
    uint32_t k = nlg_kept(r);
    if (i >= k) return -1;
    *out = r->ev[(r->total - k + i) % NLG_N];
    return 0;
}

static const char *name(uint8_t code) { return code < NLG_COUNT ? NLG_NAME[code] : "?"; }

int nlg_fmt(const nlg_ev *e, char *out, size_t cap)
{
    unsigned long ds = (unsigned long)(e->t_ms / 100u);
    if (e->b)
        return snprintf(out, cap, "%lu.%lu:%s:%u:%u", ds / 10, ds % 10, name(e->code), e->a, e->b);
    return snprintf(out, cap, "%lu.%lu:%s:%u", ds / 10, ds % 10, name(e->code), e->a);
}

int nlg_tail(const nlg_ring *r, unsigned n, char *out, size_t cap)
{
    if (!cap) return 0;
    out[0] = 0;
    uint32_t k = nlg_kept(r);
    if (n > k) n = (unsigned)k;
    /* drop the oldest of the n until everything fits */
    for (unsigned skip = 0; skip <= n; skip++) {
        size_t len = 0;
        int ok = 1;
        int w = snprintf(out, cap, "ev=");
        if (w < 0 || (size_t)w >= cap) { out[0] = 0; return 0; }
        len = (size_t)w;
        for (unsigned j = skip; j < n; j++) {
            nlg_ev e;
            char one[48];
            nlg_get(r, k - n + j, &e);
            int m = nlg_fmt(&e, one, sizeof one);
            if (m < 0) { ok = 0; break; }
            size_t need = (size_t)m + (j > skip ? 1 : 0);
            if (len + need >= cap) { ok = 0; break; }
            if (j > skip) out[len++] = ',';
            memcpy(out + len, one, (size_t)m);
            len += (size_t)m;
            out[len] = 0;
        }
        if (ok) return (int)len;
    }
    out[0] = 0;
    return 0;
}

static int json_one(const nlg_ev *e, char *out, size_t cap)
{
    return snprintf(out, cap, "{\"t\":%lu,\"e\":\"%s\",\"a\":%u,\"b\":%u}", (unsigned long)e->t_ms, name(e->code), e->a, e->b);
}

static int json_from(const nlg_ev *ev, unsigned n, char *out, size_t cap)
{
    if (cap < 3) { if (cap) out[0] = 0; return 0; }
    for (unsigned skip = 0; skip <= n; skip++) {
        size_t len = 1;
        int ok = 1;
        out[0] = '[';
        for (unsigned j = skip; j < n; j++) {
            char one[80];
            int m = json_one(&ev[j], one, sizeof one);
            size_t need = (size_t)m + (j > skip ? 1 : 0);
            if (m < 0 || len + need + 1 >= cap) { ok = 0; break; }
            if (j > skip) out[len++] = ',';
            memcpy(out + len, one, (size_t)m);
            len += (size_t)m;
        }
        if (ok) { out[len++] = ']'; out[len] = 0; return (int)len; }
    }
    out[0] = '['; out[1] = ']'; out[2] = 0;
    return 2;
}

int nlg_json(const nlg_ring *r, char *out, size_t cap)
{
    nlg_ev tmp[NLG_N];
    uint32_t k = nlg_kept(r);
    for (uint32_t i = 0; i < k; i++) nlg_get(r, i, &tmp[i]);
    return json_from(tmp, (unsigned)k, out, cap);
}

int nlg_json_arr(const nlg_ev *ev, unsigned n, char *out, size_t cap) { return json_from(ev, n, out, cap); }
