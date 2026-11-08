/* See nori_wnprov.h. */
#include "nori_wnprov.h"

#include <stdio.h>
#include <string.h>

/* ---- SHA-256 (FIPS 180-4), small rather than fast: a few blocks per provisioning ---- */
typedef struct { uint32_t h[8]; uint8_t b[64]; size_t n; uint64_t total; } sha;
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(sha *s, const uint8_t *p)
{
    uint32_t w[64], a[8];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    memcpy(a, s->h, sizeof a);
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = a[7] + (ROR(a[4], 6) ^ ROR(a[4], 11) ^ ROR(a[4], 25)) + ((a[4] & a[5]) ^ (~a[4] & a[6])) + K[i] + w[i];
        uint32_t t2 = (ROR(a[0], 2) ^ ROR(a[0], 13) ^ ROR(a[0], 22)) + ((a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]));
        memmove(a + 1, a, 7 * sizeof a[0]);
        a[4] += t1;
        a[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++) s->h[i] += a[i];
    nw_wipe(w, sizeof w);
    nw_wipe(a, sizeof a);
}
static void sha_init(sha *s)
{
    static const uint32_t H[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, H, sizeof H);
    s->n = 0;
    s->total = 0;
}
static void sha_add(sha *s, const uint8_t *p, size_t n)
{
    s->total += n;
    while (n) {
        size_t k = 64 - s->n < n ? 64 - s->n : n;
        memcpy(s->b + s->n, p, k);
        s->n += k; p += k; n -= k;
        if (s->n == 64) { sha_block(s, s->b); s->n = 0; }
    }
}
static void sha_end(sha *s, uint8_t *out)
{
    uint64_t bits = s->total * 8;
    uint8_t pad = 0x80, z = 0, len[8];
    sha_add(s, &pad, 1);
    while (s->n != 56) sha_add(s, &z, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_add(s, len, 8);
    for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(s->h[i] >> (24 - 8 * j));
    nw_wipe(s, sizeof *s);
}
void nwp_sha256(const uint8_t *b, size_t n, uint8_t out[32])
{
    sha s;
    sha_init(&s);
    sha_add(&s, b, n);
    sha_end(&s, out);
}
void nwp_hmac(const uint8_t *key, size_t klen, const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t out[32])
{
    uint8_t k[64], pad[64], in[32];
    sha s;
    memset(k, 0, sizeof k);
    if (klen > 64) nwp_sha256(key, klen, k); else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha_init(&s);
    sha_add(&s, pad, 64);
    if (an) sha_add(&s, a, an);
    if (bn) sha_add(&s, b, bn);
    sha_end(&s, in);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha_init(&s);
    sha_add(&s, pad, 64);
    sha_add(&s, in, 32);
    sha_end(&s, out);
    nw_wipe(k, sizeof k);
    nw_wipe(pad, sizeof pad);
    nw_wipe(in, sizeof in);
}

/* ---- envelope ---- */
static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

int nwp_parse(const uint8_t *p, size_t n, nwp_op *o)
{
    memset(o, 0, sizeof *o);
    if (n < 5 || n > NWP_PAYLOAD_MAX || p[0] != 1 || p[1] < NWP_OP_ADD || p[1] > NWP_OP_DEL || (p[2] & ~(NWP_FL_FORCE | NWP_FL_UPDATE))) return NW_E_FORMAT;
    if (p[4] > NW_MAX) return NW_E_FULL;
    o->op = p[1];
    o->force = (p[2] & NWP_FL_FORCE) != 0;
    o->update = (p[2] & NWP_FL_UPDATE) != 0;
    o->pos = p[3];
    o->k = p[4];
    size_t i = 5;
    for (int e = 0; e < o->k; e++) {
        if (n - i < 1) goto bad;
        size_t sl = p[i++];
        if (n - i < sl + 2) goto bad;
        const uint8_t *ss = p + i;
        i += sl;
        size_t pl = p[i++];
        if (n - i < pl + 1) goto bad;
        const uint8_t *pp = p + i;
        i += pl;
        uint8_t fl = p[i++];
        if (nw_net_set(&o->nets[e], (const char *)ss, sl, (const char *)pp, pl, fl) != NW_OK) goto bad;
    }
    if (i != n) goto bad;
    return NW_OK;
bad:
    nw_wipe(o, sizeof *o);
    return NW_E_FORMAT;
}

int nwp_open(const char *secret, const uint8_t *nonce, const uint8_t *ct, size_t n, const uint8_t *tag, nwp_op *out)
{
    static const char LABEL[] = "nori-wifi-v1";
    static uint8_t plain[NWP_PAYLOAD_MAX];
    uint8_t k[32], ke[32], km[32], mine[32], be[4];
    int r;
    if (n > NWP_PAYLOAD_MAX) return NW_E_FORMAT;
    nwp_hmac((const uint8_t *)secret, strlen(secret), (const uint8_t *)LABEL, sizeof LABEL - 1, nonce, NWP_NONCE, k);
    nwp_hmac(k, 32, (const uint8_t *)"enc", 3, NULL, 0, ke);
    nwp_hmac(k, 32, (const uint8_t *)"mac", 3, NULL, 0, km);
    nwp_hmac(km, 32, nonce, NWP_NONCE, ct, n, mine);
    if (!ct_equal(mine, tag, NWP_TAG)) {
        r = NW_E_AUTH;
    } else {
        for (size_t o = 0, blk = 0; o < n; o += 32, blk++) {
            uint8_t ks[32];
            be[0] = (uint8_t)(blk >> 24); be[1] = (uint8_t)(blk >> 16); be[2] = (uint8_t)(blk >> 8); be[3] = (uint8_t)blk;
            nwp_hmac(ke, 32, be, 4, NULL, 0, ks);
            for (size_t i = 0; i < 32 && o + i < n; i++) plain[o + i] = ct[o + i] ^ ks[i];
            nw_wipe(ks, sizeof ks);
        }
        r = nwp_parse(plain, n, out);
    }
    nw_wipe(plain, sizeof plain);
    nw_wipe(k, sizeof k);
    nw_wipe(ke, sizeof ke);
    nw_wipe(km, sizeof km);
    return r;
}

/* ---- transactions ---- */
void nwp_abort(nwp_ctx *c)
{
    c->open = 0;
    c->len = 0;
    nw_wipe(c->nonce, sizeof c->nonce);
    nw_wipe(c->buf, sizeof c->buf);
}

static const char *secret(nwp_ctx *c)
{
    const char *k = c->key ? c->key(c->ctx) : NULL;
    return k && strlen(k) == 32 ? k : NULL;
}
static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[i * 2] = H[b[i] >> 4]; out[i * 2 + 1] = H[b[i] & 15]; }
    out[n * 2] = 0;
}
static int unhex(const char *s, uint8_t *b, size_t n)
{
    if (strlen(s) != n * 2) return -1;
    for (size_t i = 0; i < n * 2; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return -1;
        b[i / 2] = (uint8_t)(i & 1 ? b[i / 2] | v : v << 4);
    }
    return 0;
}
static int begin(nwp_ctx *c, uint32_t now)
{
    if (!secret(c)) return NW_E_NOKEY;
    nwp_abort(c);
    c->rnd(c->ctx, c->nonce, sizeof c->nonce);
    c->born = now;
    c->open = 1;
    return NW_OK;
}
static int alive(const nwp_ctx *c, uint32_t now) { return c->open && now - c->born < NWP_TTL_MS; }

