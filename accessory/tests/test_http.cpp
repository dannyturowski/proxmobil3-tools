// Host tests for firmware/src/nori_http.cpp (the HTTP OTA head/multipart parser,
// compiled unchanged).  Run via tests/run_tests.sh under ASan/UBSan.
//   test_http                    unit suite
//   test_http --request OUT      parse one raw HTTP request from stdin exactly as
//                                the firmware does (body in irregular pieces),
//                                write the image to OUT, print the verdict
#include "nori_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Sink {
    std::vector<uint8_t> got;
    int starts = 0;
    size_t size = 0;
    bool start_ok = true, data_ok = true;
    size_t limit = (size_t)-1;
};
static bool s_start(void *c, size_t n)
{
    Sink *s = (Sink *)c;
    s->starts++;
    s->size = n;
    return s->start_ok && n <= s->limit;
}
static bool s_data(void *c, const uint8_t *p, size_t n)
{
    Sink *s = (Sink *)c;
    s->got.insert(s->got.end(), p, p + n);
    return s->data_ok;
}

static std::string image(size_t n)
{
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = (char)((i * 131 + 7) & 0xFF);
    s[0] = (char)0xE9;
    return s;
}
static const char *B = "----nori1234";
static std::string body(const std::string &img, const char *disp = "form-data; name=\"firmware\"; filename=\"firmware.bin\"",
                        const char *extra = "Content-Type: application/octet-stream\r\n", const char *end = nullptr)
{
    std::string b = std::string("--") + B + "\r\nContent-Disposition: " + disp + "\r\n" + extra + "\r\n" + img;
    b += end ? std::string(end) : std::string("\r\n--") + B + "--\r\n";
    return b;
}
// feed in pieces of `step` bytes (0: all at once); returns final state
static int run(const std::string &b, Sink &s, size_t step = 0, long cl = -1, const char **err = nullptr)
{
    nh_body x;
    nh_body_init(&x, B, cl < 0 ? (long)b.size() : cl);
    nh_sink k = { &s, s_start, s_data };
    int st = NH_B_PART;
    size_t i = 0;
    while (i < b.size() && st != NH_B_ERROR) {
        size_t n = step ? std::min(step, b.size() - i) : b.size() - i;
        st = nh_body_feed(&x, (const uint8_t *)b.data() + i, n, &k);
        i += n;
    }
    if (err) *err = x.err;
    return st;
}

static int head(const std::string &h, nh_req *r) { return nh_parse_head(h.data(), h.size(), r); }

