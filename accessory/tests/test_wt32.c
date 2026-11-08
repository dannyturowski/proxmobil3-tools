/* wt32-r1: the device core as the WT32 firmware builds it (-DND_DIAG_JOIN=1, no motion options).
 * Fake HAL: network status as main.cpp's w_status reports Ethernet, DIAG extras in main.cpp's
 * token order.  Checks that every reply is a frame nori-linkd's nl_parse accepts (<= 10 args),
 * that the DIAG extras arrive as one ';'-joined token (cut to fit, never past the line limit),
 * that HB carries the Ethernet address, and that OTAKEY reaches the HAL and is never echoed. */
#include "nori_dev.h"

#include <stdio.h>
#include <string.h>

#if !ND_DIAG_JOIN || ND_TAP_AXIS || ND_I2CSCAN || ND_GAME
#error "build with -DND_DIAG_JOIN=1 only (the wt32 firmware's flags)"
#endif

static int checks, fails;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static char uart[65536];
static size_t uart_n;
static int net = ND_WIFI_UP;
static const char *diag_text;
static char key_seen[33];

static size_t h_room(void *c) { (void)c; return sizeof uart - uart_n; }
static size_t h_queued(void *c) { (void)c; return 0; }
static void h_write(void *c, const char *s, size_t n) { (void)c; memcpy(uart + uart_n, s, n); uart_n += n; }
static int h_status(void *c, char *ip, size_t cap, int *rssi)
{
    (void)c;
    if (net == ND_WIFI_UP) { snprintf(ip, cap, "192.168.100.123"); *rssi = 0; }
    return net;
}
static void h_power(void *c, int on) { (void)c; (void)on; }
static void h_diag(void *c, char *out, size_t cap) { (void)c; snprintf(out, cap, "%s", diag_text); }
static int h_key(void *c, const char *h) { (void)c; memcpy(key_seen, h, 33); return 0; }
static const nd_hal HAL = { NULL, h_room, h_queued, h_write, h_status, NULL, NULL, h_power,
                            NULL, NULL, NULL, NULL, NULL, NULL, h_diag, h_key };

static uint32_t hseq;
static void host_send(nd_dev *d, const char *type, const char *args, uint32_t now)
{
    char line[NL_LINE_MAX + 2];
    int n = nl_format(line, sizeof line, ++hseq, type, args);
    check("host frame formats", n > 0);
    if (n > 0) nd_uart_input(d, (const uint8_t *)line, (size_t)n, now);
}

/* Parse everything the device sent since the last call; return the last frame of the given type. */
static int last_frame(const char *type, nl_frame *out, int *bad)
{
    static nl_frame f;                 /* argv points into f.store: never copy a frame, parse into *out */
    char *hit = NULL;
    size_t hit_n = 0;
    *bad = 0;
    char *p = uart, *end = uart + uart_n;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) { (*bad)++; break; }
        if (nl_parse(p, (size_t)(nl - p), &f) != NL_OK) (*bad)++;
        else if (!strcmp(f.type, type)) { hit = p; hit_n = (size_t)(nl - p); }
        p = nl + 1;
    }
    int found = hit && nl_parse(hit, hit_n, out) == NL_OK;
    uart_n = 0;
    return found;
}

int main(void)
{
    static nd_dev d;
    nd_cfg cfg;
    nl_frame f;
    int bad;
    nd_cfg_defaults(&cfg);
    nd_init(&d, &HAL, 0x1234abcdu, "wt32-r1.0.0", "none", 2, &cfg, 0);
    nd_poll(&d, 0);
    check("unsolicited HELLO", last_frame("HELLO", &f, &bad) && !bad);
    check("HELLO fw/sensor/btn/tun", !strcmp(nl_get(&f, "fw"), "wt32-r1.0.0") && !strcmp(nl_get(&f, "sensor"), "none") &&
                                     !strcmp(nl_get(&f, "btn"), "2") && !strcmp(nl_get(&f, "tun"), "0"));
    check("HELLO wifi=up (network up over Ethernet)", !strcmp(nl_get(&f, "wifi"), "up"));

    host_send(&d, "HOST", "proto=1 role=linkd tun=1", 10);
    nd_poll(&d, 10);
    check("HELLO after HOST", last_frame("HELLO", &f, &bad) && !bad);
    nd_poll(&d, 1100);
    check("HB", last_frame("HB", &f, &bad) && !bad);
    check("HB carries the Ethernet address", nl_get(&f, "ip") && !strcmp(nl_get(&f, "ip"), "192.168.100.123") &&
                                             !strcmp(nl_get(&f, "rssi"), "0"));

    /* typical DIAG */
    diag_text = "sensor=none eth=up eip=192.168.100.123 lnk=100F/1 net=eth ota=ready otalast=none http=ready "
                "httplast=none part=app0 img=valid wifi=nocfg btn=00 heap=210k i2c=off";
    host_send(&d, "HHB", NULL, 1200);
    host_send(&d, "DIAG", NULL, 1200);
    nd_poll(&d, 1200);
    check("DIAG reply parses", last_frame("OK", &f, &bad) && !bad);
    check("DIAG reply <= 10 args", f.argc <= NL_ARGS_MAX);
    check("DIAG is 'DIAG' + 5 counters + 1 joined token", f.argc == 7 && !strcmp(f.argv[0], "DIAG"));
    check("joined token complete", f.argc == 7 && !strcmp(f.argv[6], "sensor=none;eth=up;eip=192.168.100.123;lnk=100F/1;net=eth;"
                                                         "ota=ready;otalast=none;http=ready;httplast=none;part=app0;"
                                                         "img=valid;wifi=nocfg;btn=00;heap=210k;i2c=off"));

    /* worst case: Wi-Fi fallback details present, long values */
    diag_text = "sensor=none eth=nolink eip=- lnk=0 net=wifi ota=hold otalast=receive http=nonet httplast=disconnect "
                "part=app1 img=pending wifi=joining ws=6 wr=201 ap=12 sig=-100 auth=7 btn=11 heap=4294967k i2c=off "
                "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx";
    host_send(&d, "DIAG", NULL, 1300);
    nd_poll(&d, 1300);
    check("long DIAG still one valid frame", last_frame("OK", &f, &bad) && !bad && f.argc == 7);
    check("long DIAG keeps the network tokens", f.argc == 7 && !strncmp(f.argv[6], "sensor=none;eth=nolink;eip=-;", 29));

    /* OTAKEY: reaches the HAL, never echoed */
    const char *k = "0123456789abcdef0123456789ABCDEF";
    char a[48];
    snprintf(a, sizeof a, "h=%s", k);
    host_send(&d, "OTAKEY", a, 1400);
    nd_poll(&d, 1400);
    check("OTAKEY stored (lower-cased)", !strcmp(key_seen, "0123456789abcdef0123456789abcdef"));
    check("OK OTAKEY, no hash in the output", last_frame("OK", &f, &bad) && !bad && f.argc == 1 && !strcmp(f.argv[0], "OTAKEY"));

    /* network down: HB says joining, no ip */
    net = ND_WIFI_JOINING;
    host_send(&d, "HHB", NULL, 2300);
    nd_poll(&d, 2300);
    check("HB while Ethernet has no address", last_frame("HB", &f, &bad) && !bad && !strcmp(nl_get(&f, "wifi"), "joining") &&
                                              !nl_get(&f, "ip"));

    printf("test_wt32: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
