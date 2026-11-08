/* wt32-r5: the three-way track selector, as the firmware builds it (-DND_DIAG_JOIN=1 -DND_PRESENCE=1 -DND_EXT=1).
 *   - ni_toggle: boot position silent, debounce, held contact, centre return, direct direction change,
 *     chatter, both-low rejection and its lock-out, millisecond clock wrap;
 *   - nd_toggle: EV trk5 / trk1 frames, only while a host talks, no replay on a new host session;
 *   - nori_host: accepts them; an old host (no such names) ignores the frame and keeps its sequence;
 *   - wt32-r5.1.1 ni_gesture and nd_selector: short and long results, on their own and on the wire. */
#include "nori_dev.h"
#include "nori_host.h"

#include <stdio.h>
#include <string.h>

#if !ND_DIAG_JOIN || !ND_PRESENCE || !ND_EXT || ND_TAP_AXIS || ND_I2CSCAN || ND_GAME
#error "build with -DND_DIAG_JOIN=1 -DND_PRESENCE=1 -DND_EXT=1 (the wt32 firmware's flags)"
#endif

#define DEB 30

static int checks, fails;
static void check(const char *what, int ok)
{
    checks++;
    if (!ok) { fails++; printf("FAIL %s\n", what); }
}

/* Hold a raw position for ms, sampling every 5 ms like loop(); returns the reports seen. */
typedef struct { int five, one; } seen;
static seen run(ni_toggle *t, uint32_t *now, int f, int o, uint32_t ms)
{
    seen s = { 0, 0 };
    for (uint32_t e = 0; e < ms; e += 5, *now += 5) {
        int r = ni_toggle_update(t, f, o, *now, DEB);
        if (r == NI_TOG_FIVE) s.five++;
        else if (r == NI_TOG_ONE) s.one++;
        else if (r != NI_TOG_CENTER) check("only five, one or nothing is ever reported", 0);
    }
    return s;
}
#define IS(s, f, o) ((s).five == (f) && (s).one == (o))

