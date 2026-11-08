/* INIT IPP framing; see pm3_ipp.h. */
#include "pm3_ipp.h"

#include <string.h>

uint8_t ipp_crc8(const uint8_t *b, size_t n)
{
    uint8_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int j = 0; j < 8; j++) c = (c & 1) ? (uint8_t)((c >> 1) ^ 0x8C) : (uint8_t)(c >> 1);
    }
    return c;
}

uint32_t ipp_crc32(const uint8_t *b, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= b[i];
        for (int j = 0; j < 8; j++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return c;
}

const char *ipp_type_name(uint8_t type)
{
    switch (type) {
    case IPP_VERSION: return "version";
    case IPP_VERSION_REPLY: return "version-reply";
    case IPP_STATUS: return "status";
    case IPP_STATUS_REPLY: return "status-reply";
    case IPP_HEARTBEAT: return "heartbeat";
    case IPP_STARTUP: return "module-startup";
    case IPP_CARD_RELEASE: return "card-release";
    case IPP_ABORT_HANDLING: return "abort-card-handling";
    case IPP_ISO_READ: return "iso-read";
    case IPP_ISO_RELEASED: return "iso-card-released";
    case IPP_DESFIRE_READ: return "desfire-read";
    case IPP_DESFIRE_REMOVED: return "desfire-card-removed";
    case IPP_PICC_READ: return "picc-read";
    case IPP_EMV_STATUS: return "emv-status";
    case IPP_ULTRALIGHT: return "ultralight";
    case IPP_FUNC_ENABLE: return "function-enable";
    case IPP_FUNC_ENABLE_REPLY: return "function-enable-reply";
    case IPP_READER_MESSAGE: return "reader-message";
    default: return NULL;
    }
}

size_t ipp_build(uint8_t *out, size_t cap, uint8_t seq, uint8_t reply_to, uint8_t type,
                 const uint8_t *pay, size_t n)
{
    size_t total = IPP_HDR + n + (type >= 0x80 ? 4 : 0);
    if (n > IPP_MAXPAY || total > cap || (n && !pay)) return 0;
    out[0] = IPP_SOF; out[1] = seq; out[2] = reply_to; out[3] = type;
    out[4] = (uint8_t)(n >> 8); out[5] = (uint8_t)n;
    out[6] = ipp_crc8(out, 6);
    if (n) memcpy(out + IPP_HDR, pay, n);
    if (type >= 0x80) {
        uint32_t c = ipp_crc32(pay, n);
        for (int i = 0; i < 4; i++) out[IPP_HDR + n + i] = (uint8_t)(c >> (8 * i));
    }
    return total;
}

void ipp_rx_init(ipp_rx *r) { memset(r, 0, sizeof *r); }

static void drop(ipp_rx *r, size_t k)
{
    memmove(r->rx, r->rx + k, r->used - k);
    r->used -= k;
}

unsigned ipp_rx_feed(ipp_rx *r, const uint8_t *b, size_t n, ipp_frame_cb on_frame, void *ctx)
{
    unsigned got = 0;
    while (n > 0) {
        size_t room = IPP_RX_CAP - r->used, k = n < room ? n : room;
        memcpy(r->rx + r->used, b, k);
        r->used += k; b += k; n -= k;
        for (;;) {
            if (r->used == 0) break;
            if (r->rx[0] != IPP_SOF) {
                const uint8_t *q = memchr(r->rx + 1, IPP_SOF, r->used - 1);
                size_t skip = q ? (size_t)(q - r->rx) : r->used;
                r->skipped += (unsigned)skip;
                drop(r, skip);
                continue;
            }
            if (r->used < IPP_HDR) break;
            if (ipp_crc8(r->rx, 6) != r->rx[6]) { r->bad_hdr++; drop(r, 1); continue; }
            size_t plen = ((size_t)r->rx[4] << 8) | r->rx[5];
            if (plen > IPP_MAXPAY) { r->oversize++; drop(r, 1); continue; }
            size_t total = IPP_HDR + plen + (r->rx[3] >= 0x80 ? 4 : 0);
            if (r->used < total) break;
            if (r->rx[3] >= 0x80) {
                const uint8_t *c = r->rx + IPP_HDR + plen;
                uint32_t want = c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24;
                if (want != ipp_crc32(r->rx + IPP_HDR, plen)) { r->bad_crc++; drop(r, 1); continue; }
            }
            ipp_frame f = { r->rx[1], r->rx[2], r->rx[3], (uint16_t)plen, r->rx + IPP_HDR };
            r->frames++; got++;
            if (on_frame) on_frame(ctx, &f);
            drop(r, total);
        }
    }
    return got;
}
