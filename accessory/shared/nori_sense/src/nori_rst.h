/* wt32-r3 reset diagnostics (ported from wt32-r2's firmware/src/nori_rst.h, unchanged history record,
 * plus a previous-boot trace).  Both records live in RTC slow memory (RTC_NOINIT_ATTR in main.cpp).
 *
 * BEST EFFORT, by design:
 *  - the records are plain RAM, never flash/NVS.  Power loss, and anything else that takes RTC memory
 *    down (on the ESP32 an EN/CHIP_PU pulse looks like a power-on), loses them or leaves garbage; garbage
 *    fails the magic + checksum test and the record starts over with this boot ("new").
 *  - surviving content is no proof that power stayed up, and lost content is no proof that it failed.
 *  - a reset during the few instructions of an update can drop or repeat one entry, or invalidate the
 *    trace (its checksum then fails and it reads as "none").
 *
 * nr_rtc: the reset reason of the last NR_HIST boots (most recent first) and a boot counter.
 * nr_trace: what the PREVIOUS boot looked like just before it ended: its boot id (= HELLO boot=), its
 *   uptime and lowest free heap (updated every second) and its last NR_TRACE_EV transition events.
 *   At boot the old trace is copied out (nr_trace_take) and the record restarts for this boot.
 *
 * Reason codes are esp_reset_reason_t values (ESP-IDF 4.4: 0 unknown .. 10 sdio; main.cpp checks the
 * table at compile time).  Portable C, no allocation: tests/test_sense.c runs it on the host. */
#ifndef NORI_RST_H
#define NORI_RST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nori_evlog.h"

#define NR_MAGIC   0x4e525331u   /* "NRS1": the wt32-r2 layout, unchanged */
#define NR_HIST    8             /* most recent first */
#define NR_EMPTY   0xffu
#define NR_JSON_MAX 160          /* nr_json() worst case incl. NUL; tests check it */
#define NR_DIAG_MAX 64           /* nr_diag() worst case incl. NUL */

typedef struct {
    uint32_t magic;
    uint32_t boots;              /* boots recorded since the record was (re)started, this one included */
    uint8_t hist[NR_HIST];       /* reason codes, hist[0] = this boot; NR_EMPTY = no entry */
    uint32_t check;
} nr_rtc;

static const char *const NR_NAME[] = { "unknown", "poweron", "ext", "sw", "panic", "int_wdt",
                                       "task_wdt", "wdt", "deepsleep", "brownout", "sdio" };
#define NR_NAMES (sizeof NR_NAME / sizeof NR_NAME[0])

static inline const char *nr_name(unsigned reason) { return reason < NR_NAMES ? NR_NAME[reason] : "?"; }