static void test_debounce(void)
{
    ni_toggle t;
    uint32_t now = 1000;
    seen s;

    /* boot positions */
    ni_toggle_init(&t, 0, 0, now);
    s = run(&t, &now, 0, 0, 500);
    check("boot at the centre: silent", IS(s, 0, 0) && t.stable == NI_TOG_CENTER);
    ni_toggle_init(&t, 1, 0, now);
    s = run(&t, &now, 1, 0, 2000);
    check("boot held on five: no synthetic selection, however long", IS(s, 0, 0) && t.stable == NI_TOG_FIVE);
    s = run(&t, &now, 0, 0, 100);
    check("boot hold released: the centre says nothing", IS(s, 0, 0));
    s = run(&t, &now, 1, 0, 100);
    check("then the same direction again is a real move", IS(s, 1, 0));
    ni_toggle_init(&t, 0, 1, now);
    s = run(&t, &now, 0, 1, 2000);
    check("boot held on one: no synthetic selection", IS(s, 0, 0));
    s = run(&t, &now, 1, 0, 100);
    check("boot on one, straight to five: a real move", IS(s, 1, 0));

    /* a clean move, and when it is reported */
    ni_toggle_init(&t, 0, 0, now);
    s = run(&t, &now, 1, 0, DEB - 5);
    check("not reported before the debounce time", IS(s, 0, 0));
    s = run(&t, &now, 1, 0, 15);
    check("reported once the debounce time has passed", IS(s, 1, 0));
    s = run(&t, &now, 1, 0, 5000);
    check("held contact never repeats", IS(s, 0, 0));
    s = run(&t, &now, 0, 0, 200);
    check("centre return: no action", IS(s, 0, 0) && t.stable == NI_TOG_CENTER && t.n_center == 1);
    s = run(&t, &now, 0, 1, 200);
    check("the other contact", IS(s, 0, 1));
    s = run(&t, &now, 0, 0, 200);
    s = run(&t, &now, 0, 1, 200);
    check("same direction after the centre: a new absolute move", IS(s, 0, 1));
    check("counters", t.n_five == 1 && t.n_one == 2);

    /* direct direction change: the centre was too short to debounce */
    s = run(&t, &now, 0, 0, 10);
    check("a short pass through the centre says nothing", IS(s, 0, 0));
    s = run(&t, &now, 1, 0, 100);
    check("direct one -> five reports five", IS(s, 1, 0));

    /* chatter */
    ni_toggle_init(&t, 0, 0, now);
    for (int i = 0; i < 12; i++) {
        s = run(&t, &now, i & 1 ? 0 : 1, 0, 10);
        check("contact bounce on arrival reports nothing", IS(s, 0, 0));
    }
    s = run(&t, &now, 1, 0, 100);
    check("settled: exactly one report", IS(s, 1, 0));
    for (int i = 0; i < 12; i++) {
        s = run(&t, &now, i & 1 ? 1 : 0, 0, 10);
        check("dropouts shorter than the debounce on a held contact: no repeat", IS(s, 0, 0));
    }
    s = run(&t, &now, 1, 0, 200);
    check("held after the dropouts: no repeat", IS(s, 0, 0) && t.n_five == 1);

    /* both low */
    ni_toggle_init(&t, 0, 0, now);
    s = run(&t, &now, 1, 1, 500);
    check("both low: rejected", IS(s, 0, 0) && t.stable == NI_TOG_BOTH && t.locked && t.n_both == 1);
    s = run(&t, &now, 1, 0, 200);
    check("leaving both-low to five is not a choice", IS(s, 0, 0) && t.n_locked == 1 && t.locked);
    s = run(&t, &now, 0, 1, 200);
    check("nor to one, until the centre", IS(s, 0, 0) && t.n_locked == 2);
    s = run(&t, &now, 0, 0, 200);
    check("the centre clears the lock", !t.locked && IS(s, 0, 0));
    s = run(&t, &now, 0, 1, 200);
    check("then moves count again", IS(s, 0, 1));
    s = run(&t, &now, 1, 1, 10);
    s = run(&t, &now, 0, 1, 200);
    check("a both-low blip shorter than the debounce changes nothing", IS(s, 0, 0) && !t.locked);
    ni_toggle_init(&t, 1, 1, now);
    check("both low at boot: locked", t.locked && t.stable == NI_TOG_BOTH);
    s = run(&t, &now, 1, 0, 200);
    check("both low at boot, then five: refused", IS(s, 0, 0));
    s = run(&t, &now, 0, 0, 200);
    s = run(&t, &now, 1, 0, 200);
    check("after the centre: accepted", IS(s, 1, 0));

    /* the millisecond clock wraps after 49 days */
    now = 0xFFFFFFF0u;
    ni_toggle_init(&t, 0, 0, now);
    s = run(&t, &now, 1, 0, 100);
    check("debounce across the clock wrap", IS(s, 1, 0));
}

/* ---- the link ---- */
static char uart[8192];
static size_t uart_n;
static size_t h_room(void *c) { (void)c; return sizeof uart - uart_n; }
static size_t h_queued(void *c) { (void)c; return 0; }
static void h_write(void *c, const char *s, size_t n) { (void)c; memcpy(uart + uart_n, s, n); uart_n += n; }
static const nd_hal HAL = { .uart_room = h_room, .uart_queued = h_queued, .uart_write = h_write };

static void host_frame(nd_dev *d, uint32_t *seq, const char *type, const char *args, uint32_t now)
{
    char line[NL_LINE_MAX + 2];
    int n = nl_format(line, sizeof line, ++*seq, type, args);
    nd_uart_input(d, (const uint8_t *)line, (size_t)n, now);
}
static int count(const char *needle)
{
    int k = 0;
    uart[uart_n] = 0;
    for (const char *p = uart; (p = strstr(p, needle)); p += strlen(needle)) k++;
    return k;
}

