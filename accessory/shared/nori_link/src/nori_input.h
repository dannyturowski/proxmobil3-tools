/* Accessory input conditioning: button debounce, tap arbitration, shake
 * suppression and event rate limiting.  Pure logic on millisecond timestamps,
 * so the ESP32 firmware and the native tests run exactly the same code. */
#ifndef NORI_INPUT_H
#define NORI_INPUT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- buttons ---- */
typedef struct {
    uint8_t stable;        /* debounced level: 1 = pressed */
    uint8_t raw;           /* last raw sample */
    uint8_t armed;         /* 0 until the button has been seen released (held/stuck at boot) */
    uint32_t raw_since;    /* when raw last changed */
} ni_button;

void ni_button_init(ni_button *b, int raw_pressed, uint32_t now);
/* Sample the pin. Returns +1 on a debounced press, -1 on release, 0 otherwise.
 * A button pressed at boot reports nothing until it has been released once;
 * holding a button never repeats. */
int ni_button_update(ni_button *b, int raw_pressed, uint32_t now, uint16_t debounce_ms);

/* ---- three-way track selector (wt32-r5) ----
 * A centre-off toggle: two active-low contacts, each joining its pin to ground.  Only a debounced
 * move INTO a direction is reported, and it reports where the lever now is, never "the other way":
 *  - the position at boot is taken as given: nothing is reported for it;
 *  - holding a contact never repeats, and a dropout shorter than the debounce is not a new move;
 *  - returning to the centre reports nothing;
 *  - going straight from one direction to the other reports the new direction;
 *  - both contacts low cannot be a lever position (wiring fault): nothing is reported until the
 *    centre has been seen again, so the way out of a fault is never mistaken for a choice. */
enum { NI_TOG_CENTER = 0, NI_TOG_FIVE = 1, NI_TOG_ONE = 2, NI_TOG_BOTH = 3 };

typedef struct {
    uint8_t stable;        /* debounced position, NI_TOG_* */
    uint8_t raw;           /* last raw position */
    uint8_t locked;        /* both contacts were low: refuse directions until the centre */
    uint32_t raw_since;    /* when raw last changed */
    uint32_t n_five, n_one, n_center, n_both, n_locked;   /* diagnostics: debounced entries; n_locked = refused */
} ni_toggle;

void ni_toggle_init(ni_toggle *t, int raw_five, int raw_one, uint32_t now);
/* Sample both contacts (1 = low).  Returns NI_TOG_FIVE or NI_TOG_ONE on a reportable move, else
 * NI_TOG_CENTER (0): nothing to report. */
int ni_toggle_update(ni_toggle *t, int raw_five, int raw_one, uint32_t now, uint16_t debounce_ms);

/* ---- selector gestures (wt32-r5.1.1) ----
 * Classifies each move ni_toggle reported as short or long, alongside it: the move itself is still
 * reported at once, so the games see no added delay.  One gesture gives at most one result:
 *  - long at NI_GES_HOLD_MS from the contact settling, once, and nothing on release;
 *  - short on release, if it was not long: NI_GES_RELEASE_MS after the contact opened, or at once on a
 *    straight move to the other direction.  That result comes before the new direction's own gesture;
 *  - a return to the same direction inside NI_GES_RELEASE_MS is the same gesture, however ni_toggle
 *    sees it, so a worn contact cannot turn one hold into a feed or into a second hold;
 *  - no result for a lever that was off centre at boot, both contacts low, or ni_gesture_cancel: all of
 *    these stay silent until the centre has been seen, even across a straight move. */
enum { NI_GES_NONE = 0, NI_GES_SHORT_FIVE = 1, NI_GES_SHORT_ONE = 2, NI_GES_LONG_FIVE = 3, NI_GES_LONG_ONE = 4 };
#define NI_GES_HOLD_MS 1000u
#define NI_GES_RELEASE_MS 80u

typedef struct {
    uint8_t pos;           /* direction of the gesture in progress, NI_TOG_CENTER: none */
    uint8_t fired;         /* it has been reported long */
    uint8_t blocked;       /* refuse gestures until the centre */
    uint32_t since;        /* when its contact settled */
    uint32_t session;      /* nd_selector: the host session it belongs to */
    uint32_t n_short, n_long, n_cancel;   /* diagnostics; n_cancel = unreported gestures given up */
} ni_gesture;

void ni_gesture_init(ni_gesture *g, const ni_toggle *t);
/* After ni_toggle_update, with its result.  Returns NI_GES_*.  Report it before that move. */
int ni_gesture_update(ni_gesture *g, const ni_toggle *t, int pos, uint32_t now);
void ni_gesture_cancel(ni_gesture *g);

/* ---- taps ---- */
enum { NI_NONE = 0, NI_TAP = 1, NI_DTAP = 2 };

