/* R3 nhio_open checks on a pty (no hardware): raw 8N1 at the requested speed,
 * HUPCL cleared so close() never toggles DTR/RTS, exclusive ownership, and the
 * DTR/RTS release (TIOCMBIC) tolerated where there are no modem lines.
 * On a real CH340 the release itself is verified on hardware (parent). */
#define _GNU_SOURCE
#include "nori_hostio.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("FAIL %s\n", m); } } while (0)

int main(void)
{
    char err[160];
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m)) { puts("no pty"); return 2; }
    const char *s = ptsname(m);
    int fd = nhio_open(s, 921600, err, sizeof err);
    CHECK(fd >= 0, "921600 open on a pty succeeds (modem-line release tolerated)");
    struct termios t;
    CHECK(fd >= 0 && tcgetattr(fd, &t) == 0, "tcgetattr");
    CHECK(!(t.c_cflag & HUPCL), "HUPCL cleared");
    CHECK((t.c_cflag & (CLOCAL | CREAD)) == (CLOCAL | CREAD), "CLOCAL|CREAD");
    CHECK(!(t.c_cflag & CRTSCTS), "no hardware flow control");
    CHECK((t.c_cflag & CSIZE) == CS8 && !(t.c_cflag & (PARENB | CSTOPB)), "8N1");
    CHECK(cfgetispeed(&t) == B921600 && cfgetospeed(&t) == B921600, "921600 both ways");
    CHECK(!(t.c_lflag & (ICANON | ECHO)) && !(t.c_iflag & (IXON | IXOFF)), "raw, no XON/XOFF");
    int fd2 = nhio_open(s, 921600, err, sizeof err);
    CHECK(fd2 < 0, "second owner refused (flock)");
    CHECK(nhio_open(s, 1000000, err, sizeof err) < 0, "unsupported baud refused");
    close(fd);
    fd = nhio_open(s, 115200, err, sizeof err);
    CHECK(fd >= 0 && tcgetattr(fd, &t) == 0 && cfgetospeed(&t) == B115200 && !(t.c_cflag & HUPCL), "reopen at 115200 (r1/r2 firmware)");
    close(fd);
    close(m);
    printf("hostio: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
