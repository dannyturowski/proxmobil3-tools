/* Host unit tests for the PM3 library: IPP framing, the NFC presence engine, PIC32
 * output parsing, scanner commands, and the port and USB identity checks.
 * No hardware; time is simulated.  Build and run: make unit */
#define _GNU_SOURCE
#include "pm3_ipp.h"
#include "pm3_nfc.h"
#include "pm3_pic32.h"
#include "pm3_scanner.h"
#include "pm3_serial.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_checks, g_fail;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define SP(buf, ...) CHECK((size_t)snprintf((buf), sizeof (buf), __VA_ARGS__) < sizeof (buf))

/* ---- capture harness ----------------------------------------------------------------------- */
typedef struct { uint8_t type, seq, reply_to; uint16_t len; uint8_t pay[16]; } Tx;
typedef struct {
    Tx tx[256]; int ntx;
    char out[64 * 1024]; size_t nout;
    ipp_rx txrx;
    uint8_t dev_seq;
} Cap;

static void cap_frame(void *ctx, const ipp_frame *f)
{
    Cap *c = ctx;
    if (c->ntx >= 256) return;
    Tx *t = &c->tx[c->ntx++];
    t->type = f->type; t->seq = f->seq; t->reply_to = f->reply_to; t->len = f->len;
    memcpy(t->pay, f->payload, f->len < 16 ? f->len : 16);
}
static void cap_send(void *ctx, const uint8_t *b, size_t n) { Cap *c = ctx; ipp_rx_feed(&c->txrx, b, n, cap_frame, c); }
static void cap_emit(void *ctx, const char *line)
{
    Cap *c = ctx;
    size_t n = strlen(line);
    if (c->nout + n + 2 < sizeof c->out) { memcpy(c->out + c->nout, line, n); c->nout += n; c->out[c->nout++] = '\n'; c->out[c->nout] = 0; }
}
static int count_str(const char *hay, const char *needle)
{
    int k = 0;
    for (const char *p = hay; (p = strstr(p, needle)); p += strlen(needle)) k++;
    return k;
}
static int tx_count(const Cap *c, uint8_t type)
{
    int k = 0;
    for (int i = 0; i < c->ntx; i++) k += c->tx[i].type == type;
    return k;
}
static const Tx *tx_last(const Cap *c, uint8_t type)
{
    for (int i = c->ntx - 1; i >= 0; i--) if (c->tx[i].type == type) return &c->tx[i];
    return NULL;
}
/* The simulated reader sends one frame. */
static void dev(pm3_nfc *z, Cap *c, uint8_t reply_to, uint8_t type, const uint8_t *pay, size_t n, double t)
{
    uint8_t b[IPP_HDR + IPP_MAXPAY + 4];
    if (++c->dev_seq == 0) c->dev_seq = 1;
    size_t len = ipp_build(b, sizeof b, c->dev_seq, reply_to, type, pay, n);
    pm3_nfc_bytes(z, b, len, t);
}
static void run(pm3_nfc *z, double from, double to)
{
    for (double t = from; t <= to + 1e-9; t += 0.01) pm3_nfc_tick(z, t);
}
/* Bring an engine to ready, acknowledging every enable; returns the time reached. */
static double setup_ready(pm3_nfc *z, Cap *c, double t)
{
    for (int guard = 0; guard < 2000 && z->stage != PM3_ST_READY; guard++) {
        pm3_nfc_tick(z, t);
        const Tx *e = tx_last(c, IPP_FUNC_ENABLE);
        static uint8_t answered[256];
        if (e && !answered[e->seq] && z->stage == PM3_ST_ENABLE) {
            uint8_t ack[4] = { 0, e->pay[1], 0, 0 };
            answered[e->seq] = 1;
            dev(z, c, e->seq, IPP_FUNC_ENABLE_REPLY, ack, 4, t + 0.005);
            memset(answered, 0, sizeof answered);
        }
        t += 0.01;
    }
    return t;
}

