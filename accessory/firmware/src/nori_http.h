// Nori accessory R2 HTTP OTA: request-head and multipart parsing, free of
// Arduino/ESP-IDF so tests/test_http.cpp exercises the exact firmware logic on
// the host.  Deliberately strict: only what requests/urllib3 and curl -F send
// is accepted (one CRLF-framed request head, one file part, final boundary).
#pragma once
#include <stddef.h>
#include <stdint.h>

static const size_t NH_HEAD_MAX = 2048, NH_PART_HEAD_MAX = 512, NH_BOUNDARY_MAX = 70;

struct nh_req {
    char method[8];
    char path[32];                  // request target, query included
    char auth[512];                 // Authorization value ("" if absent)
    char boundary[NH_BOUNDARY_MAX + 1];
    char md5[33];                   // X-Nori-Image-MD5, lower-case hex ("" if absent)
    long content_length;            // -1 if absent
    bool multipart;                 // Content-Type multipart/form-data with a boundary
    bool expect_continue;           // Expect: 100-continue (curl), answered only after authentication
    size_t head_len;                // bytes of the head including the blank line
};

// Parse the request head at the start of buf.  1 complete (r filled), 0 need
// more bytes, -1 malformed/unsupported/too long (no Transfer-Encoding, no
// duplicate Content-Length, header fields within their buffers).
int nh_parse_head(const char *buf, size_t n, nh_req *r);

// Value of Digest parameter `name` (e.g. "uri") in an Authorization value, as
// a whole parameter name (so "nonce" does not match "cnonce").  false if absent
// or longer than cap-1.
bool nh_auth_param(const char *auth, const char *name, char *out, size_t cap);

// Multipart body of POST /update, streamed in arbitrary pieces: exactly one
// part, name="firmware" with a filename, then the closing boundary.  The image
// size follows from Content-Length, so it is known (and bounded) before the
// first image byte is handed to the sink.
struct nh_sink {
    void *ctx;
    bool (*start)(void *ctx, size_t image_size);     // false aborts (nothing written yet)
    bool (*data)(void *ctx, const uint8_t *p, size_t n);
};

enum { NH_B_PART, NH_B_DATA, NH_B_TRAILER, NH_B_DONE, NH_B_ERROR };

struct nh_body {
    int state;
    const char *err;                // reason once state == NH_B_ERROR
    long content_length, consumed;
    size_t image_size, written;
    char boundary[NH_BOUNDARY_MAX + 1];
    char hold[NH_PART_HEAD_MAX + 1];  // part head, then trailer
    size_t held;
};

void nh_body_init(nh_body *b, const char *boundary, long content_length);
// Feed body bytes; returns the new state.  Bytes past Content-Length are an error.
int nh_body_feed(nh_body *b, const uint8_t *p, size_t n, const nh_sink *s);
