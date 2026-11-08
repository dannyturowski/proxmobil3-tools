/* wt32-r3 presence detector; see nori_presence.h. */
#include "nori_presence.h"

#include <stdio.h>
#include <string.h>

void np_defaults(np_cfg *c)
{
    c->near_mm = NP_NEAR_MM_DEF;
    c->far_mm = NP_FAR_MM_DEF;
    c->min_mm = NP_MIN_MM_DEF;
    c->min_zones = NP_MIN_ZONES_DEF;
    c->on_frames = NP_ON_FRAMES_DEF;
    c->off_frames = NP_OFF_FRAMES_DEF;
    c->stale_ms = NP_STALE_MS_DEF;
}

int np_cfg_valid(const np_cfg *c)
{
    return c->near_mm >= 100 && c->near_mm <= NP_MAX_MM && c->far_mm >= c->near_mm && c->far_mm <= NP_MAX_MM &&
           c->min_mm <= 500 && c->min_mm < c->near_mm && c->min_zones >= 1 && c->min_zones <= 16 &&
           c->on_frames >= 1 && c->on_frames <= 20 && c->off_frames >= 1 && c->off_frames <= 50 &&
           c->stale_ms >= 500 && c->stale_ms <= 10000;
}

int np_cfg_set(np_cfg *c, const char *key, uint32_t v)
{
    if (!strcmp(key, "near") && v >= 100 && v <= NP_MAX_MM) c->near_mm = (uint16_t)v;
    else if (!strcmp(key, "far") && v >= 100 && v <= NP_MAX_MM) c->far_mm = (uint16_t)v;
    else if (!strcmp(key, "min") && v <= 500) c->min_mm = (uint16_t)v;
    else if (!strcmp(key, "zones") && v >= 1 && v <= 16) c->min_zones = (uint8_t)v;
    else if (!strcmp(key, "on") && v >= 1 && v <= 20) c->on_frames = (uint8_t)v;
    else if (!strcmp(key, "off") && v >= 1 && v <= 50) c->off_frames = (uint8_t)v;
    else if (!strcmp(key, "stale") && v >= 500 && v <= 10000) c->stale_ms = (uint16_t)v;
    else return -1;
    return 0;
}

int np_cfg_format(const np_cfg *c, char *out, size_t cap)
{
    return snprintf(out, cap, "near=%u far=%u min=%u zones=%u on=%u off=%u stale=%u", c->near_mm, c->far_mm, c->min_mm,
                    c->min_zones, c->on_frames, c->off_frames, c->stale_ms);
}

void np_init(np_state *s)
{
    memset(s, 0, sizeof *s);
    s->nearest_mm = -1;
}

int np_zone_valid(const np_cfg *c, int16_t mm, uint8_t status, int nb_target)
{
    if (nb_target == 0) return 0;                    /* < 0: not reported */
    if (status != 5 && status != 9) return 0;
    return mm >= (int)c->min_mm && mm <= NP_MAX_MM;
}

static int set_level(np_state *s, int level, uint32_t now)
{
    if (level == s->level) return 0;
    s->level = level;
    s->level_since_ms = now;
    s->on_run = s->off_run = 0;
    if (level) { s->rises++; return 1; }
    s->falls++;
    return -1;
}

void np_bad_frame(np_state *s)
{
    s->bad_frames++;
    s->on_run = 0;                                   /* a rise needs consecutive good near frames */
    s->grid_ok = 0;
}

int np_tick(np_state *s, const np_cfg *c, uint32_t now)
{
    if (!s->level || !s->have_frame || now - s->last_good_ms < c->stale_ms) return 0;
    s->stale_falls++;
    return set_level(s, 0, now);
}

int np_frame_update(np_state *s, const np_cfg *c, const np_frame *f, uint32_t now)
{
    if (!f || f->zones == 0 || f->zones > NP_ZONES_MAX || !f->distance_mm || !f->target_status) {
        np_bad_frame(s);
        return 0;
    }
    int lvl0 = s->level;
    np_tick(s, c, now);                              /* a long gap before this frame ends a held presence first */
    if (s->have_frame && now - s->last_good_ms >= c->stale_ms) s->on_run = 0;   /* and a rise starts over */
    int valid = 0, nearz = 0, held = 0, nearest = -1, grid = f->zones == NP_GRID_ZONES;
    for (unsigned i = 0; i < f->zones; i++) {
        int nt = f->nb_target ? f->nb_target[i] : -1;
        int16_t mm = f->distance_mm[i];
        if (grid) s->grid_mm[i] = -1;
        if (!np_zone_valid(c, mm, f->target_status[i], nt)) {
            if (nt != 0 && f->target_status[i] != 255) s->zones_rejected++;
            continue;
        }
        if (grid) s->grid_mm[i] = mm;
        valid++;
        if (nearest < 0 || mm < nearest) nearest = mm;
        if (mm <= (int)c->near_mm) nearz++;
        if (mm <= (int)c->far_mm) held++;
    }
    s->frames++;
    s->have_frame = 1;
    s->last_good_ms = now;
    s->nearest_mm = (int16_t)nearest;
    s->valid_zones = (uint8_t)valid;
    s->near_zones = (uint8_t)nearz;
    s->held_zones = (uint8_t)held;
    s->grid_ok = grid;
    if (!s->level) {
        s->on_run = nearz >= c->min_zones ? (uint8_t)(s->on_run < 255 ? s->on_run + 1 : 255) : 0;
        if (s->on_run >= c->on_frames) set_level(s, 1, now);
    } else {
        s->off_run = held < c->min_zones ? (uint8_t)(s->off_run < 255 ? s->off_run + 1 : 255) : 0;
        if (s->off_run >= c->off_frames) set_level(s, 0, now);
    }
    return s->level - lvl0;                          /* net change over the whole call (stale fall + rise = 0) */
}

int np_fault(np_state *s, uint32_t now)
{
    int r = 0;
    if (s->level) { s->fault_falls++; r = set_level(s, 0, now); }
    s->on_run = s->off_run = 0;
    s->have_frame = 0;
    s->grid_ok = 0;
    s->nearest_mm = -1;
    s->valid_zones = s->near_zones = s->held_zones = 0;
    return r;
}

int np_effective(const np_state *s, const np_cfg *c, uint32_t now, uint32_t margin_ms)
{
    return s->level && s->have_frame && now - s->last_good_ms < (uint32_t)c->stale_ms + margin_ms;
}

int np_grid_format(const np_state *s, uint32_t now, char *out, size_t cap)
{
    if (cap < NP_GRID_TEXT_MAX) { if (cap) out[0] = 0; return -1; }
    uint32_t age = now - s->last_good_ms;
    if (!s->grid_ok || !s->have_frame || age >= NP_GRID_STALE_MS)
        return snprintf(out, cap, "zf=%lu za=- zg=-", (unsigned long)s->frames);
    int n = snprintf(out, cap, "zf=%lu za=%lu zg=", (unsigned long)s->frames, (unsigned long)age);
    for (int i = 0; i < NP_GRID_ZONES; i++)
        n += snprintf(out + n, cap - (size_t)n, "%03x", s->grid_mm[i] < 0 ? NP_GRID_NONE : (unsigned)s->grid_mm[i]);
    return n;
}
