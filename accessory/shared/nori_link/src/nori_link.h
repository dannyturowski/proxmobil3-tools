/* Nori accessory link, protocol version 1.
 *
 * One frame per text line, printable ASCII only:
 *
 *     $NL1 <seq> <TYPE>[ <arg>...]*XXXX\n
 *
 * XXXX is CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over the bytes between
 * '$' and '*', in hex.  <seq> is decimal and increases by one for every frame a
 * sender emits during one boot/session.  Args are tokens of 0x21..0x7E without
 * '$' or '*', usually key=value.  Anything that is not a well-formed frame
 * (ESP32 ROM boot text, line noise, truncated or oversized lines) is counted and
 * dropped.  The same C code runs on the ESP32, in the Nori app and in the host
 * link helper, so all three agree on the format.
 *
 * This file is portable C99 with no allocation.
 */
#ifndef NORI_LINK_H
#define NORI_LINK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NL_PROTO           1
#define NL_LINE_MAX        320     /* bytes from '$' up to (not including) '\n' */
#define NL_ARGS_MAX        10
#define NL_TUN_CHUNK       180     /* raw tunnel bytes per TDATA line (240 base64 chars) */
#define NL_TUN_WINDOW      4096    /* unacknowledged tunnel bytes in flight, per direction */
#define NL_HB_MS           1000    /* both sides send a heartbeat this often */
#define NL_PEER_TIMEOUT_MS 3500    /* no valid frame for this long => peer considered gone */
#define NL_EV_STALE_MS     2000    /* events older than this (device clock) are ignored */

/* parse results */
enum { NL_OK = 0, NL_E_NOISE = -1, NL_E_LONG = -2, NL_E_CRC = -3, NL_E_SYNTAX = -4, NL_E_MAGIC = -5 };

/* accessory input events (EV frames) */
/* wt32-r3 appends pon/poff: debounced presence level changes (VL53L5CX).  Hosts built before r3 map
 * unknown names to NL_EV_NONE and ignore the frame, so the additions are backward compatible. */
/* wt32-r5 appends trk5/trk1: the centre-off three-way lever moved to a direction.  Absolute positions, not
 * toggles: trk5 = GPIO15 contact (lever down), trk1 = GPIO2 contact (lever up).  Names kept for compatibility. */
/* wt32-r5.1.1 appends tsh5/tsh1 (short release) and tlg5/tlg1 (held 1 s): what the move just reported by
 * trk5/trk1 turned out to be.  They never replace those: each follows its own trk, at most one per move. */
enum { NL_EV_NONE, NL_EV_TAP, NL_EV_DTAP, NL_EV_B1D, NL_EV_B1U, NL_EV_B2D, NL_EV_B2U, NL_EV_TILT, NL_EV_PON, NL_EV_POFF,
       NL_EV_TRK5, NL_EV_TRK1, NL_EV_TSH5, NL_EV_TSH1, NL_EV_TLG5, NL_EV_TLG1, NL_EV_COUNT };
extern const char *const NL_EV_NAME[NL_EV_COUNT];
int nl_ev_from_name(const char *s);   /* NL_EV_NONE if unknown */

typedef struct {
    uint32_t seq;
    const char *type;
    int argc;
    const char *argv[NL_ARGS_MAX];
    char store[NL_LINE_MAX + 1];
} nl_frame;

uint16_t nl_crc16(const void *data, size_t n);
size_t nl_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);   /* chars written + NUL; 0 if cap too small */
int nl_b64_decode(const char *in, uint8_t *out, size_t cap);               /* bytes, or -1 if malformed/too big */
int nl_token_ok(const char *s, size_t n);

/* Build "$NL1 <seq> <type>[ <args>]*XXXX\n".  args: NULL/"" or space separated tokens.
 * Returns the line length including '\n', or -1 if anything is invalid or too long. */
int nl_format(char *out, size_t cap, uint32_t seq, const char *type, const char *args);

/* Parse one line (from '$', without '\n'; a trailing '\r' is tolerated). */
int nl_parse(const char *line, size_t n, nl_frame *f);
const char *nl_get(const nl_frame *f, const char *key);            /* value of key=value, or NULL */
int nl_get_u32(const nl_frame *f, const char *key, uint32_t *out); /* decimal; 0 ok, -1 missing/bad */
int nl_get_hex32(const nl_frame *f, const char *key, uint32_t *out);
int nl_parse_u32(const char *s, uint32_t *out);

/* Line assembler: finds frames in an arbitrary byte stream. */
typedef void (*nl_frame_cb)(void *ctx, const nl_frame *f, const char *raw, size_t rawlen);
typedef struct {
    char buf[NL_LINE_MAX + 1];
    size_t n;
    int state;          /* internal */
    int junk;           /* internal: non-frame bytes seen on this line */
    uint32_t frames, noise, too_long, bad_crc, bad_syntax;
    nl_frame frame;     /* internal: the frame handed to on_frame */
} nl_rx;
void nl_rx_init(nl_rx *r);
/* Feed bytes; on_frame is called for each valid frame, at most max_frames times
 * (<= 0: unlimited).  Returns the number of bytes consumed. */
size_t nl_rx_feed(nl_rx *r, const uint8_t *b, size_t n, nl_frame_cb on_frame, void *ctx, int max_frames);

#ifdef __cplusplus
}
#endif
#endif