typedef struct {
    uint16_t dtap_wait_ms;     /* a single tap waits this long for a second tap; 0 = report singles at once */
    uint16_t refractory_ms;    /* after a reported tap, ignore further taps this long */
    uint8_t  shake_hits;       /* this many raw hits within shake_window_ms => shaking */
    uint16_t shake_window_ms;
    uint16_t shake_quiet_ms;   /* while shaking, stay muted until this long without hits */
} ni_tap_cfg;

#define NI_SHAKE_HIST 8
typedef struct {
    uint8_t pending;           /* a single tap is waiting for a possible second tap */
    uint32_t pending_at;
    uint8_t refr;              /* refractory period active */
    uint32_t refractory_until;
    uint32_t muted_until;
    uint8_t muted;
    uint32_t hits[NI_SHAKE_HIST];
    uint8_t nhits, hpos;
    uint32_t suppressed;       /* diagnostics: hits dropped by refractory/shake */
} ni_tap;

void ni_tap_defaults(ni_tap_cfg *c);
/* R4 game profile: report a single tap at once (dtap_wait_ms 0), 80 ms refractory,
 * and a shake guard (8 hits in 700 ms) that a fast player cannot reach. */
void ni_tap_game_defaults(ni_tap_cfg *c);
void ni_tap_init(ni_tap *t);
/* Feed the hardware tap flags polled at `now` (either may be 0).  Returns NI_TAP,
 * NI_DTAP or NI_NONE.  Call regularly even without flags so pending singles mature.
 * One physical gesture is reported once, never as a TAP followed by a DTAP:
 *  - dtap_wait_ms > 0: a single waits; a second tap (or the chip's double flag)
 *    inside the wait turns it into one DTAP instead.
 *  - dtap_wait_ms == 0: every tap is a TAP at once.  A chip double flag arriving
 *    after its first tap was already reported counts as one more TAP, not a DTAP.
 *    A DTAP only appears if the chip reports both flags before a single was seen. */
int ni_tap_update(ni_tap *t, const ni_tap_cfg *c, int single, int dbl, uint32_t now);

/* ---- polled accelerometer tap detector (R4, LIS3DSH; r5.1: in nori_motion.c, with the axis gate) ----
 * For chips without a usable tap engine.  Samples in mg at a fixed rate.  A slow
 * per-axis baseline (first-order low-pass, time constant 2^hp_shift samples)
 * tracks gravity and posture; a hit is the high-passed residual's magnitude
 * rising above ths_mg while armed.  After a hit, and after a reset, the detector
 * stays disarmed until the residual has stayed below rearm_mg for quiet_samples
 * in a row: the ringing of one knock is one hit, sustained vibration gives none,
 * and slow tilting never builds a residual. */
typedef struct {
    uint16_t ths_mg;           /* hit threshold on |residual| */
    uint16_t rearm_mg;         /* quiet level needed to re-arm (< ths_mg) */
    uint16_t quiet_samples;    /* consecutive quiet samples to re-arm */
    uint16_t warmup_samples;   /* after a reset: baseline settles, no hits */
    uint8_t hp_shift;          /* baseline time constant = 2^hp_shift samples */
    /* r5.1 optional axis gate (see ni_motion_sample).  ni_motion_defaults() leaves it off. */
    uint8_t axis;              /* NI_AXIS_ANY (gate off: r5 behaviour), NI_AXIS_X, NI_AXIS_Y, NI_AXIS_Z */
    uint8_t axis_pct;          /* the chosen axis must carry >= axis_pct % of |residual| at the peak */
    uint8_t peak_samples;      /* the peak is the largest |residual| in this many samples from the crossing (>= 1) */
    uint16_t reject_hold_samples;   /* after a rejected candidate, candidates starting within this many samples are held */
    /* r5.2 baseline hold (0 = off, r5/r5.1 behaviour; ni_motion_defaults() leaves it off).  From a
     * threshold crossing, for at most this many samples, a sample whose |residual| is >= rearm_mg does
     * not move the baseline: the knock's own pulse no longer drags the high-pass reference, so its
     * ring is measured against the pre-knock posture and the detector re-arms as soon as the ring has
     * died out (see GAME-MODE.md).  Quiet samples and every sample after the hold update it as usual. */
    uint16_t freeze_samples;
    /* r5.2 onset re-trigger (0 = off, r5/r5.1 behaviour).  While disarmed, and at least retrig_min
     * samples after the last crossing, a sample above ths_mg AND above 3x the largest |residual| of
     * the previous retrig_window samples (<= NI_RETRIG_MAX) is a new knock: it is handled exactly
     * like a crossing of the armed detector.  A decaying ring never grows 3x over its own recent
     * peak, a fresh impulse does; this breaks the lock-out where knocks arriving faster than the ring
     * dies out would each keep the detector disarmed (GAME-MODE.md). */
    uint8_t retrig_window;
    uint16_t retrig_min;
} ni_motion_cfg;
#define NI_RETRIG_MAX 16

