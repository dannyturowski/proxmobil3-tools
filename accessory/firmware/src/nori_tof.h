// wt32-r3: VL53L5CX sensor task (see nori_tof.cpp).  loop() only reads snapshots and posts requests;
// all I2C happens in the task, so a missing, slow or failing sensor never blocks the UART link,
// Ethernet/Wi-Fi, OTA or the buttons.
#pragma once
#include <stdint.h>

extern "C" {
#include "nori_presence.h"
#include "nori_tofsm.h"
}

struct tof_snap {
    nt_sm sm;                 // supervisor state and counters
    np_state np;              // presence detector state and counters; wt32-r5.1.2: also the last 4x4 frame, copied whole under the lock
    np_cfg cfg;               // presence thresholds in use
    int known;                // a good frame was seen this boot
    int enabled;
    int bus_ok;               // the I2C driver is installed
    uint32_t pub_ms;          // when this snapshot was taken (task side)
    uint32_t xfer, nack, tmo, err;   // platform layer bus counters
    int last_err;             // esp_err_t of the last failed bus transaction
    uint8_t uld_status;       // last non-zero ULD status (0x80 bit: a bus failure inside the ULD call)
    uint32_t skipped;         // transactions not attempted because the bus was poisoned in that ULD call
    int task_ok;              // 0: the task could not be created (stage reads "notask"; no I2C at all)
    int8_t temp_c;            // sensor die temperature from the last good frame
    uint32_t stack_free;      // task stack high-water mark (bytes left)
    uint32_t loops;
};

typedef void (*tof_log_fn)(uint8_t code, uint8_t a, uint16_t b);

void tof_begin(int enabled, const np_cfg *cfg, tof_log_fn log);   // starts the task (pinned to core 1)
void tof_snapshot(tof_snap *out);
void tof_pause(bool on);      // OTA: no I2C until resumed
void tof_enable(bool on);     // SENSOR en=0|1
void tof_restart();           // SENSOR re=1
void tof_set_cfg(const np_cfg *c);