static int apply(nwp_ctx *c)
{
    int active = c->active ? c->active(c->ctx) : -1, r;
    nwp_op *o = &c->op;
    c->next = *c->t;
    if (o->op == NWP_OP_ADD) r = nw_op_add(&c->next, o->nets, o->k, o->pos, o->update, active, o->force);
    else if (o->op == NWP_OP_REPLACE) r = nw_op_replace(&c->next, o->nets, o->k, active, o->force);
    else r = o->k == 1 ? nw_op_del(&c->next, o->nets[0].ssid, o->nets[0].ssid_len, active, o->force) : NW_E_FORMAT;
    if (r == NW_OK) {
        c->old = *c->t;
        r = nw_commit(c->t, &c->next, c->kv);
        if (r == NW_OK && c->changed) c->changed(c->ctx, &c->old, 0);
    }
    nw_wipe(&c->next, sizeof c->next);
    nw_wipe(&c->old, sizeof c->old);
    return r;
}

/* The nonce is burnt whatever happens here: one attempt per nonce. */
static int finish(nwp_ctx *c, const uint8_t *ct, size_t n, const uint8_t *tag, uint32_t now)
{
    const char *k = secret(c);
    int r;
    if (!k) r = NW_E_NOKEY;
    else if (!alive(c, now)) r = NW_E_EXPIRED;
    else r = nwp_open(k, c->nonce, ct, n, tag, &c->op);
    if (r == NW_OK) r = apply(c);
    nw_wipe(&c->op, sizeof c->op);
    nwp_abort(c);
    return r;
}