enum { NI_AXIS_ANY = 0, NI_AXIS_X = 1, NI_AXIS_Y = 2, NI_AXIS_Z = 3 };
enum { NI_CAND_NONE = 0, NI_CAND_PENDING, NI_CAND_ACCEPTED, NI_CAND_HELD };

/* The last finished candidate (diagnostics). */
typedef struct {
    char verdict;              /* 0 none yet, 'A' accepted, 'R' rejected by the axis gate, 'H' held (reject hold) */
    uint8_t share[3];          /* |x|,|y|,|z| as % of |residual| at the peak */
    uint32_t mg;               /* |residual| at the peak */
} ni_peak;

typedef struct {
    int32_t base[3];           /* baseline, mg * 256 */
    uint32_t n;                /* samples since reset */
    uint16_t quiet;            /* current run of quiet samples */
    uint8_t armed;
    uint32_t hits;             /* diagnostics */
    uint32_t peak_mg;          /* largest residual since the caller last cleared it */
    /* r5.1 */
    uint8_t cand;              /* NI_CAND_*: a candidate whose peak is still being tracked */
    uint8_t cand_n;            /* samples of it seen so far */
    int64_t cand_r2;           /* largest |residual|^2 so far, and its components (mg) */
    int32_t cand_v[3];
    uint16_t hold;             /* samples left in the reject hold */
    uint32_t rejects;          /* diagnostics: candidates refused by the axis gate */
    uint32_t held;             /* diagnostics: candidates dropped inside a reject hold */
    uint32_t dom[3];           /* finished candidates by dominant axis since the caller last cleared it */
    ni_peak last;
    uint16_t frz;              /* r5.2: samples left in the baseline hold */
    uint16_t since;            /* r5.2: samples since the last crossing (saturating) */
    uint8_t wpos;              /* r5.2: |residual|^2 history for the onset re-trigger */
    int64_t win[NI_RETRIG_MAX];
    uint32_t retrigs;          /* r5.2 diagnostics: crossings found by the onset re-trigger */
} ni_motion;

/* Defaults for a sample rate (Hz): ths 1000 mg, rearm 40 % of ths, 40 ms quiet,
 * 160 ms warm-up, ~40 ms baseline.  r5.1: axis gate off (NI_AXIS_ANY), axis_pct 70,
 * 10 ms peak window, 80 ms reject hold. */
void ni_motion_defaults(ni_motion_cfg *c, uint16_t odr_hz);
void ni_motion_reset(ni_motion *m);   /* after a gap in the sample stream (a pending candidate is dropped) */
/* One sample (mg).  Returns 1 on a hit, else 0.
 * r5.1 axis gate.  A candidate starts where the armed detector crosses ths_mg (exactly
 * the r5 hit point; arming and quiet re-arm are unchanged).
 *  - axis == NI_AXIS_ANY: the hit is returned at the crossing, as in r5 (bit-identical
 *    results); the following samples are only watched for the diagnostics in m->last.
 *  - otherwise the decision waits for the peak, the largest |residual| among the
 *    peak_samples samples starting at the crossing, and the hit is returned on the last
 *    of them only if |residual[axis]| >= axis_pct % of |residual| there (sign-agnostic).
 *    A refused candidate counts in m->rejects, never as a hit, and starts a hold of
 *    reject_hold_samples during which a new candidate is dropped (m->held), so a refused
 *    bump cannot be followed by a tap from its own aftermath any sooner than a reported
 *    tap could (the firmware sets the hold to the tap refractory time). */
int ni_motion_sample(ni_motion *m, const ni_motion_cfg *c, int32_t x, int32_t y, int32_t z);
/* r5.1 DIAG tokens: "ax=<any|x|y|z>/<pct> rej=<n> held=<n> pk=<V>/x<%>/y<%>/z<%>/<mg> dom=<x>/<y>/<z>"
 * (pk=- before the first candidate).  Clears dom (like the caller clears peak_mg).
 * Returns the snprintf-style length; never writes past cap. */
int ni_motion_diag(ni_motion *m, const ni_motion_cfg *c, char *out, size_t cap);

/* ---- token bucket: at most `burst` events, refilled at `per_sec` ---- */
typedef struct { uint32_t milli; uint32_t last; uint8_t init; } ni_bucket;
int ni_bucket_take(ni_bucket *b, uint32_t now, uint16_t per_sec, uint8_t burst);

#ifdef __cplusplus
}
#endif
#endif
