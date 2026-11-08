/* VL53L5CX ULD platform layer for the WT32-ETH01; see platform.h.
 *
 * Only ever called from the firmware's ToF task (firmware/src/nori_tof.cpp), never from loop() or an
 * ISR, so the static bounce/command buffers need no lock.  Every bus transaction:
 *  - carries at most p->chunk (<= VL_PLAT_BOUNCE) data bytes after the 2-byte register index (the sensor
 *    auto-increments the index; nori_i2c_chunk.h splits the ULD's 32 KB firmware writes);
 *  - is copied to RAM first (the ULD hands us flash-resident firmware and configuration arrays);
 *  - blocks the calling task, not the CPU (the IDF driver waits on its ISR).  p->xfer_timeout_ms is the
 *    REQUESTED bound: ESP-IDF 4.4.7's i2c_master_cmd_begin (components/driver/i2c.c ~1480) waits for each
 *    driver event at least I2C_CMD_ALIVE_INTERVAL_TICK (1 s) before checking the deadline, so a
 *    transaction that gets no interrupt at all can block ~1 s; a stuck SCL/SDA normally ends much
 *    sooner through the controller's own timeout interrupt (not measured on this board);
 *  - after one failure the bus is "poisoned" for the rest of the ULD call (see platform.h), so these
 *    worst cases do not multiply inside the ULD's 200-iteration poll loops;
 *  - counts its failure: n_nack (ESP_FAIL = no ACK), n_timeout (ESP_ERR_TIMEOUT = bus busy/stuck),
 *    n_err (anything else).
 * A failed transaction returns VL_PLAT_ERR (0x80); the ULD ORs it into its status. */
#include "platform.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nori_i2c_chunk.h"

#define VL_PLAT_ERR ((uint8_t)0x80)

static uint8_t bounce[2 + VL_PLAT_BOUNCE];
static uint8_t cmdbuf[I2C_LINK_RECOMMENDED_SIZE(3)];

/* build: OR of the i2c_master_* link-builder results (a truncated link is never executed) */
static int run(VL53L5CX_Platform *p, i2c_cmd_handle_t cmd, esp_err_t build)
{
    esp_err_t e = !cmd ? ESP_ERR_NO_MEM : build != ESP_OK ? build
                : i2c_master_cmd_begin((i2c_port_t)p->port, cmd, pdMS_TO_TICKS(p->xfer_timeout_ms) + 1);
    if (cmd) i2c_cmd_link_delete_static(cmd);
    p->n_xfer++;
    if (e == ESP_OK) return 0;
    p->last_err = (int)e;
    p->poisoned = 1;
    if (e == ESP_FAIL) p->n_nack++;
    else if (e == ESP_ERR_TIMEOUT) p->n_timeout++;
    else p->n_err++;
    return 1;
}

static uint32_t chunk_of(const VL53L5CX_Platform *p)
{
    return p->chunk && p->chunk <= VL_PLAT_BOUNCE ? p->chunk : VL_PLAT_BOUNCE;
}

static int wr_chunk(void *ctx, uint16_t reg, uint8_t *buf, uint32_t n)
{
    VL53L5CX_Platform *p = (VL53L5CX_Platform *)ctx;
    if (p->poisoned) { p->n_skipped++; return 1; }
    bounce[0] = (uint8_t)(reg >> 8);
    bounce[1] = (uint8_t)reg;
    memcpy(bounce + 2, buf, n);
    i2c_cmd_handle_t cmd = i2c_cmd_link_create_static(cmdbuf, sizeof cmdbuf);
    esp_err_t b = ESP_OK;
    if (cmd) {
        b |= i2c_master_start(cmd);
        b |= i2c_master_write_byte(cmd, (uint8_t)(p->address & 0xFE) | I2C_MASTER_WRITE, true);
        b |= i2c_master_write(cmd, bounce, n + 2, true);
        b |= i2c_master_stop(cmd);
    }
    return run(p, cmd, b);
}

static int rd_chunk(void *ctx, uint16_t reg, uint8_t *buf, uint32_t n)
{
    VL53L5CX_Platform *p = (VL53L5CX_Platform *)ctx;
    if (p->poisoned) { p->n_skipped++; return 1; }
    bounce[0] = (uint8_t)(reg >> 8);
    bounce[1] = (uint8_t)reg;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create_static(cmdbuf, sizeof cmdbuf);
    esp_err_t b = ESP_OK;
    if (cmd) {
        b |= i2c_master_start(cmd);
        b |= i2c_master_write_byte(cmd, (uint8_t)(p->address & 0xFE) | I2C_MASTER_WRITE, true);
        b |= i2c_master_write(cmd, bounce, 2, true);
        b |= i2c_master_start(cmd);
        b |= i2c_master_write_byte(cmd, (uint8_t)(p->address & 0xFE) | I2C_MASTER_READ, true);
        b |= i2c_master_read(cmd, buf, n, I2C_MASTER_LAST_NACK);
        b |= i2c_master_stop(cmd);
    }
    return run(p, cmd, b);
}