int nwp_cmd(nwp_ctx *c, const nl_frame *f, char *reply, size_t cap, uint32_t now)
{
    if (!strcmp(f->type, "WNBEGIN")) {
        char h[NWP_NONCE * 2 + 1];
        int r = f->argc ? NW_E_FORMAT : begin(c, now);
        if (r != NW_OK) { snprintf(reply, cap, "ERR WNBEGIN %s", nw_err_word(r)); return 1; }
        hex(c->nonce, sizeof c->nonce, h);
        snprintf(reply, cap, "OK WNBEGIN n=%s", h);
        return 1;
    }
    if (!strcmp(f->type, "WNDATA")) {
        uint8_t b[NL_TUN_CHUNK + 3];
        uint32_t off = 0;
        const char *d = nl_get(f, "d");
        int n = d && f->argc == 2 && nl_get_u32(f, "o", &off) == 0 ? nl_b64_decode(d, b, sizeof b) : -1;
        if (!alive(c, now)) { nwp_abort(c); snprintf(reply, cap, "ERR WNDATA expired"); return 1; }
        if (n <= 0 || off != c->len) { nwp_abort(c); snprintf(reply, cap, "ERR WNDATA format"); return 1; }
        if ((size_t)n > sizeof c->buf - c->len) { nwp_abort(c); snprintf(reply, cap, "ERR WNDATA full"); return 1; }
        memcpy(c->buf + c->len, b, (size_t)n);
        c->len += (size_t)n;
        snprintf(reply, cap, "OK WNDATA o=%lu", (unsigned long)c->len);
        return 1;
    }
    if (!strcmp(f->type, "WNEND")) {
        uint8_t tag[NWP_TAG];
        const char *t = nl_get(f, "t");
        int r;
        if (!t || f->argc != 1 || unhex(t, tag, sizeof tag)) { nwp_abort(c); r = NW_E_FORMAT; }
        else r = finish(c, c->buf, c->len, tag, now);
        if (r == NW_OK) snprintf(reply, cap, "OK WNEND n=%u g=%lu", c->t->n, (unsigned long)c->t->gen);
        else snprintf(reply, cap, "ERR WNEND %s", nw_err_word(r));
        return 1;
    }
    if (!strcmp(f->type, "WNABORT")) {
        nwp_abort(c);
        snprintf(reply, cap, "OK WNABORT");
        return 1;
    }
    return 0;
}

int nwp_http_nonce(nwp_ctx *c, char *out, size_t cap, uint32_t now)
{
    if (cap < NWP_NONCE * 2 + 1) return NW_E_FORMAT;
    int r = begin(c, now);
    if (r == NW_OK) hex(c->nonce, sizeof c->nonce, out);
    return r;
}

int nwp_http_post(nwp_ctx *c, const uint8_t *body, size_t n, uint32_t now, char *json, size_t cap)
{
    int r;
    if (n < NWP_NONCE + NWP_TAG || n > NWP_HTTP_MAX) { nwp_abort(c); r = NW_E_FORMAT; }
    else if (!alive(c, now) || !ct_equal(body, c->nonce, NWP_NONCE)) { nwp_abort(c); r = NW_E_EXPIRED; }
    else r = finish(c, body + NWP_NONCE, n - NWP_NONCE - NWP_TAG, body + n - NWP_TAG, now);
    if (r == NW_OK) { snprintf(json, cap, "{\"ok\":1,\"n\":%u,\"g\":%lu}\n", c->t->n, (unsigned long)c->t->gen); return 200; }
    snprintf(json, cap, "{\"ok\":0,\"err\":\"%s\"}\n", nw_err_word(r));
    return r == NW_E_AUTH || r == NW_E_NOKEY ? 403 : r == NW_E_STORE ? 500 : r == NW_E_ACTIVE || r == NW_E_EXISTS ? 409 : 400;
}

int nwp_wifiset(nwp_ctx *c, const char *ssid, const char *psk)
{
    nw_net e;
    int r = nw_net_set(&e, ssid, strlen(ssid), psk, strlen(psk), NW_F_HIDDEN);   /* as r5.0.1: tried even if no scan shows it */
    if (r == NW_OK) {
        c->next = *c->t;
        r = nw_op_primary(&c->next, &e);
    }
    if (r == NW_OK) {
        c->old = *c->t;
        r = nw_commit(c->t, &c->next, c->kv);
        if (r == NW_OK && c->changed) c->changed(c->ctx, &c->old, 1);
    }
    nw_wipe(&e, sizeof e);
    nw_wipe(&c->next, sizeof c->next);
    nw_wipe(&c->old, sizeof c->old);
    return r == NW_SAME ? NW_OK : r;
}

int nwp_clear(nwp_ctx *c)
{
    memset(&c->next, 0, sizeof c->next);
    c->old = *c->t;
    int r = nw_commit(c->t, &c->next, c->kv);
    if (r == NW_OK) {
        nwp_abort(c);
        if (c->changed) c->changed(c->ctx, &c->old, 0);
    }
    nw_wipe(&c->old, sizeof c->old);
    return r;
}
