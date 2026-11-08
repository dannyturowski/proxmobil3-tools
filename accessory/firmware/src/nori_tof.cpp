// wt32-r3: the VL53L5CX task.  The portable supervisor (nori_tofsm) decides the next step, this file
// performs it with ST's ULD (lib/vl53l5cx_uld) and feeds good frames to the presence detector
// (nori_presence).  Nothing here prints: diagnostics leave only through snapshots (DIAG / HTTP).
//
// Scheduling: one FreeRTOS task, pinned to core 1 (where loop() runs) at loop()'s priority 1, 6 KB stack.
// Every I2C transaction blocks this task on the driver's ISR (the CPU is free meanwhile), every ULD wait
// is vTaskDelay, so loop(), the UART, lwIP/Wi-Fi/Ethernet (core 0, higher priorities) and the IDLE0 task
// the task watchdog monitors are never starved, including during the ~84 KB firmware upload of INIT.
// Memory: the ULD state (VL53L5CX_Configuration, ~2.3 KB) and one results frame are static; the 86 KB
// sensor firmware stays in flash and is copied to RAM 256 bytes per transaction (platform.c).
#include "nori_tof.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" {
#include "vl53l5cx_api.h"
#include "nori_evlog.h"
}

#ifndef NORI_TOF_HZ
#define NORI_TOF_HZ 5              // frames per second (low frequency on purpose: power and I2C load)
#endif
#ifndef NORI_TOF_INTEG_MS
#define NORI_TOF_INTEG_MS 20       // autonomous-mode integration time per frame
#endif
#ifndef NORI_TOF_I2C_HZ
#define NORI_TOF_I2C_HZ 400000
#endif
#ifndef NORI_TOF_PORT
#define NORI_TOF_PORT 0
#endif
static_assert(NORI_TOF_HZ >= 1 && NORI_TOF_HZ <= 15, "4x4 low-frequency ranging");
static_assert(NORI_TOF_INTEG_MS >= 2 && NORI_TOF_INTEG_MS < 1000 / NORI_TOF_HZ - 10, "integration time must fit in the frame period");
// wt32-r5.1.2: np_state.grid_mm takes distance_mm[i] as zone i; true only for 4x4 with one target per zone.
static_assert(VL53L5CX_RESOLUTION_4X4 == NP_GRID_ZONES && VL53L5CX_NB_TARGET_PER_ZONE == 1, "depth grid: one distance per 4x4 zone");

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static tof_snap pub;                      // guarded by mux
static np_cfg req_cfg;                    // guarded by mux
static bool req_cfg_new, req_pause, req_enable = true, req_restart;
static tof_log_fn logf_;

// task-owned
static VL53L5CX_Configuration uld;
static VL53L5CX_ResultsData res;
static nt_sm sm;
static nt_cfg ncfg;
static np_state np;
static np_cfg pcfg;
static int known, bus_ok, task_ok;
static uint8_t uld_status;
static int8_t temp_c;
static uint32_t loops;

static void log_ev(uint8_t code, uint8_t a, uint16_t b) { if (logf_) logf_(code, a, b); }

static void publish(uint32_t now)
{
    tof_snap s;
    s.sm = sm;
    s.np = np;
    s.cfg = pcfg;
    s.known = known;
    s.enabled = sm.enabled;
    s.bus_ok = bus_ok;
    s.pub_ms = now;
    s.xfer = uld.platform.n_xfer;
    s.nack = uld.platform.n_nack;
    s.tmo = uld.platform.n_timeout;
    s.err = uld.platform.n_err;
    s.last_err = uld.platform.last_err;
    s.uld_status = uld_status;
    s.skipped = uld.platform.n_skipped;
    s.task_ok = task_ok;
    s.temp_c = temp_c;
    s.stack_free = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    s.loops = loops;
    portENTER_CRITICAL(&mux);
    pub = s;
    portEXIT_CRITICAL(&mux);
}