/* ---- tests --------------------------------------------------------------------------------- */
static void test_crc_and_framing(void)
{
    CHECK(ipp_crc8((const uint8_t *)"123456789", 9) == 0xA1);
    CHECK(ipp_crc32((const uint8_t *)"123456789", 9) == 0x340BC6D9u);
    uint8_t b[64];
    size_t n = ipp_build(b, sizeof b, 5, 0, IPP_STATUS, NULL, 0);
    CHECK(n == 7 && b[0] == 0xBC && b[1] == 5 && b[3] == 0x04 && b[6] == ipp_crc8(b, 6));
    uint8_t pay[3] = { 1, 2, 3 };
    n = ipp_build(b, sizeof b, 9, 4, IPP_ISO_READ, pay, 3);
    CHECK(n == 7 + 3 + 4);
    uint32_t c = ipp_crc32(pay, 3);
    CHECK(b[10] == (uint8_t)c && b[13] == (uint8_t)(c >> 24));
    CHECK(ipp_build(b, 8, 1, 0, IPP_ISO_READ, pay, 3) == 0);        /* does not fit */
    CHECK(ipp_build(b, sizeof b, 1, 0, IPP_STATUS, NULL, 2) == 0);   /* length without payload */

    Cap cap; memset(&cap, 0, sizeof cap);
    ipp_rx r; ipp_rx_init(&r);
    uint8_t stream[200]; size_t sn = 0;
    stream[sn++] = 0x00; stream[sn++] = 0x55;                       /* line noise */
    size_t a = ipp_build(stream + sn, sizeof stream - sn, 1, 0, IPP_HEARTBEAT, NULL, 0);
    stream[sn + 6] ^= 0xFF; sn += a;                                 /* bad header CRC */
    sn += ipp_build(stream + sn, sizeof stream - sn, 2, 0, IPP_ISO_READ, pay, 3);
    stream[sn - 1] ^= 1;                                             /* bad payload CRC */
    sn += ipp_build(stream + sn, sizeof stream - sn, 3, 7, IPP_STATUS_REPLY, pay, 2);
    for (size_t i = 0; i < sn; i++) ipp_rx_feed(&r, stream + i, 1, cap_frame, &cap);   /* byte by byte */
    CHECK(r.bad_hdr >= 1 && r.bad_crc == 1 && cap.ntx == 1 && cap.tx[0].type == IPP_STATUS_REPLY && cap.tx[0].reply_to == 7);
    /* oversize length is rejected and the parser resynchronises */
    uint8_t big[7] = { 0xBC, 1, 0, 0x07, 0x7F, 0xFF, 0 };
    big[6] = ipp_crc8(big, 6);
    ipp_rx_init(&r); cap.ntx = 0;
    ipp_rx_feed(&r, big, 7, cap_frame, &cap);
    n = ipp_build(b, sizeof b, 4, 0, IPP_HEARTBEAT, NULL, 0);
    ipp_rx_feed(&r, b, n, cap_frame, &cap);
    CHECK(r.oversize == 1 && cap.ntx == 1 && cap.tx[0].type == IPP_HEARTBEAT);
    CHECK(!strcmp(ipp_type_name(0xD1), "emv-status") && ipp_type_name(0x99) == NULL);
}

