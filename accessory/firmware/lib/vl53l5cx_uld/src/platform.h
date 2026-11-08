/* VL53L5CX ULD platform layer for the WT32-ETH01 (ESP32, ESP-IDF 4.4 legacy I2C master driver).
 *
 * Replaces ST's porting/platform.h from STMicroelectronics/stm32-vl53l5cx v1.0.7 (ULD VL53L5CX_1.3.9);
 * the prototypes RdByte/WrByte/RdMulti/WrMulti/SwapBuffer/WaitMs are ST's, the bodies (platform.c) are
 * nori's.  vl53l5cx_api.c/.h and vl53l5cx_buffers.h are ST's files, unmodified (see SOURCE.md).
 *
 * Output selection (fewer outputs = shorter I2C frames, smaller VL53L5CX_ResultsData): the presence
 * detector only needs distance, target status and the number of targets per zone. */
#ifndef _PLATFORM_H_
#define _PLATFORM_H_
#pragma once

#include <stdint.h>
#include <string.h>

typedef struct {
    uint16_t address;          /* 8-bit form, as ST uses it: 0x52 (7-bit 0x29) */
    int port;                  /* i2c_port_t */
    uint32_t xfer_timeout_ms;  /* per bus transaction */
    uint32_t chunk;            /* max data bytes per transaction (<= VL_PLAT_BOUNCE) */
    /* diagnostics, updated by platform.c */
    uint32_t n_xfer, n_nack, n_timeout, n_err;
    uint32_t n_skipped;        /* transactions refused because the bus was poisoned */
    int last_err;              /* esp_err_t of the last failed transaction, 0 none */
    int poisoned;              /* review fix: set by the first failed transaction; every later Rd/Wr of the
                                * same ULD call fails at once (no bus traffic) and WaitMs yields one tick
                                * only, so a dead sensor ends a ULD call in ~0.2 s instead of up to ~30 s.
                                * The caller clears it before each step (nori_tof.cpp mark()). */
} VL53L5CX_Platform;

#define VL_PLAT_BOUNCE 256     /* RAM bounce buffer: flash-resident firmware bytes are copied here first */

#ifndef VL53L5CX_NB_TARGET_PER_ZONE
#define VL53L5CX_NB_TARGET_PER_ZONE (1U)
#endif

#define VL53L5CX_DISABLE_AMBIENT_PER_SPAD
#define VL53L5CX_DISABLE_NB_SPADS_ENABLED
/* NB_TARGET_DETECTED kept */
#define VL53L5CX_DISABLE_SIGNAL_PER_SPAD
#define VL53L5CX_DISABLE_RANGE_SIGMA_MM
/* DISTANCE_MM kept */
#define VL53L5CX_DISABLE_REFLECTANCE_PERCENT
/* TARGET_STATUS kept */
#define VL53L5CX_DISABLE_MOTION_INDICATOR

#define PROCESSOR_LITTLE_ENDIAN
#ifdef PROCESSOR_LITTLE_ENDIAN
#define SWAP_UINT16(x) (x)
#define SWAP_UINT32(x) (x)
#else
#define SWAP_UINT16(x) (((x) >> 8) | ((x) << 8))
#define SWAP_UINT32(x) (((x) >> 24) | (((x) & 0x00FF0000) >> 8) | (((x) & 0x0000FF00) << 8) | ((x) << 24))
#endif

#ifdef __cplusplus
extern "C" {
#endif

uint8_t RdByte(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t *p_value);
uint8_t WrByte(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t value);
uint8_t RdMulti(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t *p_values, uint32_t size);
uint8_t WrMulti(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t *p_values, uint32_t size);
void SwapBuffer(uint8_t *buffer, uint16_t size);
uint8_t WaitMs(VL53L5CX_Platform *p_platform, uint32_t TimeMs);

/* nori additions */
int vl_plat_bus_begin(int port, int sda, int scl, uint32_t hz);   /* 0 ok (esp_err_t otherwise) */
void vl_plat_bus_end(int port);
/* 9 SCL pulses + STOP with the driver removed, then the driver reinstalled. 0 ok. */
int vl_plat_bus_clear(int port, int sda, int scl, uint32_t hz);
/* Address-only write: 0 ACK, 1 NACK, 2 bus timeout/error. */
int vl_plat_probe(VL53L5CX_Platform *p);

#ifdef __cplusplus
}
#endif
#endif
