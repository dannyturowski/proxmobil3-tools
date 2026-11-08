/* wt32-r5.1 test device for tests/test_wifi_provision.py: the real link core (nori_dev.c) and the real saved-network code
 * (shared/nori_wifi) behind a pty, with a RAM store.  Only the HAL is new.
 *   wn_dev <pty path> <secret> <control fifo in> <control fifo out>
 * The control pair is the HTTP side, driven by the Python fake server: "N" -> nonce, "P <hex body>" -> "<status> <json>",
 * "A <index|-1>" sets the network in use, "D" dumps the table in hex (test values only, never in a real firmware). */
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "nori_dev.h"
#include "nori_wnet.h"
#include "nori_wnprov.h"

static int fd = -1;
static nd_dev dev;
static nw_table wn;
static nwp_ctx P;
static int active = -1;
static char secret[40];

typedef struct { char key[16]; uint8_t v[1024]; int n, used; } item;
static item kv[8];
static item *kfind(const char *k) { for (int i = 0; i < 8; i++) if (kv[i].used && !strcmp(kv[i].key, k)) return &kv[i]; return NULL; }
static void kset(const char *k, const void *b, int n)
{
    item *i = kfind(k);
    for (int j = 0; j < 8 && !i; j++) if (!kv[j].used) i = &kv[j];
    snprintf(i->key, sizeof i->key, "%s", k);
    memcpy(i->v, b, (size_t)n);
    i->n = n;
    i->used = 1;
}
static int g_blob(void *c, const char *k, uint8_t *b, size_t cap) { (void)c; item *i = kfind(k); if (!i) return -1; if ((size_t)i->n > cap) return -2; memcpy(b, i->v, (size_t)i->n); return i->n; }
static int p_blob(void *c, const char *k, const uint8_t *b, size_t n) { (void)c; kset(k, b, (int)n); return 0; }
static int g_str(void *c, const char *k, char *o, size_t cap) { (void)c; item *i = kfind(k); if (!i) return -1; if ((size_t)i->n >= cap) return -2; memcpy(o, i->v, (size_t)i->n); o[i->n] = 0; return i->n; }
static int p_str(void *c, const char *k, const char *s) { (void)c; kset(k, s, (int)strlen(s)); return 0; }
static int k_del(void *c, const char *k) { (void)c; item *i = kfind(k); if (i) i->used = 0; return 0; }
static const nw_kv KV = { NULL, g_blob, p_blob, g_str, p_str, k_del };

static uint32_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}
static const char *key_cb(void *c) { (void)c; return secret; }
static void rnd_cb(void *c, uint8_t *b, size_t n) { (void)c; FILE *f = fopen("/dev/urandom", "r"); if (!f || fread(b, 1, n, f) != n) exit(3); fclose(f); }
static int act_cb(void *c) { (void)c; return active; }

static size_t u_room(void *c) { (void)c; return 4096; }
static size_t u_queued(void *c) { (void)c; return 0; }
static void u_write(void *c, const char *s, size_t n) { (void)c; if (write(fd, s, n) < 0) exit(0); }
static int w_status(void *c, char *ip, size_t cap, int *rssi) { (void)c; (void)ip; (void)cap; (void)rssi; return wn.n ? ND_WIFI_JOINING : ND_WIFI_NOCFG; }
static int w_set(void *c, const char *s, const char *p) { (void)c; int r = nwp_wifiset(&P, s, p); return r == NW_OK ? 0 : r == NW_E_FULL ? -2 : -1; }
static int w_clear(void *c) { (void)c; return nwp_clear(&P) == NW_OK ? 0 : -1; }
static void w_power(void *c, int on) { (void)c; (void)on; }
static int cfg_save(void *c, const nd_cfg *k) { (void)c; (void)k; return 0; }
static void diag(void *c, char *o, size_t cap) { (void)c; snprintf(o, cap, "wn=%d/%u", active, wn.n); }
static int page(void *c, const char *pg, char *o, size_t cap)
{
    (void)c;
    if (strcmp(pg, "wifi")) return -1;
    snprintf(o, cap, "wn=%d/%u gen=%lu", active, wn.n, (unsigned long)wn.gen);
    return 0;
}
static int ext(void *c, const nl_frame *f, char *r, size_t cap) { (void)c; return nwp_cmd(&P, f, r, cap, now_ms()); }
static int okey(void *c, const char *h) { (void)c; memcpy(secret, h, 33); return 0; }
static const nd_hal HAL = { NULL, u_room, u_queued, u_write, w_status, w_set, w_clear, w_power, NULL, NULL, NULL, NULL, NULL,
                            cfg_save, diag, okey, page, ext };