static void test_setup_sequence(void)
{
    Cap c; memset(&c, 0, sizeof c); ipp_rx_init(&c.txrx);
    pm3_nfc z;
    pm3_nfc_init(&z, NULL, cap_send, cap_emit, &c, 0);
    run(&z, 0, 1.1);
    CHECK(c.ntx == 0);                                               /* listens first */
    run(&z, 1.2, 2.0);
    CHECK(c.ntx >= 5 && c.tx[0].type == 0x46 && c.tx[1].type == 0x32 && c.tx[2].type == 0x02 && c.tx[3].type == 0x04 &&
          c.tx[4].type == 0xE4 && c.tx[4].pay[0] == 0 && c.tx[4].pay[1] == 6 && c.tx[4].pay[2] == 1 && c.tx[4].pay[3] == 1);
    CHECK(c.tx[1].seq == c.tx[0].seq + 1);                           /* own sequence increments */
    /* an unasked or wrong-function ack does not advance */
    uint8_t wrong[4] = { 0, 7, 0, 0 }, ok6[4] = { 0, 6, 0, 0 };
    dev(&z, &c, c.tx[4].seq, IPP_FUNC_ENABLE_REPLY, wrong, 4, 2.0);
    dev(&z, &c, 99, IPP_FUNC_ENABLE_REPLY, ok6, 4, 2.0);
    CHECK(z.fn_idx == 0);
    /* a rejection retries after 5 s */
    uint8_t rej[4] = { 0, 6, 0x12, 0x34 };
    const Tx *e = tx_last(&c, 0xE4);
    dev(&z, &c, e->seq, IPP_FUNC_ENABLE_REPLY, rej, 4, 2.05);
    CHECK(z.fn_rejects == 1 && strstr(c.out, "\"function_rejected\"") && strstr(c.out, "\"status\":\"1234\""));
    double t = setup_ready(&z, &c, 2.1);
    CHECK(z.stage == PM3_ST_READY && t > 7.0);
    CHECK(strstr(c.out, "\"event\":\"ready\"") && count_str(c.out, "\"function_enabled\"") == 3);
    int e4 = 0;
    for (int i = 0; i < c.ntx; i++) if (c.tx[i].type == 0xE4) e4 |= 1 << (c.tx[i].pay[1] & 15);
    CHECK(e4 == ((1 << 6) | (1 << 7) | (1 << 10)));
    /* status polling continues after ready and is correlated */
    int before = tx_count(&c, 0x04);
    run(&z, t, t + 5.1);
    CHECK(tx_count(&c, 0x04) == before + 1);
    const Tx *s = tx_last(&c, 0x04);
    uint8_t st[2] = { 0, 0 };
    dev(&z, &c, s->seq, IPP_STATUS_REPLY, st, 2, t + 5.2);
    CHECK(z.status_ok == 1 && strstr(c.out, "\"event\":\"status\",\"code\":\"0000\""));
    dev(&z, &c, s->seq, IPP_STATUS_REPLY, st, 2, t + 5.3);         /* answered twice: second is uncorrelated */
    CHECK(z.status_ok == 1 && z.uncorrelated >= 1);
    uint8_t st3[3] = { 0, 0, 0 };
    dev(&z, &c, s->seq, IPP_STATUS_REPLY, st3, 3, t + 5.3);
    CHECK(z.status_malformed == 1);
    /* the transmit allowlist */
    int sent = c.ntx;
    CHECK(!pm3_nfc_send(&z, 0x10, 0, NULL, 0));                      /* reset */
    CHECK(!pm3_nfc_send(&z, 0xB4, 0, st, 2));                        /* APDU */
    CHECK(!pm3_nfc_send(&z, 0xD0, 0, NULL, 0));                      /* EMV */
    CHECK(!pm3_nfc_send(&z, 0x20, 0, st, 2));                        /* LED */
    CHECK(c.ntx == sent && z.tx_refused == 4);
}

static void test_status_missing_and_link(void)
{
    Cap c; memset(&c, 0, sizeof c); ipp_rx_init(&c.txrx);
    pm3_nfc z;
    pm3_nfc_init(&z, NULL, cap_send, cap_emit, &c, 0);
    double t = setup_ready(&z, &c, 0);
    for (int i = 0; i < 600; i++) {                                  /* heartbeats only, status never answered */
        double u = t + i * 0.05;
        pm3_nfc_tick(&z, u);
        if (i % 40 == 0) dev(&z, &c, 0, IPP_HEARTBEAT, NULL, 0, u);
    }
    CHECK(z.status_missing >= 3 && count_str(c.out, "\"event\":\"status_missing\"") == 1);
    CHECK(!strstr(c.out, "\"state\":\"silent\""));
    const Tx *s = tx_last(&c, 0x04);
    uint8_t st[2] = { 0x01, 0x02 };
    dev(&z, &c, s->seq, IPP_STATUS_REPLY, st, 2, t + 30);
    CHECK(strstr(c.out, "\"status_restored\"") && strstr(c.out, "\"code\":\"0102\""));
    run(&z, t + 30, t + 41);                                         /* nothing at all for 11 s */
    CHECK(strstr(c.out, "\"link\",\"state\":\"silent\""));
    dev(&z, &c, 0, IPP_HEARTBEAT, NULL, 0, t + 41.5);
    CHECK(strstr(c.out, "\"link\",\"state\":\"alive\""));
}

