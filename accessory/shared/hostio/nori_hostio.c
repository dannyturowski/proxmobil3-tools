/* Linux host helpers for the accessory port; see nori_hostio.h. */
#define _GNU_SOURCE
#include "nori_hostio.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>
#include <unistd.h>

void nhio_policy_defaults(nhio_policy *p)
{
    memset(p, 0, sizeof *p);
    p->vid = NHIO_DEFAULT_VID;
    p->pid = NHIO_DEFAULT_PID;
    p->sysfs = "/sys";
}

static void say(char *err, size_t cap, const char *msg)
{
    if (err && cap) snprintf(err, cap, "%s", msg);
}

static int read_hex(const char *path, unsigned *out)
{
    char b[16];
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, b, sizeof b - 1);
    close(fd);
    if (n <= 0) return -1;
    b[n] = 0;
    char *end;
    unsigned long v = strtoul(b, &end, 16);
    if (end == b) return -1;
    *out = (unsigned)v;
    return 0;
}

/* Find the USB device (directory with idVendor) above a char device in sysfs.
 * 1: found (vid/pid filled), 0: no USB parent, -1: sysfs unreadable. */
static int usb_parent(const nhio_policy *p, dev_t rdev, unsigned *vid, unsigned *pid)
{
    char link[PATH_MAX], real[PATH_MAX];
    snprintf(link, sizeof link, "%s/dev/char/%u:%u", p->sysfs, major(rdev), minor(rdev));
    if (!realpath(link, real)) return -1;
    for (int up = 0; up < 8; up++) {
        char f[PATH_MAX + 16];
        snprintf(f, sizeof f, "%s/idVendor", real);
        if (access(f, R_OK) == 0) {
            char g[PATH_MAX + 16];
            snprintf(g, sizeof g, "%s/idProduct", real);
            if (read_hex(f, vid) != 0 || read_hex(g, pid) != 0) return -1;
            return 1;
        }
        char *slash = strrchr(real, '/');
        if (!slash || slash == real) break;
        *slash = 0;
        if (strlen(real) <= strlen(p->sysfs)) break;
    }
    return 0;
}

int nhio_usb_id(const char *path, const char *sysfs, unsigned *vid, unsigned *pid)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISCHR(st.st_mode)) return -1;
    nhio_policy p;
    nhio_policy_defaults(&p);
    if (sysfs) p.sysfs = sysfs;
    return usb_parent(&p, st.st_rdev, vid, pid);
}

static int same_dev(const struct stat *st, const char *other)
{
    struct stat o;
    return other && stat(other, &o) == 0 && S_ISCHR(o.st_mode) && o.st_rdev == st->st_rdev;
}

int nhio_check(const char *path, const nhio_policy *p, char *err, size_t errcap)
{
    struct stat st;
    if (!path || stat(path, &st) != 0) { say(err, errcap, "accessory device not present"); return -1; }
    if (!S_ISCHR(st.st_mode)) { say(err, errcap, "accessory path is not a character device"); return -1; }
    if (same_dev(&st, p->nfc)) { say(err, errcap, "accessory path is the NFC reader; refused"); return -1; }
    if (same_dev(&st, p->scanner)) { say(err, errcap, "accessory path is the barcode scanner; refused"); return -1; }
    unsigned maj = major(st.st_rdev);
    /* 1 mem, 4 virtual consoles/ttyS, 5 tty/console/ptmx, 29 framebuffer, 7 vcs */
    if (maj == 1 || maj == 4 || maj == 5 || maj == 7 || maj == 29) {
        say(err, errcap, "accessory path is a console/system device; refused");
        return -1;
    }
    unsigned vid = 0, pid = 0;
    int u = usb_parent(p, st.st_rdev, &vid, &pid);
    if (u == 1) {
        if (vid != p->vid || pid != p->pid) {
            char m[96];
            snprintf(m, sizeof m, "accessory USB id %04x:%04x is not the expected %04x:%04x; refused", vid, pid, p->vid, p->pid);
            say(err, errcap, m);
            return -1;
        }
        return 0;
    }
    if (p->allow_pty && maj >= 136 && maj <= 143) return 0;   /* Unix98 pty slaves */
    say(err, errcap, u < 0 ? "cannot read sysfs identity of accessory path; refused" : "accessory path is not a USB serial device; refused");
    return -1;
}

int nhio_resolve(const char *spec, const nhio_policy *p, char *out, size_t cap, char *err, size_t errcap)
{
    if (strncmp(spec, "usb:", 4) != 0) {
        if ((size_t)snprintf(out, cap, "%s", spec) >= cap) { say(err, errcap, "path too long"); return -1; }
        return 0;
    }
    const char *port = spec + 4;
    size_t n = strlen(port);
    if (n == 0 || n > 24 || strspn(port, "0123456789.-") != n) { say(err, errcap, "bad USB port (expected e.g. usb:2-1.3)"); return -1; }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/bus/usb/devices/%s:1.0", p->sysfs, port);
    DIR *d = opendir(dir);
    if (!d) { say(err, errcap, "no interface 0 at that USB port (unplugged, or no driver bound)"); return -1; }
    char found[64] = "";
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t dl = strlen(e->d_name);
        if (dl < 16 && !strncmp(e->d_name, "ttyUSB", 6) && dl > 6 && strspn(e->d_name + 6, "0123456789") == dl - 6) {
            memcpy(found, e->d_name, dl + 1);
            break;
        }
    }
    closedir(d);
    if (!found[0]) { say(err, errcap, "USB port has no ttyUSB (is the ch341 driver loaded?)"); return -1; }
    if ((size_t)snprintf(out, cap, "/dev/%s", found) >= cap) { say(err, errcap, "path too long"); return -1; }
    return 0;
}

static speed_t baud_const(int baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

int nhio_open(const char *path, int baud, char *err, size_t errcap)
{
    speed_t sp = baud_const(baud);
    if (!sp) { say(err, errcap, "unsupported baud rate"); return -1; }
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { say(err, errcap, strerror(errno)); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        say(err, errcap, "port is already owned by another Nori process");
        close(fd);
        return -1;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { say(err, errcap, "not a serial port"); close(fd); return -1; }
    cfmakeraw(&tio);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    /* HUPCL off: close() must not toggle DTR/RTS (the ESP32 board's auto-reset/boot lines). */
    tio.c_cflag &= ~(CSTOPB | PARENB | CSIZE | CRTSCTS | HUPCL);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { say(err, errcap, "cannot configure port"); close(fd); return -1; }
    /* open() raised DTR and RTS (the kernel does that on every open); release both
     * so neither EN nor IO0 is driven in normal use.  Best effort: ptys have no
     * modem lines (ENOTTY/EINVAL). */
    int lines = TIOCM_DTR | TIOCM_RTS;
    if (ioctl(fd, TIOCMBIC, &lines) != 0 && errno != ENOTTY && errno != EINVAL) {
        say(err, errcap, "cannot release DTR/RTS");
        close(fd);
        return -1;
    }
    tcflush(fd, TCIFLUSH);
    return fd;
}

int nhio_outq(int fd)
{
    int n = 0;
    if (ioctl(fd, TIOCOUTQ, &n) != 0 || n < 0) return 0;
    return n;
}
