/* Nori accessory link framing; see nori_link.h. */
#include "nori_link.h"

#include <string.h>

const char *const NL_EV_NAME[NL_EV_COUNT] = { "none", "tap", "dtap", "b1d", "b1u", "b2d", "b2u", "tilt", "pon", "poff", "trk5", "trk1",
                                             "tsh5", "tsh1", "tlg5", "tlg1" };

int nl_ev_from_name(const char *s)
{
    for (int i = 1; i < NL_EV_COUNT; i++)
        if (!strcmp(s, NL_EV_NAME[i])) return i;
    return NL_EV_NONE;
}

uint16_t nl_crc16(const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFF;
    while (n--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int i = 0; i < 8; i++) crc = (uint16_t)(crc & 0x8000 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t nl_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)
{
    size_t need = (n + 2) / 3 * 4;
    if (cap < need + 1) return 0;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int nl_b64_decode(const char *in, uint8_t *out, size_t cap)
{
    size_t n = strlen(in), o = 0;
    if (n % 4) return -1;
    for (size_t i = 0; i < n; i += 4) {
        int v[4], pad = 0;
        for (int k = 0; k < 4; k++) {
            if (in[i + k] == '=') {
                if (i + 4 != n || k < 2) return -1;   /* padding only at the very end */
                v[k] = 0; pad++;
            } else {
                if (pad) return -1;                   /* data after padding */
                v[k] = b64_val(in[i + k]);
                if (v[k] < 0) return -1;
            }
        }
        uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        int bytes = 3 - pad;
        if (o + (size_t)bytes > cap) return -1;
        out[o++] = (uint8_t)(w >> 16);
        if (bytes > 1) out[o++] = (uint8_t)(w >> 8);
        if (bytes > 2) out[o++] = (uint8_t)w;
    }
    return (int)o;
}

int nl_token_ok(const char *s, size_t n)
{
    if (n == 0) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x21 || c > 0x7E || c == '$' || c == '*') return 0;
    }
    return 1;
}

static int type_ok(const char *t)
{
    size_t n = strlen(t);
    if (n < 2 || n > 8) return 0;
    for (size_t i = 0; i < n; i++) if (t[i] < 'A' || t[i] > 'Z') return 0;
    return 1;
}

static const char HEX[] = "0123456789ABCDEF";

int nl_format(char *out, size_t cap, uint32_t seq, const char *type, const char *args)
{
    if (!type_ok(type)) return -1;
    char body[NL_LINE_MAX];
    size_t n = 0;
    /* "NL1 " + seq + " " + type */
    char num[11];
    int k = 0;
    uint32_t s = seq;
    do { num[k++] = (char)('0' + s % 10); s /= 10; } while (s);
    const char *pre = "NL1 ";
    size_t tl = strlen(type), al = args ? strlen(args) : 0;
    if (4 + (size_t)k + 1 + tl + (al ? 1 + al : 0) + 7 > NL_LINE_MAX) return -1;   /* '$' '*' XXXX + margin */
    memcpy(body, pre, 4); n = 4;
    while (k) body[n++] = num[--k];
    body[n++] = ' ';
    memcpy(body + n, type, tl); n += tl;
    if (al) {
        /* tokens separated by exactly one space */
        size_t i = 0;
        while (i < al) {
            size_t j = i;
            while (j < al && args[j] != ' ') j++;
            if (!nl_token_ok(args + i, j - i)) return -1;
            i = j + 1;
            if (j < al && i >= al) return -1;          /* trailing space */
        }
        body[n++] = ' ';
        memcpy(body + n, args, al); n += al;
    }
    uint16_t crc = nl_crc16(body, n);
    size_t total = 1 + n + 5 + 1;
    if (total > cap || total - 1 > NL_LINE_MAX) return -1;
    out[0] = '$';
    memcpy(out + 1, body, n);
    out[1 + n] = '*';
    for (int d = 0; d < 4; d++) out[2 + n + d] = HEX[(crc >> (12 - 4 * d)) & 15];
    out[6 + n] = '\n';
    if (total < cap) out[total] = 0;
    return (int)total;
}

