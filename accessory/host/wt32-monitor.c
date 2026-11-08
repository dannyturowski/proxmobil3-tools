/* wt32-monitor: generic host-side monitor for the ESP32 accessory link.
 *
 * Opens exactly the configured serial port (path or usb:<bus-port>), after refusing
 * the PM3's NFC reader and barcode scanner, consoles and USB adapters with the wrong
 * identity.  Runs the host side of link protocol 1 (HOST/HELLO handshake, host
 * heartbeat) and prints one JSON object per line on stdout:
 *
 *   {"t":0.512,"event":"hello","boot":"1a2b3c4d","fw":"wt32-r5.1.2","sensor":"vl53l5cx","wifi":"up","tun_cap":false}
 *   {"t":3.004,"event":"input","name":"b1d","device_ms":123456}
 *   {"t":4.100,"event":"reply","status":"OK","what":"DIAG","args":["rx=10","noise=0", ...]}
 *
 * Commands come from --cmd (sent after each HELLO), --diag-every, or one per line on
 * stdin with --stdin.  Only read-only commands (DIAG, IICSCAN) are sent unless
 * --allow-config is given; credential-bearing commands (WIFISET, OTAKEY, WN*) are
 * never sent by this tool.  It does not relay the SSH tunnel and does not drive any
 * application: pipe its output wherever you like.
 *
 * Exit: 0 normal end, 2 usage, 3 port refused/unusable, 4 I/O error, 5 --until-reply timed out. */
#define _DEFAULT_SOURCE
#include "nori_host.h"
#include "nori_hostio.h"
#include "nori_link.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TOOL_VERSION "0.1.0"
#define MAX_CMDS 8

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef struct {
    int fd;
    double t0;
    nh_session s;
    nl_rx rx;
    int heartbeats, allow_config, failed;
    const char *cmds[MAX_CMDS];
    int ncmds, pending_replies, until_reply;
    unsigned hb_n, ev_n, replies;
} Mon;

/* JSON string body: printable ASCII only reaches here (link tokens are 0x21..0x7E). */
static void jstr(const char *s)
{
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') putchar('\\');
        if ((unsigned char)*s >= 0x20 && (unsigned char)*s < 0x7F) putchar(*s);
    }
}

static void line_start(Mon *m, const char *event) { printf("{\"t\":%.3f,\"event\":\"%s\"", now_s() - m->t0, event); }
static void line_end(void) { puts("}"); fflush(stdout); }

static int cmd_allowed(const Mon *m, const char *kind)
{
    if (!strcmp(kind, "DIAG") || !strcmp(kind, "IICSCAN")) return 1;
    if (!strcmp(kind, "WIFISET") || !strcmp(kind, "OTAKEY") || !strncmp(kind, "WN", 2)) return 0;   /* never */
    if (!strcmp(kind, "HOST") || !strcmp(kind, "HHB") || kind[0] == 'T') return 0;                  /* session/tunnel */
    return m->allow_config;
}

static void send_line(Mon *m, const char *line, int n)
{
    if (n <= 0) return;
    for (int off = 0; off < n;) {
        ssize_t w = write(m->fd, line + off, (size_t)(n - off));
        if (w > 0) { off += (int)w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EINTR) { m->failed = 1; return; }
        struct pollfd p = { m->fd, POLLOUT, 0 };
        if (poll(&p, 1, 200) <= 0) { m->failed = 1; return; }
    }
}

/* "KIND [args]" -> one frame.  Returns 0 sent, -1 refused or malformed. */
static int send_cmd(Mon *m, const char *text)
{
    char kind[16], line[NL_LINE_MAX + 2];
    const char *sp = strchr(text, ' ');
    size_t kn = sp ? (size_t)(sp - text) : strlen(text);
    if (kn == 0 || kn >= sizeof kind) goto bad;
    memcpy(kind, text, kn); kind[kn] = 0;
    const char *args = sp ? sp + 1 : NULL;
    if (!cmd_allowed(m, kind)) {
        line_start(m, "command_refused"); printf(",\"what\":\""); jstr(kind); printf("\""); line_end();
        return -1;
    }
    int n = nh_format(&m->s, line, sizeof line, kind, args && *args ? args : NULL);
    if (n < 0) goto bad;
    send_line(m, line, n);
    line_start(m, "command_sent"); printf(",\"what\":\""); jstr(kind); printf("\""); line_end();
    return 0;
bad:
    line_start(m, "command_invalid"); line_end();
    return -1;
}