static void test_head()
{
    nh_req r;
    std::string g = "GET /status HTTP/1.1\r\nHost: 192.168.0.28\r\nAuthorization: Digest username=\"nori\", uri=\"/status\"\r\n\r\n";
    CHECK(head(g, &r) == 1 && !strcmp(r.method, "GET") && !strcmp(r.path, "/status") && r.head_len == g.size());
    CHECK(!strncmp(r.auth, "Digest username=", 16) && r.content_length == -1 && !r.multipart);
    for (size_t i = 0; i < g.size(); i++) CHECK(nh_parse_head(g.data(), i, &r) == 0);   // every prefix: need more
    CHECK(head(g + "POST /x", &r) == 1 && r.head_len == g.size());                    // stops at the blank line

    std::string p = "POST /update HTTP/1.1\r\nhost: x\r\ncontent-length: 1234\r\ncontent-type: multipart/form-data; boundary=\"----nori1234\"\r\n"
                    "X-Nori-Image-MD5: 0123456789ABCDEF0123456789abcdef\r\nExpect: 100-continue\r\n\r\n";
    CHECK(head(p, &r) == 1 && r.content_length == 1234 && r.multipart && !strcmp(r.boundary, B));
    CHECK(!strcmp(r.md5, "0123456789abcdef0123456789abcdef") && r.expect_continue);
    CHECK(head("POST /update HTTP/1.1\r\nContent-Type: multipart/form-data; boundary=abc\r\n\r\n", &r) == 1 && !strcmp(r.boundary, "abc"));
    CHECK(head("POST /update HTTP/1.1\r\nContent-Type: text/plain\r\n\r\n", &r) == 1 && !r.multipart);

    const char *bad[] = {
        "get /status HTTP/1.1\r\n\r\n",                                     // method case
        "GET status HTTP/1.1\r\n\r\n",                                      // not origin-form
        "GET /status HTTP/2\r\n\r\n",
        "GET /status\r\n\r\n",
        "GET  /status HTTP/1.1\r\n\r\n",
        "GET /aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa HTTP/1.1\r\n\r\n",          // path > 31
        "TOOLONGMETHOD / HTTP/1.1\r\n\r\n",
        "POST /update HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Length: -5\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Length: 5x\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Length: 1234567890\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Length:\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Type: multipart/form-data\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Type: multipart/form-data; boundary=\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Type: multipart/form-data; boundary=a\"b\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Type: multipart/form-data; boundary=a; boundary=b\r\n\r\n",
        "POST /update HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Type: text/plain\r\n\r\n",
        "POST /update HTTP/1.1\r\nX-Nori-Image-MD5: 0123\r\n\r\n",
        "POST /update HTTP/1.1\r\nX-Nori-Image-MD5: 0123456789abcdef0123456789abcdeg\r\n\r\n",
        "POST /update HTTP/1.1\r\nExpect: something\r\n\r\n",
        "GET /status HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n",
        "GET /status HTTP/1.1\r\nBad Name: x\r\n\r\n",
        "GET /status HTTP/1.1\r\nNoColon\r\n\r\n",
        "GET /status HTTP/1.1\r\nX: a\nb\r\n\r\n",
        "GET /status HTTP/1.1\r\nAuthorization: a\r\nAuthorization: b\r\n\r\n",
    };
    for (const char *h : bad) {
        int v = nh_parse_head(h, strlen(h), &r);
        if (v != -1) printf("  accepted: %s\n", h);
        CHECK(v == -1);
    }
    std::string boundary71 = "POST /update HTTP/1.1\r\nContent-Type: multipart/form-data; boundary=" + std::string(71, 'a') + "\r\n\r\n";
    CHECK(head(boundary71, &r) == -1);
    std::string auth_long = "GET /status HTTP/1.1\r\nAuthorization: " + std::string(600, 'a') + "\r\n\r\n";
    CHECK(head(auth_long, &r) == -1);
    std::string huge = "GET /status HTTP/1.1\r\nX: " + std::string(NH_HEAD_MAX, 'a');
    CHECK(head(huge, &r) == -1);                                              // no blank line within 2 KB
    std::string almost = "GET /status HTTP/1.1\r\nX: " + std::string(NH_HEAD_MAX - 40, 'a');
    CHECK(head(almost, &r) == 0);
}

static void test_auth_param()
{
    const char *a = "Digest username=\"nori\", realm=\"nori-ota\", nonce=\"N1\", uri=\"/update\", response=\"R\", "
                    "opaque=\"O\", algorithm=\"MD5\", qop=\"auth\", nc=00000002, cnonce=\"C1\"";
    char v[40];
    CHECK(nh_auth_param(a, "uri", v, sizeof v) && !strcmp(v, "/update"));
    CHECK(nh_auth_param(a, "nonce", v, sizeof v) && !strcmp(v, "N1"));
    CHECK(nh_auth_param(a, "cnonce", v, sizeof v) && !strcmp(v, "C1"));
    CHECK(nh_auth_param(a, "nc", v, sizeof v) && !strcmp(v, "00000002"));
    const char *b = "Digest cnonce=\"C\", nonce=\"N\", xuri=\"/evil\", uri=\"/status\"";
    CHECK(nh_auth_param(b, "nonce", v, sizeof v) && !strcmp(v, "N"));
    CHECK(nh_auth_param(b, "uri", v, sizeof v) && !strcmp(v, "/status"));
    CHECK(!nh_auth_param("Digest username=\"nori\"", "uri", v, sizeof v));
    CHECK(!nh_auth_param("Digest uri=\"/unterminated", "uri", v, sizeof v));
    CHECK(!nh_auth_param("Digest uri=\"/a\\\"b\"", "uri", v, sizeof v));
    CHECK(!nh_auth_param("Digest uri=\"/0123456789\"", "uri", v, 5));
}