// Outcome of one ULD call: bus failures (counted by platform.c) win over the ULD's own status.
struct bus_mark { uint32_t nack, tmo, err; };
static bus_mark mark()
{
    uld.platform.poisoned = 0;                    // each step starts with a usable bus
    return { uld.platform.n_nack, uld.platform.n_timeout, uld.platform.n_err };
}
static int outcome(const bus_mark &m, uint8_t st)
{
    if (st) uld_status = st;
    if (uld.platform.n_timeout != m.tmo || uld.platform.n_err != m.err) return NT_R_BUS;
    if (uld.platform.n_nack != m.nack) return NT_R_NACK;
    return st ? NT_R_ERR : NT_R_OK;
}

static int do_busclr()
{
    int e = bus_ok ? vl_plat_bus_clear(NORI_TOF_PORT, NORI_PIN_SDA, NORI_PIN_SCL, NORI_TOF_I2C_HZ)
                   : vl_plat_bus_begin(NORI_TOF_PORT, NORI_PIN_SDA, NORI_PIN_SCL, NORI_TOF_I2C_HZ);
    bus_ok = e == 0;
    return bus_ok ? NT_R_OK : NT_R_BUS;
}

static int do_probe()
{
    if (!bus_ok && do_busclr() != NT_R_OK) return NT_R_BUS;
    int r = vl_plat_probe(&uld.platform);
    return r == 0 ? NT_R_OK : r == 1 ? NT_R_NACK : NT_R_BUS;
}

static int do_idchk()
{
    bus_mark m = mark();
    uint8_t alive = 0;
    uint8_t st = vl53l5cx_is_alive(&uld, &alive);
    int r = outcome(m, st);
    return r == NT_R_OK && !alive ? NT_R_ERR : r;
}

static int do_init()
{
    bus_mark m = mark();
    return outcome(m, vl53l5cx_init(&uld));
}

static int do_config()
{
    bus_mark m = mark();
    uint8_t st = vl53l5cx_set_resolution(&uld, VL53L5CX_RESOLUTION_4X4);   // review fix: stop at the first failure
    if (!st) st = vl53l5cx_set_ranging_frequency_hz(&uld, NORI_TOF_HZ);
    if (!st) st = vl53l5cx_set_ranging_mode(&uld, VL53L5CX_RANGING_MODE_AUTONOMOUS);
    if (!st) st = vl53l5cx_set_integration_time_ms(&uld, NORI_TOF_INTEG_MS);
    if (!st) st = vl53l5cx_set_target_order(&uld, VL53L5CX_TARGET_ORDER_CLOSEST);
    return outcome(m, st);
}

static int do_start()
{
    bus_mark m = mark();
    return outcome(m, vl53l5cx_start_ranging(&uld));
}

static int do_stop()
{
    bus_mark m = mark();
    return outcome(m, vl53l5cx_stop_ranging(&uld));
}

static int do_poll(uint32_t now)
{
    bus_mark m = mark();
    uint8_t ready = 0;
    uint8_t st = vl53l5cx_check_data_ready(&uld, &ready);
    int r = outcome(m, st);
    if (r == NT_R_BUS || r == NT_R_NACK) { np_bad_frame(&np); return r; }
    if (r != NT_R_OK) { np_bad_frame(&np); return NT_R_BADFRAME; }   // the sensor reported a GO2 error
    if (!ready) return NT_R_NOTREADY;
    m = mark();
    st = vl53l5cx_get_ranging_data(&uld, &res);
    r = outcome(m, st);
    if (r != NT_R_OK) { np_bad_frame(&np); return r == NT_R_ERR ? NT_R_BADFRAME : r; }
    np_frame f = { VL53L5CX_RESOLUTION_4X4, res.distance_mm, res.target_status, res.nb_target_detected };
    np_frame_update(&np, &pcfg, &f, now);
    temp_c = res.silicon_temp_degc;
    known = 1;
    return NT_R_OK;
}

