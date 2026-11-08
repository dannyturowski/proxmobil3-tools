// See nori_http.h.  Pure C++ (string.h only): built into the firmware and into
// tests/test_http.cpp unchanged.
#include "nori_http.h"

#include <string.h>

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// case-insensitive: does s[0..n) equal lit?
static bool ieq(const char *s, size_t n, const char *lit)
{
    size_t m = strlen(lit);
    if (n != m) return false;
    for (size_t i = 0; i < n; i++) if (lower(s[i]) != lower(lit[i])) return false;
    return true;
}
static bool iprefix(const char *s, size_t n, const char *lit)
{
    size_t m = strlen(lit);
    return n >= m && ieq(s, m, lit);
}
static const char *find(const char *s, size_t n, const char *lit)
{
    size_t m = strlen(lit);
    for (size_t i = 0; i + m <= n; i++) if (!memcmp(s + i, lit, m)) return s + i;
    return nullptr;
}
static void trim(const char **s, size_t *n)
{
    while (*n && (**s == ' ' || **s == '\t')) { (*s)++; (*n)--; }
    while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t')) (*n)--;
}
static bool copy(char *dst, size_t cap, const char *s, size_t n)
{
    if (n >= cap) return false;
    memcpy(dst, s, n);
    dst[n] = 0;
    return true;
}
static bool hex(char c) { c = lower(c); return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
static bool tchar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c && strchr("!#$%&'*+-.^_`|~", c));
}
static bool bchar(char c)    // RFC 2046 boundary characters
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c && strchr("'()+_,-./:=? ", c));
}
static bool visible(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c < 0x20 && c != '\t') || c == 0x7F) return false;
    }
    return true;
}

// multipart/form-data; boundary=X  (X optionally quoted)
static bool content_type(const char *v, size_t n, nh_req *r)
{
    if (!iprefix(v, n, "multipart/form-data")) return true;   // not ours: multipart stays false
    const char *p = v + 19, *e = v + n;
    while (p < e) {
        if (*p != ';') return false;
        p++;
        while (p < e && *p == ' ') p++;
        const char *k = p;
        while (p < e && *p != '=') p++;
        if (p == e) return false;
        size_t kn = (size_t)(p - k);
        p++;
        const char *val = p;
        size_t vn;
        if (p < e && *p == '"') {
            val = ++p;
            while (p < e && *p != '"') p++;
            if (p == e) return false;
            vn = (size_t)(p - val);
            p++;
        } else {
            while (p < e && *p != ';') p++;
            vn = (size_t)(p - val);
            trim(&val, &vn);
        }
        while (p < e && *p == ' ') p++;
        if (ieq(k, kn, "boundary")) {
            if (r->multipart || vn == 0 || vn > NH_BOUNDARY_MAX || val[vn - 1] == ' ') return false;
            for (size_t i = 0; i < vn; i++) if (!bchar(val[i])) return false;
            copy(r->boundary, sizeof r->boundary, val, vn);
            r->multipart = true;
        }
    }
    return r->multipart;
}

