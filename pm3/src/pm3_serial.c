/* Serial-port helpers; see pm3_serial.h. */
#define _GNU_SOURCE
#include "pm3_serial.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* snprintf into a char array; false if the result was truncated. */
#define PATHF(buf, ...) ((size_t)snprintf((buf), sizeof (buf), __VA_ARGS__) < sizeof (buf))

double pm3_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static speed_t baud_const(int baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return 0;
    }
}

int pm3_serial_open(const char *path, int baud, char *err, size_t errcap)
{
    speed_t sp = baud_const(baud);
    struct stat st;
    if (!sp) { snprintf(err, errcap, "unsupported baud %d", baud); return -1; }
    if (stat(path, &st) != 0) { snprintf(err, errcap, "%s", strerror(errno)); return -1; }
    if (!S_ISCHR(st.st_mode)) { snprintf(err, errcap, "not a character device"); return -1; }
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { snprintf(err, errcap, "open: %s", strerror(errno)); return -1; }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        snprintf(err, errcap, "locked by another program (%s)", strerror(errno));
        close(fd);
        return -1;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { snprintf(err, errcap, "not a tty: %s", strerror(errno)); close(fd); return -1; }
    cfmakeraw(&tio);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    tio.c_cflag &= ~(tcflag_t)(CSTOPB | PARENB | CSIZE | CRTSCTS);
    tio.c_cflag |= CS8 | CLOCAL | CREAD;
    tio.c_iflag &= ~(tcflag_t)(IXON | IXOFF | IXANY);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { snprintf(err, errcap, "tcsetattr: %s", strerror(errno)); close(fd); return -1; }
    return fd;
}

int pm3_port_users(const char *path, const char *proc, pid_t *pid, char *comm, size_t commcap)
{
    struct stat want;
    if (stat(path, &want) != 0 || !S_ISCHR(want.st_mode)) return -1;
    DIR *d = opendir(proc);
    if (!d) return -1;
    int count = 0;
    pid_t self = getpid();
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end;
        long p = strtol(e->d_name, &end, 10);
        if (*end || p <= 0 || p == self) continue;
        char fdpath[PATH_MAX];
        if (!PATHF(fdpath, "%s/%ld/fd", proc, p)) continue;
        DIR *fds = opendir(fdpath);
        if (!fds) continue;
        struct dirent *f;
        int hit = 0;
        while (!hit && (f = readdir(fds))) {
            if (f->d_name[0] == '.') continue;
            char one[PATH_MAX];
            struct stat st;
            if (!PATHF(one, "%s/%s", fdpath, f->d_name)) continue;
            if (stat(one, &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == want.st_rdev) hit = 1;
        }
        closedir(fds);
        if (!hit) continue;
        if (count++ == 0) {
            if (pid) *pid = (pid_t)p;
            if (comm && commcap) {
                char cp[PATH_MAX];
                comm[0] = 0;
                if (!PATHF(cp, "%s/%ld/comm", proc, p)) cp[0] = 0;
                FILE *c = fopen(cp, "r");
                if (c) {
                    if (fgets(comm, (int)commcap, c)) comm[strcspn(comm, "\n")] = 0;
                    fclose(c);
                }
            }
        }
    }
    closedir(d);
    return count;
}

static int read_hex_file(const char *path, unsigned *out)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int ok = fscanf(f, "%x", out) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

int pm3_usb_id(const char *path, const char *sysfs, unsigned *vid, unsigned *pid)
{
    char real[PATH_MAX], dev[PATH_MAX], link[PATH_MAX], root[PATH_MAX];
    if (!realpath(path, real)) return -1;
    if (!realpath(sysfs, root)) return -1;
    const char *name = strrchr(real, '/');
    name = name ? name + 1 : real;
    if (!PATHF(link, "%s/class/tty/%s/device", root, name)) return -1;
    if (!realpath(link, dev)) return 0;          /* no sysfs entry: not a USB serial device */
    /* Walk up from the tty's device until a directory has idVendor/idProduct. */
    for (int depth = 0; depth < 8; depth++) {
        char v[PATH_MAX + 16], p[PATH_MAX + 16];
        if (!PATHF(v, "%s/idVendor", dev) || !PATHF(p, "%s/idProduct", dev)) return -1;
        if (read_hex_file(v, vid) == 0 && read_hex_file(p, pid) == 0) return 1;
        char *slash = strrchr(dev, '/');
        if (!slash || slash == dev) break;
        *slash = 0;
        if (strlen(dev) <= strlen(root) || strncmp(dev, root, strlen(root))) break;   /* never leave sysfs */
    }
    return 0;
}

int pm3_write_all(int fd, const void *buf, size_t n, int timeout_ms)
{
    const unsigned char *b = buf;
    double end = pm3_now() + timeout_ms / 1000.0;
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w > 0) { b += w; n -= (size_t)w; continue; }
        if (w < 0 && errno != EAGAIN && errno != EINTR) return -1;
        double left = end - pm3_now();
        if (left <= 0) return -1;
        struct pollfd p = { fd, POLLOUT, 0 };
        poll(&p, 1, (int)(left * 1000) + 1);
    }
    return 0;
}
