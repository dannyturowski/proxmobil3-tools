/* Linux host helpers for the accessory port, shared by the Nori app and
 * nori-linkd.  Nothing here enumerates or probes ttys: the only device ever
 * opened is the one explicitly configured, after these checks. */
#ifndef NORI_HOSTIO_H
#define NORI_HOSTIO_H

#include <stddef.h>

#define NHIO_DEFAULT_VID 0x1a86   /* QinHeng CH340 on the AITRIP ESP32-WROVER-DEV */
#define NHIO_DEFAULT_PID 0x7523

typedef struct {
    const char *nfc, *scanner;     /* configured NFC/barcode devices (may be NULL) */
    unsigned vid, pid;             /* required USB identity when the port is a USB serial device */
    int allow_pty;                 /* also accept a Unix98 pty slave (nori-linkd's app pty, tests) */
    const char *sysfs;             /* "/sys" (tests point this at a fake tree) */
} nhio_policy;

void nhio_policy_defaults(nhio_policy *p);

/* "usb:2-1.3" -> "/dev/ttyUSBn" bound to that USB port's interface 0 (looked up
 * only under that port's sysfs directory).  Plain paths are copied unchanged.
 * Returns 0 or -1 with a reason in err. */
int nhio_resolve(const char *spec, const nhio_policy *p, char *out, size_t cap, char *err, size_t errcap);

/* Refuse anything that is not a character device, is the same device as the NFC
 * reader or barcode scanner (compared by st_rdev, so symlinks and aliases are
 * caught), is a console/framebuffer, is a USB device with the wrong VID:PID, or
 * is neither USB nor (when allowed) a pty: on-board UARTs are never opened. */
int nhio_check(const char *path, const nhio_policy *p, char *err, size_t errcap);

/* USB identity of a char device: 1 = USB (vid/pid set), 0 = not USB, -1 = unknown. */
int nhio_usb_id(const char *path, const char *sysfs, unsigned *vid, unsigned *pid);

/* Open raw 8N1 nonblocking at baud, take an exclusive flock (so the app and the
 * helper can never both own the port), drop stale input.  R3: HUPCL cleared and
 * DTR/RTS released after configuring (no ESP32 auto-reset/boot-strap drive).
 * -1 on failure. */
int nhio_open(const char *path, int baud, char *err, size_t errcap);

/* Bytes still queued in the kernel for output (TIOCOUTQ), 0 if unknown. */
int nhio_outq(int fd);

#endif