int nh_parse_head(const char *buf, size_t n, nh_req *r)
{
    memset(r, 0, sizeof *r);
    r->content_length = -1;
    size_t lim = n < NH_HEAD_MAX ? n : NH_HEAD_MAX;
    const char *end = find(buf, lim, "\r\n\r\n");
    if (!end) return n >= NH_HEAD_MAX ? -1 : 0;
    r->head_len = (size_t)(end - buf) + 4;

    // request line: METHOD SP /target SP HTTP/1.x
    const char *eol = find(buf, (size_t)(end - buf) + 2, "\r\n");
    const char *sp1 = (const char *)memchr(buf, ' ', (size_t)(eol - buf));
    if (!sp1) return -1;
    const char *sp2 = (const char *)memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
    if (!sp2 || sp1 == buf || sp2 == sp1 + 1) return -1;
    for (const char *c = buf; c < sp1; c++) if (*c < 'A' || *c > 'Z') return -1;
    if (!copy(r->method, sizeof r->method, buf, (size_t)(sp1 - buf))) return -1;
    if (sp1[1] != '/' || !visible(sp1 + 1, (size_t)(sp2 - sp1 - 1)) || memchr(sp1 + 1, '\t', (size_t)(sp2 - sp1 - 1))) return -1;
    if (!copy(r->path, sizeof r->path, sp1 + 1, (size_t)(sp2 - sp1 - 1))) return -1;
    if (!ieq(sp2 + 1, (size_t)(eol - sp2 - 1), "HTTP/1.1") && !ieq(sp2 + 1, (size_t)(eol - sp2 - 1), "HTTP/1.0")) return -1;

    bool have_ct = false;
    for (const char *line = eol + 2; line < end + 2;) {
        const char *le = find(line, (size_t)(end + 2 - line), "\r\n");
        size_t ln = (size_t)(le - line);
        const char *colon = (const char *)memchr(line, ':', ln);
        if (!colon || colon == line || !visible(line, ln)) return -1;      // also rejects bare CR/LF
        for (const char *c = line; c < colon; c++) if (!tchar(*c)) return -1;   // no obs-fold, no spaces
        const char *k = line, *v = colon + 1;
        size_t kn = (size_t)(colon - line), vn = (size_t)(le - v);
        trim(&v, &vn);
        if (ieq(k, kn, "Content-Length")) {
            if (r->content_length >= 0 || vn == 0 || vn > 9) return -1;
            long cl = 0;
            for (size_t i = 0; i < vn; i++) {
                if (v[i] < '0' || v[i] > '9') return -1;
                cl = cl * 10 + (v[i] - '0');
            }
            r->content_length = cl;
        } else if (ieq(k, kn, "Transfer-Encoding")) {
            return -1;                                                      // no chunked bodies
        } else if (ieq(k, kn, "Content-Type")) {
            if (have_ct || !content_type(v, vn, r)) return -1;
            have_ct = true;
        } else if (ieq(k, kn, "Authorization")) {
            if (r->auth[0] || vn == 0 || !copy(r->auth, sizeof r->auth, v, vn)) return -1;
        } else if (ieq(k, kn, "X-Nori-Image-MD5")) {
            if (r->md5[0] || vn != 32) return -1;
            for (size_t i = 0; i < 32; i++) {
                if (!hex(v[i])) return -1;
                r->md5[i] = lower(v[i]);
            }
        } else if (ieq(k, kn, "Expect")) {
            if (!ieq(v, vn, "100-continue")) return -1;
            r->expect_continue = true;
        }
        line = le + 2;
    }
    return 1;
}

bool nh_auth_param(const char *auth, const char *name, char *out, size_t cap)
{
    size_t nn = strlen(name), an = strlen(auth);
    for (size_t i = 0; i + nn < an; i++) {
        if (i && auth[i - 1] != ' ' && auth[i - 1] != ',') continue;
        if (strncmp(auth + i, name, nn) || auth[i + nn] != '=') continue;
        const char *v = auth + i + nn + 1;
        if (*v == '"') {
            const char *q = strchr(v + 1, '"');
            if (!q || memchr(v + 1, '\\', (size_t)(q - v - 1))) return false;
            return copy(out, cap, v + 1, (size_t)(q - v - 1));
        }
        size_t vn = strcspn(v, ", ");
        return copy(out, cap, v, vn);
    }
    return false;
}

// ---- multipart body ---------------------------------------------------------------
void nh_body_init(nh_body *b, const char *boundary, long content_length)
{
    memset(b, 0, sizeof *b);
    b->state = NH_B_PART;
    b->content_length = content_length;
    copy(b->boundary, sizeof b->boundary, boundary, strlen(boundary));
}
static int fail(nh_body *b, const char *why)
{
    if (b->state != NH_B_ERROR) b->err = why;
    b->state = NH_B_ERROR;
    return b->state;
}
static size_t trailer_len(const nh_body *b) { return 8 + strlen(b->boundary); }   // "\r\n--" B "--\r\n"