static void test_link(void)
{
    static nd_dev d;
    uint32_t seq = 0, now = 100;
    nd_init(&d, &HAL, 0x1234, "wt32-r5.0.0", "vl53l5cx", 2, NULL, now);

    check("centre / nothing: no event, no drop", nd_toggle(&d, NI_TOG_CENTER, now) == 0 && d.ev_dropped == 0);
    check("both low can never become an event", nd_toggle(&d, NI_TOG_BOTH, now) == 0 && d.ev_dropped == 0);
    check("no host yet: dropped, counted, not queued", nd_toggle(&d, NI_TOG_FIVE, now) == 0 && d.ev_dropped == 1 && d.evn == 0);

    host_frame(&d, &seq, "HOST", "proto=1 role=linkd tun=0", now);
    nd_poll(&d, now);
    check("the move made before the host came is never replayed", count("EV trk") == 0);

    now += 10; uart_n = 0;
    check("five queued", nd_toggle(&d, NI_TOG_FIVE, now) == 1);
    nd_poll(&d, now);
    check("EV trk5 on the wire", count(" EV trk5 t=110*") == 1);
    now += 10; uart_n = 0;
    check("one queued", nd_toggle(&d, NI_TOG_ONE, now) == 1);
    nd_poll(&d, now);
    check("EV trk1 on the wire", count(" EV trk1 t=120*") == 1);

    now += 500; uart_n = 0;
    host_frame(&d, &seq, "HOST", "proto=1 role=linkd tun=0", now);   /* linkd restarted, lever still held */
    nd_poll(&d, now);
    check("a new host session is not told the position (unlike presence)", count("EV trk") == 0);

    /* the rate bucket: a real flip rate gets through */
    int ok = 0;
    for (int i = 0; i < 20; i++) { now += 200; d.host_seen = now; ok += nd_toggle(&d, i & 1 ? NI_TOG_ONE : NI_TOG_FIVE, now); nd_poll(&d, now); }
    check("five flips a second are never rate limited", ok == 20);
}

/* ---- gestures: the selector as nd_selector drives it, without the link ---- */
typedef struct {
    int m5, m1, s5, s1, l5, l1;    /* moves, short and long results seen */
    uint32_t last_at;              /* when the last result came */
} gseen;

static void grun(ni_toggle *t, ni_gesture *g, gseen *c, uint32_t *now, int f, int o, uint32_t ms)
{
    for (uint32_t e = 0; e < ms; e += 5, *now += 5) {
        int pos = ni_toggle_update(t, f, o, *now, DEB);
        int r = ni_gesture_update(g, t, pos, *now);
        if (pos == NI_TOG_FIVE) c->m5++;
        if (pos == NI_TOG_ONE) c->m1++;
        if (r) c->last_at = *now;
        if (r == NI_GES_SHORT_FIVE) c->s5++;
        if (r == NI_GES_SHORT_ONE) c->s1++;
        if (r == NI_GES_LONG_FIVE) c->l5++;
        if (r == NI_GES_LONG_ONE) c->l1++;
    }
}
#define G(c, m5_, m1_, s5_, s1_, l5_, l1_) ((c).m5 == (m5_) && (c).m1 == (m1_) && (c).s5 == (s5_) && (c).s1 == (s1_) && (c).l5 == (l5_) && (c).l1 == (l1_))
#define FRESH(t, g, c, now, f, o) do { ni_toggle_init(&(t), f, o, now); ni_gesture_init(&(g), &(t)); memset(&(c), 0, sizeof (c)); } while (0)

