/* Feed one link line from stdin to the real device core with a fake OTA HAL and
 * print "<reply> <stored hash>".  Used by test_ota_updater.py to prove the
 * frames tools/nori_ota.py builds are accepted byte-for-byte. */
#include "nori_dev.h"

#include <stdio.h>
#include <string.h>

static char out[1024], stored[40];
static size_t outn;
static size_t room(void *c) { (void)c; return 1024; }
static size_t queued(void *c) { (void)c; return 0; }
static void wr(void *c, const char *s, size_t n) { (void)c; if (outn + n < sizeof out) { memcpy(out + outn, s, n); outn += n; } }
static int key(void *c, const char *h) { (void)c; snprintf(stored, sizeof stored, "%s", h); return 0; }
static const nd_hal HAL = { NULL, room, queued, wr, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, key };

static void reply(const nl_frame *f) { printf("%s %s %s\n", f->type, f->argc ? f->argv[0] : "", stored); }
static void on_frame(void *ctx, const nl_frame *f, const char *raw, size_t n) { (void)ctx; (void)raw; (void)n; if (!strcmp(f->type, "OK") || !strcmp(f->type, "ERR")) reply(f); }

int main(void)
{
    static nd_dev d;
    char line[NL_LINE_MAX + 4];
    if (!fgets(line, sizeof line, stdin)) return 1;
    nd_init(&d, &HAL, 1, "r2.0.0", "none", 0, NULL, 0);
    nd_uart_input(&d, (const uint8_t *)line, strlen(line), 0);
    nd_poll(&d, 0);
    static nl_rx rx;
    nl_rx_init(&rx);
    nl_rx_feed(&rx, (const uint8_t *)out, outn, on_frame, NULL, 0);
    return 0;
}
