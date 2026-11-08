/* wt32-r3: person presence from VL53L5CX multizone frames (portable C99, no allocation, not thread-safe:
 * one owner calls everything; the firmware's ToF task owns it and publishes copies under a lock).
 *
 * Input: one ranging frame = per-zone distance (mm), ST target status and number of targets (4x4 = 16
 * zones in this build; up to NP_ZONES_MAX).  Output: a debounced presence level with hysteresis.
 *
 *   zone valid  : nb_target >= 1 (if known), target_status 5 or 9 (ST UM2884: "range valid" and "range
 *                 valid with large pulse"), min_mm <= distance <= NP_MAX_MM.  Every other status (0 no
 *                 update, 255 no target, 4/6/10/... wrap-around, sigma, phase or "maybe" cases) is ignored:
 *                 an invalid zone is never evidence of presence (it counts as "nothing there").
 *   near frame  : >= min_zones valid zones at <= near_mm
 *   held frame  : >= min_zones valid zones at <= far_mm  (far_mm >= near_mm: the hysteresis band)
 *   rise        : level 0 and on_frames consecutive near frames
 *   fall        : level 1 and off_frames consecutive frames that are not held      (departure: re-arms)
 *   bad frame   : the driver reported an error / corrupted frame: breaks a rising run, never advances a
 *                 falling run (np_bad_frame)
 *   stale       : no good frame for stale_ms: level forced to 0 (np_tick; counted as stale_falls)
 *   fault       : the sensor was lost or is being re-initialised: level forced to 0 (np_fault)
 * So a stuck, absent or failing sensor always ends in "absent", never in a held presence, and a new
 * presence always needs on_frames fresh near frames.
 *
 * wt32-r5.1.2 depth grid (debug display only, never an input to the level): the last good 4x4 frame, one
 * mm value per zone by the same np_zone_valid rule, -1 where the zone is not valid.  The order is the
 * driver's raw zone order (row-major, zone 0 first).  How that maps to left/right/up/down as mounted has
 * NOT been physically calibrated; nothing here may claim a direction until a hand test has.
 * A bad frame or a fault clears it; np_grid_format withholds it once it is NP_GRID_STALE_MS old. */
#ifndef NORI_PRESENCE_H
#define NORI_PRESENCE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NP_ZONES_MAX 64
#define NP_MAX_MM 4000            /* VL53L5CX datasheet maximum; anything beyond is not a real target */

/* Defaults (see the firmware README for the reasoning; CFG/PRESCFG can change them at run time). */
#define NP_NEAR_MM_DEF 1000
#define NP_FAR_MM_DEF 1300
#define NP_MIN_MM_DEF 60          /* closer than this: cover glass / crosstalk / a hand on the sensor */
#define NP_MIN_ZONES_DEF 2        /* of 16: one zone alone (a fleck, a reflection) never counts */
#define NP_ON_FRAMES_DEF 3        /* at 5 Hz: 0.4 .. 0.6 s to assert */
#define NP_OFF_FRAMES_DEF 8       /* at 5 Hz: 1.4 .. 1.6 s of "nobody within far_mm" to release */
#define NP_STALE_MS_DEF 1500      /* no good frame this long: absent */
#define NP_GRID_ZONES 16          /* wt32-r5.1.2: the 4x4 frame */
#define NP_GRID_STALE_MS 1000     /* a grid this old is not shown: five frames at 5 Hz */
#define NP_GRID_NONE 0xfff        /* grid text for a zone with no valid reading; beyond NP_MAX_MM */
#define NP_GRID_TEXT_MAX 80       /* np_grid_format buffer: "zf=4294967295 za=999 zg=" + 48 + NUL = 73 */

typedef struct {
    uint16_t near_mm, far_mm, min_mm;
    uint8_t min_zones, on_frames, off_frames;
    uint16_t stale_ms;
} np_cfg;

typedef struct {
    uint8_t zones;                /* 1..NP_ZONES_MAX */
    const int16_t *distance_mm;   /* [zones] */
    const uint8_t *target_status; /* [zones] */
    const uint8_t *nb_target;     /* [zones] or NULL */
} np_frame;

typedef struct {
    int level;                    /* debounced presence, 0/1 */
    uint8_t on_run, off_run;
    int have_frame;               /* at least one good frame since np_init/np_fault */
    uint32_t last_good_ms;        /* time of the last good frame */
    uint32_t level_since_ms;
    /* the last good frame */
    int16_t nearest_mm;           /* nearest valid zone, -1: none */
    uint8_t valid_zones, near_zones, held_zones;
    /* counters since boot (np_init); never reset by np_fault */
    uint32_t frames, bad_frames, rises, falls, stale_falls, fault_falls;
    uint32_t zones_rejected;      /* zones with a target but an invalid status / range */
    /* wt32-r5.1.2: the frame `frames` counted last, taken at last_good_ms */
    int grid_ok;                  /* grid_mm holds a whole 16-zone frame; cleared by a bad frame and by np_fault */
    int16_t grid_mm[NP_GRID_ZONES];   /* raw zone order; -1: not valid */
} np_state;

void np_defaults(np_cfg *c);
/* 1 if the configuration is usable (ranges below, far >= near, near > min). */
int np_cfg_valid(const np_cfg *c);
/* Set one key from a PRESCFG token: near far min zones on off stale.  0 ok, -1 unknown key / range.
 * Ranges: near 100..4000, far 100..4000, min 0..500, zones 1..16, on 1..20, off 1..50, stale 500..10000.
 * The cross-check (np_cfg_valid) is the caller's, after all keys of one command. */
int np_cfg_set(np_cfg *c, const char *key, uint32_t v);
/* "near=1000 far=1300 min=60 zones=2 on=3 off=8 stale=1500" (7 tokens); snprintf-style. */
int np_cfg_format(const np_cfg *c, char *out, size_t cap);

void np_init(np_state *s);
int np_zone_valid(const np_cfg *c, int16_t mm, uint8_t status, int nb_target);
/* One good frame at now.  Returns +1 rise, -1 fall, 0 no change.  A frame with zones 0 or out of range
 * is treated as a bad frame. */
int np_frame_update(np_state *s, const np_cfg *c, const np_frame *f, uint32_t now);
/* The driver failed to deliver a frame (error, corrupted, timeout). Never changes the level by itself. */
void np_bad_frame(np_state *s);
/* Staleness: call often.  Returns -1 if it forced a fall. */
int np_tick(np_state *s, const np_cfg *c, uint32_t now);
/* Sensor lost / re-init: level 0 (returns -1 if it was 1), runs cleared, have_frame cleared. */
int np_fault(np_state *s, uint32_t now);
/* The level another task may act on: level && the last good frame is younger than stale_ms + margin_ms
 * (the margin keeps a slow-but-alive publisher from flapping the reader). */
int np_effective(const np_state *s, const np_cfg *c, uint32_t now, uint32_t margin_ms);
/* wt32-r5.1.2 DIAG tokens "zf=<frame> za=<age ms> zg=<16 x 3 lowercase hex mm, fff: not valid>", or
 * "zf=<frame> za=- zg=-" when there is no fresh frame.  cap must be >= NP_GRID_TEXT_MAX; returns the length or -1. */
int np_grid_format(const np_state *s, uint32_t now, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