static void test_gesture(void)
{
    ni_toggle t;
    ni_gesture g;
    gseen c;
    uint32_t now = 5000;

    /* short: on release, not before, once */
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 200);
    check("short up: the move at once, no result yet", G(c, 0, 1, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 0, 0, NI_GES_RELEASE_MS - 5);
    check("not before the release has settled", G(c, 0, 1, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 0, 0, 10);
    check("short up on release", G(c, 0, 1, 0, 1, 0, 0));
    grun(&t, &g, &c, &now, 0, 0, 3000);
    check("the centre by itself: nothing more", G(c, 0, 1, 0, 1, 0, 0));
    grun(&t, &g, &c, &now, 1, 0, 200); grun(&t, &g, &c, &now, 0, 0, 200);
    check("short down", G(c, 1, 1, 1, 1, 0, 0));

    /* long: at the threshold from the contact settling, once, no repeat, silent release */
    FRESH(t, g, c, now, 0, 0);
    uint32_t closed = now;
    grun(&t, &g, &c, &now, 0, 1, NI_GES_HOLD_MS);
    check("no hold before 1000 ms", G(c, 0, 1, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 0, 1, 5);
    check("hold at 1000 ms", G(c, 0, 1, 0, 0, 0, 1) && c.last_at - closed == NI_GES_HOLD_MS);
    grun(&t, &g, &c, &now, 0, 1, 10000);
    check("held on: no repeat", G(c, 0, 1, 0, 0, 0, 1));
    grun(&t, &g, &c, &now, 0, 0, 300);
    check("release after a hold: never a short", G(c, 0, 1, 0, 0, 0, 1));
    grun(&t, &g, &c, &now, 1, 0, 1100); grun(&t, &g, &c, &now, 0, 0, 300);
    check("hold down", G(c, 1, 1, 0, 0, 1, 1));
    grun(&t, &g, &c, &now, 1, 0, 1100); grun(&t, &g, &c, &now, 0, 0, 300);
    check("a new hold after the centre: one more", G(c, 2, 1, 0, 0, 2, 1));
    check("counters", g.n_short == 0 && g.n_long == 3 && g.n_cancel == 0);

    /* around the threshold */
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 990); grun(&t, &g, &c, &now, 0, 0, 300);
    check("990 ms: a contact opening before the threshold is a short, though debounce still sees it closed at 1000", G(c, 0, 1, 0, 1, 0, 0));
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 1010); grun(&t, &g, &c, &now, 0, 0, 300);
    check("1010 ms: long, never also short", G(c, 0, 1, 0, 0, 0, 1));
    FRESH(t, g, c, now, 0, 0);             /* the loop stalled over the threshold: raw never seen closed there */
    { uint32_t a = now;
      for (uint32_t e = 0; e < 300; e += 5, now += 5) { int pos = ni_toggle_update(&t, 0, 1, now, DEB); ni_gesture_update(&g, &t, pos, now); }
      now = a + 1500;
      grun(&t, &g, &c, &now, 0, 0, 300); }
    check("a stall across the threshold, released unseen: long, not feed", G(c, 0, 0, 0, 0, 0, 1));

    /* the boot position */
    FRESH(t, g, c, now, 0, 1);
    grun(&t, &g, &c, &now, 0, 1, 3000);
    check("boot held: silent, however long", G(c, 0, 0, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 1, 0, 2000);
    check("boot held, straight across: the move as ever, but no result until neutral", G(c, 1, 0, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 0, 0, 300);
    check("its release: nothing", G(c, 1, 0, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 1, 0, 100); grun(&t, &g, &c, &now, 0, 0, 200);
    check("after neutral: normal", G(c, 2, 0, 1, 0, 0, 0));
    FRESH(t, g, c, now, 1, 0);
    grun(&t, &g, &c, &now, 0, 0, 300); grun(&t, &g, &c, &now, 1, 0, 1100);
    check("boot on five, centre, then held: a real hold", G(c, 1, 0, 0, 0, 1, 0));

    /* both contacts low */
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 200); grun(&t, &g, &c, &now, 1, 1, 200);
    check("both low cancels a short", g.n_cancel == 1 && !g.pos);
    grun(&t, &g, &c, &now, 0, 1, 2000);
    check("out of the fault, held: nothing", G(c, 0, 1, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 0, 0, 300);
    check("and no short on release", G(c, 0, 1, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 0, 1, 1100);
    check("after the centre: normal", G(c, 0, 2, 0, 0, 0, 1));
    grun(&t, &g, &c, &now, 1, 1, 200); grun(&t, &g, &c, &now, 0, 0, 300);
    check("both low after a hold: nothing more", G(c, 0, 2, 0, 0, 0, 1));
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 300); grun(&t, &g, &c, &now, 1, 1, 10); grun(&t, &g, &c, &now, 0, 1, 300);
    grun(&t, &g, &c, &now, 0, 0, 200);
    check("a both-low blip under the debounce changes nothing", G(c, 0, 1, 0, 1, 0, 0) && !g.n_cancel);
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 985); grun(&t, &g, &c, &now, 1, 1, 40); grun(&t, &g, &c, &now, 0, 1, 500);
    check("a fault at the threshold: no hold", G(c, 0, 1, 0, 0, 0, 0));
    FRESH(t, g, c, now, 1, 1);
    grun(&t, &g, &c, &now, 1, 0, 1500); grun(&t, &g, &c, &now, 0, 0, 200);
    check("both low at boot: silent until the centre", G(c, 0, 0, 0, 0, 0, 0));

    /* bounce */
    FRESH(t, g, c, now, 0, 0);
    for (int i = 0; i < 12; i++) grun(&t, &g, &c, &now, 0, i & 1 ? 0 : 1, 10);
    check("arrival bounce: no move, no result", G(c, 0, 0, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 0, 1, 300);
    for (int i = 0; i < 12; i++) grun(&t, &g, &c, &now, 0, i & 1 ? 1 : 0, 10);
    check("dropouts under the debounce on a held contact: nothing", G(c, 0, 1, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 0, 1, 1000);
    check("and the hold still comes, once", G(c, 0, 1, 0, 0, 0, 1));
    grun(&t, &g, &c, &now, 0, 0, 5);
    for (int i = 0; i < 12; i++) grun(&t, &g, &c, &now, 0, i & 1 ? 0 : 1, 10);
    grun(&t, &g, &c, &now, 0, 0, 300);
    check("release bounce after a hold: nothing", G(c, 0, 1, 0, 0, 0, 1));

    /* brief neutral: past the debounce, inside the release time */
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 200); grun(&t, &g, &c, &now, 0, 0, 50); grun(&t, &g, &c, &now, 0, 1, 200);
    check("a brief neutral: ni_toggle sends the move again, the gesture goes on", G(c, 0, 2, 0, 0, 0, 0));
    grun(&t, &g, &c, &now, 0, 0, 200);
    check("one short for the lot", G(c, 0, 2, 0, 1, 0, 0));
    FRESH(t, g, c, now, 0, 0);
    uint32_t began = now;
    grun(&t, &g, &c, &now, 0, 1, 500); grun(&t, &g, &c, &now, 0, 0, 50); grun(&t, &g, &c, &now, 0, 1, 1000);
    check("a brief neutral inside a hold: one long, timed from the first contact", G(c, 0, 2, 0, 0, 0, 1) && c.last_at - began <= NI_GES_HOLD_MS + 60);
    grun(&t, &g, &c, &now, 0, 0, 50); grun(&t, &g, &c, &now, 0, 1, 3000);
    check("a brief neutral after the hold: no second hold", G(c, 0, 3, 0, 0, 0, 1));
    grun(&t, &g, &c, &now, 0, 0, 200);
    check("and no feed on the release", G(c, 0, 3, 0, 0, 0, 1));
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 200); grun(&t, &g, &c, &now, 0, 0, 150); grun(&t, &g, &c, &now, 0, 1, 200); grun(&t, &g, &c, &now, 0, 0, 200);
    check("a real pause between two flicks: two shorts", G(c, 0, 2, 0, 2, 0, 0));

    /* straight across */
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 200); grun(&t, &g, &c, &now, 0, 0, 10); grun(&t, &g, &c, &now, 1, 0, 200);
    check("short then straight across: the old short, then the new move", G(c, 1, 1, 0, 1, 0, 0));
    grun(&t, &g, &c, &now, 0, 0, 200);
    check("and its own short", G(c, 1, 1, 1, 1, 0, 0));
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 1100); grun(&t, &g, &c, &now, 1, 0, 1100);
    check("hold then straight across: no short for the first, a hold for the second", G(c, 1, 1, 0, 0, 1, 1));
    grun(&t, &g, &c, &now, 0, 1, 200); grun(&t, &g, &c, &now, 0, 0, 200);
    check("back across after a hold: no short for the held side", G(c, 1, 2, 0, 1, 1, 1));
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 200); grun(&t, &g, &c, &now, 0, 0, 50); grun(&t, &g, &c, &now, 1, 0, 200); grun(&t, &g, &c, &now, 0, 0, 200);
    check("across through a brief neutral: two shorts", G(c, 1, 1, 1, 1, 0, 0));

    /* cancel */
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 200);
    ni_gesture_cancel(&g);
    grun(&t, &g, &c, &now, 0, 1, 2000); grun(&t, &g, &c, &now, 1, 0, 200); grun(&t, &g, &c, &now, 0, 0, 200);
    check("cancelled: no result, even across, until the centre", G(c, 1, 1, 0, 0, 0, 0) && g.n_cancel == 1);
    grun(&t, &g, &c, &now, 0, 1, 100); grun(&t, &g, &c, &now, 0, 0, 200);
    check("then normal", G(c, 1, 2, 0, 1, 0, 0));

    /* the millisecond clock wraps */
    now = 0xFFFFFF00u;
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 1100); grun(&t, &g, &c, &now, 0, 0, 200);
    check("hold across the wrap", G(c, 0, 1, 0, 0, 0, 1));
    now = 0xFFFFFF80u;
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 300); grun(&t, &g, &c, &now, 0, 0, 200);
    check("short across the wrap", G(c, 0, 1, 0, 1, 0, 0));
    now = 0xFFFFFFA0u - 80;
    FRESH(t, g, c, now, 0, 0);
    grun(&t, &g, &c, &now, 0, 1, 80); grun(&t, &g, &c, &now, 0, 0, 200);
    check("release across the wrap", G(c, 0, 1, 0, 1, 0, 0));

    /* any noise at all: a result names the last move and there is at most one for it */
    uint32_t r = 12345;
    now = 0xFFF00000u;
    FRESH(t, g, c, now, 0, 0);
    int last = 0, open = 0, bad = 0, results = 0;
    for (int i = 0; i < 400000; i++) {
        r = r * 1664525u + 1013904223u;
        int f = (r >> 24) & 1, o = (r >> 25) & 1;
        uint32_t ms = 5 * (1 + ((r >> 8) & (i & 64 ? 0xFF : 7)));
        if ((r >> 26) & 3) f = o = 0;
        if (f && o && ((r >> 28) & 3)) f = 0;
        for (uint32_t e = 0; e < ms; e += 5, now += 5) {
            int pos = ni_toggle_update(&t, f, o, now, DEB);
            int res = ni_gesture_update(&g, &t, pos, now);
            if (res) {
                int five = res == NI_GES_SHORT_FIVE || res == NI_GES_LONG_FIVE;
                if (!open || last != (five ? NI_TOG_FIVE : NI_TOG_ONE)) bad++;
                open = 0;
                results++;
            }
            if (pos) { if (pos != last) open = 0; last = pos; open = !g.blocked && g.pos == pos && !g.fired; }
        }
    }
    check("noise: every result follows a move of its direction, at most one each", !bad && results > 1000);
}