static inline uint32_t nr_fnv(const void *p_, size_t n)   /* FNV-1a */
{
    const uint8_t *p = (const uint8_t *)p_;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static inline uint32_t nr_sum(const nr_rtc *r) { return nr_fnv(r, offsetof(nr_rtc, check)); }

/* Call once per boot.  Returns 1 if the earlier history was valid and kept, 0 if the record restarted. */
static inline int nr_boot(nr_rtc *r, uint8_t reason)
{
    int kept = r->magic == NR_MAGIC && r->check == nr_sum(r) && r->boots != 0;
    if (!kept) {
        memset(r, 0, sizeof *r);
        r->magic = NR_MAGIC;
        memset(r->hist, NR_EMPTY, sizeof r->hist);
    }
    memmove(r->hist + 1, r->hist, NR_HIST - 1);
    r->hist[0] = reason;
    if (r->boots != UINT32_MAX) r->boots++;
    r->check = nr_sum(r);
    return kept;
}

/* "3,3,1": codes most recent first, joined by sep. */
static inline int nr_hist_str(const nr_rtc *r, char sep, char *out, size_t cap)
{
    size_t n = 0;
    if (!cap) return 0;
    out[0] = 0;
    for (int i = 0; i < NR_HIST && r->hist[i] != NR_EMPTY; i++) {
        int k = i ? snprintf(out + n, cap - n, "%c%u", sep, (unsigned)r->hist[i]) : snprintf(out, cap, "%u", (unsigned)r->hist[i]);
        if (k < 0 || (size_t)k >= cap - n) break;
        n += (size_t)k;
    }
    out[n] = 0;
    return (int)n;
}

/* JSON members (no braces, no leading/trailing comma) for /status and /diag:
 *   "boot":"5eed0002","rst":3,"rst_name":"sw","rst_hw":12,"rst_hist":"3,3,1","rst_boots":3,"rst_rtc":"kept" */
static inline int nr_json(const nr_rtc *r, uint32_t boot_id, unsigned reason, unsigned hw, int kept, char *out, size_t cap)
{
    char h[NR_HIST * 4];
    nr_hist_str(r, ',', h, sizeof h);
    return snprintf(out, cap, "\"boot\":\"%08lx\",\"rst\":%u,\"rst_name\":\"%s\",\"rst_hw\":%u,\"rst_hist\":\"%s\",\"rst_boots\":%lu,\"rst_rtc\":\"%s\"",
                    (unsigned long)boot_id, reason, nr_name(reason), hw, h, (unsigned long)r->boots, kept ? "kept" : "new");
}

/* DIAG tokens: "rst=3/sw rh=3.3.1 rb=3" (rb: boots in the RTC record; rb=1 after a restart of the record). */
static inline int nr_diag(const nr_rtc *r, unsigned reason, char *out, size_t cap)
{
    char h[NR_HIST * 4];
    nr_hist_str(r, '.', h, sizeof h);
    return snprintf(out, cap, "rst=%u/%s rh=%s rb=%lu", reason, nr_name(reason), h, (unsigned long)r->boots);
}

/* ---- previous-boot trace ---- */
#define NR_TRACE_MAGIC 0x4e525431u   /* "NRT1" */
#define NR_TRACE_EV 16

typedef struct {
    uint32_t magic;
    uint32_t boot_id;
    uint32_t up_s;               /* uptime at the last update */
    uint32_t heap_min;           /* lowest free heap seen (bytes) */
    uint32_t ev_total;           /* events added this boot (the ring keeps the last NR_TRACE_EV) */
    nlg_ev ev[NR_TRACE_EV];
    uint32_t check;
} nr_trace;

static inline uint32_t nr_trace_sum(const nr_trace *t) { return nr_fnv(t, offsetof(nr_trace, check)); }
static inline int nr_trace_valid(const nr_trace *t) { return t->magic == NR_TRACE_MAGIC && t->check == nr_trace_sum(t); }

/* At boot: copy the previous boot's trace to *prev (returns 1) or report none (0), then restart it. */
static inline int nr_trace_take(nr_trace *rtc, nr_trace *prev, uint32_t boot_id)
{
    int ok = nr_trace_valid(rtc);
    if (ok) *prev = *rtc;
    else memset(prev, 0, sizeof *prev);
    memset(rtc, 0, sizeof *rtc);
    rtc->magic = NR_TRACE_MAGIC;
    rtc->boot_id = boot_id;
    rtc->heap_min = UINT32_MAX;
    rtc->check = nr_trace_sum(rtc);
    return ok;
}

static inline void nr_trace_ev(nr_trace *t, const nlg_ev *e)
{
    t->ev[t->ev_total % NR_TRACE_EV] = *e;
    t->ev_total++;
    t->check = nr_trace_sum(t);
}

static inline void nr_trace_tick(nr_trace *t, uint32_t up_s, uint32_t heap_free)
{
    t->up_s = up_s;
    if (heap_free < t->heap_min) t->heap_min = heap_free;
    t->check = nr_trace_sum(t);
}

/* The kept events of a trace, oldest first; returns how many. */
static inline unsigned nr_trace_events(const nr_trace *t, nlg_ev *out)
{
    unsigned k = t->ev_total < NR_TRACE_EV ? (unsigned)t->ev_total : NR_TRACE_EV;
    for (unsigned i = 0; i < k; i++) out[i] = t->ev[(t->ev_total - k + i) % NR_TRACE_EV];
    return k;
}

#endif
