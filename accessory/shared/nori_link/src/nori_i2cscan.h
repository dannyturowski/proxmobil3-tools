/* r5.1 I2C wiring diagnostic behind the IICSCAN command (nori_dev.h).  Portable C99, no
 * Arduino: the firmware supplies the pin/bus primitives, the host tests a fake bus.
 *
 * Sequence (the I2C master must be stopped before nsc_run; it is restarted by it):
 *  1. Line test, SDA then SCL, each: input with the internal PULL-DOWN (~45 kOhm), settle,
 *     read ("pd"), then input with the internal pull-up, settle, read ("pu").
 *       pd=1 pu=1  an external pull-up is present (it beats the weak pull-down)
 *       pd=0 pu=1  no external pull-up (or a very weak one): the bus runs on the ESP32's
 *                  ~45 kOhm internal pull-ups only, too weak for 400 kHz
 *       pd=0 pu=0  the line is held low: short to GND, a stuck slave, or the wrong pin
 *     With external pull-ups nothing moves on the bus at all.  Without them, SDA falls and
 *     rises before SCL is touched, i.e. at most a START+STOP, which leaves slaves idle.
 *  2. A line held low is "stuck".  SCL stuck: nothing can be done, no scan.  SDA stuck:
 *     standard recovery, up to 9 SCL clocks (open drain, ~100 kHz) until SDA is released,
 *     then a STOP; rec=ok if both lines then read high, else rec=fail and no scan.
 *  3. Restart the master at hz and probe 0x08..0x77 (address + write, no data: the same
 *     as the DIAG bus scan), stopping at budget_ms or after 3 bus errors in a row.
 *
 * Reply tokens (at most 8, so with "IICSCAN" and the firmware's cs= the frame stays within
 * NL_ARGS_MAX = 10):
 *   sda=<pd>/<pu> scl=<pd>/<pu>[ stuck=<sda|scl|sda,scl>][ rec=<ok|fail>/<clocks>] hz=<kHz>
 *   found=<hex,...[+N]|none|skipped>[ err=<n>[/cut]] ms=<elapsed> */
#ifndef NORI_I2CSCAN_H
#define NORI_I2CSCAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NSC_SDA = 0, NSC_SCL = 1 };
enum { NSC_PULL_DOWN = 0, NSC_PULL_UP = 1 };
#define NSC_FIRST 0x08
#define NSC_LAST 0x77
#define NSC_LIST_MAX 16             /* addresses listed in the reply; more are counted as +N */

typedef struct {
    void *ctx;
    int (*level)(void *ctx, int line, int pull);   /* make line an input with that pull, settle, read 0/1 */
    void (*drive)(void *ctx, int line, int high);  /* open drain: 0 pulls low, 1 releases (pull-up on) */
    int (*sense)(void *ctx, int line);             /* read the pad without reconfiguring it */
    void (*wait_us)(void *ctx, uint32_t us);
    int (*bus_begin)(void *ctx, uint32_t hz);      /* (re)start the I2C master; 1 ok */
    int (*probe)(void *ctx, uint8_t addr);         /* 1 ACK, 0 NACK, -1 bus error / timeout */
    uint32_t (*ms)(void *ctx);
} nsc_hal;

typedef struct {
    uint8_t sda_pd, sda_pu, scl_pd, scl_pu;
    uint8_t stuck;                 /* bit 0 SDA, bit 1 SCL */
    int8_t rec;                    /* -1 not tried, 0 failed, 1 ok */
    uint8_t clocks;                /* recovery clocks given */
    uint8_t skipped;               /* no probing (stuck line or the master did not start) */
    uint8_t cut;                   /* probing stopped early (budget or repeated bus errors) */
    uint16_t nfound, errors;
    uint8_t found[NSC_LAST - NSC_FIRST + 1];
    uint32_t ms;
} nsc_result;

void nsc_run(const nsc_hal *h, uint32_t hz, uint32_t budget_ms, nsc_result *r);
/* Reply tokens as above.  Never writes past cap; returns the length written. */
int nsc_format(const nsc_result *r, uint32_t hz, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