static void test_body()
{
    std::string img = image(5000), b = body(img);
    const size_t steps[] = { 0, 1, 2, 3, 7, 64, 511, 512, 513, 1460, 4999 };
    for (size_t step : steps) {
        Sink s;
        int st = run(b, s, step);
        CHECK(st == NH_B_DONE && s.starts == 1 && s.size == img.size() && s.got == std::vector<uint8_t>(img.begin(), img.end()));
    }
    {   // image containing CRLF--boundary-like bytes is fine: the size comes from Content-Length
        std::string tricky = std::string("\xE9") + "\r\n--" + B + "--\r\n" + image(100);
        Sink s;
        CHECK(run(body(tricky), s, 5) == NH_B_DONE && s.got.size() == tricky.size());
    }
    {   // curl-style part without Content-Type, lower-case header name
        std::string c = std::string("--") + B + "\r\ncontent-disposition: form-data; name=\"firmware\"; filename=\"f.bin\"\r\n\r\n" + img + "\r\n--" + B + "--\r\n";
        Sink s;
        CHECK(run(c, s, 100) == NH_B_DONE && s.got.size() == img.size());
    }
    struct { const char *disp, *extra, *end; const char *why; } bad[] = {
        { "form-data; name=\"filesystem\"; filename=\"spiffs.bin\"", "", nullptr, "part" },
        { "form-data; name=\"firmware\"", "", nullptr, "part" },                        // not a file
        { "form-data; name=\"firmware\"; filename=\"\"", "", nullptr, "part" },
        { "form-data; filename=\"f\"", "", nullptr, "part" },
        { "attachment; name=\"firmware\"; filename=\"f\"", "", nullptr, "part" },
        { "form-data; name=\"firmware\"; filename=\"f\"; size=\"1\"", "", nullptr, "part" },
        { "form-data; name=\"firmware\"; name=\"firmware\"; filename=\"f\"", "", nullptr, "part" },
        { "form-data; name=\"firmware\"; filename=\"f\"", "X-Other: 1\r\n", nullptr, "part" },
        { "form-data; name=\"firmware\"; filename=\"f\"", "Content-Type: a\r\nContent-Type: b\r\n", nullptr, "part" },
        { "form-data; name=\"firmware\"; filename=\"f\"", "", "\r\n--other--\r\n", "trailer" },
        { "form-data; name=\"firmware\"; filename=\"f\"", "", "\r\n--" "----nori1234" "\r\nXX", "trailer" },
    };
    for (auto &c : bad) {
        Sink s;
        const char *err = nullptr;
        int st = run(body(img, c.disp, c.extra, c.end), s, 300, -1, &err);
        CHECK(st == NH_B_ERROR && err && !strcmp(err, c.why));
        if (!strcmp(c.why, "part")) CHECK(s.starts == 0 && s.got.empty());      // refused before the sink saw anything
    }
    {   // wrong boundary on the first line
        std::string w = body(img);
        w[5] = 'X';
        Sink s;
        CHECK(run(w, s) == NH_B_ERROR && s.starts == 0);
    }
    {   // truncated: never DONE, so the firmware never calls Update.end()
        Sink s;
        CHECK(run(b.substr(0, b.size() - 1), s, 0, (long)b.size()) == NH_B_TRAILER);
        Sink s2;
        CHECK(run(b.substr(0, 2000), s2, 0, (long)b.size()) == NH_B_DATA);
        Sink s3;
        CHECK(run(b.substr(0, 40), s3, 0, (long)b.size()) == NH_B_PART && s3.starts == 0);
    }
    {   // more bytes than Content-Length
        Sink s;
        const char *err = nullptr;
        CHECK(run(b + "x", s, 0, (long)b.size(), &err) == NH_B_ERROR && !strcmp(err, "excess"));
    }
    {   // Content-Length too small to hold any image
        std::string e = body("");
        Sink s;
        const char *err = nullptr;
        CHECK(run(e, s, 0, -1, &err) == NH_B_ERROR && !strcmp(err, "size") && s.starts == 0);
    }
    {   // sink refuses at start (e.g. larger than the slot): no data delivered
        Sink s;
        s.limit = img.size() - 1;
        const char *err = nullptr;
        CHECK(run(b, s, 700, -1, &err) == NH_B_ERROR && !strcmp(err, "rejected") && s.got.empty());
    }
    {   // write failure stops at once
        Sink s;
        s.data_ok = false;
        const char *err = nullptr;
        CHECK(run(b, s, 700, -1, &err) == NH_B_ERROR && !strcmp(err, "write") && s.got.size() <= 700);
    }
    {   // part head beyond NH_PART_HEAD_MAX
        std::string big = body(img, "form-data; name=\"firmware\"; filename=\"f\"", ("Content-Type: " + std::string(600, 'a') + "\r\n").c_str());
        Sink s;
        const char *err = nullptr;
        CHECK(run(big, s, 64, -1, &err) == NH_B_ERROR && !strcmp(err, "part-head") && s.starts == 0);
    }
    {   // errors are sticky
        nh_body x;
        Sink s;
        nh_sink k = { &s, s_start, s_data };
        nh_body_init(&x, B, 10);
        CHECK(nh_body_feed(&x, (const uint8_t *)"0123456789A", 11, &k) == NH_B_ERROR);
        CHECK(nh_body_feed(&x, (const uint8_t *)"0", 1, &k) == NH_B_ERROR);
    }
}

