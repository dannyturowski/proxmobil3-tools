/* wt32-r5.1: changing the saved networks from outside.  Portable C, shared by firmware and tests.
 *
 * One sealed envelope carries a whole change, over the UART link or authenticated HTTP alike.  It is keyed with the
 * OTA secret (md5(password) as 32 hex characters, the same secret OTAKEY stores), so it needs no new credential:
 *
 *   nonce  16 random bytes from the device, single use: burnt by the first attempt to open it, gone after NWP_TTL_MS
 *   K      = HMAC-SHA256(secret, "nori-wifi-v1" | nonce)     Kenc = HMAC(K, "enc")     Kmac = HMAC(K, "mac")
 *   ct     = payload XOR (HMAC(Kenc, be32(0)) | HMAC(Kenc, be32(1)) | ...)
 *   tag    = HMAC(Kmac, nonce | ct)
 *
 * Why not just the Digest login: Digest qop=auth protects the headers, not the body, so plain passwords in a POST would
 * cross the Wi-Fi readable and replaceable.  The tag is checked before a single byte is decrypted, and an envelope is
 * applied whole or not at all; the reply carries a word and the count, never a value from it.
 *
 *   payload: version=1, op (1 add, 2 replace, 3 delete), flags (1 force, 2 update), pos, k, then k times
 *            ssid_len ssid psk_len psk net_flags.  Exact length, nothing after the last entry.
 *
 * UART frames (handled through nd_hal.ext_cmd; each is answered before the next is sent):
 *   WNBEGIN                     -> OK WNBEGIN n=<32 hex>          / ERR WNBEGIN nokey
 *   WNDATA o=<offset> d=<b64>   -> OK WNDATA o=<next>             / ERR WNDATA <format|expired|full>
 *   WNEND t=<64 hex>            -> OK WNEND n=<networks> g=<gen>  / ERR WNEND <word>
 *   WNABORT                     -> OK WNABORT
 * HTTP (Digest, as /status):
 *   GET /wifi/nonce -> {"nonce":"<32 hex>"}     POST /wifi, body nonce|ct|tag -> {"ok":1,"n":..,"g":..} / {"ok":0,"err":".."}
 *
 * WIFISET and WIFICLR stay as they were on the wire; nwp_wifiset / nwp_clear give them their table meaning. */
#ifndef NORI_WNPROV_H
#define NORI_WNPROV_H

#include "nori_link.h"
#include "nori_wnet.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NWP_NONCE 16
#define NWP_TAG 32
#define NWP_TTL_MS 60000u
#define NWP_PAYLOAD_MAX (5 + NW_MAX * (3 + NW_SSID_MAX + NW_PSK_MAX))
#define NWP_HTTP_MAX (NWP_NONCE + NWP_PAYLOAD_MAX + NWP_TAG)
enum { NWP_OP_ADD = 1, NWP_OP_REPLACE, NWP_OP_DEL };
#define NWP_FL_FORCE 1
#define NWP_FL_UPDATE 2

typedef struct {
    int op, force, update, pos, k;
    nw_net nets[NW_MAX];
} nwp_op;

typedef struct {
    nw_table *t;               /* the live table */
    const nw_kv *kv;
    void *ctx;
    const char *(*key)(void *ctx);                   /* the 32 character secret, "" if none is provisioned */
    void (*rnd)(void *ctx, uint8_t *b, size_t n);
    int (*active)(void *ctx);                        /* network in use, -1 none */
    void (*changed)(void *ctx, const nw_table *old, int primary_named);   /* after a commit */
    /* state */
    int open;
    uint8_t nonce[NWP_NONCE];
    uint32_t born;
    size_t len;
    uint8_t buf[NWP_PAYLOAD_MAX];
    nwp_op op;
    nw_table next, old;
} nwp_ctx;

void nwp_sha256(const uint8_t *b, size_t n, uint8_t out[32]);
void nwp_hmac(const uint8_t *key, size_t klen, const uint8_t *a, size_t an, const uint8_t *b, size_t bn, uint8_t out[32]);
/* Authenticate and decrypt; NW_OK, NW_E_AUTH or NW_E_FORMAT.  ct is not modified. */
int nwp_open(const char *secret, const uint8_t *nonce, const uint8_t *ct, size_t n, const uint8_t *tag, nwp_op *out);
int nwp_parse(const uint8_t *p, size_t n, nwp_op *out);

int nwp_cmd(nwp_ctx *c, const nl_frame *f, char *reply, size_t cap, uint32_t now);    /* 1: a WN* frame, reply written */
int nwp_http_nonce(nwp_ctx *c, char *hex, size_t cap, uint32_t now);                  /* NW_OK or NW_E_NOKEY */
int nwp_http_post(nwp_ctx *c, const uint8_t *body, size_t n, uint32_t now, char *json, size_t cap);   /* HTTP status */
void nwp_abort(nwp_ctx *c);
int nwp_wifiset(nwp_ctx *c, const char *ssid, const char *psk);                       /* NW_OK, NW_E_FULL, NW_E_STORE... */
int nwp_clear(nwp_ctx *c);

#ifdef __cplusplus
}
#endif
#endif