/* ---- gestures on the wire ---- */
static const char *order_of(const char *a, const char *b)
{
    uart[uart_n] = 0;
    const char *pa = strstr(uart, a), *pb = strstr(uart, b);
    return pa && pb && pa < pb ? "ok" : "no";
}
static void sel(nd_dev *d, ni_toggle *t, ni_gesture *g, uint32_t *now, int f, int o, uint32_t ms)
{
    for (uint32_t e = 0; e < ms; e += 5, *now += 5) {
        d->host_seen = d->host_alive ? *now : d->host_seen;
        nd_selector(d, t, g, f, o, *now, DEB, NULL);
        nd_poll(d, *now);
    }
}
static void test_gesture_link(void)
{
    static nd_dev d;
    ni_toggle t;
    ni_gesture g;
    uint32_t seq = 0, now = 100;
    nd_init(&d, &HAL, 0x1234, "wt32-r5.1.1", "vl53l5cx", 2, NULL, now);
    ni_toggle_init(&t, 0, 0, now);
    ni_gesture_init(&g, &t);

    /* before any host: the move is lost, so its gesture is too, even if a host arrives in time for it */
    sel(&d, &t, &g, &now, 0, 1, 100);
    host_frame(&d, &seq, "HOST", "proto=1 role=linkd tun=0", now);
    uart_n = 0;
    sel(&d, &t, &g, &now, 0, 1, 1500);
    sel(&d, &t, &g, &now, 0, 0, 200);
    check("a move no host heard: no result, no hold, on release", count(" EV t") == 0);

    uart_n = 0;
    sel(&d, &t, &g, &now, 0, 1, 100); sel(&d, &t, &g, &now, 0, 0, 200);
    check("short up on the wire: trk1 then tsh1", count(" EV trk1 t=") == 1 && count(" EV tsh1 t=") == 1 && !strcmp(order_of(" EV trk1", " EV tsh1"), "ok"));
    uart_n = 0;
    sel(&d, &t, &g, &now, 1, 0, 100); sel(&d, &t, &g, &now, 0, 0, 200);
    check("short down: trk5 then tsh5", count(" EV trk5 t=") == 1 && count(" EV tsh5 t=") == 1 && !strcmp(order_of(" EV trk5", " EV tsh5"), "ok"));
    uart_n = 0;
    sel(&d, &t, &g, &now, 0, 1, 1200); sel(&d, &t, &g, &now, 0, 0, 200);
    check("hold up: trk1 then tlg1, no tsh1", count(" EV trk1 t=") == 1 && count(" EV tlg1 t=") == 1 && count(" EV tsh") == 0);
    uart_n = 0;
    sel(&d, &t, &g, &now, 1, 0, 1200); sel(&d, &t, &g, &now, 0, 0, 200);
    check("hold down: trk5 then tlg5", count(" EV trk5 t=") == 1 && count(" EV tlg5 t=") == 1 && count(" EV tsh") == 0);

    uart_n = 0;
    sel(&d, &t, &g, &now, 0, 1, 200); sel(&d, &t, &g, &now, 1, 0, 200); sel(&d, &t, &g, &now, 0, 0, 200);
    check("straight across on the wire: trk1 tsh1 trk5 tsh5, in that order", !strcmp(order_of(" EV trk1", " EV tsh1"), "ok") && !strcmp(order_of(" EV tsh1", " EV trk5"), "ok") &&
                                                                              !strcmp(order_of(" EV trk5", " EV tsh5"), "ok"));

    /* a restarted host (HOST while still alive), lever held */
    uart_n = 0;
    sel(&d, &t, &g, &now, 0, 1, 300);
    host_frame(&d, &seq, "HOST", "proto=1 role=linkd tun=0", now);
    sel(&d, &t, &g, &now, 0, 1, 2000);
    sel(&d, &t, &g, &now, 0, 0, 200);
    check("host restarted during the gesture: no hold, no short", count(" EV ts") == 0 && count(" EV tl") == 0 && g.n_cancel >= 1);
    uart_n = 0;
    sel(&d, &t, &g, &now, 0, 1, 100); sel(&d, &t, &g, &now, 0, 0, 200);
    check("the next gesture is normal", count(" EV tsh1 t=") == 1);

    /* the host goes silent just as the lever moves; it times out during the gesture and comes back */
    uart_n = 0;
    for (uint32_t e = 0; e < NL_PEER_TIMEOUT_MS - 200; e += 5, now += 5) { nd_selector(&d, &t, &g, 0, 0, now, DEB, NULL); nd_poll(&d, now); }
    for (uint32_t e = 0; e < 400; e += 5, now += 5) { nd_selector(&d, &t, &g, 1, 0, now, DEB, NULL); nd_poll(&d, now); }
    check("host timed out during the gesture", !d.host_alive && count(" EV trk5 t=") == 1 && count(" EV ts") == 0 && count(" EV tl") == 0);
    host_frame(&d, &seq, "HHB", NULL, now);
    check("back", d.host_alive);
    sel(&d, &t, &g, &now, 1, 0, 2000);
    check("a gesture begun before the loss: no hold after it", count(" EV tl") == 0);
    sel(&d, &t, &g, &now, 0, 0, 200);
    check("nor a short on release", count(" EV ts") == 0);
    uart_n = 0;
    sel(&d, &t, &g, &now, 1, 0, 100); sel(&d, &t, &g, &now, 0, 0, 200);
    check("then normal", count(" EV tsh5 t=") == 1);

    /* results are not charged to the rate bucket: fast flips in a game keep every move */
    int moves = 0;
    uart_n = 0;
    for (int i = 0; i < 100; i++) {
        int f = i & 1, o = !f, q = 0;
        for (int k = 0; k < 8; k++, now += 5) { d.host_seen = now; if (nd_selector(&d, &t, &g, f, o, now, DEB, &q) && q) moves++; nd_poll(&d, now); }
        for (int k = 0; k < 12; k++, now += 5) { d.host_seen = now; if (nd_selector(&d, &t, &g, 0, 0, now, DEB, &q) && q) moves++; nd_poll(&d, now); if (uart_n > sizeof uart / 2) uart_n = 0; }
    }
    check("ten flips a second, with their results: not one move rate limited", moves == 100);

    /* a move the bucket refused has no gesture */
    uart_n = 0;
    d.bucket.milli = 0; d.bucket.last = now;
    d.host_seen = now;
    { int q = 1; nd_selector(&d, &t, &g, 0, 0, now, DEB, &q); }
    sel(&d, &t, &g, &now, 0, 0, 200);
    uart_n = 0;
    d.bucket.milli = 0; d.bucket.last = now + 30;   /* empty just as the move is reported */
    for (uint32_t e = 0; e < 60; e += 5, now += 5) { d.host_seen = now; d.bucket.milli = 0; d.bucket.last = now; nd_selector(&d, &t, &g, 0, 1, now, DEB, NULL); nd_poll(&d, now); }
    sel(&d, &t, &g, &now, 0, 1, 1500); sel(&d, &t, &g, &now, 0, 0, 200);
    check("a refused move: no result later", count(" EV t") == 0);
}