static void test_card_presence(void)
{
    Cap c; memset(&c, 0, sizeof c); ipp_rx_init(&c.txrx);
    pm3_nfc z;
    pm3_nfc_init(&z, NULL, cap_send, cap_emit, &c, 0);
    double t = setup_ready(&z, &c, 0) + 5;
    run(&z, t - 5, t);
    /* payload that looks like a UID: it must never appear in the output */
    uint8_t card[12] = { 0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x13, 0x37, 0x42, 0x00, 0x00, 0x00 };
    int rel0 = tx_count(&c, 0x32);
    dev(&z, &c, 0, IPP_ISO_READ, card, sizeof card, t);
    uint8_t card_seq = c.dev_seq;
    CHECK(strstr(c.out, "\"kind\":\"card\",\"frame\":\"iso-read\",\"type\":177,\"outcome\":\"accepted\",\"since_last_s\":null"));
    CHECK(tx_count(&c, 0x32) == rel0 + 1 && tx_last(&c, 0x32)->reply_to == card_seq);
    for (int i = 1; i <= 10; i++) { pm3_nfc_tick(&z, t + i * 0.2); dev(&z, &c, 0, IPP_ISO_READ, card, sizeof card, t + i * 0.2); }
    CHECK(z.held == 10 && tx_count(&c, 0x32) == rel0 + 1 && count_str(c.out, "\"presence\"") == 1);
    CHECK(!strstr(c.out, "dead") && !strstr(c.out, "DEAD") && !strstr(c.out, "beef") && !strstr(c.out, "1337"));
    int ab = tx_count(&c, 0x46);
    run(&z, t + 2.0, t + 5.2);                                        /* 3 s quiet: absent, then one rearm abort */
    CHECK(strstr(c.out, "\"absent\",\"kind\":\"card\",\"reason\":\"timeout\""));
    CHECK(tx_count(&c, 0x46) == ab + 1 && strstr(c.out, "\"rearm_abort\""));
    run(&z, t + 5.2, t + 9);
    CHECK(tx_count(&c, 0x46) == ab + 1);                              /* once per episode */

    /* DESFire with removal: absence confirmed 0.3 s after the removal frame */
    t += 10;
    run(&z, t - 1, t);
    dev(&z, &c, 0, IPP_DESFIRE_READ, card, sizeof card, t);
    CHECK(count_str(c.out, "\"outcome\":\"accepted\"") == 2);
    dev(&z, &c, 0, IPP_DESFIRE_REMOVED, NULL, 0, t + 0.5);
    run(&z, t + 0.5, t + 0.7);
    CHECK(!strstr(c.out, "\"reason\":\"removal\""));
    run(&z, t + 0.7, t + 0.85);
    CHECK(strstr(c.out, "\"reason\":\"removal\""));
    /* a new report just after a confirmed removal but within 0.35 s of the last accept is cooldown */
    t += 5;
    run(&z, t - 1, t);
    dev(&z, &c, 0, IPP_DESFIRE_READ, card, sizeof card, t);
    dev(&z, &c, 0, IPP_DESFIRE_REMOVED, NULL, 0, t + 0.01);
    pm3_nfc_tick(&z, t + 0.32);
    dev(&z, &c, 0, IPP_DESFIRE_READ, card, sizeof card, t + 0.33);
    CHECK(z.cooldown == 1 && strstr(c.out, "\"outcome\":\"cooldown\""));
    /* Ultralight field reports: arrival p[0]=0, no release sent */
    t += 10;
    run(&z, t - 5, t);
    int rel = tx_count(&c, 0x32);
    uint8_t ul_in[9] = { 0, 0, 0x04, 1, 2, 3, 4, 5, 6 }, ul_out[1] = { 1 };
    dev(&z, &c, 0, IPP_ULTRALIGHT, ul_in, sizeof ul_in, t);
    CHECK(strstr(c.out, "\"frame\":\"ultralight\"") && tx_count(&c, 0x32) == rel);
    dev(&z, &c, 0, IPP_ULTRALIGHT, ul_out, 1, t + 0.2);
    run(&z, t + 0.2, t + 0.6);
    CHECK(count_str(c.out, "\"reason\":\"removal\"") >= 2);
    /* --no-release */
    Cap c2; memset(&c2, 0, sizeof c2); ipp_rx_init(&c2.txrx);
    pm3_nfc_cfg cfg; pm3_nfc_cfg_defaults(&cfg); cfg.release_cards = 0; cfg.rearm_abort = 0;
    pm3_nfc_init(&z, &cfg, cap_send, cap_emit, &c2, 0);
    t = setup_ready(&z, &c2, 0) + 1;
    int r0 = tx_count(&c2, 0x32), a0 = tx_count(&c2, 0x46);
    dev(&z, &c2, 0, IPP_ISO_READ, card, sizeof card, t);
    run(&z, t, t + 6);
    CHECK(tx_count(&c2, 0x32) == r0 && tx_count(&c2, 0x46) == a0 && strstr(c2.out, "\"accepted\""));
}

