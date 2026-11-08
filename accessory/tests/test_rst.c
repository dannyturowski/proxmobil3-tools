/* wt32-r2 reset diagnostics, ported unchanged to wt32-r3 (shared/nori_sense/src/nori_rst.h) on the host:
 *   - an RTC record holding garbage, a wrong magic or a bad checksum restarts with this boot ("new");
 *   - a valid record keeps its history across boots, most recent first, the oldest dropped after NR_HIST;
 *   - reason names, out-of-range codes;
 *   - nr_json() / nr_diag() worst cases fit NR_JSON_MAX / NR_DIAG_MAX (the firmware sizes its /status,
 *     /net and DIAG buffers with them) and the DIAG tokens are valid link tokens. */
#include "nori_rst.h"
#include "nori_link.h"

#include <stdio.h>
#include <string.h>

static int checks, fails;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

static int tokens_ok(const char *s)        /* space-separated link tokens, as nd_dev joins them */
{
    const char *p = s;
    while (*p) {
        const char *e = strchr(p, ' ');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (!n || !nl_token_ok(p, n)) return 0;
        p += n + (e ? 1 : 0);
    }
    return 1;
}

int main(void)
{
    nr_rtc r;
    char j[NR_JSON_MAX + 64], g[NR_DIAG_MAX + 64], h[64];

    /* power-on garbage */
    memset(&r, 0xa5, sizeof r);
    check("garbage: record restarts", nr_boot(&r, 1) == 0);
    check("garbage: one entry, boots=1", r.boots == 1 && r.hist[0] == 1 && r.hist[1] == NR_EMPTY);
    nr_hist_str(&r, ',', h, sizeof h);
    check("hist of one", !strcmp(h, "1"));

    /* software / watchdog resets keep it */
    check("sw reset: kept", nr_boot(&r, 3) == 1);
    check("task_wdt reset: kept", nr_boot(&r, 6) == 1);
    nr_hist_str(&r, ',', h, sizeof h);
    check("most recent first", !strcmp(h, "6,3,1") && r.boots == 3);
    nr_hist_str(&r, '.', h, sizeof h);
    check("DIAG separator", !strcmp(h, "6.3.1"));

    /* the ring keeps NR_HIST */
    for (int i = 0; i < 10; i++) nr_boot(&r, 4);
    nr_hist_str(&r, ',', h, sizeof h);
    check("ring keeps NR_HIST entries", !strcmp(h, "4,4,4,4,4,4,4,4") && r.boots == 13);

    /* corruption: any byte, magic, checksum, zero boots */
    nr_rtc c = r;
    c.hist[3] ^= 1;
    check("flipped history byte: restarts", nr_boot(&c, 3) == 0 && c.boots == 1);
    c = r;
    c.boots++;
    check("changed count: restarts", nr_boot(&c, 3) == 0 && c.boots == 1);
    c = r;
    c.magic = 0x4e525330u;
    c.check = nr_sum(&c);
    check("old/other magic: restarts", nr_boot(&c, 3) == 0);
    c = r;
    c.check ^= 0x80000000u;
    check("bad checksum: restarts", nr_boot(&c, 3) == 0);
    memset(&c, 0, sizeof c);
    check("all zero (check of zeros is not 0): restarts", nr_boot(&c, 1) == 0 && c.boots == 1);

    /* boots saturates */
    c = r;
    c.boots = UINT32_MAX;
    c.check = nr_sum(&c);
    check("boots saturates", nr_boot(&c, 3) == 1 && c.boots == UINT32_MAX);

    /* names */
    check("names", !strcmp(nr_name(0), "unknown") && !strcmp(nr_name(1), "poweron") && !strcmp(nr_name(3), "sw") &&
                   !strcmp(nr_name(4), "panic") && !strcmp(nr_name(5), "int_wdt") && !strcmp(nr_name(6), "task_wdt") &&
                   !strcmp(nr_name(7), "wdt") && !strcmp(nr_name(9), "brownout") && !strcmp(nr_name(10), "sdio"));
    check("out of range name", !strcmp(nr_name(11), "?") && !strcmp(nr_name(255), "?"));

    /* formatting, typical */
    memset(&r, 0, sizeof r);
    nr_boot(&r, 1);
    nr_boot(&r, 3);
    nr_boot(&r, 3);
    nr_json(&r, 0x5eed0002u, 3, 12, 1, j, sizeof j);
    check("json typical", !strcmp(j, "\"boot\":\"5eed0002\",\"rst\":3,\"rst_name\":\"sw\",\"rst_hw\":12,"
                                     "\"rst_hist\":\"3,3,1\",\"rst_boots\":3,\"rst_rtc\":\"kept\""));
    nr_diag(&r, 3, g, sizeof g);
    check("diag typical", !strcmp(g, "rst=3/sw rh=3.3.1 rb=3") && tokens_ok(g));

    /* worst case: longest name, two-digit codes everywhere, 10-digit count, out-of-range reason */
    memset(r.hist, 10, sizeof r.hist);
    r.boots = UINT32_MAX;
    r.check = nr_sum(&r);
    int jn = nr_json(&r, 0xffffffffu, 6, 4294967295u, 0, j, sizeof j);
    check("json worst case fits NR_JSON_MAX", jn > 0 && jn < NR_JSON_MAX);
    int jn2 = nr_json(&r, 0xffffffffu, 4294967295u, 4294967295u, 0, j, sizeof j);
    check("json, absurd reason, fits NR_JSON_MAX", jn2 > 0 && jn2 < NR_JSON_MAX);
    check("json carries \"new\" and the full history", strstr(j, "\"rst_rtc\":\"new\"") && strstr(j, "\"rst_hist\":\"10,10,10,10,10,10,10,10\""));
    int gn = nr_diag(&r, 6, g, sizeof g);
    check("diag worst case fits NR_DIAG_MAX", gn > 0 && gn < NR_DIAG_MAX && tokens_ok(g));
    int gn2 = nr_diag(&r, 4294967295u, g, sizeof g);
    check("diag, absurd reason, fits NR_DIAG_MAX", gn2 > 0 && gn2 < NR_DIAG_MAX && tokens_ok(g));
    printf("  worst: json %d (NR_JSON_MAX %d), diag %d (NR_DIAG_MAX %d)\n", jn2, NR_JSON_MAX, gn2, NR_DIAG_MAX);

    /* small buffers never overflow */
    char tiny[5];
    nr_hist_str(&r, ',', tiny, sizeof tiny);
    check("hist in a small buffer: whole entries only", !strcmp(tiny, "10"));
    nr_json(&r, 1, 3, 12, 1, tiny, sizeof tiny);
    check("json in a small buffer: terminated", strlen(tiny) == 4);

    printf("test_rst: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
