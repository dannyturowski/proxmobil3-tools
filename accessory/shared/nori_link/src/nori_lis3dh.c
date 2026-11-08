/* ST LIS3DH register-level driver (r5).  See nori_lis3dh.h and docs/LIS3DH-ST-REFERENCE.md. */
#include "nori_lis3dh.h"

#include <stdio.h>
#include <string.h>

static int wr1(const nl3_bus *b, uint8_t reg, uint8_t v) { return b->write(b->ctx, reg, &v, 1); }
static int rd(const nl3_bus *b, uint8_t reg, uint8_t *v, uint8_t n)
{
    return b->read(b->ctx, (uint8_t)(n > 1 ? (reg | NL3_AUTO_INC) : reg), v, n);   /* DS p26: SUB(7) = 1 for multi-byte */
}

nl3_regs nl3_regs_for(int fifo, int hr)
{
    nl3_regs r;
    r.ctrl1 = NL3_ODR_400HZ | NL3_XYZ_EN;                  /* 0x77: 400 Hz, LPen 0 (normal/HR), Z Y X on */
    r.ctrl4 = (uint8_t)(NL3_BDU | NL3_FS_8G | (hr ? NL3_HR : 0));   /* 0xA0 normal, 0xA8 HR; BLE 0, ST 00, SIM 0 */
    r.ctrl5 = fifo ? NL3_FIFO_EN : 0x00;                   /* BOOT 0, no latched/4D interrupts */
    r.fifo_ctrl = fifo ? NL3_FM_STREAM : 0x00;             /* stream / bypass; TR 0, FTH 0 */
    return r;
}

/* Registers never written here (deliberately):
 *  - CTRL_REG0 0x1E: reserved in Rev 1 of the datasheet (writing reserved registers "may cause
 *    permanent damage"); Rev 2 default 0x10 keeps the SDO/SA0 pull-up, which is what we want.
 *  - INT1/INT2/CLICK/ACT registers 0x30-0x3F: they drive nothing (CTRL_REG3/6 route nothing to
 *    INT1/INT2) and INT2_* / ACT_* are not listed in Rev 1; power-on default is 0 for all.
 *  - REFERENCE 0x26: only used by the high-pass filter, which is bypassed (CTRL_REG2 = 0). */
int nl3_config(const nl3_bus *b, int fifo, int hr)
{
    const nl3_regs r = nl3_regs_for(fifo, hr);
    int ok = wr1(b, NL3_CTRL_REG1, 0x00) &&                /* power-down (ODR 0000), axes off, while configuring */
             wr1(b, NL3_TEMP_CFG_REG, 0x00) &&             /* aux ADC and temperature sensor off */
             wr1(b, NL3_CTRL_REG2, 0x00) &&                /* HPF normal mode, FDS 0: unfiltered data to OUT and FIFO */
             wr1(b, NL3_CTRL_REG3, 0x00) &&                /* nothing routed to INT1 */
             wr1(b, NL3_CTRL_REG4, r.ctrl4) &&
             wr1(b, NL3_CTRL_REG5, r.ctrl5) &&
             wr1(b, NL3_CTRL_REG6, 0x00) &&                /* nothing routed to INT2, active-high */
             wr1(b, NL3_FIFO_CTRL_REG, 0x00) &&            /* bypass first: resets the FIFO (DS sec. 5.1.1) */
             wr1(b, NL3_FIFO_CTRL_REG, r.fifo_ctrl) &&
             wr1(b, NL3_CTRL_REG1, r.ctrl1);               /* start sampling last */
    if (!ok) return NL3_EIO;
    uint8_t v[7], f;                                       /* 0x1F..0x25 in one auto-increment read */
    if (!rd(b, NL3_TEMP_CFG_REG, v, 7) || !rd(b, NL3_FIFO_CTRL_REG, &f, 1)) return NL3_EIO;
    int same = v[0] == 0x00 &&                             /* TEMP_CFG_REG: bits 5:0 are fixed 0 (Table 27) */
               v[1] == r.ctrl1 &&
               v[2] == 0x00 &&
               (v[3] & 0xFE) == 0x00 &&                    /* CTRL_REG3 bit 0 is unused (Table 35) */
               v[4] == r.ctrl4 &&
               (v[5] & 0xCF) == r.ctrl5 &&                 /* CTRL_REG5 bits 5:4 unused (Table 40) */
               (v[6] & 0xFA) == 0x00 &&                    /* CTRL_REG6 bits 2 and 0 unused (Table 42) */
               f == r.fifo_ctrl;
    return same ? NL3_OK : NL3_EVERIFY;
}

