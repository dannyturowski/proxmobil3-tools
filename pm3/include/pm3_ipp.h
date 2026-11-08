/* INIT IPP framing used by the PROXmobil3 contactless reader module on its UART.
 *
 *   byte 0      0xBC start of frame
 *   byte 1      sequence number (sender's own, nonzero, incrementing)
 *   byte 2      replyTo: the peer's sequence number this frame answers, or 0
 *   byte 3      message type; types >= 0x80 carry a payload CRC
 *   bytes 4-5   payload length, big endian
 *   byte 6      CRC-8 Dallas/Maxim over bytes 0..5
 *   payload     <length> bytes
 *   [CRC-32]    types >= 0x80 only: JAMCRC over the payload, little endian
 *
 * Field layout as documented by the PM3 community (wiki.pm3.dev cvend page and
 * Kevin Wallace's notes); this is an independent implementation.  Portable C99,
 * no allocation, no I/O. */
#ifndef PM3_IPP_H
#define PM3_IPP_H

#include <stddef.h>
#include <stdint.h>

#define IPP_SOF     0xBC
#define IPP_HDR     7
#define IPP_MAXPAY  1024
#define IPP_RX_CAP  4096

/* Message types this toolkit names.  Anything else is reported by number only. */
enum {
    IPP_VERSION        = 0x02, IPP_VERSION_REPLY = 0x03,
    IPP_STATUS         = 0x04, IPP_STATUS_REPLY  = 0x05,
    IPP_HEARTBEAT      = 0x07, IPP_STARTUP       = 0x0F,
    IPP_CARD_RELEASE   = 0x32, IPP_ABORT_HANDLING = 0x46,
    IPP_ISO_READ       = 0xB1, IPP_ISO_RELEASED  = 0xB3,
    IPP_DESFIRE_READ   = 0xB9, IPP_DESFIRE_REMOVED = 0xBB,
    IPP_PICC_READ      = 0xBE, IPP_EMV_STATUS    = 0xD1,
    IPP_ULTRALIGHT     = 0xD5, IPP_FUNC_ENABLE   = 0xE4,
    IPP_FUNC_ENABLE_REPLY = 0xE5, IPP_READER_MESSAGE = 0xED
};

/* Reader card functions accepted by IPP_FUNC_ENABLE. */
enum { IPP_FN_ISO = 6, IPP_FN_DESFIRE = 7, IPP_FN_ULTRALIGHT = 10 };

uint8_t  ipp_crc8(const uint8_t *b, size_t n);   /* Dallas/Maxim: reflected 0x8C, init 0 */
uint32_t ipp_crc32(const uint8_t *b, size_t n);  /* JAMCRC: reflected 0xEDB88320, init ~0, no final xor */

/* Short lower-case name for a message type, or NULL if unnamed. */
const char *ipp_type_name(uint8_t type);

/* Build one frame into out.  Returns its length, or 0 if it does not fit. */
size_t ipp_build(uint8_t *out, size_t cap, uint8_t seq, uint8_t reply_to, uint8_t type,
                 const uint8_t *pay, size_t n);

typedef struct {
    uint8_t seq, reply_to, type;
    uint16_t len;
    const uint8_t *payload;      /* valid only during the callback */
} ipp_frame;

typedef void (*ipp_frame_cb)(void *ctx, const ipp_frame *f);

/* Byte-stream deframer.  Resynchronises on the next 0xBC after any error. */
typedef struct {
    uint8_t rx[IPP_RX_CAP];
    size_t used;
    unsigned frames, bad_hdr, bad_crc, oversize, skipped;
} ipp_rx;

void ipp_rx_init(ipp_rx *r);
/* Feed bytes; on_frame runs for every valid frame.  Returns frames delivered. */
unsigned ipp_rx_feed(ipp_rx *r, const uint8_t *b, size_t n, ipp_frame_cb on_frame, void *ctx);

#endif