static void hexout(FILE *o, const char *b, size_t n) { for (size_t i = 0; i < n; i++) fprintf(o, "%02x", (uint8_t)b[i]); fputc('-', o); }

static void control(char *line, FILE *out)
{
    static uint8_t body[2048];
    char js[96], h[40];
    if (line[0] == 'N') {
        int r = nwp_http_nonce(&P, h, sizeof h, now_ms());
        fprintf(out, "%s\n", r == NW_OK ? h : "nokey");
    } else if (line[0] == 'P') {
        size_t n = 0;
        for (const char *s = line + 2; s[0] && s[1] && s[0] != '\n' && n < sizeof body; s += 2) { unsigned v; sscanf(s, "%2x", &v); body[n++] = (uint8_t)v; }
        int code = nwp_http_post(&P, body, n, now_ms(), js, sizeof js);
        js[strcspn(js, "\n")] = 0;
        fprintf(out, "%d %s\n", code, js);
    } else if (line[0] == 'A') {
        active = atoi(line + 2);
        fprintf(out, "ok\n");
    } else if (line[0] == 'D') {
        fprintf(out, "%u %lu ", wn.n, (unsigned long)wn.gen);
        for (int i = 0; i < wn.n; i++) { hexout(out, wn.net[i].ssid, wn.net[i].ssid_len); hexout(out, wn.net[i].psk, wn.net[i].psk_len); fprintf(out, "%u ", wn.net[i].flags); }
        fputc('\n', out);
    }
    fflush(out);
}

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    fd = open(argv[1], O_RDWR | O_NOCTTY);
    if (fd < 0) return 2;
    struct termios t;
    if (!tcgetattr(fd, &t)) { cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t); }
    snprintf(secret, sizeof secret, "%s", argv[2]);
    int ci = open(argv[3], O_RDONLY | O_NONBLOCK);
    FILE *co = fopen(argv[4], "w");
    if (ci < 0 || !co) return 2;
    int ckeep = open(argv[3], O_WRONLY);          /* so the fifo never reports hang-up between commands */
    (void)ckeep;
    p_str(NULL, NW_K_SSID, "legacy-home");
    p_str(NULL, NW_K_PSK, "legacy-home-password");
    nw_load(&wn, &KV);
    P.t = &wn; P.kv = &KV; P.key = key_cb; P.rnd = rnd_cb; P.active = act_cb;
    nd_cfg cfg;
    nd_cfg_defaults(&cfg);
    nd_init(&dev, &HAL, 0x1234, "wt32-test", "none", 0, &cfg, now_ms());
    static char cbuf[4096];
    size_t cn = 0;
    for (;;) {
        struct pollfd p[2] = { { fd, POLLIN, 0 }, { ci, POLLIN, 0 } };
        poll(p, 2, 20);
        if (p[0].revents & POLLIN) {
            uint8_t b[512];
            ssize_t n = read(fd, b, sizeof b);
            if (n > 0) nd_uart_input(&dev, b, (size_t)n, now_ms());
            else if (n == 0) return 0;
        }
        if (p[1].revents & POLLIN) {
            ssize_t n = read(ci, cbuf + cn, sizeof cbuf - cn - 1);
            if (n > 0) cn += (size_t)n;
            char *nl;
            while ((nl = memchr(cbuf, '\n', cn))) {
                *nl = 0;
                control(cbuf, co);
                size_t used = (size_t)(nl - cbuf) + 1;
                memmove(cbuf, nl + 1, cn - used);
                cn -= used;
            }
        }
        nd_poll(&dev, now_ms());
    }
}
