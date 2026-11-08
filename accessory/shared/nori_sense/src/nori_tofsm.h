/* wt32-r3: VL53L5CX supervisor (portable C99).  Decides WHAT the sensor task does next; the firmware
 * glue (firmware/src/nori_tof.cpp) does it with the ST ULD and reports the outcome.  Host-tested
 * (tests/test_sense.c) with scripted outcomes, so retry/backoff/timeout behaviour is known without
 * hardware.
 *
 *   PROBE   address ACK at 0x29 (7-bit).  NACK: "absent", backoff 2 s doubling to 60 s.
 *   IDCHK   vl53l5cx_is_alive (device 0xF0 rev 0x02).  Mismatch/error: backoff.
 *   INIT    vl53l5cx_init (SW reboot + ~84 KB firmware upload + NVM/xtalk/config; ~2-3 s at 400 kHz).
 *   CONFIG  4x4, ranging frequency, autonomous mode + integration time, closest target first.
 *   START   vl53l5cx_start_ranging.
 *   POLL    check_data_ready every poll_ms; when ready get_ranging_data.  A frame (good or bad) arrives
 *           at least every frame_timeout_ms or the session is restarted; max_bad consecutive bad frames
 *           (driver errors / corrupted frames) also restart it.
 *   STOP    best effort vl53l5cx_stop_ranging before a restart (its failure is counted, not fatal).
 *   BUSCLR  I2C bus clear (9 SCL pulses + STOP, driver reinstalled) after a bus timeout and before every
 *           INIT retry: a slave holding SDA low cannot wedge the bus for good.
 *   Every failure of IDCHK/INIT/CONFIG/START ends in BACKOFF: 5 s doubling to 60 s.  A session that
 *   delivered good frames for 60 s resets the backoff.  Nothing is ever retried in a tight loop, and
 *   every step is bounded in time by the ULD's own timeouts (2 s per poll-for-answer, 5 s stop).
 *   PAUSE (OTA in progress) and DISABLE (TOF en=0) stop all I2C activity at the next step. */
#ifndef NORI_TOFSM_H
#define NORI_TOFSM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NT_S_OFF, NT_S_PROBE, NT_S_IDCHK, NT_S_INIT, NT_S_CONFIG, NT_S_START, NT_S_RANGING, NT_S_STOP, NT_S_BACKOFF,
    NT_S_PAUSED, NT_S_COUNT
};
extern const char *const NT_STAGE_NAME[NT_S_COUNT];   /* off probe idchk init config start ranging stop backoff paused */

/* actions for the glue */
enum { NT_A_WAIT, NT_A_PROBE, NT_A_IDCHK, NT_A_INIT, NT_A_CONFIG, NT_A_START, NT_A_POLL, NT_A_STOP, NT_A_BUSCLR };
extern const char *const NT_ACTION_NAME[];

/* outcomes */
enum {
    NT_R_OK,        /* done (POLL: a good frame was read) */
    NT_R_NOTREADY,  /* POLL only: no new frame yet */
    NT_R_BADFRAME,  /* POLL only: data ready but the read/parse failed (driver status != 0) */
    NT_R_NACK,      /* address not acknowledged */
    NT_R_BUS,       /* bus timeout / arbitration / driver failure */
    NT_R_ERR        /* ULD status != 0 (or id mismatch) */
};

typedef struct {
    uint16_t poll_ms;             /* data-ready poll period (default 40) */
    uint16_t frame_timeout_ms;    /* no frame at all for this long: restart (default 1500 at 5 Hz) */
    uint8_t max_bad;              /* consecutive bad frames: restart (default 5) */
    uint32_t absent_min_ms, absent_max_ms;   /* PROBE NACK backoff 2000 .. 60000 */
    uint32_t fail_min_ms, fail_max_ms;       /* other failures 5000 .. 60000 */
    uint32_t stable_ms;           /* good ranging this long resets the backoff (60000) */
} nt_cfg;

typedef struct {
    int stage;
    int enabled, paused;
    int act;                      /* action handed out by nt_next, -1 none outstanding */
    uint32_t wait_until;
    uint32_t resume_at;           /* end of the backoff that follows a STOP */
    uint32_t backoff_ms;          /* the next failure backoff */
    uint32_t absent_ms;           /* the next NACK backoff */
    int bus_dirty;                /* do a BUSCLR before the next PROBE/INIT */
    int need_stop;                /* ranging was started: STOP before re-init */
    uint32_t next_poll, last_frame, ranging_since;
    uint8_t bad_run;
    int present;                  /* device answered PROBE at least once this boot */
    /* diagnostics */
    int last_fail_act, last_fail_res;    /* -1: none */
    uint32_t last_fail_ms;
    uint32_t probes, nacks, bus_errs, busclrs, busclr_fail, inits_ok, inits_fail, cfg_fail, start_fail, id_fail;
    uint32_t frames, bad_frames, timeouts, restarts, stops_fail;
    uint32_t init_ms_last;        /* duration of the last successful INIT */
    uint32_t act_started;
} nt_sm;

void nt_defaults(nt_cfg *c);
void nt_init(nt_sm *s, int enabled, uint32_t now);
void nt_enable(nt_sm *s, int on, uint32_t now);
void nt_pause(nt_sm *s, int on, uint32_t now);
/* Force a full restart (TOF re=1). */
void nt_restart(nt_sm *s, uint32_t now);
/* Next action.  NT_A_WAIT: *wait_ms is how long nothing needs to happen (the caller may sleep that
 * long, capped by its own responsiveness). */
int nt_next(nt_sm *s, const nt_cfg *c, uint32_t now, uint32_t *wait_ms);
/* Outcome of the action nt_next handed out.  Returns 1 if this result ended a RANGING session.
 * Callers should rather test nt_ranging() after every step: whenever it is 0 (including a timeout
 * decided inside nt_next, a pause or a disable) the presence state must be faulted (np_fault). */
int nt_result(nt_sm *s, const nt_cfg *c, int res, uint32_t now);
/* 1 while frames are expected (RANGING). */
int nt_ranging(const nt_sm *s);
/* "tof=ranging f=123 bf=0 to=0 rs=0 ini=1/0 nk=0 ..." compact diag; snprintf-style. */
int nt_diag(const nt_sm *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
