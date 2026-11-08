/* pm3-nfc-presence: anonymous NFC presence monitor for the PROXmobil3 reader module.
 *
 * Opens the reader UART (default /dev/ttymxc3, 115200 8N1), runs the reader setup and
 * prints one JSON object per line on stdout: setup progress, card and phone/payment
 * presence, absence, status, reader-message metadata and periodic health.  No card
 * data is ever printed.  Human-readable diagnostics go to stderr.
 *
 * It refuses a port that another process already has open (normally the application
 * that owns the reader): stop that application first.  See docs/software/pm3-nfc-presence.md.
 *
 * Exit: 0 normal end (--duration or signal), 2 usage, 3 port busy or unusable, 4 I/O error. */
#define _GNU_SOURCE
#include "pm3_nfc.h"
#include "pm3_serial.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define TOOL_VERSION "0.1.0"

static volatile sig_atomic_t g_stop;
static void on_signal(int s) { (void)s; g_stop = 1; }

typedef struct { int fd; int failed; } Io;

static void io_send(void *ctx, const uint8_t *b, size_t n)
{
    Io *io = ctx;
    if (io->fd >= 0 && pm3_write_all(io->fd, b, n, 500) != 0) io->failed = 1;
}

static void io_emit(void *ctx, const char *line)
{
    (void)ctx;
    fputs(line, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static int parse_funcs(const char *s, pm3_nfc_cfg *c)
{
    c->nfuncs = 0;
    while (*s) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s || v < 1 || v > 255 || c->nfuncs >= PM3_NFC_MAX_FUNCS) return -1;
        c->funcs[c->nfuncs++] = (uint8_t)v;
        s = end;
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return c->nfuncs ? 0 : -1;
}

static int usage(FILE *o)
{
    fputs("usage: pm3-nfc-presence [options]\n"
          "  --device PATH        reader UART (default /dev/ttymxc3)\n"
          "  --funcs LIST         card functions to enable, comma separated (default 6,7,10: ISO, DESFire, Ultralight)\n"
          "  --no-phone           do not report 63-byte 0xD1 frames as phone/payment presence\n"
          "  --no-release         never send card release (0x32) after a card report\n"
          "  --no-rearm-abort     never send abort (0x46) after a card presence ends\n"
          "  --frames             one metadata line per received frame (type, length, seq; never payload)\n"
          "  --health-every S     health line interval (default 30, 0 = only at exit)\n"
          "  --duration S         exit after S seconds (default: run until SIGINT/SIGTERM)\n"
          "  --force-shared       open even if another process has the port open (NOT recommended)\n"
          "  --version\n", o);
    return o == stdout ? 0 : 2;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/ttymxc3";
    double duration = 0;
    int force = 0;
    pm3_nfc_cfg cfg;
    pm3_nfc_cfg_defaults(&cfg);
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(o, "--device") && more) dev = argv[++i];
        else if (!strcmp(o, "--funcs") && more) { if (parse_funcs(argv[++i], &cfg)) return usage(stderr); }
        else if (!strcmp(o, "--no-phone")) cfg.phone_presence = 0;
        else if (!strcmp(o, "--no-release")) cfg.release_cards = 0;
        else if (!strcmp(o, "--no-rearm-abort")) cfg.rearm_abort = 0;
        else if (!strcmp(o, "--frames")) cfg.frames = 1;
        else if (!strcmp(o, "--health-every") && more) { cfg.health_every = atof(argv[++i]); if (cfg.health_every < 0) return usage(stderr); }
        else if (!strcmp(o, "--duration") && more) { duration = atof(argv[++i]); if (duration <= 0) return usage(stderr); }
        else if (!strcmp(o, "--force-shared")) force = 1;
        else if (!strcmp(o, "--version")) { puts("pm3-nfc-presence " TOOL_VERSION); return 0; }
        else if (!strcmp(o, "--help") || !strcmp(o, "-h")) return usage(stdout);
        else return usage(stderr);
    }

    pid_t pid = 0;
    char comm[64] = "";
    int users = pm3_port_users(dev, "/proc", &pid, comm, sizeof comm);
    if (users < 0) { fprintf(stderr, "pm3-nfc-presence: cannot examine %s (missing or not a character device)\n", dev); return 3; }
    if (users > 0 && !force) {
        fprintf(stderr, "pm3-nfc-presence: %s is already open by pid %ld (%s)%s; stop that program first\n",
                dev, (long)pid, comm[0] ? comm : "?", users > 1 ? " and others" : "");
        return 3;
    }
    char err[160];
    Io io = { -1, 0 };
    io.fd = pm3_serial_open(dev, 115200, err, sizeof err);
    if (io.fd < 0) { fprintf(stderr, "pm3-nfc-presence: %s: %s\n", dev, err); return 3; }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    pm3_nfc z;
    double t0 = pm3_now();
    {
        char line[400], funcs[32] = "";
        time_t wall = time(NULL);
        struct tm tm;
        char iso[32];
        gmtime_r(&wall, &tm);
        strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm);
        for (int k = 0; k < cfg.nfuncs; k++)
            snprintf(funcs + strlen(funcs), sizeof funcs - strlen(funcs), "%s%u", k ? "," : "", cfg.funcs[k]);
        snprintf(line, sizeof line,
                 "{\"t\":0.000,\"event\":\"start\",\"tool\":\"pm3-nfc-presence\",\"version\":\"%s\",\"utc\":\"%s\","
                 "\"funcs\":[%s],\"phone_presence\":%s,\"release_cards\":%s,\"rearm_abort\":%s,\"shared\":%s}",
                 TOOL_VERSION, iso, funcs, cfg.phone_presence ? "true" : "false", cfg.release_cards ? "true" : "false",
                 cfg.rearm_abort ? "true" : "false", users > 0 ? "true" : "false");
        io_emit(NULL, line);
    }
    pm3_nfc_init(&z, &cfg, io_send, io_emit, &io, t0);

    int rc = 0;
    tcflush(io.fd, TCIFLUSH);
    while (!g_stop) {
        double now = pm3_now();
        if (duration > 0 && now - t0 >= duration) break;
        struct pollfd p = { io.fd, POLLIN, 0 };
        int r = poll(&p, 1, 20);
        now = pm3_now();
        if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(p.revents & POLLIN)) {
            fprintf(stderr, "pm3-nfc-presence: port error\n");
            rc = 4;
            break;
        }
        if (r > 0 && (p.revents & POLLIN)) {
            uint8_t buf[512];
            ssize_t n = read(io.fd, buf, sizeof buf);
            if (n > 0) pm3_nfc_bytes(&z, buf, (size_t)n, now);
            else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                fprintf(stderr, "pm3-nfc-presence: read failed: %s\n", n == 0 ? "end of file" : strerror(errno));
                rc = 4;
                break;
            }
        }
        pm3_nfc_tick(&z, now);
        if (io.failed) { fprintf(stderr, "pm3-nfc-presence: write failed\n"); rc = 4; break; }
    }

    /* Leave the reader as the stock start sequence expects to find it. */
    if (!io.failed) {
        pm3_nfc_release(&z, 0, pm3_now());
        usleep(200000);
        pm3_nfc_release(&z, 1, pm3_now());
        tcdrain(io.fd);
    }
    pm3_nfc_health(&z, pm3_now());
    {
        char line[128];
        snprintf(line, sizeof line, "{\"t\":%.3f,\"event\":\"exit\",\"code\":%d}", pm3_now() - t0, rc);
        io_emit(NULL, line);
    }
    close(io.fd);
    return rc;
}