static void on_frame(void *ctx, const nl_frame *f, const char *raw, size_t rawlen)
{
    Mon *m = ctx;
    nh_event ev;
    int k = nh_frame(&m->s, f, raw, rawlen, now_s(), &ev);
    switch (k) {
    case NH_F_HELLO:
    case NH_F_REBOOT:
        if (!strcmp(f->type, "HB")) { line_start(m, "reboot"); printf(",\"note\":\"heartbeat from a new boot; waiting for HELLO\""); line_end(); break; }
        line_start(m, k == NH_F_REBOOT ? "hello_after_reboot" : "hello");
        printf(",\"boot\":\"%08x\",\"fw\":\"", (unsigned)m->s.boot); jstr(m->s.fw);
        printf("\",\"sensor\":\""); jstr(m->s.sensor);
        printf("\",\"wifi\":\""); jstr(m->s.wifi);
        printf("\",\"tun_cap\":%s", m->s.tun_cap ? "true" : "false");
        line_end();
        for (int i = 0; i < m->ncmds; i++) send_cmd(m, m->cmds[i]);
        break;
    case NH_F_HB:
        m->hb_n++;
        if (m->heartbeats) {
            line_start(m, "heartbeat");
            printf(",\"device_ms\":%u", (unsigned)m->s.dev_ms);
            for (int i = 0; i < f->argc; i++) {
                const char *a = f->argv[i];
                if (!strncmp(a, "wifi=", 5) || !strncmp(a, "ip=", 3) || !strncmp(a, "rssi=", 5) || !strncmp(a, "evdrop=", 7) || !strncmp(a, "tun=", 4)) {
                    const char *eq = strchr(a, '=');
                    printf(",\""); fwrite(a, 1, (size_t)(eq - a), stdout); printf("\":\""); jstr(eq + 1); printf("\"");
                }
            }
            line_end();
        }
        break;
    case NH_F_EVENT:
        m->ev_n++;
        line_start(m, "input");
        printf(",\"name\":\"%s\",\"device_ms\":%u", NL_EV_NAME[ev.kind], (unsigned)ev.t);
        line_end();
        break;
    case NH_F_DROPPED:
        line_start(m, "dropped"); printf(",\"type\":\""); jstr(f->type); printf("\",\"duplicates\":%u,\"stale\":%u", m->s.n_dup, m->s.n_stale); line_end();
        break;
    case NH_F_OTHER:
        if (!strcmp(f->type, "OK") || !strcmp(f->type, "ERR")) {
            m->replies++;
            line_start(m, "reply");
            printf(",\"status\":\"%s\",\"what\":\"", f->type); jstr(f->argc ? f->argv[0] : "");
            printf("\",\"args\":[");
            for (int i = 1; i < f->argc; i++) { printf("%s\"", i > 1 ? "," : ""); jstr(f->argv[i]); printf("\""); }
            printf("]");
            line_end();
            if (m->pending_replies > 0) m->pending_replies--;
        }
        break;
    default: break;
    }
}

static int usage(FILE *o)
{
    fputs("usage: wt32-monitor --device PATH|usb:PORT [options]\n"
          "  --baud N             link speed (default 460800, must match the firmware)\n"
          "  --usb-id VID:PID     required USB identity (default 10c4:ea60, CP2102)\n"
          "  --allow-pty          also accept a pty (simulator tests)\n"
          "  --nfc PATH           device that must never be opened (default /dev/ttymxc3)\n"
          "  --scanner PATH       device that must never be opened (default none)\n"
          "  --cmd \"KIND args\"    send after every HELLO (repeatable, max 8), e.g. --cmd DIAG --cmd \"DIAG p=tof\"\n"
          "  --diag-every S       send DIAG every S seconds while connected\n"
          "  --stdin              also read one command per line from stdin\n"
          "  --allow-config       permit CFG/WIFIEN/WIFICLR/MODE/other non-read-only commands\n"
          "  --heartbeats         print every device heartbeat (default: counted only)\n"
          "  --until-reply        exit after one reply per --cmd (exit 5 if none within --duration or 10 s)\n"
          "  --duration S         exit after S seconds\n"
          "  --version\n", o);
    return o == stdout ? 0 : 2;
}