uint8_t RdByte(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t *p_value)
{
    return RdMulti(p_platform, RegisterAdress, p_value, 1);
}

uint8_t WrByte(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t value)
{
    return WrMulti(p_platform, RegisterAdress, &value, 1);
}

uint8_t RdMulti(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t *p_values, uint32_t size)
{
    return ni2c_chunked(p_platform, RegisterAdress, p_values, size, chunk_of(p_platform), rd_chunk) ? VL_PLAT_ERR : 0;
}

uint8_t WrMulti(VL53L5CX_Platform *p_platform, uint16_t RegisterAdress, uint8_t *p_values, uint32_t size)
{
    return ni2c_chunked(p_platform, RegisterAdress, p_values, size, chunk_of(p_platform), wr_chunk) ? VL_PLAT_ERR : 0;
}

void SwapBuffer(uint8_t *buffer, uint16_t size)          /* ST's reference implementation */
{
    uint32_t i, tmp;
    for (i = 0; i < size; i = i + 4) {
        tmp = ((uint32_t)buffer[i] << 24) | ((uint32_t)buffer[i + 1] << 16) | ((uint32_t)buffer[i + 2] << 8) | (uint32_t)buffer[i + 3];
        memcpy(&(buffer[i]), &tmp, 4);
    }
}

uint8_t WaitMs(VL53L5CX_Platform *p_platform, uint32_t TimeMs)
{
    TickType_t t = p_platform && p_platform->poisoned ? 1 : pdMS_TO_TICKS(TimeMs);   /* poisoned: the call is failing anyway */
    vTaskDelay(t ? t : 1);                                  /* always yields: never a busy wait */
    return 0;
}

int vl_plat_bus_begin(int port, int sda, int scl, uint32_t hz)
{
    i2c_config_t c;
    memset(&c, 0, sizeof c);
    c.mode = I2C_MODE_MASTER;
    c.sda_io_num = sda;
    c.scl_io_num = scl;
    c.sda_pullup_en = true;                                 /* weak; the breakout has its own pull-ups */
    c.scl_pullup_en = true;
    c.master.clk_speed = hz;
    esp_err_t e = i2c_param_config((i2c_port_t)port, &c);
    if (e == ESP_OK) e = i2c_driver_install((i2c_port_t)port, I2C_MODE_MASTER, 0, 0, 0);
    return (int)e;
}

void vl_plat_bus_end(int port) { i2c_driver_delete((i2c_port_t)port); }

int vl_plat_bus_clear(int port, int sda, int scl, uint32_t hz)
{
    vl_plat_bus_end(port);
    gpio_reset_pin((gpio_num_t)sda);
    gpio_reset_pin((gpio_num_t)scl);
    gpio_set_direction((gpio_num_t)sda, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction((gpio_num_t)scl, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode((gpio_num_t)sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)scl, GPIO_PULLUP_ONLY);
    gpio_set_level((gpio_num_t)sda, 1);
    gpio_set_level((gpio_num_t)scl, 1);
    esp_rom_delay_us(10);
    for (int i = 0; i < 9 && !gpio_get_level((gpio_num_t)sda); i++) {   /* clock a stuck slave out of its byte */
        gpio_set_level((gpio_num_t)scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level((gpio_num_t)scl, 1);
        esp_rom_delay_us(5);
    }
    gpio_set_level((gpio_num_t)sda, 0);                      /* STOP: SDA rises while SCL is high */
    esp_rom_delay_us(5);
    gpio_set_level((gpio_num_t)scl, 1);
    esp_rom_delay_us(5);
    gpio_set_level((gpio_num_t)sda, 1);
    esp_rom_delay_us(5);
    return vl_plat_bus_begin(port, sda, scl, hz);
}

int vl_plat_probe(VL53L5CX_Platform *p)
{
    uint32_t nack = p->n_nack;
    p->poisoned = 0;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create_static(cmdbuf, sizeof cmdbuf);
    esp_err_t b = ESP_OK;
    if (cmd) {
        b |= i2c_master_start(cmd);
        b |= i2c_master_write_byte(cmd, (uint8_t)(p->address & 0xFE) | I2C_MASTER_WRITE, true);
        b |= i2c_master_stop(cmd);
    }
    if (run(p, cmd, b) == 0) return 0;
    return p->n_nack != nack ? 1 : 2;
}