// Content-Disposition: form-data; name="firmware"; filename="..."
static bool disposition(const char *v, size_t n)
{
    const char *p = v, *e = v + n;
    if (!iprefix(p, n, "form-data")) return false;
    p += 9;
    bool name = false, file = false;
    while (p < e) {
        if (*p != ';') return false;
        p++;
        while (p < e && *p == ' ') p++;
        const char *k = p;
        while (p < e && *p != '=') p++;
        if (p + 1 >= e || p[1] != '"') return false;
        size_t kn = (size_t)(p - k);
        const char *val = p + 2;
        const char *q = (const char *)memchr(val, '"', (size_t)(e - val));
        if (!q) return false;
        size_t vn = (size_t)(q - val);
        p = q + 1;
        while (p < e && *p == ' ') p++;
        if (ieq(k, kn, "name") && !name) {
            if (vn != 8 || memcmp(val, "firmware", 8)) return false;   // any other field (e.g. "filesystem")
            name = true;
        } else if (ieq(k, kn, "filename") && !file) {
            if (!vn) return false;
            file = true;
        } else {
            return false;
        }
    }
    return name && file;
}
static bool part_head(nh_body *b, size_t hl)
{
    const char *h = b->hold, *e = b->hold + hl - 2;      // e: the final empty line's CRLF
    size_t bl = strlen(b->boundary);
    if (hl < 2 + bl + 2 || memcmp(h, "--", 2) || memcmp(h + 2, b->boundary, bl) || memcmp(h + 2 + bl, "\r\n", 2)) return false;
    bool cd = false, ct = false;
    for (const char *line = h + 2 + bl + 2; line < e;) {
        const char *le = find(line, (size_t)(e + 2 - line), "\r\n");
        size_t ln = (size_t)(le - line);
        const char *colon = (const char *)memchr(line, ':', ln);
        if (!colon || !visible(line, ln)) return false;
        const char *v = colon + 1;
        size_t vn = (size_t)(le - v);
        trim(&v, &vn);
        if (ieq(line, (size_t)(colon - line), "Content-Disposition") && !cd) { if (!disposition(v, vn)) return false; cd = true; }
        else if (ieq(line, (size_t)(colon - line), "Content-Type") && !ct) ct = true;
        else return false;
        line = le + 2;
    }
    return cd;
}

int nh_body_feed(nh_body *b, const uint8_t *p, size_t n, const nh_sink *s)
{
    if (b->state == NH_B_ERROR) return b->state;
    if ((long)n > b->content_length - b->consumed) return fail(b, "excess");
    b->consumed += (long)n;
    while (n) {
        if (b->state == NH_B_PART) {
            size_t old = b->held, room = NH_PART_HEAD_MAX - old, take = n < room ? n : room;
            memcpy(b->hold + old, p, take);
            b->held += take;
            size_t from = old >= 3 ? old - 3 : 0;
            const char *t = find(b->hold + from, b->held - from, "\r\n\r\n");
            if (!t) {
                if (b->held == NH_PART_HEAD_MAX) return fail(b, "part-head");
                p += take; n -= take;
                continue;
            }
            size_t hl = (size_t)(t - b->hold) + 4;
            if (!part_head(b, hl)) return fail(b, "part");
            if (b->content_length < (long)(hl + trailer_len(b)) + 1) return fail(b, "size");
            p += hl - old; n -= hl - old;
            b->image_size = (size_t)(b->content_length - (long)hl - (long)trailer_len(b));
            b->held = 0;
            b->state = NH_B_DATA;
            if (!s->start(s->ctx, b->image_size)) return fail(b, "rejected");
        } else if (b->state == NH_B_DATA) {
            size_t left = b->image_size - b->written, k = n < left ? n : left;
            if (!s->data(s->ctx, p, k)) return fail(b, "write");
            b->written += k; p += k; n -= k;
            if (b->written == b->image_size) b->state = NH_B_TRAILER;
        } else if (b->state == NH_B_TRAILER) {
            size_t tl = trailer_len(b), k = n < tl - b->held ? n : tl - b->held;
            memcpy(b->hold + b->held, p, k);
            b->held += k; p += k; n -= k;
            if (b->held == tl) {
                size_t bl = strlen(b->boundary);
                if (memcmp(b->hold, "\r\n--", 4) || memcmp(b->hold + 4, b->boundary, bl) || memcmp(b->hold + 4 + bl, "--\r\n", 4))
                    return fail(b, "trailer");
                b->state = NH_B_DONE;
            }
        } else {
            return fail(b, "excess");                      // unreachable: Content-Length bounds the input
        }
    }
    return b->state;
}
