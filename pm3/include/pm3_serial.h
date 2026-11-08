/* Serial-port helpers shared by the PM3 tools (Linux).
 *
 * Nothing here enumerates or probes ports: each tool opens only the path it was
 * given, and only after checking that no other process already has it open. */
#ifndef PM3_SERIAL_H
#define PM3_SERIAL_H

#include <stddef.h>
#include <sys/types.h>

/* Open raw 8N1, no flow control, nonblocking, with an exclusive advisory lock.
 * baud: 9600, 19200, 38400, 57600, 115200, 230400, 460800 or 921600.
 * Returns the fd, or -1 with a reason in err. */
int pm3_serial_open(const char *path, int baud, char *err, size_t errcap);

/* Other processes holding `path` open (compared by device number, so symlinks and
 * aliases count).  Returns their number; the first one's pid and name are stored.
 * proc: "/proc" (tests may point it elsewhere).  -1 if `path` cannot be examined. */
int pm3_port_users(const char *path, const char *proc, pid_t *pid, char *comm, size_t commcap);

/* USB identity of a tty: 1 = USB (vid/pid set), 0 = not USB, -1 = unknown.
 * sysfs: "/sys" (tests may point it at a fake tree). */
int pm3_usb_id(const char *path, const char *sysfs, unsigned *vid, unsigned *pid);

/* Write everything within timeout_ms.  0 ok, -1 error or timeout. */
int pm3_write_all(int fd, const void *buf, size_t n, int timeout_ms);

/* Monotonic seconds. */
double pm3_now(void);

#endif
