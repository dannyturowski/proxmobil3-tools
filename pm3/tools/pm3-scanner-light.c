/* pm3-scanner-light: switch the barcode scanner's illumination LEDs off (or set a
 * runtime level) and read the level back.  Code CR8200-family engines only.
 *
 *   pm3-scanner-light --device /dev/serial/by-id/usb-FTDI_..._<SERIAL>-if00-port0 off
 *   pm3-scanner-light --device ... level 25
 *   pm3-scanner-light --device ... get
 *
 * The setting is a runtime one: re-apply it at boot, before the program that reads
 * barcodes opens the port.  The tool refuses a port another process has open, and a
 * USB serial device whose identity is not the expected FTDI bridge (so it can never
 * talk to an ESP32 accessory adapter by mistake).  One JSON line on stdout.
 *
 * Exit: 0 done (and, with readback, confirmed), 1 no or wrong readback, 2 usage,
 * 3 port refused or unusable, 4 I/O error.  See docs/software/pm3-scanner-light.md. */
#define _GNU_SOURCE
#include "pm3_scanner.h"
#include "pm3_serial.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define TOOL_VERSION "0.1.0"

static int usage(FILE *o)
{
    fputs("usage: pm3-scanner-light --device PATH [options] off | level N | get\n"
          "  --device PATH      scanner serial port; prefer the /dev/serial/by-id/ name\n"
          "  --usb-id VID:PID   required USB identity (default 0403:6015, FTDI FT231X)\n"
          "  --any-usb-id       skip the USB identity check (pty tests, other bridges)\n"
          "  --no-readback      do not query the level after setting it\n"
          "  --wait S           readback deadline (default 2)\n"
          "  --sysfs DIR        sysfs root for the identity check (default /sys)\n"
          "  --force-shared     open even if another process has the port open (NOT recommended)\n"
          "  --version\n", o);
    return o == stdout ? 0 : 2;
}

/* Collect the reply for up to `wait` seconds, ending early once a level token is seen. */
static int query(int fd, double wait, int *level)
{
    static const char q[] = "RDILG\r\n";
    char buf[1024];
    size_t used = 0;
    tcflush(fd, TCIFLUSH);
    if (pm3_write_all(fd, q, sizeof q - 1, 1000) != 0) return -2;
    double end = pm3_now() + wait;
    while (pm3_now() < end && used < sizeof buf - 1) {
        struct pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 50) <= 0) continue;
        ssize_t n = read(fd, buf + used, sizeof buf - 1 - used);
        if (n > 0) {
            used += (size_t)n;
            if (pm3_scanner_parse_level(buf, used, level)) {
                usleep(50000);                       /* let the rest of the reply arrive and be discarded */
                return 0;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) return -2;
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *dev = NULL, *sysfs = "/sys";
    unsigned want_vid = PM3_SCANNER_VID, want_pid = PM3_SCANNER_PID;
    int any_id = 0, readback = 1, force = 0, level = -1, get_only = 0, i = 1;
    double wait = 2;
    for (; i < argc && argv[i][0] == '-'; i++) {
        const char *o = argv[i];
        int more = i + 1 < argc;
        if (!strcmp(o, "--device") && more) dev = argv[++i];
        else if (!strcmp(o, "--usb-id") && more) { if (sscanf(argv[++i], "%x:%x", &want_vid, &want_pid) != 2) return usage(stderr); }
        else if (!strcmp(o, "--any-usb-id")) any_id = 1;
        else if (!strcmp(o, "--no-readback")) readback = 0;
        else if (!strcmp(o, "--wait") && more) { wait = atof(argv[++i]); if (wait <= 0 || wait > 30) return usage(stderr); }
        else if (!strcmp(o, "--sysfs") && more) sysfs = argv[++i];
        else if (!strcmp(o, "--force-shared")) force = 1;
        else if (!strcmp(o, "--version")) { puts("pm3-scanner-light " TOOL_VERSION); return 0; }
        else if (!strcmp(o, "--help") || !strcmp(o, "-h")) return usage(stdout);
        else return usage(stderr);
    }
    if (!dev || i >= argc) return usage(stderr);
    if (!strcmp(argv[i], "off") && i + 1 == argc) level = 0;
    else if (!strcmp(argv[i], "get") && i + 1 == argc) get_only = 1;
    else if (!strcmp(argv[i], "level") && i + 2 == argc) {
        char *end;
        long v = strtol(argv[i + 1], &end, 10);
        if (*end || v < 0 || v > PM3_SCANNER_MAX_LEVEL) return usage(stderr);
        level = (int)v;
    } else return usage(stderr);
    if (get_only && !readback) return usage(stderr);

    unsigned vid = 0, pid = 0;
    int usb = pm3_usb_id(dev, sysfs, &vid, &pid);
    if (!any_id) {
        if (usb != 1) { fprintf(stderr, "pm3-scanner-light: refusing %s: not a USB serial device (use --any-usb-id to override)\n", dev); return 3; }
        if (vid != want_vid || pid != want_pid) {
            fprintf(stderr, "pm3-scanner-light: refusing %s: USB id %04x:%04x, expected %04x:%04x\n", dev, vid, pid, want_vid, want_pid);
            return 3;
        }
    }
    pid_t owner = 0;
    char comm[64] = "";
    int users = pm3_port_users(dev, "/proc", &owner, comm, sizeof comm);
    if (users < 0) { fprintf(stderr, "pm3-scanner-light: cannot examine %s\n", dev); return 3; }
    if (users > 0 && !force) {
        fprintf(stderr, "pm3-scanner-light: %s is already open by pid %ld (%s); run this before that program starts\n",
                dev, (long)owner, comm[0] ? comm : "?");
        return 3;
    }
    char err[160];
    int fd = pm3_serial_open(dev, 115200, err, sizeof err);
    if (fd < 0) { fprintf(stderr, "pm3-scanner-light: %s: %s\n", dev, err); return 3; }

    int rc = 0, got = -1;
    if (!get_only) {
        char cmd[24];
        int n = pm3_scanner_level_cmd(cmd, sizeof cmd, level);
        if (n < 0 || pm3_write_all(fd, cmd, (size_t)n, 1000) != 0) { fprintf(stderr, "pm3-scanner-light: write failed\n"); close(fd); return 4; }
        tcdrain(fd);
        usleep(300000);                              /* the engine applies the setting before answering queries */
    }
    if (readback) {
        int q = query(fd, wait, &got);
        if (q == -2) { fprintf(stderr, "pm3-scanner-light: I/O error during readback\n"); close(fd); return 4; }
        if (q != 0) rc = 1;
        else if (!get_only && got != level) rc = 1;
    }
    close(fd);
    if (get_only && got < 0) printf("{\"event\":\"scanner_light\",\"action\":\"get\",\"level\":null}\n");
    else if (get_only) printf("{\"event\":\"scanner_light\",\"action\":\"get\",\"level\":%d}\n", got);
    else if (!readback) printf("{\"event\":\"scanner_light\",\"action\":\"set\",\"requested\":%d,\"readback\":\"skipped\"}\n", level);
    else if (got < 0) printf("{\"event\":\"scanner_light\",\"action\":\"set\",\"requested\":%d,\"readback\":\"none\"}\n", level);
    else printf("{\"event\":\"scanner_light\",\"action\":\"set\",\"requested\":%d,\"readback\":%d,\"confirmed\":%s}\n",
                level, got, got == level ? "true" : "false");
    return rc;
}