static void task(void *)
{
    int last_stage = -1;
    for (;;) {
        uint32_t now = millis();
        loops++;
        bool p, en, rs, nc;
        np_cfg c;
        portENTER_CRITICAL(&mux);
        p = req_pause; en = req_enable; rs = req_restart; nc = req_cfg_new;
        c = req_cfg;
        req_restart = false; req_cfg_new = false;
        portEXIT_CRITICAL(&mux);
        if (nc && np_cfg_valid(&c)) pcfg = c;     // never run with an inconsistent configuration
        nt_pause(&sm, p, now);
        nt_enable(&sm, en, now);
        if (rs) nt_restart(&sm, now);

        uint32_t wait = 0;
        int act = nt_next(&sm, &ncfg, now, &wait);
        int r = NT_R_OK;
        switch (act) {
        case NT_A_WAIT: break;
        case NT_A_BUSCLR: r = do_busclr(); break;
        case NT_A_PROBE: r = do_probe(); break;
        case NT_A_IDCHK: r = do_idchk(); break;
        case NT_A_INIT: r = do_init(); break;
        case NT_A_CONFIG: r = do_config(); break;
        case NT_A_START: r = do_start(); break;
        case NT_A_POLL: r = do_poll(now); break;
        case NT_A_STOP: r = do_stop(); break;
        default: break;
        }
        now = millis();
        if (act != NT_A_WAIT) nt_result(&sm, &ncfg, r, now);
        if (nt_ranging(&sm)) np_tick(&np, &pcfg, now);
        else if (np.level || np.have_frame) np_fault(&np, now);   // not ranging (failure, pause, disabled): absent
        int stage = sm.paused ? NT_S_PAUSED : sm.stage;
        if (stage != last_stage) {
            last_stage = stage;
            log_ev(NLG_TOF, (uint8_t)stage, 0);
        }
        publish(now);
        if (act == NT_A_WAIT) {
            uint32_t ms = wait < 5 ? 5 : wait > 50 ? 50 : wait;   // stay responsive to pause/enable requests
            vTaskDelay(pdMS_TO_TICKS(ms));
        } else {
            vTaskDelay(1);                                     // always yield between steps
        }
    }
}

void tof_begin(int enabled, const np_cfg *cfg, tof_log_fn log)
{
    logf_ = log;
    np_defaults(&pcfg);
    if (np_cfg_valid(cfg)) pcfg = *cfg;
    req_cfg = pcfg;
    req_enable = enabled != 0;
    memset(&uld, 0, sizeof uld);
    uld.platform.address = VL53L5CX_DEFAULT_I2C_ADDRESS;   // 0x52 (7-bit 0x29)
    uld.platform.port = NORI_TOF_PORT;
    uld.platform.xfer_timeout_ms = 50;
    uld.platform.chunk = VL_PLAT_BOUNCE;
    nt_defaults(&ncfg);
    ncfg.frame_timeout_ms = (uint16_t)(3000 / NORI_TOF_HZ + 900);   // 5 Hz: 1500 ms
    nt_init(&sm, enabled, millis());
    np_init(&np);
    task_ok = 1;
    publish(millis());
    if (xTaskCreatePinnedToCore(task, "tof", 6144, nullptr, 1, nullptr, 1) != pdPASS) {
        task_ok = 0;                              // no task: no I2C, presence never known; reported, not hidden
        publish(millis());
        log_ev(NLG_TOF, 255, 0);
    }
}

void tof_snapshot(tof_snap *out)
{
    portENTER_CRITICAL(&mux);
    *out = pub;
    portEXIT_CRITICAL(&mux);
}

void tof_pause(bool on)
{
    portENTER_CRITICAL(&mux);
    req_pause = on;
    portEXIT_CRITICAL(&mux);
}

void tof_enable(bool on)
{
    portENTER_CRITICAL(&mux);
    req_enable = on;
    portEXIT_CRITICAL(&mux);
}

void tof_restart()
{
    portENTER_CRITICAL(&mux);
    req_restart = true;
    portEXIT_CRITICAL(&mux);
}

void tof_set_cfg(const np_cfg *c)
{
    portENTER_CRITICAL(&mux);
    req_cfg = *c;
    req_cfg_new = true;
    portEXIT_CRITICAL(&mux);
}