static void test_phone_presence(void)
{
    Cap c; memset(&c, 0, sizeof c); ipp_rx_init(&c.txrx);
    pm3_nfc z;
    pm3_nfc_init(&z, NULL, cap_send, cap_emit, &c, 0);
    uint8_t d1[63];
    memset(d1, 0xA5, sizeof d1);
    dev(&z, &c, 0, IPP_EMV_STATUS, d1, 63, 0.5);                     /* during setup */
    CHECK(strstr(c.out, "\"phone_ignored\",\"reason\":\"setup\""));
    double t = setup_ready(&z, &c, 0.6);
    dev(&z, &c, 0, IPP_EMV_STATUS, d1, 63, t + 1);                   /* startup grace */
    CHECK(strstr(c.out, "\"reason\":\"startup-grace\""));
    t += 10;
    run(&z, t - 6, t);
    dev(&z, &c, 0, IPP_EMV_STATUS, d1, 63, t);
    CHECK(strstr(c.out, "\"kind\":\"phone_or_payment\",\"frame\":\"emv-status\",\"type\":209,\"outcome\":\"accepted\""));
    for (int i = 1; i <= 5; i++) dev(&z, &c, 0, IPP_EMV_STATUS, d1, 63, t + i * 0.1);   /* held phone: one burst */
    CHECK(z.accepted[PM3_KIND_PHONE] == 1 && !strstr(c.out, "\"reason\":\"burst\""));
    dev(&z, &c, 0, IPP_EMV_STATUS, d1, 63, t + 1.2);                 /* 0.7 s quiet: a new presence */
    CHECK(z.accepted[PM3_KIND_PHONE] == 2);
    /* wrong shapes are never presence */
    dev(&z, &c, 0, IPP_EMV_STATUS, d1, 62, t + 3);
    dev(&z, &c, 5, IPP_EMV_STATUS, d1, 63, t + 3.5);
    CHECK(z.accepted[PM3_KIND_PHONE] == 2);
    /* a 0xD1 right after a card report belongs to that card */
    t += 10;
    run(&z, t - 5, t);
    uint8_t card[10] = { 0 };
    dev(&z, &c, 0, IPP_ISO_READ, card, sizeof card, t);
    dev(&z, &c, 0, IPP_EMV_STATUS, d1, 63, t + 0.08);
    CHECK(strstr(c.out, "\"reason\":\"recent-card\"") && z.accepted[PM3_KIND_PHONE] == 2);
    CHECK(!strstr(c.out, "a5") && !strstr(c.out, "A5"));
    /* disabled */
    Cap c2; memset(&c2, 0, sizeof c2); ipp_rx_init(&c2.txrx);
    pm3_nfc_cfg cfg; pm3_nfc_cfg_defaults(&cfg); cfg.phone_presence = 0;
    pm3_nfc_init(&z, &cfg, cap_send, cap_emit, &c2, 0);
    t = setup_ready(&z, &c2, 0) + 10;
    run(&z, t - 10, t);
    dev(&z, &c2, 0, IPP_EMV_STATUS, d1, 63, t);
    CHECK(z.accepted[PM3_KIND_PHONE] == 0 && !strstr(c2.out, "phone"));
}