int nl_parse_u32(const char *s, uint32_t *out)
{
    if (!s || !*s) return -1;
    uint64_t v = 0;
    size_t n = 0;
    for (; s[n]; n++) {
        if (s[n] < '0' || s[n] > '9' || n >= 10) return -1;
        v = v * 10 + (uint64_t)(s[n] - '0');
    }
    if (v > 0xFFFFFFFFu || (n > 1 && s[0] == '0')) return -1;
    *out = (uint32_t)v;
    return 0;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int nl_parse(const char *line, size_t n, nl_frame *f)
{
    if (n && line[n - 1] == '\r') n--;
    if (n == 0 || line[0] != '$') return NL_E_NOISE;
    if (n > NL_LINE_MAX) return NL_E_LONG;
    if (n < 1 + 8 + 5 || line[n - 5] != '*') return NL_E_SYNTAX;   /* shortest: $NL1 0 XX*CCCC */
    uint16_t want = 0;
    for (int d = 0; d < 4; d++) {
        int v = hexval(line[n - 4 + d]);
        if (v < 0) return NL_E_SYNTAX;
        want = (uint16_t)(want << 4 | v);
    }
    size_t bl = n - 6;                       /* body: line[1 .. n-6] */
    if (nl_crc16(line + 1, bl) != want) return NL_E_CRC;
    memcpy(f->store, line + 1, bl);
    f->store[bl] = 0;
    /* split into tokens on single spaces */
    const char *tok[NL_ARGS_MAX + 3];
    int nt = 0;
    char *p = f->store;
    for (;;) {
        char *sp = strchr(p, ' ');
        size_t tl = sp ? (size_t)(sp - p) : strlen(p);
        if (!nl_token_ok(p, tl)) return NL_E_SYNTAX;
        if (nt == NL_ARGS_MAX + 3) return NL_E_SYNTAX;
        tok[nt++] = p;
        if (!sp) break;
        *sp = 0;
        p = sp + 1;
    }
    if (nt < 3) return NL_E_SYNTAX;
    if (strcmp(tok[0], "NL1")) return NL_E_MAGIC;
    if (nl_parse_u32(tok[1], &f->seq) != 0 || !type_ok(tok[2])) return NL_E_SYNTAX;
    f->type = tok[2];
    f->argc = nt - 3;
    for (int i = 0; i < f->argc; i++) f->argv[i] = tok[3 + i];
    return NL_OK;
}

const char *nl_get(const nl_frame *f, const char *key)
{
    size_t kl = strlen(key);
    for (int i = 0; i < f->argc; i++)
        if (!strncmp(f->argv[i], key, kl) && f->argv[i][kl] == '=') return f->argv[i] + kl + 1;
    return NULL;
}

int nl_get_u32(const nl_frame *f, const char *key, uint32_t *out)
{
    return nl_parse_u32(nl_get(f, key), out);
}

int nl_get_hex32(const nl_frame *f, const char *key, uint32_t *out)
{
    const char *s = nl_get(f, key);
    if (!s || strlen(s) != 8) return -1;
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        int h = hexval(s[i]);
        if (h < 0) return -1;
        v = v << 4 | (uint32_t)h;
    }
    *out = v;
    return 0;
}

enum { RX_HUNT, RX_IN, RX_DISCARD };

void nl_rx_init(nl_rx *r) { memset(r, 0, sizeof *r); }

size_t nl_rx_feed(nl_rx *r, const uint8_t *b, size_t n, nl_frame_cb on_frame, void *ctx, int max_frames)
{
    nl_frame *f = &r->frame;
    int got = 0;
    size_t i = 0;
    while (i < n && (max_frames <= 0 || got < max_frames)) {
        uint8_t c = b[i++];
        if (c == '$') {                           /* a frame always starts here; drop any partial line */
            if (r->state == RX_IN || r->junk) r->noise++;
            r->junk = 0;
            r->state = RX_IN;
            r->n = 0;
            r->buf[r->n++] = '$';
            continue;
        }
        if (c == '\n') {
            if (r->state == RX_IN) {
                int e = nl_parse(r->buf, r->n, f);
                if (e == NL_OK) { r->frames++; got++; if (on_frame) on_frame(ctx, f, r->buf, r->n); }
                else if (e == NL_E_CRC) r->bad_crc++;
                else r->bad_syntax++;
            } else if (r->junk) r->noise++;
            r->state = RX_HUNT; r->junk = 0; r->n = 0;
            continue;
        }
        switch (r->state) {
        case RX_HUNT:
            if (c != '\r' && c != ' ' && c != '\t') r->junk = 1;
            break;
        case RX_IN:
            if (c == '\r') break;                 /* tolerated; only meaningful right before '\n' */
            if (c < 0x20 || c > 0x7E) { r->noise++; r->state = RX_DISCARD; r->junk = 0; break; }
            if (r->n >= NL_LINE_MAX) { r->too_long++; r->state = RX_DISCARD; r->junk = 0; break; }
            r->buf[r->n++] = (char)c;
            break;
        default:
            break;                                /* RX_DISCARD: wait for '\n' or '$' */
        }
    }
    return i;
}