int nl3_alive(const nl3_bus *b, int fifo, int hr)
{
    const nl3_regs r = nl3_regs_for(fifo, hr);
    uint8_t id = 0, c[5];
    return rd(b, NL3_WHO_AM_I, &id, 1) && id == NL3_ID && rd(b, NL3_CTRL_REG1, c, 5) &&
           c[0] == r.ctrl1 && c[3] == r.ctrl4 && (c[4] & NL3_FIFO_EN) == r.ctrl5;
}

int32_t nl3_mg(const uint8_t *p)
{
    return (int32_t)(int16_t)(uint16_t)(p[0] | (p[1] << 8)) / 4;   /* left-justified, +-8 g: 1/4 mg per 16-bit LSB */
}

static int feed(const nl3_sink *s, const uint8_t *p)
{
    return s->sample(s->ctx, nl3_mg(p), nl3_mg(p + 2), nl3_mg(p + 4));
}

int nl3_poll(const nl3_bus *b, int fifo, const nl3_sink *s, int *hit, uint32_t *ovr)
{
    uint8_t p[7];
    if (fifo) {
        uint8_t src;
        if (!rd(b, NL3_FIFO_SRC_REG, &src, 1)) return NL3_EIO;
        int n = (src & NL3_SRC_EMPTY) ? 0 : (src & NL3_SRC_FSS);
        if (src & NL3_SRC_OVRN) {                          /* full: 32 unread, the next ODR overwrites (Table 52) */
            n = NL3_FIFO_DEPTH;
            (*ovr)++;
            s->gap(s->ctx);                                /* a gap (loop() stalled >= 80 ms): never compare across it */
        }
        for (int i = 0; i < n; i++) {                      /* one 6-byte read per sample; roll-over not relied on */
            if (!rd(b, NL3_OUT_X_L, p, 6)) return NL3_EIO;
            *hit |= feed(s, p);
        }
        return n;
    }
    if (!rd(b, NL3_STATUS_REG, p, 7)) return NL3_EIO;     /* STATUS + XYZ in one auto-increment read */
    if (p[0] & NL3_ST_ZYXOR) (*ovr)++;                     /* one set overwritten: the baseline copes (as r4) */
    if (!(p[0] & NL3_ST_ZYXDA)) return 0;
    *hit |= feed(s, p + 1);
    return 1;
}

int nl3_probe(int (*read_id)(void *ctx, uint8_t addr, uint8_t *id), void *ctx, uint8_t *addr, char *name, size_t cap)
{
    static const uint8_t mine[] = { NL3_ADDR_SA0_HIGH, NL3_ADDR_SA0_LOW };
    static const uint8_t lis3dsh[] = { 0x1E, 0x1D };
    int answered = 0;
    snprintf(name, cap, "absent");
    for (size_t i = 0; i < sizeof mine; i++) {
        uint8_t id = 0;
        if (!read_id(ctx, mine[i], &id)) continue;
        answered = 1;
        if (id == NL3_ID) { *addr = mine[i]; snprintf(name, cap, "lis3dh"); return 1; }
        snprintf(name, cap, "unsupported-%02x", id);
    }
    if (answered) return 0;
    for (size_t i = 0; i < sizeof lis3dsh; i++) {         /* name the reverse mix-up instead of saying "absent" */
        uint8_t id = 0;
        if (read_id(ctx, lis3dsh[i], &id) && id == NL3_LIS3DSH_ID) { snprintf(name, cap, "lis3dsh-wrong-build"); break; }
    }
    return 0;
}