int main(int argc, char **argv)
{
    Mon m;
    memset(&m, 0, sizeof m);
    const char *spec = NULL;
    int baud = 460800, use_stdin = 0;
    double duration = 0, diag_every = 0;
    nhio_policy pol;
    nhio_policy_defaults(&pol);
    pol.vid = 0x10c4; pol.pid = 0xea60;
    pol.nfc = "/dev/ttymxc3";
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(o, "--device") && more) spec = argv[++i];
        else if (!strcmp(o, "--baud") && more) baud = atoi(argv[++i]);
        else if (!strcmp(o, "--usb-id") && more) { if (sscanf(argv[++i], "%x:%x", &pol.vid, &pol.pid) != 2) return usage(stderr); }
        else if (!strcmp(o, "--allow-pty")) pol.allow_pty = 1;
        else if (!strcmp(o, "--nfc") && more) pol.nfc = argv[++i];
        else if (!strcmp(o, "--scanner") && more) pol.scanner = argv[++i];
        else if (!strcmp(o, "--cmd") && more) { if (m.ncmds >= MAX_CMDS) return usage(stderr); m.cmds[m.ncmds++] = argv[++i]; }
        else if (!strcmp(o, "--diag-every") && more) { diag_every = atof(argv[++i]); if (diag_every < 0.2) return usage(stderr); }
        else if (!strcmp(o, "--stdin")) use_stdin = 1;
        else if (!strcmp(o, "--allow-config")) m.allow_config = 1;
        else if (!strcmp(o, "--heartbeats")) m.heartbeats = 1;
        else if (!strcmp(o, "--until-reply")) m.until_reply = 1;
        else if (!strcmp(o, "--duration") && more) { duration = atof(argv[++i]); if (duration <= 0) return usage(stderr); }
        else if (!strcmp(o, "--version")) { puts("wt32-monitor " TOOL_VERSION); return 0; }
        else if (!strcmp(o, "--help") || !strcmp(o, "-h")) return usage(stdout);
        else return usage(stderr);
    }
    if (!spec || (m.until_reply && !m.ncmds)) return usage(stderr);
    for (int i = 0; i < m.ncmds; i++) {
        char kind[16];
        size_t kn = strcspn(m.cmds[i], " ");
        if (kn == 0 || kn >= sizeof kind) return usage(stderr);
        memcpy(kind, m.cmds[i], kn); kind[kn] = 0;
        if (!cmd_allowed(&m, kind)) { fprintf(stderr, "wt32-monitor: --cmd %s is not allowed (see --allow-config)\n", kind); return 2; }
    }
    if (m.until_reply && duration <= 0) duration = 10;

    char path[256], err[160];
    if (nhio_resolve(spec, &pol, path, sizeof path, err, sizeof err) != 0 || nhio_check(path, &pol, err, sizeof err) != 0) {
        fprintf(stderr, "wt32-monitor: refusing %s: %s\n", spec, err);
        return 3;
    }
    m.fd = nhio_open(path, baud, err, sizeof err);
    if (m.fd < 0) { fprintf(stderr, "wt32-monitor: cannot open %s: %s\n", path, err); return 3; }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    m.t0 = now_s();
    m.pending_replies = m.ncmds;
    nh_open(&m.s, "linkd", 0, m.t0);
    nl_rx_init(&m.rx);
    line_start(&m, "start");
    printf(",\"tool\":\"wt32-monitor\",\"version\":\"%s\",\"baud\":%d", TOOL_VERSION, baud);
    line_end();

    int rc = 0, was_ready = 0;
    double next_diag = 0;
    char inbuf[NL_LINE_MAX + 2];
    size_t inlen = 0;
    while (!g_stop) {
        double now = now_s();
        if (duration > 0 && now - m.t0 >= duration) { if (m.until_reply && m.pending_replies > 0) rc = 5; break; }
        if (m.until_reply && m.pending_replies == 0) break;
        struct pollfd p[2] = { { m.fd, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
        int np = use_stdin ? 2 : 1;
        int r = poll(p, (nfds_t)np, 50);
        now = now_s();
        if (r > 0 && (p[0].revents & POLLIN)) {
            uint8_t b[512];
            ssize_t n = read(m.fd, b, sizeof b);
            if (n > 0) nl_rx_feed(&m.rx, b, (size_t)n, on_frame, &m, 0);
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) { fprintf(stderr, "wt32-monitor: read failed\n"); rc = 4; break; }
        } else if (r > 0 && (p[0].revents & (POLLERR | POLLHUP | POLLNVAL))) { fprintf(stderr, "wt32-monitor: port closed\n"); rc = 4; break; }
        if (use_stdin && r > 0 && (p[1].revents & (POLLIN | POLLHUP))) {
            char c;
            ssize_t n = read(STDIN_FILENO, &c, 1);
            if (n <= 0) use_stdin = 0;
            else if (c == '\n') { inbuf[inlen] = 0; if (inlen) send_cmd(&m, inbuf); inlen = 0; }
            else if (c != '\r' && inlen < sizeof inbuf - 1) inbuf[inlen++] = c;
        }
        char out[NL_LINE_MAX + 2];
        int close_now = 0;
        int n = nh_tick(&m.s, now, out, sizeof out, &close_now);
        if (n > 0) send_line(&m, out, n);
        int ready = m.s.state == NH_READY;
        if (ready != was_ready) {
            line_start(&m, "session"); printf(",\"state\":\"%s\"", ready ? "ready" : "waiting-for-hello"); line_end();
            was_ready = ready;
        }
        if (close_now) {                                 /* silent device: keep the port, start over */
            line_start(&m, "session"); printf(",\"state\":\"silent\""); line_end();
            nh_open(&m.s, "linkd", 0, now);
        }
        if (ready && diag_every > 0 && now >= next_diag) { next_diag = now + diag_every; send_cmd(&m, "DIAG"); }
        if (m.failed) { fprintf(stderr, "wt32-monitor: write failed\n"); rc = 4; break; }
    }
    line_start(&m, "exit");
    printf(",\"code\":%d,\"heartbeats\":%u,\"inputs\":%u,\"replies\":%u,\"frames\":%u,\"noise\":%u,\"bad_crc\":%u,\"too_long\":%u,"
           "\"bad_syntax\":%u,\"duplicates\":%u,\"stale\":%u,\"reboots\":%u",
           rc, m.hb_n, m.ev_n, m.replies, (unsigned)m.rx.frames, (unsigned)m.rx.noise, (unsigned)m.rx.bad_crc,
           (unsigned)m.rx.too_long, (unsigned)m.rx.bad_syntax, m.s.n_dup, m.s.n_stale, m.s.n_reboot);
    line_end();
    close(m.fd);
    return rc;
}