static void test_module_restart_and_reader_message(void)
{
    Cap c; memset(&c, 0, sizeof c); ipp_rx_init(&c.txrx);
    pm3_nfc z;
    pm3_nfc_init(&z, NULL, cap_send, cap_emit, &c, 0);
    double t = setup_ready(&z, &c, 0) + 2;
    run(&z, t - 2, t);
    uint8_t card[10] = { 0 };
    dev(&z, &c, 0, IPP_ISO_READ, card, sizeof card, t);
    dev(&z, &c, 0, IPP_STARTUP, NULL, 0, t + 0.5);
    CHECK(strstr(c.out, "\"module_startup\"") && strstr(c.out, "\"reason\":\"module-restart\"") && z.stage == PM3_ST_LISTEN);
    t = setup_ready(&z, &c, t + 0.5);
    CHECK(strstr(c.out, "\"resync\":true") && z.setups == 2);
    dev(&z, &c, 0, IPP_ISO_READ, card, sizeof card, t + 0.5);        /* a held card re-reported after restart */
    CHECK(strstr(c.out, "\"outcome\":\"resync\"") && z.accepted[PM3_KIND_CARD] == 1);
    /* reader log text is never output */
    char msg[40] = "\x02" "SECRET-PAN 4111111111111111 x";
    dev(&z, &c, 0, IPP_READER_MESSAGE, (const uint8_t *)msg, strlen(msg), t + 6);
    CHECK(strstr(c.out, "\"reader_message\",\"level\":2,\"len\":30,\"format\":\"text\",\"text\":\"omitted\""));
    CHECK(!strstr(c.out, "SECRET") && !strstr(c.out, "4111"));
    uint8_t bin[4] = { 1, 0xFF, 0x00, 0x01 };
    dev(&z, &c, 0, IPP_READER_MESSAGE, bin, 4, t + 6.1);
    CHECK(strstr(c.out, "\"format\":\"binary\""));
    pm3_nfc_health(&z, t + 7);
    CHECK(strstr(c.out, "\"health\",\"link\":\"alive\",\"configured\":true") && strstr(c.out, "\"reader_messages\":2"));
    /* frame metadata mode reports type/len only */
    Cap c2; memset(&c2, 0, sizeof c2); ipp_rx_init(&c2.txrx);
    pm3_nfc_cfg cfg; pm3_nfc_cfg_defaults(&cfg); cfg.frames = 1;
    pm3_nfc_init(&z, &cfg, cap_send, cap_emit, &c2, 0);
    uint8_t pay[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
    dev(&z, &c2, 0, 0x99, pay, 5, 0.1);
    CHECK(strstr(c2.out, "\"frame\",\"type\":153,\"name\":\"unknown\",\"len\":5") && !strstr(c2.out, "2233"));
    /* release on exit: abort then release */
    int n = c2.ntx;
    pm3_nfc_release(&z, 0, 1); pm3_nfc_release(&z, 1, 1.2);
    CHECK(c2.ntx == n + 2 && c2.tx[n].type == 0x46 && c2.tx[n + 1].type == 0x32);
}

static void test_pic32_parse(void)
{
    int v = -1;
    CHECK(pm3_pic32_parse("PIC32: com/usb/available 8/0/1\nAddr 20491:\t 48\n", 0x500b, &v) && v == 48);
    CHECK(pm3_pic32_parse("Addr 20491: 0\r\n", 0x500b, &v) && v == 0);
    CHECK(pm3_pic32_parse("Addr 20491: 3273", 0x500b, &v) && v == 3273);
    CHECK(pm3_pic32_parse("Addr 20544: 12087\n", 0x5040, &v) && v == 12087);
    CHECK(!pm3_pic32_parse("Addr 20544: 12087\n", 0x500b, &v));      /* another register's line */
    CHECK(!pm3_pic32_parse("Addr 20491: -1\n", 0x500b, &v));
    CHECK(!pm3_pic32_parse("Addr 20491: 65536\n", 0x500b, &v));
    CHECK(!pm3_pic32_parse("Addr 20491: 48oops\n", 0x500b, &v));
    CHECK(!pm3_pic32_parse("Addr 20491: error\n", 0x500b, &v));
    CHECK(!pm3_pic32_parse("XAddr 20491: 5\n", 0x500b, &v));
    CHECK(!pm3_pic32_parse("", 0x500b, &v));
    CHECK(pm3_pic32_find("ambient") && pm3_pic32_find("ambient")->addr == 0x500b);
    CHECK(pm3_pic32_find("input-voltage") && !pm3_pic32_find("backlight"));
}

static void test_scanner(void)
{
    char b[32];
    CHECK(pm3_scanner_level_cmd(b, sizeof b, 0) == 10 && !strcmp(b, "RDILPMB0\r\n"));
    CHECK(pm3_scanner_level_cmd(b, sizeof b, 100) == 12 && !strcmp(b, "RDILPMB100\r\n"));
    CHECK(pm3_scanner_level_cmd(b, sizeof b, 101) == -1 && pm3_scanner_level_cmd(b, sizeof b, -1) == -1);
    CHECK(pm3_scanner_level_cmd(b, 5, 0) == -1);
    int v = -1;
    const char real[] = "<EN><IM ET=\"3\" JQ=\"35\" JS=\"30\" /></EN><RD><IL LO=\"0\" MB=\"25\" AC=\"0\" /></RD>";
    CHECK(pm3_scanner_parse_level(real, sizeof real - 1, &v) && v == 25);
    const char zero[] = "\x02\x06<RD><IL LO=\"0\" MB=\"0\" AC=\"0\" /></RD>\r\n";
    CHECK(pm3_scanner_parse_level(zero, sizeof zero - 1, &v) && v == 0);
    CHECK(pm3_scanner_parse_level("TE0 MB40 RO0", 12, &v) && v == 40);
    CHECK(!pm3_scanner_parse_level("XMB=\"5\"", 7, &v));
    CHECK(!pm3_scanner_parse_level("MB=\"5", 5, &v));
    CHECK(!pm3_scanner_parse_level("MB101", 5, &v));
    CHECK(!pm3_scanner_parse_level("MB5X", 4, &v));
    CHECK(!pm3_scanner_parse_level("MB", 2, &v));
    CHECK(!pm3_scanner_parse_level("<RD><IL LO=\"0\" />", 17, &v));
}

static void write_file(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(s, f); fclose(f); }
}

