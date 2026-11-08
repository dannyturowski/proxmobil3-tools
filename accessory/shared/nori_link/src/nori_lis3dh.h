/* ST LIS3DH register-level driver (r5): portable C99, no Arduino, host-tested.
 *
 * Source: ST datasheet DocID17530 Rev 2 (December 2016); every register/bit used
 * here is listed with its page/table in docs/LIS3DH-ST-REFERENCE.md.
 *
 * The driver only produces samples in mg; tap detection is the shared ni_motion
 * detector (nori_input.c), exactly as for the LIS3DSH build.  The bus callbacks
 * are raw I2C register transfers: the driver itself sets the sub-address MSB
 * (auto-increment, DS p25-26 sec. 6.1.1) for every multi-byte transfer. */
#ifndef NORI_LIS3DH_H
#define NORI_LIS3DH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* I2C addresses (DS p25 sec. 6.1.1, Table 16): 001100xb, x = SA0. */
#define NL3_ADDR_SA0_HIGH 0x19     /* SDO/SA0 high, i.e. left open: internal pull-up (Table 2 note 1) */
#define NL3_ADDR_SA0_LOW  0x18     /* SDO/SA0 to GND */
#define NL3_AUTO_INC      0x80     /* SUB(7): auto-increment (DS p25-26) */

/* Register map (DS p31-32, Table 21). */
#define NL3_WHO_AM_I      0x0F     /* reads 0x33 (Table 24) */
#define NL3_TEMP_CFG_REG  0x1F
#define NL3_CTRL_REG1     0x20
#define NL3_CTRL_REG2     0x21
#define NL3_CTRL_REG3     0x22
#define NL3_CTRL_REG4     0x23
#define NL3_CTRL_REG5     0x24
#define NL3_CTRL_REG6     0x25
#define NL3_STATUS_REG    0x27
#define NL3_OUT_X_L       0x28
#define NL3_FIFO_CTRL_REG 0x2E
#define NL3_FIFO_SRC_REG  0x2F
#define NL3_ID            0x33
#define NL3_LIS3DSH_ID    0x3F     /* the LIS3DSH answers 0x3F at 0x1D/0x1E: different chip, other build */

/* Values (see nl3_regs()). */
#define NL3_ODR_400HZ     0x70     /* CTRL_REG1 ODR[3:0] = 0111 (Table 31) */
#define NL3_XYZ_EN        0x07     /* CTRL_REG1 Zen Yen Xen (Table 29) */
#define NL3_BDU           0x80     /* CTRL_REG4 bit 7 (Table 37/38) */
#define NL3_FS_8G         0x20     /* CTRL_REG4 FS[1:0] = 10, bits 5:4 */
#define NL3_HR            0x08     /* CTRL_REG4 bit 3 */
#define NL3_FIFO_EN       0x40     /* CTRL_REG5 bit 6 (Table 40/41) */
#define NL3_FM_STREAM     0x80     /* FIFO_CTRL_REG FM[1:0] = 10, bits 7:6 (Table 48/50) */
#define NL3_SRC_WTM       0x80     /* FIFO_SRC_REG (Table 51/52) */
#define NL3_SRC_OVRN      0x40
#define NL3_SRC_EMPTY     0x20
#define NL3_SRC_FSS       0x1F
#define NL3_ST_ZYXOR      0x80     /* STATUS_REG (Table 46/47) */
#define NL3_ST_ZYXDA      0x08
#define NL3_FIFO_DEPTH    32       /* sec. 3.6 / 5.1 */
#define NL3_ODR_HZ        400

/* Raw register transfers; return 1 on success (all bytes ACKed / received), 0 on failure.
 * `sub` is sent as is: the driver adds NL3_AUTO_INC itself when n > 1. */
typedef struct {
    void *ctx;
    int (*write)(void *ctx, uint8_t sub, const uint8_t *v, uint8_t n);
    int (*read)(void *ctx, uint8_t sub, uint8_t *v, uint8_t n);
} nl3_bus;

typedef struct {
    uint8_t ctrl1, ctrl4, ctrl5, fifo_ctrl;
} nl3_regs;

/* Register values for the build options: fifo (stream mode) and hr (high-resolution).
 * Always 400 Hz, X/Y/Z on, +-8 g, BDU, little endian, no self-test, no HPF, no INT routing. */
nl3_regs nl3_regs_for(int fifo, int hr);

enum { NL3_OK = 0, NL3_EIO = -1, NL3_EVERIFY = -2 };
/* Configure from power-down and read back every register written.  NL3_OK, NL3_EIO
 * (a transfer failed) or NL3_EVERIFY (a read-back value differs). */
int nl3_config(const nl3_bus *b, int fifo, int hr);
/* Still our chip and still configured?  WHO_AM_I == 0x33 and CTRL_REG1/4/5 as configured. */
int nl3_alive(const nl3_bus *b, int fifo, int hr);

/* One axis from OUT_x_L/OUT_x_H (BLE = 0: L at the lower address), +-8 g, in mg.
 * The output is left-justified two's complement, so a 16-bit LSB is 1/4 mg in both
 * normal (10-bit, 16 mg/digit) and high-resolution (12-bit, 4 mg/digit) mode. */
int32_t nl3_mg(const uint8_t *p);

typedef struct {
    void *ctx;
    void (*gap)(void *ctx);                                           /* samples were lost: reset the detector */
    int (*sample)(void *ctx, int32_t x_mg, int32_t y_mg, int32_t z_mg); /* returns 1 on a hit */
} nl3_sink;

/* Drain new samples into the sink.  Returns the number of samples read (>= 0) or
 * NL3_EIO.  *hit is OR'ed with the sink's results; *ovr is incremented on FIFO_SRC
 * OVRN (fifo) or STATUS ZYXOR (no fifo).  OVRN means 32 unread samples (Table 52):
 * 32 are read and gap() is called first, so a gap is never compared across. */
int nl3_poll(const nl3_bus *b, int fifo, const nl3_sink *s, int *hit, uint32_t *ovr);

/* Probe.  read_id(ctx, addr, &id) reads WHO_AM_I (single byte, sub-address 0x0F) at a
 * 7-bit address and returns 1 if the device answered.  Tries 0x19 then 0x18.
 * Returns 1 with *addr set if a LIS3DH (0x33) answered; else 0 with `name` set to
 * "unsupported-xx" (something else at 0x19/0x18), "lis3dsh-wrong-build" (a 0x3F chip at
 * 0x1D/0x1E: the LIS3DSH, use the lis3dsh build) or "absent". */
int nl3_probe(int (*read_id)(void *ctx, uint8_t addr, uint8_t *id), void *ctx, uint8_t *addr, char *name, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
