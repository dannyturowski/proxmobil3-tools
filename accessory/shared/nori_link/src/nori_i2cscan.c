/* r5.1 I2C wiring diagnostic; see nori_i2cscan.h. */
#include "nori_i2cscan.h"

#include <stdio.h>
#include <string.h>

#define HALF_US 5                   /* recovery clock half period: ~100 kHz */

static void recover(const nsc_hal *h, nsc_result *r)
{
    h->drive(h->ctx, NSC_SDA, 1);                   /* SDA released (open drain, pull-up) */
    for (int i = 0; i < 9 && !h->sense(h->ctx, NSC_SDA); i++) {
        h->drive(h->ctx, NSC_SCL, 0); h->wait_us(h->ctx, HALF_US);
        h->drive(h->ctx, NSC_SCL, 1); h->wait_us(h->ctx, HALF_US);
        r->clocks++;
    }
    /* STOP: SDA low while SCL is low, SCL high, then SDA high */
    h->drive(h->ctx, NSC_SCL, 0); h->wait_us(h->ctx, HALF_US);
    h->drive(h->ctx, NSC_SDA, 0); h->wait_us(h->ctx, HALF_US);
    h->drive(h->ctx, NSC_SCL, 1); h->wait_us(h->ctx, HALF_US);
    h->drive(h->ctx, NSC_SDA, 1); h->wait_us(h->ctx, HALF_US);
    r->rec = (int8_t)(h->level(h->ctx, NSC_SDA, NSC_PULL_UP) && h->level(h->ctx, NSC_SCL, NSC_PULL_UP));
}

void nsc_run(const nsc_hal *h, uint32_t hz, uint32_t budget_ms, nsc_result *r)
{
    memset(r, 0, sizeof *r);
    r->rec = -1;
    uint32_t t0 = h->ms(h->ctx);
    r->sda_pd = (uint8_t)(h->level(h->ctx, NSC_SDA, NSC_PULL_DOWN) != 0);
    r->sda_pu = (uint8_t)(h->level(h->ctx, NSC_SDA, NSC_PULL_UP) != 0);
    r->scl_pd = (uint8_t)(h->level(h->ctx, NSC_SCL, NSC_PULL_DOWN) != 0);
    r->scl_pu = (uint8_t)(h->level(h->ctx, NSC_SCL, NSC_PULL_UP) != 0);
    r->stuck = (uint8_t)((!r->sda_pu ? 1 : 0) | (!r->scl_pu ? 2 : 0));
    if (r->stuck & 2) r->skipped = 1;               /* SCL low: cannot clock, nothing to probe with */
    else if (r->stuck & 1) {
        recover(h, r);
        if (r->rec != 1) r->skipped = 1;
    }
    if (!h->bus_begin(h->ctx, hz)) r->skipped = 1;   /* always restart the master, scan or not */
    int run = 0;
    for (unsigned a = NSC_FIRST; !r->skipped && a <= NSC_LAST; a++) {
        if (h->ms(h->ctx) - t0 >= budget_ms) { r->cut = 1; break; }
        int p = h->probe(h->ctx, (uint8_t)a);
        if (p > 0) { r->found[r->nfound++] = (uint8_t)a; run = 0; }
        else if (p < 0) {
            r->errors++;
            if (++run >= 3) { r->cut = 1; break; }
        } else run = 0;
    }
    r->ms = h->ms(h->ctx) - t0;
}

int nsc_format(const nsc_result *r, uint32_t hz, char *out, size_t cap)
{
    if (!cap) return 0;
    size_t n = 0;
    out[0] = 0;
#define PUT(...) do { if (n < cap) { int k_ = snprintf(out + n, cap - n, __VA_ARGS__); if (k_ > 0) n += (size_t)k_; if (n >= cap) n = cap - 1; } } while (0)
    PUT("sda=%u/%u scl=%u/%u", r->sda_pd, r->sda_pu, r->scl_pd, r->scl_pu);
    if (r->stuck) PUT(" stuck=%s", r->stuck == 3 ? "sda,scl" : r->stuck == 1 ? "sda" : "scl");
    if (r->rec >= 0) PUT(" rec=%s/%u", r->rec ? "ok" : "fail", r->clocks);
    PUT(" hz=%lu found=", (unsigned long)(hz / 1000));
    if (r->skipped) PUT("skipped");
    else if (!r->nfound) PUT("none");
    else {
        for (int i = 0; i < r->nfound && i < NSC_LIST_MAX; i++) PUT("%s%02x", i ? "," : "", r->found[i]);
        if (r->nfound > NSC_LIST_MAX) PUT("+%u", (unsigned)(r->nfound - NSC_LIST_MAX));
    }
    if (r->errors || r->cut) PUT(" err=%u%s", r->errors, r->cut ? "/cut" : "");
    PUT(" ms=%lu", (unsigned long)r->ms);
#undef PUT
    return (int)n;
}
