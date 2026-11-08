/* See nori_wnet.h. */
#include "nori_wnet.h"

#include <string.h>

const char *const NW_LD_NAME[NW_LD_COUNT] = { "empty", "blob", "migrated", "corrupt-legacy", "corrupt-empty", "mirrored", "adopted",
                                              "cleared", "legacy-bad" };

const char *nw_err_word(int code)
{
    switch (code) {
    case NW_SAME: case NW_OK: return "ok";
    case NW_E_FULL: return "full";
    case NW_E_EXISTS: return "exists";
    case NW_E_ACTIVE: return "active";
    case NW_E_EMPTY: return "empty";
    case NW_E_STORE: return "store";
    case NW_E_DUP: return "dup";
    case NW_E_ABSENT: return "absent";
    case NW_E_AUTH: return "auth";
    case NW_E_NOKEY: return "nokey";
    case NW_E_EXPIRED: return "expired";
    default: return "format";
    }
}

void nw_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--) *v++ = 0;
}

uint32_t nw_crc32(const uint8_t *b, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *b++;
        for (int i = 0; i < 8; i++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

static int is_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

int nw_net_set(nw_net *o, const char *ssid, size_t ssid_len, const char *psk, size_t psk_len, uint8_t flags)
{
    if (!ssid || ssid_len < 1 || ssid_len > NW_SSID_MAX || memchr(ssid, 0, ssid_len)) return NW_E_FORMAT;
    if (psk_len && (!psk || memchr(psk, 0, psk_len))) return NW_E_FORMAT;
    if (psk_len != 0 && (psk_len < 8 || psk_len > NW_PSK_MAX)) return NW_E_FORMAT;
    if (psk_len == NW_PSK_MAX) for (size_t i = 0; i < psk_len; i++) if (!is_hex(psk[i])) return NW_E_FORMAT;
    if (flags & ~NW_FLAGS_ALL) return NW_E_FORMAT;
    memset(o, 0, sizeof *o);
    memcpy(o->ssid, ssid, ssid_len);
    if (psk_len) memcpy(o->psk, psk, psk_len);
    o->ssid_len = (uint8_t)ssid_len;
    o->psk_len = (uint8_t)psk_len;
    o->flags = flags;
    return NW_OK;
}

int nw_find(const nw_table *t, const char *ssid, size_t ssid_len)
{
    for (int i = 0; i < t->n; i++)
        if (t->net[i].ssid_len == ssid_len && !memcmp(t->net[i].ssid, ssid, ssid_len)) return i;
    return -1;
}

int nw_same_login(const nw_net *a, const nw_net *b)
{
    return a->ssid_len == b->ssid_len && a->psk_len == b->psk_len && !memcmp(a->ssid, b->ssid, a->ssid_len) &&
           !memcmp(a->psk, b->psk, a->psk_len);
}

static int net_ok(const nw_net *e)
{
    nw_net c;
    int r = e->ssid_len <= NW_SSID_MAX && e->psk_len <= NW_PSK_MAX ? nw_net_set(&c, e->ssid, e->ssid_len, e->psk, e->psk_len, e->flags) : NW_E_FORMAT;
    nw_wipe(&c, sizeof c);
    return r;
}
static int list_ok(const nw_net *nets, int k)
{
    if (k < 0 || k > NW_MAX) return NW_E_FULL;
    for (int i = 0; i < k; i++) {
        if (net_ok(&nets[i]) != NW_OK) return NW_E_FORMAT;
        for (int j = 0; j < i; j++)
            if (nets[i].ssid_len == nets[j].ssid_len && !memcmp(nets[i].ssid, nets[j].ssid, nets[i].ssid_len)) return NW_E_DUP;
    }
    return NW_OK;
}
/* the lock-out rule: is this login still there, unchanged? */
static int kept(const nw_table *t, const nw_net *e)
{
    int i = nw_find(t, e->ssid, e->ssid_len);
    return i >= 0 && nw_same_login(&t->net[i], e);
}
static int guard(const nw_table *old, const nw_table *next, int active, int force)
{
    if (force) return NW_OK;
    if (active >= 0 && active < old->n && !kept(next, &old->net[active])) return NW_E_ACTIVE;
    if (old->n && !kept(next, &old->net[0])) return NW_E_ACTIVE;
    return NW_OK;
}
static void insert(nw_table *t, int pos, const nw_net *e)
{
    memmove(&t->net[pos + 1], &t->net[pos], sizeof t->net[0] * (size_t)(t->n - pos));
    t->net[pos] = *e;
    t->n++;
}

int nw_op_primary(nw_table *t, const nw_net *net)
{
    if (net_ok(net) != NW_OK) return NW_E_FORMAT;
    int i = nw_find(t, net->ssid, net->ssid_len);
    if (i == 0 && nw_same_login(&t->net[0], net)) return NW_SAME;
    if (i < 0 && t->n >= NW_MAX) return NW_E_FULL;
    nw_net e = *net;
    if (i >= 0) {                                  /* keeps its hidden flag: WIFISET has no such argument */
        e.flags = t->net[i].flags;
        memmove(&t->net[1], &t->net[0], sizeof t->net[0] * (size_t)i);
        t->net[0] = e;
    } else {
        insert(t, 0, &e);
    }
    nw_wipe(&e, sizeof e);
    return NW_OK;
}

int nw_op_add(nw_table *t, const nw_net *nets, int k, int pos, int update, int active, int force)
{
    static nw_table c;                             /* loop() only; wiped below */
    int r = list_ok(nets, k);
    if (r != NW_OK) return r;
    if (k < 1) return NW_E_EMPTY;
    c = *t;
    if (pos < 0 || pos > c.n) pos = c.n;
    for (int i = 0; i < k && r == NW_OK; i++) {
        int f = nw_find(&c, nets[i].ssid, nets[i].ssid_len);
        if (f >= 0 && !update) r = NW_E_EXISTS;
        else if (f >= 0) c.net[f] = nets[i];
        else if (c.n >= NW_MAX) r = NW_E_FULL;
        else insert(&c, pos++, &nets[i]);
    }
    if (r == NW_OK) r = guard(t, &c, active, force);
    if (r == NW_OK) *t = c;
    nw_wipe(&c, sizeof c);
    return r;
}

int nw_op_replace(nw_table *t, const nw_net *nets, int k, int active, int force)
{
    static nw_table c;
    int r = list_ok(nets, k);
    if (r != NW_OK) return r;
    if (k < 1) return NW_E_EMPTY;
    memset(&c, 0, sizeof c);
    c.gen = t->gen;
    c.pend = t->pend;
    c.n = (uint8_t)k;
    memcpy(c.net, nets, sizeof c.net[0] * (size_t)k);
    r = guard(t, &c, active, force);
    if (r == NW_OK) *t = c;
    nw_wipe(&c, sizeof c);
    return r;
}

int nw_op_del(nw_table *t, const char *ssid, size_t ssid_len, int active, int force)
{
    static nw_table c;
    int i = nw_find(t, ssid, ssid_len);
    if (i < 0) return NW_E_ABSENT;
    if (t->n == 1) return NW_E_EMPTY;
    c = *t;
    memmove(&c.net[i], &c.net[i + 1], sizeof c.net[0] * (size_t)(c.n - i - 1));
    c.n--;
    nw_wipe(&c.net[c.n], sizeof c.net[0]);
    int r = guard(t, &c, active, force);
    if (r == NW_OK) *t = c;
    nw_wipe(&c, sizeof c);
    return r;
}

/* magic "NWT1", version, n, pend, 0, gen (LE), then per net: ssid_len psk_len flags ssid psk; CRC-32 (LE) of all of it */
static void put32(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

int nw_encode(const nw_table *t, uint8_t *b, size_t cap)
{
    size_t need = 12 + 4;
    if (t->n > NW_MAX) return -1;
    for (int i = 0; i < t->n; i++) need += 3u + t->net[i].ssid_len + t->net[i].psk_len;
    if (need > cap) return -1;
    memcpy(b, "NWT1", 4);
    b[4] = NW_BLOB_VER; b[5] = t->n; b[6] = t->pend; b[7] = 0;
    put32(b + 8, t->gen);
    size_t o = 12;
    for (int i = 0; i < t->n; i++) {
        const nw_net *e = &t->net[i];
        b[o++] = e->ssid_len; b[o++] = e->psk_len; b[o++] = e->flags;
        memcpy(b + o, e->ssid, e->ssid_len); o += e->ssid_len;
        memcpy(b + o, e->psk, e->psk_len); o += e->psk_len;
    }
    put32(b + o, nw_crc32(b, o));
    return (int)(o + 4);
}

int nw_decode(nw_table *t, const uint8_t *b, size_t n)
{
    memset(t, 0, sizeof *t);
    if (n < 16 || n > NW_BLOB_MAX || memcmp(b, "NWT1", 4) || b[4] != NW_BLOB_VER || b[5] > NW_MAX || b[6] > 1) return NW_E_FORMAT;
    if (nw_crc32(b, n - 4) != get32(b + n - 4)) return NW_E_FORMAT;
    size_t o = 12, end = n - 4;
    for (int i = 0; i < b[5]; i++) {
        if (end - o < 3) goto bad;
        size_t sl = b[o], pl = b[o + 1];
        uint8_t fl = b[o + 2];
        o += 3;
        if (sl + pl > end - o || nw_net_set(&t->net[i], (const char *)b + o, sl, (const char *)b + o + sl, pl, fl) != NW_OK) goto bad;
        for (int j = 0; j < i; j++)
            if (t->net[j].ssid_len == sl && !memcmp(t->net[j].ssid, b + o, sl)) goto bad;
        o += sl + pl;
    }
    if (o != end) goto bad;
    t->n = b[5]; t->pend = b[6]; t->gen = get32(b + 8);
    return NW_OK;
bad:
    nw_wipe(t, sizeof *t);
    return NW_E_FORMAT;
}

static int put_table(const nw_table *t, const nw_kv *kv)
{
    static uint8_t b[NW_BLOB_MAX];
    int n = nw_encode(t, b, sizeof b);
    int r = n > 0 && kv->put_blob(kv->ctx, NW_K_BLOB, b, (size_t)n) == 0 ? 0 : -1;
    nw_wipe(b, sizeof b);
    return r;
}
static int mirror(const nw_table *t, const nw_kv *kv)
{
    if (!t->n) {
        int a = kv->del(kv->ctx, NW_K_SSID), b = kv->del(kv->ctx, NW_K_PSK);
        return a || b ? -1 : 0;
    }
    if (kv->put_str(kv->ctx, NW_K_SSID, t->net[0].ssid)) return -1;
    return kv->put_str(kv->ctx, NW_K_PSK, t->net[0].psk) ? -1 : 0;
}
static void finish_mirror(nw_table *t, const nw_kv *kv)
{
    if (mirror(t, kv)) return;                     /* stays pending: repeated on the next boot */
    t->pend = 0;
    if (put_table(t, kv)) t->pend = 1;
}

int nw_commit(nw_table *cur, const nw_table *next, const nw_kv *kv)
{
    static nw_table t;
    t = *next;
    t.gen = cur->gen + 1;
    /* n = 0 always rewrites the mirror: WIFICLR must take legacy keys with it even if they were never loaded */
    int differs = !t.n || !cur->n || !nw_same_login(&cur->net[0], &t.net[0]);
    t.pend = differs ? 1 : 0;
    if (put_table(&t, kv)) { nw_wipe(&t, sizeof t); return NW_E_STORE; }
    if (differs) finish_mirror(&t, kv);
    *cur = t;
    nw_wipe(&t, sizeof t);
    return NW_OK;
}

int nw_load(nw_table *t, const nw_kv *kv)
{
    static uint8_t b[NW_BLOB_MAX];
    static nw_table next;
    static char ssid[NW_SSID_MAX + 2], psk[NW_PSK_MAX + 2];
    nw_net leg;
    int rc;
    memset(t, 0, sizeof *t);
    memset(&leg, 0, sizeof leg);
    int n = kv->get_blob(kv->ctx, NW_K_BLOB, b, sizeof b);
    int have_blob = n > 0, ok = have_blob && nw_decode(t, b, (size_t)n) == NW_OK;
    nw_wipe(b, sizeof b);
    int sl = kv->get_str(kv->ctx, NW_K_SSID, ssid, sizeof ssid);
    int pl = sl > 0 ? kv->get_str(kv->ctx, NW_K_PSK, psk, sizeof psk) : -1;
    int unreadable = sl == -2 || pl == -2;
    int have = sl > 0, bad = 0;
    if (have && nw_net_set(&leg, ssid, (size_t)sl, psk, pl > 0 ? (size_t)pl : 0, NW_F_HIDDEN) != NW_OK) { have = 0; bad = 1; }
    nw_wipe(ssid, sizeof ssid);
    nw_wipe(psk, sizeof psk);

    if (!ok) {
        memset(t, 0, sizeof *t);
        if (have) {                                /* r5.0.1's network becomes profile 0; the legacy keys stay */
            t->n = 1;
            t->net[0] = leg;
            put_table(t, kv);
            rc = have_blob ? NW_LD_CORRUPT_LEGACY : NW_LD_MIGRATED;
        } else {
            rc = bad ? NW_LD_LEGACY_BAD : have_blob ? NW_LD_CORRUPT_EMPTY : NW_LD_EMPTY;
        }
    } else if (t->pend) {                          /* power was lost inside nw_commit(): the blob is the truth */
        finish_mirror(t, kv);
        rc = NW_LD_MIRRORED;
    } else if (unreadable || bad) {                /* cannot tell what happened: leave everything alone */
        rc = NW_LD_BLOB;
    } else if (!have && t->n) {                    /* an older firmware ran WIFICLR after a rollback */
        memset(&next, 0, sizeof next);
        nw_commit(t, &next, kv);
        rc = NW_LD_CLEARED;
    } else if (have && (!t->n || !nw_same_login(&t->net[0], &leg))) {   /* an older firmware ran WIFISET: same meaning here */
        next = *t;
        if (nw_find(&next, leg.ssid, leg.ssid_len) < 0 && next.n >= NW_MAX) nw_wipe(&next.net[--next.n], sizeof next.net[0]);
        nw_op_primary(&next, &leg);
        nw_commit(t, &next, kv);
        rc = NW_LD_ADOPTED;
    } else {
        rc = NW_LD_BLOB;
    }
    nw_wipe(&next, sizeof next);
    nw_wipe(&leg, sizeof leg);
    return rc;
}