static void test_host(void)
{
    check("name table", nl_ev_from_name("trk5") == NL_EV_TRK5 && nl_ev_from_name("trk1") == NL_EV_TRK1 &&
                            nl_ev_from_name("pon") == NL_EV_PON && nl_ev_from_name("poff") == NL_EV_POFF &&
                            nl_ev_from_name("trk2") == NL_EV_NONE);
    check("earlier event numbers are unchanged", NL_EV_TAP == 1 && NL_EV_B2U == 6 && NL_EV_TILT == 7 && NL_EV_POFF == 9 && NL_EV_TRK5 == 10 && NL_EV_TRK1 == 11);
    check("gesture names", nl_ev_from_name("tsh5") == NL_EV_TSH5 && nl_ev_from_name("tsh1") == NL_EV_TSH1 &&
                               nl_ev_from_name("tlg5") == NL_EV_TLG5 && nl_ev_from_name("tlg1") == NL_EV_TLG1 && nl_ev_from_name("tsh2") == NL_EV_NONE);
    nh_session s;
    nh_event ev;
    nl_frame f;
    char line[NL_LINE_MAX + 2];
    memset(&s, 0, sizeof s);
    nh_open(&s, "linkd", 0, 0.0);
    int n = nl_format(line, sizeof line, 1, "HELLO", "boot=00001234 proto=1 fw=wt32-r5.0.0 sensor=vl53l5cx btn=2 wifi=up tun=1");
    nl_parse(line, (size_t)n - 1, &f);
    check("hello", nh_frame(&s, &f, line, (size_t)n - 1, 0.1, &ev) == NH_F_HELLO);
    n = nl_format(line, sizeof line, 2, "EV", "trk5 t=500");
    nl_parse(line, (size_t)n - 1, &f);
    check("host accepts trk5", nh_frame(&s, &f, line, (size_t)n - 1, 0.2, &ev) == NH_F_EVENT && ev.kind == NL_EV_TRK5 && ev.t == 500);
    n = nl_format(line, sizeof line, 3, "EV", "trk1 t=600");
    nl_parse(line, (size_t)n - 1, &f);
    check("host accepts trk1", nh_frame(&s, &f, line, (size_t)n - 1, 0.3, &ev) == NH_F_EVENT && ev.kind == NL_EV_TRK1);
    /* what a linkd built before r5 does: the name is not in its table */
    n = nl_format(line, sizeof line, 4, "EV", "trkx t=700");
    nl_parse(line, (size_t)n - 1, &f);
    check("an unknown name is ignored, the session goes on", nh_frame(&s, &f, line, (size_t)n - 1, 0.4, &ev) == NH_F_IGNORED && s.state == NH_READY);
    n = nl_format(line, sizeof line, 5, "EV", "b1d t=800");
    nl_parse(line, (size_t)n - 1, &f);
    check("and the next event still arrives", nh_frame(&s, &f, line, (size_t)n - 1, 0.5, &ev) == NH_F_EVENT && ev.kind == NL_EV_B1D);
    n = nl_format(line, sizeof line, 6, "EV", "tsh1 t=900");
    nl_parse(line, (size_t)n - 1, &f);
    check("host accepts tsh1", nh_frame(&s, &f, line, (size_t)n - 1, 0.6, &ev) == NH_F_EVENT && ev.kind == NL_EV_TSH1 && ev.t == 900);
    n = nl_format(line, sizeof line, 7, "EV", "tlg5 t=1000");
    nl_parse(line, (size_t)n - 1, &f);
    check("host accepts tlg5", nh_frame(&s, &f, line, (size_t)n - 1, 0.7, &ev) == NH_F_EVENT && ev.kind == NL_EV_TLG5);
}

int main(void)
{
    test_debounce();
    test_gesture();
    test_link();
    test_gesture_link();
    test_host();
    printf("toggle: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