// ---- driver: one raw request on stdin -------------------------------------------
static int request(const char *out)
{
    std::string in;
    char tmp[4096];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) in.append(tmp, n);
    static nh_req r;
    int pr = nh_parse_head(in.data(), in.size() < NH_HEAD_MAX ? in.size() : NH_HEAD_MAX, &r);
    if (pr != 1) { printf("ERR head %d\n", pr); return 0; }
    printf("HEAD %s %s cl=%ld multipart=%d md5=%s auth=%d\n", r.method, r.path, r.content_length, r.multipart, r.md5[0] ? r.md5 : "-", r.auth[0] != 0);
    if (strcmp(r.method, "POST")) return 0;
    if (!r.multipart || r.content_length < 0) { printf("ERR format\n"); return 0; }
    std::string b = in.substr(r.head_len);
    if ((long)b.size() > r.content_length) b.resize((size_t)r.content_length);
    Sink s;
    nh_body x;
    nh_body_init(&x, r.boundary, r.content_length);
    nh_sink k = { &s, s_start, s_data };
    int st = NH_B_PART;
    size_t i = 0, step = 1;
    while (i < b.size() && st != NH_B_ERROR) {                 // irregular pieces, as TCP delivers them
        size_t m = std::min(step, b.size() - i);
        st = nh_body_feed(&x, (const uint8_t *)b.data() + i, m, &k);
        i += m;
        step = step * 7 % 1499 + 1;
    }
    if (st != NH_B_DONE) { printf("ERR %s\n", st == NH_B_ERROR ? x.err : "short"); return 0; }
    FILE *f = fopen(out, "wb");
    if (!f || fwrite(s.got.data(), 1, s.got.size(), f) != s.got.size()) { printf("ERR io\n"); return 1; }
    fclose(f);
    printf("OK %zu\n", s.got.size());
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--request")) return request(argv[2]);
    test_head();
    test_auth_param();
    test_body();
    printf("http: %d checks, %d failures\n", checks, failures);
    return failures != 0;
}