static void test_identity_checks(void)
{
    char root[] = "/tmp/pm3-test-XXXXXX";
    if (!mkdtemp(root)) { CHECK(0); return; }
    char p[512], q[512];
    /* fake sysfs: class/tty/ttyFAKE0/device -> devices/usb1/1-1/1-1:1.0/ttyFAKE0, ids at 1-1 */
    SP(p, "%s/sys/devices/usb1/1-1/1-1:1.0/ttyFAKE0", root);
    SP(q, "mkdir -p '%s' '%s/sys/class/tty/ttyFAKE0' '%s/dev'", p, root, root);
    CHECK(system(q) == 0);
    SP(q, "%s/sys/class/tty/ttyFAKE0/device", root);
    CHECK(symlink(p, q) == 0);
    SP(p, "%s/sys/devices/usb1/1-1/idVendor", root); write_file(p, "0403\n");
    SP(p, "%s/sys/devices/usb1/1-1/idProduct", root); write_file(p, "6015\n");
    SP(p, "%s/dev/ttyFAKE0", root); write_file(p, "");
    SP(q, "%s/sys", root);
    unsigned vid = 0, pid = 0;
    CHECK(pm3_usb_id(p, q, &vid, &pid) == 1 && vid == 0x0403 && pid == 0x6015);
    SP(p, "%s/dev/ttyOTHER", root); write_file(p, "");
    CHECK(pm3_usb_id(p, q, &vid, &pid) == 0);                         /* no sysfs entry: not USB */
    CHECK(pm3_usb_id("/nonexistent/tty", q, &vid, &pid) == -1);

    /* fake /proc: pid 4242 holds /dev/null open */
    struct stat st;
    if (stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode)) {
        SP(p, "%s/proc/4242/fd", root);
        SP(q, "mkdir -p '%s' '%s/proc/77/fd' '%s/proc/self-not-a-pid'", p, root, root);
        CHECK(system(q) == 0);
        SP(q, "%s/3", p); CHECK(symlink("/dev/null", q) == 0);
        SP(q, "%s/proc/77/fd/0", root); CHECK(symlink("/dev/zero", q) == 0);
        SP(q, "%s/proc/4242/comm", root); write_file(q, "reader-app\n");
        SP(q, "%s/proc", root);
        pid_t who = 0;
        char comm[32] = "";
        CHECK(pm3_port_users("/dev/null", q, &who, comm, sizeof comm) == 1 && who == 4242 && !strcmp(comm, "reader-app"));
        CHECK(pm3_port_users("/dev/full", q, &who, comm, sizeof comm) == 0);
        SP(p, "%s/dev/ttyFAKE0", root);
        CHECK(pm3_port_users(p, q, &who, comm, sizeof comm) == -1);   /* not a character device */
    }
    SP(q, "rm -rf '%s'", root);
    CHECK(system(q) == 0);
}

int main(void)
{
    test_crc_and_framing();
    test_setup_sequence();
    test_status_missing_and_link();
    test_card_presence();
    test_phone_presence();
    test_module_restart_and_reader_message();
    test_pic32_parse();
    test_scanner();
    test_identity_checks();
    printf("test_pm3: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
