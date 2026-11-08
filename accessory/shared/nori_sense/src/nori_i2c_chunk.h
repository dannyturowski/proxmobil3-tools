/* wt32-r3: split one VL53L5CX register-block transfer into bus transactions of at most `chunk` bytes.
 * The sensor auto-increments its 16-bit register index, so a block at reg..reg+size-1 is the same as
 * consecutive blocks at reg+off.  Used by the ULD platform layer (firmware/lib/vl53l5cx_uld/src/platform.c)
 * so no transaction is ever larger than the RAM bounce buffer, and host-tested in tests/test_sense.c. */
#ifndef NORI_I2C_CHUNK_H
#define NORI_I2C_CHUNK_H

#include <stdint.h>

typedef int (*ni2c_xfer_fn)(void *ctx, uint16_t reg, uint8_t *buf, uint32_t n);   /* 0 ok */

/* 0 ok; the first failing transaction's code otherwise; -1 if the block would run past index 0xFFFF
 * or chunk is 0. */
static inline int ni2c_chunked(void *ctx, uint16_t reg, uint8_t *p, uint32_t size, uint32_t chunk, ni2c_xfer_fn fn)
{
    if (!chunk || (uint32_t)reg + size > 0x10000u) return -1;
    for (uint32_t off = 0; off < size;) {
        uint32_t n = size - off < chunk ? size - off : chunk;
        int r = fn(ctx, (uint16_t)(reg + off), p + off, n);
        if (r) return r;
        off += n;
    }
    return 0;
}

#endif
