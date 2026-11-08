/* wt32-r5.1: saved Wi-Fi networks.  Links the firmware's own shared/nori_wifi sources; only the world around them is fake:
 * a clock, a key-value store that can lose power after any write, and a radio with access points that come and go.
 * Nothing here restates the logic under test: the checks are on what the fake radio and store were asked to do. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nori_wnet.h"
#include "nori_wnprov.h"
#include "nori_wnsm.h"

static int fails, checks;
#define CK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d %s: ", __func__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- fake NVS: every put/del is atomic, as nvs_set_* is; power can die before any of them ---- */
typedef struct { char key[16]; uint8_t v[1024]; int n; int is_str; int used; } kv_item;
typedef struct { kv_item it[8]; int writes, die_after, read_err; } kvs;
static kv_item *kv_find(kvs *s, const char *k)
{
    for (int i = 0; i < 8; i++) if (s->it[i].used && !strcmp(s->it[i].key, k)) return &s->it[i];
    return NULL;
}
static int kv_alive(kvs *s) { if (s->die_after >= 0 && s->writes >= s->die_after) return 0; s->writes++; return 1; }
static int f_get_blob(void *c, const char *k, uint8_t *b, size_t cap)
{
    kv_item *i = kv_find(c, k);
    if (!i) return -1;
    if ((size_t)i->n > cap) return -2;
    memcpy(b, i->v, (size_t)i->n);
    return i->n;
}
static void kv_set(kvs *s, const char *k, const void *b, int n, int str)
{
    kv_item *i = kv_find(s, k);
    for (int j = 0; j < 8 && !i; j++) if (!s->it[j].used) i = &s->it[j];
    memset(i, 0, sizeof *i);
    snprintf(i->key, sizeof i->key, "%s", k);
    memcpy(i->v, b, (size_t)n);
    i->n = n; i->is_str = str; i->used = 1;
}
static int f_put_blob(void *c, const char *k, const uint8_t *b, size_t n)
{
    if (!kv_alive(c)) return -1;
    kv_set(c, k, b, (int)n, 0);
    return 0;
}
static int f_get_str(void *c, const char *k, char *o, size_t cap)
{
    kvs *s = c;
    if (s->read_err) return -2;
    kv_item *i = kv_find(s, k);
    if (!i) return -1;
    if ((size_t)i->n + 1 > cap) return -2;
    memcpy(o, i->v, (size_t)i->n);
    o[i->n] = 0;
    return i->n;
}
static int f_put_str(void *c, const char *k, const char *v)
{
    if (!kv_alive(c)) return -1;
    kv_set(c, k, v, (int)strlen(v), 1);
    return 0;
}
static int f_del(void *c, const char *k)
{
    if (!kv_alive(c)) return -1;
    kv_item *i = kv_find(c, k);
    if (i) i->used = 0;
    return 0;
}
static kvs KV;
static const nw_kv KVH = { &KV, f_get_blob, f_put_blob, f_get_str, f_put_str, f_del };
static void kv_reset(void) { memset(&KV, 0, sizeof KV); KV.die_after = -1; }
static void kv_legacy(const char *s, const char *p) { kv_set(&KV, "ssid", s, (int)strlen(s), 1); kv_set(&KV, "psk", p, (int)strlen(p), 1); }
static const char *kv_str(const char *k)
{
    static char b[2][128]; static int w;
    kv_item *i = kv_find(&KV, k);
    if (!i) return NULL;
    w ^= 1;
    memcpy(b[w], i->v, (size_t)i->n);
    b[w][i->n] = 0;
    return b[w];
}
static nw_net mk(const char *s, const char *p)
{
    nw_net e;
    if (nw_net_set(&e, s, strlen(s), p, strlen(p), 0) != NW_OK) { printf("bad test net\n"); exit(2); }
    return e;
}
static nw_net mkh(const char *s, const char *p) { nw_net e = mk(s, p); e.flags = NW_F_HIDDEN; return e; }
static void set_list(nw_table *t, const nw_net *e, int k) { memset(t, 0, sizeof *t); t->n = (uint8_t)k; memcpy(t->net, e, sizeof *e * (size_t)k); }

#define HOME "home-net"
#define HOMEPW "home-password"
#define SPOT "phone-hotspot"
#define SPOTPW "hotspot-password"

/* ---- store ---- */
static void t_migration(void)
{
    nw_table t;
    kv_reset();
    kv_legacy(HOME, HOMEPW);
    CK(nw_load(&t, &KVH) == NW_LD_MIGRATED, "first boot migrates");
    CK(t.n == 1 && !strcmp(t.net[0].ssid, HOME) && !strcmp(t.net[0].psk, HOMEPW), "legacy is profile 0");
    CK(t.net[0].flags & NW_F_HIDDEN, "r5.0.1 joined without seeing it in a scan; so must the migrated profile");
    CK(!strcmp(kv_str("ssid"), HOME) && !strcmp(kv_str("psk"), HOMEPW), "legacy keys survive for rollback");
    int w = KV.writes;
    CK(nw_load(&t, &KVH) == NW_LD_BLOB && t.n == 1, "second boot just loads");
    CK(KV.writes == w, "no flash writes on a normal boot (%d)", KV.writes - w);

    kv_reset();
    KV.die_after = 0;                              /* power dies during migration: nothing lost, repeated next boot */
    kv_legacy(HOME, HOMEPW);
    CK(nw_load(&t, &KVH) == NW_LD_MIGRATED && t.n == 1, "usable even if the blob could not be written");
    KV.die_after = -1;
    CK(nw_load(&t, &KVH) == NW_LD_MIGRATED && t.n == 1 && kv_find(&KV, "wnet"), "retried");

    kv_reset();
    kv_set(&KV, "ssid", HOME, (int)strlen(HOME), 1);
    CK(nw_load(&t, &KVH) == NW_LD_MIGRATED && t.n == 1 && t.net[0].psk_len == 0, "open network, no psk key");
    kv_reset();
    CK(nw_load(&t, &KVH) == NW_LD_EMPTY && t.n == 0 && !kv_find(&KV, "wnet"), "nothing saved: nothing written");
    kv_reset();
    kv_legacy(HOME, "short");
    CK(nw_load(&t, &KVH) == NW_LD_LEGACY_BAD && t.n == 0, "an impossible legacy password is not used");
    CK(!strcmp(kv_str("psk"), "short"), "and not destroyed");
}

static void t_validation(void)
{
    nw_net e;
    char s33[34], p64[65];
    memset(s33, 'a', 33); s33[33] = 0;
    memset(p64, 'g', 64); p64[64] = 0;
    CK(nw_net_set(&e, "", 0, "password", 8, 0) == NW_E_FORMAT, "empty ssid");
    CK(nw_net_set(&e, s33, 33, "password", 8, 0) == NW_E_FORMAT, "33 byte ssid");
    CK(nw_net_set(&e, s33, 32, "password", 8, 0) == NW_OK, "32 byte ssid");
    CK(nw_net_set(&e, "a\0b", 3, "password", 8, 0) == NW_E_FORMAT, "NUL in ssid");
    CK(nw_net_set(&e, "a", 1, "pass\0ord", 8, 0) == NW_E_FORMAT, "NUL in psk");
    CK(nw_net_set(&e, "a", 1, "seven77", 7, 0) == NW_E_FORMAT, "7 byte psk");
    CK(nw_net_set(&e, "a", 1, p64, 63, 0) == NW_OK, "63 byte passphrase");
    CK(nw_net_set(&e, "a", 1, p64, 64, 0) == NW_E_FORMAT, "64 characters must be hex");
    memset(p64, 'a', 64);
    CK(nw_net_set(&e, "a", 1, p64, 64, 0) == NW_OK, "64 hex digits");
    CK(nw_net_set(&e, "a", 1, NULL, 0, 0) == NW_OK, "open");
    CK(nw_net_set(&e, "a", 1, NULL, 0, 2) == NW_E_FORMAT, "unknown flag");
    CK(nw_net_set(&e, "\xff\xfe", 2, NULL, 0, 0) == NW_OK, "SSIDs are bytes, not text");
}

static void t_ops(void)
{
    nw_table t, b;
    nw_net h = mk(HOME, HOMEPW), s = mk(SPOT, SPOTPW), x = mk("third", "third-password"), l[NW_MAX + 1];
    memset(&t, 0, sizeof t);
    CK(nw_op_primary(&t, &h) == NW_OK && t.n == 1, "WIFISET on an empty table");
    CK(nw_op_primary(&t, &h) == NW_SAME, "same login: nothing to do");
    CK(nw_op_add(&t, &s, 1, 255, 0, -1, 0) == NW_OK && t.n == 2 && !strcmp(t.net[1].ssid, SPOT), "additive append");
    b = t;
    CK(nw_op_add(&t, &s, 1, 255, 0, -1, 0) == NW_E_EXISTS && !memcmp(&t, &b, sizeof t), "no silent overwrite");
    nw_net s2 = mk(SPOT, "changed-password");
    CK(nw_op_add(&t, &s2, 1, 255, 1, -1, 0) == NW_OK && !strcmp(t.net[1].psk, "changed-password") && t.n == 2, "explicit update");
    nw_net h2 = mk(HOME, "wrong-password");
    b = t;
    CK(nw_op_add(&t, &h2, 1, 255, 1, -1, 0) == NW_E_ACTIVE && !memcmp(&t, &b, sizeof t), "primary's login is protected");
    CK(nw_op_add(&t, &h2, 1, 255, 1, -1, 1) == NW_OK && !strcmp(t.net[0].psk, "wrong-password"), "unless forced");
    t = b;

    /* WIFISET keeps the fallbacks and the previous primary */
    CK(nw_op_primary(&t, &x) == NW_OK && t.n == 3 && !strcmp(t.net[0].ssid, "third") && !strcmp(t.net[1].ssid, HOME) && !strcmp(t.net[2].ssid, SPOT), "new SSID goes first, nothing dropped");
    CK(nw_op_primary(&t, &s2) == NW_OK && !strcmp(t.net[0].ssid, SPOT) && t.n == 3 && !strcmp(t.net[1].ssid, "third"), "known SSID moves up");

    memset(&t, 0, sizeof t);
    for (int i = 0; i < NW_MAX + 1; i++) { char n[3] = { 'n', (char)('0' + i), 0 }; l[i] = mk(n, "password"); }
    CK(nw_op_replace(&t, l, NW_MAX + 1, -1, 0) == NW_E_FULL && t.n == 0, "more than NW_MAX");
    CK(nw_op_replace(&t, l, NW_MAX, -1, 0) == NW_OK && t.n == NW_MAX, "exactly NW_MAX");
    CK(nw_op_primary(&t, &h) == NW_E_FULL && t.n == NW_MAX, "WIFISET never evicts silently");
    CK(nw_op_add(&t, &h, 1, 255, 0, -1, 0) == NW_E_FULL, "add on a full table");

    set_list(&t, (nw_net[]){ h, s }, 2);
    b = t;
    CK(nw_op_replace(&t, NULL, 0, -1, 1) == NW_E_EMPTY, "empty replace, even forced");
    CK(nw_op_replace(&t, (nw_net[]){ x, x }, 2, -1, 1) == NW_E_DUP, "duplicate SSID");
    CK(nw_op_replace(&t, &s, 1, -1, 0) == NW_E_ACTIVE && !memcmp(&t, &b, sizeof t), "replace dropping the primary needs force");
    CK(nw_op_replace(&t, (nw_net[]){ x, h }, 2, 1, 0) == NW_E_ACTIVE, "replace dropping the network in use");
    CK(nw_op_replace(&t, (nw_net[]){ h2, s }, 2, -1, 0) == NW_E_ACTIVE, "replace changing the primary's password");
    CK(nw_op_replace(&t, (nw_net[]){ s, h, x }, 3, 0, 0) == NW_OK && t.n == 3 && !strcmp(t.net[0].ssid, SPOT), "reorder keeps every login");
    t = b;
    CK(nw_op_del(&t, "nope", 4, -1, 0) == NW_E_ABSENT, "delete unknown");
    CK(nw_op_del(&t, HOME, strlen(HOME), -1, 0) == NW_E_ACTIVE, "delete primary needs force");
    CK(nw_op_del(&t, SPOT, strlen(SPOT), 1, 0) == NW_E_ACTIVE, "delete the network in use needs force");
    CK(nw_op_del(&t, SPOT, strlen(SPOT), 0, 0) == NW_OK && t.n == 1, "delete a fallback");
    CK(nw_op_del(&t, HOME, strlen(HOME), -1, 1) == NW_E_EMPTY && t.n == 1, "the last one only goes with WIFICLR");
}

static void t_blob(void)
{
    uint8_t b[NW_BLOB_MAX], c[NW_BLOB_MAX];
    nw_table t, u;
    char ss[33], pp[64];
    memset(&t, 0, sizeof t);
    memset(ss, 's', 32); ss[32] = 0;
    memset(pp, 'p', 63); pp[63] = 0;
    for (int i = 0; i < NW_MAX; i++) { ss[0] = (char)('a' + i); t.net[i] = mk(ss, pp); }
    t.n = NW_MAX;
    t.gen = 77;
    int n = nw_encode(&t, b, sizeof b);
    CK(n > 0 && n <= NW_BLOB_MAX, "largest table fits (%d)", n);
    CK(nw_decode(&u, b, (size_t)n) == NW_OK && u.n == NW_MAX && u.gen == 77 && !strcmp(u.net[7].psk, pp), "round trip");
    CK(nw_encode(&t, b, (size_t)n - 1) == -1, "bounded");
    for (int i = 0; i < n; i++) CK(nw_decode(&u, b, (size_t)i) == NW_E_FORMAT, "truncated to %d", i);
    srand(1);
    int accepted = 0;
    for (int k = 0; k < 40000; k++) {
        memcpy(c, b, (size_t)n);
        for (int j = 0, m = 1 + rand() % 6; j < m; j++) c[rand() % n] ^= (uint8_t)(1 + rand() % 255);
        if (nw_decode(&u, c, (size_t)n) == NW_OK) accepted++;
    }
    CK(accepted == 0, "the CRC stops damage (%d accepted)", accepted);
    /* a correct CRC on a nonsense body: every field is still checked */
    static const struct { int off; uint8_t v; } bad[] = { { 4, 2 }, { 5, 9 }, { 6, 2 }, { 12, 0 }, { 12, 200 }, { 13, 3 }, { 14, 9 } };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        memcpy(c, b, (size_t)n);
        c[bad[i].off] = bad[i].v;
        uint32_t crc = nw_crc32(c, (size_t)n - 4);
        for (int j = 0; j < 4; j++) c[n - 4 + j] = (uint8_t)(crc >> (8 * j));
        CK(nw_decode(&u, c, (size_t)n) == NW_E_FORMAT, "field %zu", i);
    }
    t.n = 2;
    t.net[1] = t.net[0];
    n = nw_encode(&t, b, sizeof b);
    CK(nw_decode(&u, b, (size_t)n) == NW_E_FORMAT, "duplicate SSID in a blob");
}

/* every change, power lost before each single write, then a reboot */
typedef void (*change_fn)(nw_table *t);
static void ch_set_new(nw_table *t) { nw_table n = *t; nw_net e = mk("new-net", "new-password"); nw_op_primary(&n, &e); nw_commit(t, &n, &KVH); }
static void ch_add(nw_table *t) { nw_table n = *t; nw_net e = mk("extra", "extra-password"); nw_op_add(&n, &e, 1, 255, 0, -1, 0); nw_commit(t, &n, &KVH); }
static void ch_clear(nw_table *t) { nw_table n; memset(&n, 0, sizeof n); nw_commit(t, &n, &KVH); }
static void power_cut(change_fn f, const char *name, int expect_n, const char *expect_first)
{
    int cleared = expect_n == 0, finished = 0;
    for (int cut = 0; cut < 12 && !finished; cut++) {
        nw_table t, r, n;
        kv_reset();
        kv_legacy(HOME, HOMEPW);
        nw_load(&t, &KVH);
        memset(&n, 0, sizeof n);
        n = t; { nw_net e = mk(SPOT, SPOTPW); nw_op_add(&n, &e, 1, 255, 0, -1, 0); }
        nw_commit(&t, &n, &KVH);
        KV.writes = 0;
        KV.die_after = cut;
        f(&t);
        int wrote = KV.writes;
        KV.die_after = -1;
        int ld = nw_load(&r, &KVH);
        nw_load(&t, &KVH);
        CK(r.n == t.n, "%s cut %d: a second boot agrees", name, cut);
        if (cut == 0) {
            CK(r.n == 2 && !strcmp(r.net[0].ssid, HOME), "%s cut 0: old table intact (ld %s)", name, NW_LD_NAME[ld]);
        } else {
            CK(r.n == expect_n, "%s cut %d: table n=%d (ld %s)", name, cut, r.n, NW_LD_NAME[ld]);
            if (expect_first) CK(!strcmp(r.net[0].ssid, expect_first), "%s cut %d: primary", name, cut);
            if (cleared) CK(!kv_find(&KV, "ssid") && !kv_find(&KV, "psk"), "%s cut %d: nothing left to resurrect", name, cut);
            else CK(!strcmp(kv_str("ssid"), r.net[0].ssid) && !strcmp(kv_str("psk"), r.net[0].psk), "%s cut %d: legacy mirrors the primary", name, cut);
        }
        if (wrote < cut) finished = 1;
    }
    CK(finished, "%s: the sequence ended within 12 writes", name);
}

static void t_power(void)
{
    power_cut(ch_set_new, "WIFISET", 3, "new-net");
    power_cut(ch_add, "add", 3, HOME);
    power_cut(ch_clear, "WIFICLR", 0, NULL);
}

static void t_rollback_interplay(void)
{
    nw_table t, n;
    nw_net s = mk(SPOT, SPOTPW);
    kv_reset();
    kv_legacy(HOME, HOMEPW);
    nw_load(&t, &KVH);
    n = t; nw_op_add(&n, &s, 1, 255, 0, -1, 0); nw_commit(&t, &n, &KVH);
    CK(nw_load(&t, &KVH) == NW_LD_BLOB, "stable");

    /* r5.0.1 ran WIFISET while rolled back, then r5.1 came back */
    kv_legacy("other-net", "other-password");
    CK(nw_load(&t, &KVH) == NW_LD_ADOPTED && t.n == 3 && !strcmp(t.net[0].ssid, "other-net") && !strcmp(t.net[1].ssid, HOME), "the rollback's WIFISET is honoured");
    CK(nw_load(&t, &KVH) == NW_LD_BLOB, "once");

    /* r5.0.1 ran WIFICLR: the fallbacks must not come back */
    kv_item *i = kv_find(&KV, "ssid"); i->used = 0;
    i = kv_find(&KV, "psk"); i->used = 0;
    CK(nw_load(&t, &KVH) == NW_LD_CLEARED && t.n == 0, "the rollback's WIFICLR is honoured");
    CK(nw_load(&t, &KVH) == NW_LD_BLOB && t.n == 0, "tombstone stays");

    /* a read error on the legacy keys proves nothing */
    kv_reset();
    kv_legacy(HOME, HOMEPW);
    nw_load(&t, &KVH);
    KV.read_err = 1;
    CK(nw_load(&t, &KVH) == NW_LD_BLOB && t.n == 1, "unreadable legacy keys are not a WIFICLR");

    /* the blob damaged: the mirror saves the primary */
    kv_reset();
    kv_legacy(HOME, HOMEPW);
    nw_load(&t, &KVH);
    kv_find(&KV, "wnet")->v[20] ^= 0x55;
    CK(nw_load(&t, &KVH) == NW_LD_CORRUPT_LEGACY && t.n == 1 && !strcmp(t.net[0].psk, HOMEPW), "damaged blob falls back to the mirror");
    kv_reset();
    kv_set(&KV, "wnet", "junk", 4, 0);
    CK(nw_load(&t, &KVH) == NW_LD_CORRUPT_EMPTY && t.n == 0, "damaged blob, no mirror");

    /* store failure: nothing changes, no half state */
    kv_reset();
    kv_legacy(HOME, HOMEPW);
    nw_load(&t, &KVH);
    nw_table b = t;
    n = t; nw_op_primary(&n, &s);
    KV.writes = 0; KV.die_after = 0;
    CK(nw_commit(&t, &n, &KVH) == NW_E_STORE && !memcmp(&t, &b, sizeof t), "NVS full: refused, table untouched");
}

/* ---- fake radio ---- */
typedef struct { char ssid[33], psk[65]; int in_range, hidden, rssi, dhcp, flaky_ms; } ap;
enum { W_IDLE, W_CONNECTING, W_ASSOC, W_UP };
static struct {
    uint32_t now;
    ap aps[6];
    int naps;
    const nw_table *t;
    int on, st, target, assoc, ip, scanning, scan_fail, scan_hang;
    uint32_t t_scan, t_next, t_drop;
    uint8_t reason;
    uint32_t disc;
    int fg_scans, bg_scans, joins[NW_MAX], join_total, leaves, stops, joining_during_scan, ota;
    int ops_in_ota;
    int start_refused;
} W;
static nws S;
static nw_table T;
static nws_in IN;

static ap *ap_for(int i)
{
    for (int k = 0; k < W.naps; k++) if (!strcmp(W.aps[k].ssid, W.t->net[i].ssid)) return &W.aps[k];
    return NULL;
}
static void r_on(void *c) { (void)c; W.on = 1; }
static void r_off(void *c) { (void)c; W.on = 0; W.st = W_IDLE; W.assoc = W.ip = 0; }
static void touched(void) { if (W.ota) W.ops_in_ota++; }
static int r_scan_start(void *c, int bg)
{
    (void)c;
    touched();
    if (W.start_refused) return -1;
    if (W.st == W_CONNECTING) W.joining_during_scan++;
    if (bg) W.bg_scans++; else W.fg_scans++;
    W.scanning = 1;
    W.t_scan = W.now + (bg ? 1500 : 3000);
    return 0;
}
static int r_scan_poll(void *c, nws_seen *o)
{
    (void)c;
    if (W.scan_hang) return 0;
    if ((int32_t)(W.now - W.t_scan) < 0) return 0;
    W.scanning = 0;
    if (W.scan_fail) return -1;
    for (int i = 0; i < W.t->n; i++) {
        ap *a = ap_for(i);
        if (a && a->in_range && !a->hidden) { o[i].vis = 1; o[i].rssi = (int8_t)a->rssi; }
    }
    return 1;
}
static void r_scan_stop(void *c) { (void)c; W.scanning = 0; W.stops++; }
static int r_join(void *c, int i)
{
    (void)c;
    touched();
    W.joins[i]++;
    W.join_total++;
    W.target = i;
    W.st = W_CONNECTING;
    W.t_next = W.now + 1000;
    return 0;
}
static void radio_down(uint8_t why)
{
    int was = W.st != W_IDLE;
    W.st = W_IDLE;
    W.assoc = W.ip = 0;
    if (was) { W.reason = why; W.disc++; }
}
static void r_leave(void *c) { (void)c; touched(); W.leaves++; radio_down(8); }
static const nws_hal HAL = { NULL, r_on, r_off, r_scan_start, r_scan_poll, r_scan_stop, r_join, r_leave, NULL };

static void world(void)
{
    if (W.st == W_CONNECTING && (int32_t)(W.now - W.t_next) >= 0) {
        ap *a = ap_for(W.target);
        if (!a || !a->in_range) radio_down(201);
        else if (strcmp(a->psk, W.t->net[W.target].psk)) radio_down(15);
        else { W.st = W_ASSOC; W.assoc = 1; W.t_next = W.now + 1000; }
    } else if (W.st == W_ASSOC && (int32_t)(W.now - W.t_next) >= 0) {
        ap *a = ap_for(W.target);
        if (a->dhcp) { W.st = W_UP; W.ip = 1; if (a->flaky_ms) W.t_drop = W.now + (uint32_t)a->flaky_ms; }
    }
    if (W.st >= W_ASSOC) {
        ap *a = ap_for(W.target);
        if (!a || !a->in_range) radio_down(200);
        else if (W.st == W_UP && a->flaky_ms && (int32_t)(W.now - W.t_drop) >= 0) radio_down(200);
    }
}
static void step(void)
{
    W.now += 250;
    world();
    IN.assoc = W.assoc; IN.ip = W.ip; IN.disc_seq = W.disc; IN.reason = W.reason;
    nws_tick(&S, &T, &IN, W.now);
}
static void run(uint32_t ms) { for (uint32_t i = 0; i < ms; i += 250) step(); }
static int run_until_up(uint32_t ms, int idx)
{
    for (uint32_t i = 0; i < ms; i += 250) { step(); if (nws_active(&S) == idx) return 1; }
    return 0;
}
static void add_ap(const char *s, const char *p, int rssi)
{
    ap *a = &W.aps[W.naps++];
    memset(a, 0, sizeof *a);
    snprintf(a->ssid, sizeof a->ssid, "%s", s);
    snprintf(a->psk, sizeof a->psk, "%s", p);
    a->in_range = 1; a->rssi = rssi; a->dhcp = 1;
}
static void setup2(uint32_t t0)
{
    memset(&W, 0, sizeof W);
    W.now = t0;
    W.t = &T;
    memset(&IN, 0, sizeof IN);
    IN.wanted = 1;
    set_list(&T, (nw_net[]){ mk(HOME, HOMEPW), mk(SPOT, SPOTPW) }, 2);
    nws_init(&S, &HAL);
    add_ap(HOME, HOMEPW, -50);
    add_ap(SPOT, SPOTPW, -60);
}
#define AP_HOME (&W.aps[0])
#define AP_SPOT (&W.aps[1])

static void t_priority(void)
{
    setup2(0);
    CK(run_until_up(20000, 0), "both in range: the primary");
    CK(W.joins[1] == 0, "the fallback was not touched");
    CK(W.fg_scans == 1, "one scan");
    int j = W.join_total;
    run(24u * 3600u * 1000u);
    CK(W.bg_scans == 0 && W.fg_scans == 1 && W.join_total == j && W.leaves == 0, "24 h healthy on the primary: no scan, no join, no leave");

    setup2(0);
    AP_HOME->in_range = 0;
    CK(run_until_up(20000, 1), "primary out of range: the hotspot");
    CK(W.joins[0] == 0, "not visible and not hidden: not even tried");
}

static void t_wrong_password(void)
{
    setup2(0);
    snprintf(AP_HOME->psk, sizeof AP_HOME->psk, "the-password-changed");
    CK(run_until_up(30000, 1), "wrong password on the primary falls through to the hotspot");
    CK(S.p[0].fails[NWF_AUTH] == 1 && S.p[0].last_reason == 15, "classified as auth (%lu)", (unsigned long)S.p[0].fails[NWF_AUTH]);
    run(3600000);
    CK(nws_active(&S) == 1, "stays on the hotspot");
    CK(S.p[0].tries < 16, "a wrong password is retried slowly (%lu tries in an hour)", (unsigned long)S.p[0].tries);
    CK(S.upgrade_fails >= 1, "each try returned to the hotspot");

    setup2(0);                                     /* every network wrong: bounded retry, no spin */
    snprintf(AP_HOME->psk, sizeof AP_HOME->psk, "the-password-changed");
    snprintf(AP_SPOT->psk, sizeof AP_SPOT->psk, "the-password-changed");
    run(3600000);
    CK(nws_active(&S) < 0, "nothing joined");
    CK(W.join_total <= 2 * 16, "bounded: %d joins in an hour", W.join_total);
    CK(W.fg_scans <= 90, "bounded: %d scans in an hour", W.fg_scans);
    snprintf(AP_HOME->psk, sizeof AP_HOME->psk, "%s", HOMEPW);
    CK(run_until_up(330000, 0), "recovers within one backoff once the password is right again");
}

static void t_no_dhcp(void)
{
    setup2(0);
    AP_HOME->dhcp = 0;
    CK(run_until_up(60000, 1), "no address on the primary: next network");
    CK(S.p[0].fails[NWF_DHCP] == 1, "classified as dhcp");
    CK(W.now <= 30000, "within the timeouts (%lu)", (unsigned long)W.now);
}

static void t_hotspot_comes_and_goes(void)
{
    setup2(0);
    AP_HOME->in_range = 0;
    CK(run_until_up(20000, 1), "on the hotspot");
    run(120000);
    AP_SPOT->in_range = 0;                         /* hotspot switched off */
    run(120000);
    CK(nws_active(&S) < 0, "nothing in range");
    int s0 = W.fg_scans, j0 = W.joins[1];
    run(600000);
    CK(W.fg_scans - s0 <= 12 && W.fg_scans - s0 >= 8, "rescanning at the slow rate (%d in 10 min)", W.fg_scans - s0);
    CK(W.joins[1] == j0, "not joining what is not there");
    AP_SPOT->in_range = 1;                         /* reappears */
    CK(run_until_up(70000, 1), "back within one rescan");

    /* primary up, hotspot irrelevant, primary dies: falls to hotspot, primary returns: moves back */
    setup2(0);
    CK(run_until_up(20000, 0), "primary");
    run(60000);
    AP_HOME->in_range = 0;
    CK(run_until_up(40000, 1), "primary gone: hotspot");
    CK(S.p[0].fails[NWF_NOAP] == 1, "one fast rejoin, then the scan decided (%lu)", (unsigned long)S.p[0].fails[NWF_NOAP]);
    run(30000);
    AP_HOME->in_range = 1;
    int b = W.bg_scans;
    CK(run_until_up(NWS_BG_PERIOD_MS + 2 * NWS_BG_CONFIRM_MS + 30000, 0), "primary back: returns to it");
    CK(S.upgrades == 1, "as an upgrade");
    CK(W.bg_scans - b >= 2, "after two sightings");
}

static void t_stable_fallback(void)
{
    setup2(0);
    AP_HOME->in_range = 0;
    run_until_up(20000, 1);
    run(3600000);
    CK(W.bg_scans <= 13, "an hour on a fallback: %d background scans", W.bg_scans);
    CK(W.leaves == 0 || S.upgrades == 0, "no move");
    CK(nws_active(&S) == 1, "still up");

    /* primary visible but too weak: never leave a working link for it */
    setup2(0);
    AP_HOME->in_range = 0;
    run_until_up(20000, 1);
    AP_HOME->in_range = 1;
    AP_HOME->rssi = NWS_UPGRADE_RSSI - 5;
    int l = W.leaves;
    run(3600000);
    CK(nws_active(&S) == 1 && W.leaves == l, "weak primary ignored");

    /* primary seen once, then gone: one sighting is not enough */
    setup2(0);
    AP_HOME->in_range = 0;
    run_until_up(20000, 1);
    run(NWS_BG_FIRST_MS - 1000);
    AP_HOME->in_range = 1;
    int b = W.bg_scans;
    while (W.bg_scans == b) step();
    run(2000);
    AP_HOME->in_range = 0;
    l = W.leaves;
    run(1200000);
    CK(W.leaves == l && nws_active(&S) == 1, "single sighting ignored");

    /* an upgrade that fails goes straight back and is not retried for a long time */
    setup2(0);
    snprintf(AP_HOME->psk, sizeof AP_HOME->psk, "the-password-changed");
    AP_HOME->in_range = 0;
    run_until_up(20000, 1);
    run(60000);
    AP_HOME->in_range = 1;
    int moves = 0, was = 1;
    for (uint32_t i = 0; i < 3600000; i += 250) {
        step();
        int a = nws_active(&S);
        if (a < 0 && was >= 0) moves++;
        was = a;
    }
    CK(S.upgrade_fails >= 1 && S.upgrade_fails <= 5, "hold-down: %lu failed moves in an hour", (unsigned long)S.upgrade_fails);
    CK(moves == (int)S.upgrade_fails, "the link was only ever left for those (%d)", moves);
    CK(nws_active(&S) == 1 || W.st != W_UP, "back on the hotspot");
}

static void t_reconnect(void)
{
    setup2(0);
    run_until_up(20000, 0);
    run(60000);
    int s = W.fg_scans;
    AP_HOME->in_range = 0;                         /* a short outage of a stable link */
    run(500);
    AP_HOME->in_range = 1;
    CK(run_until_up(20000, 0), "rejoined");
    CK(W.fg_scans == s + (S.p[0].fails[NWF_NOAP] ? 1 : 0), "fast path: no scan needed when the join works");
    CK(W.joins[1] == 0, "the fallback was not touched");

    /* a link that flaps must not thrash: it is penalised, and the fallback takes over */
    setup2(0);
    AP_HOME->flaky_ms = 5000;
    run(60000);
    run(300000);
    CK(S.p[0].fails[NWF_LOST] >= 1, "flap counted");
    CK(W.joins[0] <= 8, "not rejoined in a tight loop (%d in ~6 min)", W.joins[0]);
    CK(nws_active(&S) == 1, "settled on the working fallback");
}

static void t_ip_loss(void)
{
    setup2(0);
    run_until_up(20000, 0);
    run(60000);
    W.ip = 0;                                      /* associated, address gone */
    AP_HOME->dhcp = 0;
    W.st = W_ASSOC;
    W.t_next = W.now + 3600000;
    run(5000);
    CK(nws_active(&S) == 0 && W.leaves == 0, "a short gap is tolerated");
    CK(run_until_up(40000, 1), "a lost address moves on");
    CK(S.p[0].fails[NWF_DHCP] == 1, "counted as dhcp");
    W.ip = 1;

    setup2(0);
    run_until_up(20000, 0);
    run(60000);
    W.ip = 0; W.st = W_ASSOC; W.t_next = W.now + 4000;
    run(6000);
    CK(nws_active(&S) == 0 && W.leaves == 0, "renewed in time: nothing happened");
}

static void t_ota(void)
{
    /* on a fallback, background scan due, OTA runs */
    setup2(0);
    AP_HOME->in_range = 0;
    run_until_up(20000, 1);
    run(40000);
    AP_HOME->in_range = 1;
    IN.hold = 1; W.ota = 1;
    int b = W.bg_scans;
    run(1800000);
    CK(W.bg_scans == b && W.ops_in_ota == 0, "30 min of OTA: no scan, join or leave (%d)", W.ops_in_ota);
    CK(nws_active(&S) == 1, "link untouched");
    IN.hold = 0; W.ota = 0;
    run(NWS_BG_FIRST_MS - 1000);
    CK(W.bg_scans == b, "and a quiet minute after it");

    /* a background scan in progress when the OTA starts is stopped at once */
    setup2(0);
    AP_HOME->in_range = 0;
    run_until_up(20000, 1);
    int b2 = W.bg_scans;
    while (W.bg_scans == b2) step();
    CK(S.state == NWS_BG && W.scanning, "scanning");
    IN.hold = 1;
    step();
    CK(!W.scanning && W.stops == 1 && S.state == NWS_UP, "stopped");
    IN.hold = 0;

    /* the link drops during an OTA: nothing until it is over, then normal recovery */
    setup2(0);
    run_until_up(20000, 0);
    run(60000);
    IN.hold = 1; W.ota = 1;
    AP_HOME->in_range = 0;
    run(300000);
    CK(W.ops_in_ota == 0, "no recovery attempt under hold");
    IN.hold = 0; W.ota = 0;
    AP_HOME->in_range = 1;
    CK(run_until_up(20000, 0), "recovered after it");

    /* unproven image / open tunnel: no background work, but a lost link is still recovered */
    setup2(0);
    AP_HOME->in_range = 0;
    run_until_up(20000, 1);
    AP_HOME->in_range = 1;
    IN.no_bg = 1;
    run(1200000);
    CK(W.bg_scans == 0 && nws_active(&S) == 1, "no_bg: nothing");
    AP_SPOT->in_range = 0;
    CK(run_until_up(40000, 0), "no_bg still recovers");
}

static void t_hidden(void)
{
    setup2(0);
    T.net[0] = mkh(HOME, HOMEPW);
    AP_HOME->hidden = 1;
    CK(run_until_up(20000, 0), "a hidden network joins without being seen");
    CK(W.joins[1] == 0, "and keeps its priority");

    setup2(0);
    T.net[0] = mkh(HOME, HOMEPW);
    AP_HOME->in_range = 0;
    CK(run_until_up(20000, 1), "absent hidden network: one short try, then the next");
    CK(W.joins[0] == 1, "one try (%d)", W.joins[0]);
    AP_SPOT->in_range = 0;
    run(10000);
    W.joins[0] = 0;
    run(3600000);
    CK(W.joins[0] >= 3 && W.joins[0] <= 12, "blind tries back off (%d in an hour)", W.joins[0]);
    AP_HOME->in_range = 1;
    CK(run_until_up(NWS_BLIND_MAX_MS + 70000, 0), "and still find it");

    setup2(0);                                     /* hidden primary cannot displace a working fallback */
    T.net[0] = mkh(HOME, HOMEPW);
    AP_HOME->hidden = 1; AP_HOME->in_range = 0;
    run_until_up(20000, 1);
    AP_HOME->in_range = 1;
    int l = W.leaves;
    run(3600000);
    CK(nws_active(&S) == 1 && W.leaves == l, "a working link is not given up on a guess");
}

static void t_scan_failure(void)
{
    setup2(0);
    W.scan_fail = 1;
    CK(run_until_up(20000, 0), "a failed scan: joins blind, as r5.0.1 did");
    setup2(0);
    W.scan_hang = 1;
    CK(run_until_up(20000, 0), "a scan that never ends: same, after NWS_SCAN_MAX_MS");
    CK(W.stops == 1, "and it was stopped");
    setup2(0);
    W.start_refused = 1;
    CK(run_until_up(20000, 0), "a scan that cannot start: same");
}

static void t_stale_disconnect(void)
{
    /* the core's first_connect retry or our own leave: reason 8 arrives after the next join began */
    setup2(0);
    run(3500);
    CK(S.state == NWS_JOIN && !W.assoc, "joining, not yet associated");
    W.disc++; W.reason = 8;
    step();
    CK(S.state == NWS_JOIN && S.p[0].fails[NWF_AUTH] + S.p[0].fails[NWF_ASSOC] == 0, "not a failure of this attempt");
    CK(run_until_up(10000, 0), "the attempt goes on");
}

static void t_wrap(void)
{
    setup2(0xFFFFFFFFu - 6000u);                   /* a backoff that ends past the wrap must not look expired */
    snprintf(AP_HOME->psk, sizeof AP_HOME->psk, "the-password-changed");
    while (!S.p[0].fails[NWF_AUTH]) step();
    CK(W.now > 0x80000000u && S.p[0].nb < 0x80000000u, "the backoff crosses the wrap");
    step();
    CK(S.p[0].nb_on, "still backing off right after the failure");
    run(10000);
    CK(S.p[0].nb_on && S.p[0].tries == 1, "and ten seconds later");

    uint32_t t0 = 0xFFFFFFFFu - 40000u;            /* millis() wraps during the attempts */
    setup2(t0);
    snprintf(AP_HOME->psk, sizeof AP_HOME->psk, "the-password-changed");
    CK(run_until_up(30000, 1), "fallback across the wrap");
    run(3600000);
    CK(nws_active(&S) == 1, "backoff survives the wrap");
    CK(S.p[0].tries >= 3, "the primary is still retried (%lu)", (unsigned long)S.p[0].tries);

    setup2(0);                                     /* a backoff left to expire long ago never looks like the future */
    AP_HOME->dhcp = 0;
    run_until_up(60000, 1);
    W.now += 0x7FFFFFFFu + 1000u;
    AP_SPOT->in_range = 0;
    AP_HOME->dhcp = 1;
    CK(run_until_up(70000, 0), "after 25 days");
}

static void t_table_change(void)
{
    nw_table old;
    /* a new fallback while on the primary: nothing happens to the link */
    setup2(0);
    run_until_up(20000, 0);
    run(60000);
    old = T;
    { nw_net e = mk("third", "third-password"); nw_op_add(&T, &e, 1, 255, 0, 0, 0); }
    nws_remap(&S, &old, &T, W.now);
    int l = W.leaves;
    run(600000);
    CK(W.leaves == l && nws_active(&S) == 0 && W.bg_scans == 0, "link untouched");

    /* reorder under a working link: the link stays, its index follows */
    old = T;
    { nw_net l3[3] = { T.net[1], T.net[0], T.net[2] }; nw_op_replace(&T, l3, 3, 0, 0); }
    nws_remap(&S, &old, &T, W.now);
    step();
    CK(nws_active(&S) == 1 && W.leaves == l, "index follows the SSID");
    AP_SPOT->rssi = -50;
    W.target = 1;
    CK(run_until_up(NWS_BG_PERIOD_MS + 120000, 0), "and the new first choice is taken up calmly");

    /* the network in use removed (forced): reselect */
    setup2(0);
    run_until_up(20000, 0);
    old = T;
    nw_op_del(&T, HOME, strlen(HOME), 0, 1);
    nws_remap(&S, &old, &T, W.now);
    CK(run_until_up(20000, 0), "now on the remaining network, index 0");
    CK(W.joins[0] == 2, "joined anew");

    /* WIFISET with a new password on the network in use */
    setup2(0);
    run_until_up(20000, 0);
    snprintf(AP_HOME->psk, sizeof AP_HOME->psk, "new-home-password");
    old = T;
    { nw_net e = mk(HOME, "new-home-password"); nw_op_primary(&T, &e); }
    nws_remap(&S, &old, &T, W.now);
    nws_kick(&S);
    run(1000);
    CK(W.leaves == 1, "the old login is left");
    CK(run_until_up(20000, 0), "rejoined with the new one");

    /* WIFISET naming a new primary while on a fallback, under OTA: waits for the end of the hold */
    setup2(0);
    AP_HOME->in_range = 0;
    run_until_up(20000, 1);
    AP_HOME->in_range = 1;
    IN.hold = 1; W.ota = 1;
    nws_kick(&S);
    run(60000);
    CK(W.ops_in_ota == 0 && nws_active(&S) == 1, "deferred");
    IN.hold = 0; W.ota = 0;
    CK(run_until_up(20000, 0), "then done");

    /* WIFICLR while up */
    setup2(0);
    run_until_up(20000, 0);
    old = T;
    memset(&T, 0, sizeof T);
    nws_remap(&S, &old, &T, W.now);
    IN.wanted = 0;
    step();
    CK(S.state == NWS_OFF && !W.on, "radio off");
    int j = W.join_total;
    run(600000);
    CK(W.join_total == j && W.fg_scans == 1, "and silent");

    /* failure history does not follow a corrected password */
    setup2(0);
    snprintf(AP_HOME->psk, sizeof AP_HOME->psk, "the-password-changed");
    run_until_up(30000, 1);
    CK(S.p[0].nb_on, "backing off");
    old = T;
    { nw_net e = mk(HOME, "the-password-changed"); nw_op_primary(&T, &e); }
    nws_remap(&S, &old, &T, W.now);
    nws_kick(&S);
    CK(run_until_up(20000, 0), "tried at once");
}

/* ---- provisioning ---- */
#define SECRET "0123456789abcdef0123456789abcdef"
static nw_table PT;
static int p_active = -1, p_changed, p_primary;
static uint8_t p_rnd = 1;
static const char *p_key_val = SECRET;
static const char *p_key(void *c) { (void)c; return p_key_val; }
static void p_rand(void *c, uint8_t *b, size_t n) { (void)c; for (size_t i = 0; i < n; i++) b[i] = p_rnd++; }
static int p_act(void *c) { (void)c; return p_active; }
static void p_chg(void *c, const nw_table *o, int pn) { (void)c; (void)o; p_changed++; p_primary = pn; }
static nwp_ctx P;
static void p_reset(void)
{
    kv_reset();
    kv_legacy(HOME, HOMEPW);
    nw_load(&PT, &KVH);
    memset(&P, 0, sizeof P);
    P.t = &PT; P.kv = &KVH; P.key = p_key; P.rnd = p_rand; P.active = p_act; P.changed = p_chg;
    p_active = -1; p_changed = 0; p_key_val = SECRET;
}
static size_t payload(uint8_t *o, int op, int flags, int pos, const nw_net *e, int k)
{
    size_t i = 0;
    o[i++] = 1; o[i++] = (uint8_t)op; o[i++] = (uint8_t)flags; o[i++] = (uint8_t)pos; o[i++] = (uint8_t)k;
    for (int j = 0; j < k; j++) {
        o[i++] = e[j].ssid_len; memcpy(o + i, e[j].ssid, e[j].ssid_len); i += e[j].ssid_len;
        o[i++] = e[j].psk_len; memcpy(o + i, e[j].psk, e[j].psk_len); i += e[j].psk_len;
        o[i++] = e[j].flags;
    }
    return i;
}
/* the sender's half: tools/nori_wifi_profiles.py is the real one, checked against this device code in test_wifi_provision.py */
static void seal(const char *sec, const uint8_t *nonce, const uint8_t *p, size_t n, uint8_t *ct, uint8_t *tag)
{
    uint8_t k[32], ke[32], km[32], ks[32], be[4] = { 0 };
    nwp_hmac((const uint8_t *)sec, strlen(sec), (const uint8_t *)"nori-wifi-v1", 12, nonce, NWP_NONCE, k);
    nwp_hmac(k, 32, (const uint8_t *)"enc", 3, NULL, 0, ke);
    nwp_hmac(k, 32, (const uint8_t *)"mac", 3, NULL, 0, km);
    for (size_t o = 0, b = 0; o < n; o += 32, b++) {
        be[3] = (uint8_t)b;
        nwp_hmac(ke, 32, be, 4, NULL, 0, ks);
        for (size_t i = 0; i < 32 && o + i < n; i++) ct[o + i] = p[o + i] ^ ks[i];
    }
    nwp_hmac(km, 32, nonce, NWP_NONCE, ct, n, tag);
}
static char LOG[1 << 16];
static void log_add(const char *s) { strncat(LOG, s, sizeof LOG - strlen(LOG) - 2); strcat(LOG, "\n"); }
static int frame(const char *type, const char *args, char *reply, size_t cap, uint32_t now)
{
    char line[NL_LINE_MAX + 2];
    nl_frame f;
    int n = nl_format(line, sizeof line, 1, type, args);
    if (n < 0 || nl_parse(line, (size_t)n - 1, &f) != NL_OK) return -1;
    int r = nwp_cmd(&P, &f, reply, cap, now);
    log_add(reply);
    return r;
}
static void tohex(const uint8_t *b, size_t n, char *o) { for (size_t i = 0; i < n; i++) sprintf(o + i * 2, "%02x", b[i]); }
static int uart_send(const uint8_t *p, size_t n, const char *sec, char *reply, size_t cap, uint32_t now)
{
    uint8_t nonce[NWP_NONCE], ct[NWP_PAYLOAD_MAX], tag[NWP_TAG];
    char args[300], h[65], b64[260];
    if (frame("WNBEGIN", NULL, reply, cap, now) != 1 || strncmp(reply, "OK WNBEGIN n=", 13)) return -1;
    for (int i = 0; i < NWP_NONCE; i++) { unsigned v; sscanf(reply + 13 + i * 2, "%2x", &v); nonce[i] = (uint8_t)v; }
    seal(sec, nonce, p, n, ct, tag);
    for (size_t o = 0; o < n; o += 180) {
        size_t k = n - o < 180 ? n - o : 180;
        nl_b64_encode(ct + o, k, b64, sizeof b64);
        snprintf(args, sizeof args, "o=%zu d=%s", o, b64);
        if (frame("WNDATA", args, reply, cap, now) != 1 || strncmp(reply, "OK", 2)) return -1;
    }
    tohex(tag, 32, h);
    snprintf(args, sizeof args, "t=%s", h);
    return frame("WNEND", args, reply, cap, now);
}

static void t_crypto(void)
{
    uint8_t o[32], key[20];
    char h[65];
    nwp_sha256((const uint8_t *)"abc", 3, o);
    tohex(o, 32, h);
    CK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), "SHA-256 abc");
    nwp_sha256((const uint8_t *)"", 0, o);
    tohex(o, 32, h);
    CK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), "SHA-256 empty");
    static const char M[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    nwp_sha256((const uint8_t *)M, sizeof M - 1, o);
    tohex(o, 32, h);
    CK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"), "SHA-256 two blocks");
    memset(key, 0x0b, sizeof key);
    nwp_hmac(key, 20, (const uint8_t *)"Hi There", 8, NULL, 0, o);
    tohex(o, 32, h);
    CK(!strcmp(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"), "HMAC RFC 4231 case 1");
}

static void t_provision(void)
{
    uint8_t p[NWP_PAYLOAD_MAX + 64];
    char r[320];
    nw_net s = mk(SPOT, SPOTPW), h2 = mk(HOME, "typo-password");
    size_t n;

    p_reset();
    n = payload(p, NWP_OP_ADD, 0, 255, &s, 1);
    CK(uart_send(p, n, SECRET, r, sizeof r, 1000) == 1 && !strncmp(r, "OK WNEND n=2 g=", 15), "additive over UART: %s", r);
    CK(PT.n == 2 && !strcmp(PT.net[1].ssid, SPOT) && p_changed == 1 && !p_primary, "landed, primary kept");
    CK(!strcmp(kv_str("ssid"), HOME), "the mirror did not move");
    nw_table st;
    CK(nw_load(&st, &KVH) == NW_LD_BLOB && st.n == 2, "and it is on flash");

    p_reset();
    n = payload(p, NWP_OP_ADD, 0, 255, &s, 1);
    CK(uart_send(p, n, "ffffffffffffffffffffffffffffffff", r, sizeof r, 1000) == 1 && !strcmp(r, "ERR WNEND auth"), "wrong key: %s", r);
    CK(PT.n == 1 && p_changed == 0, "nothing applied");
    CK(frame("WNEND", "t=0000000000000000000000000000000000000000000000000000000000000000", r, sizeof r, 1000) == 1 && !strcmp(r, "ERR WNEND expired"), "the nonce was burnt: %s", r);

    p_reset();                                     /* flipped ciphertext */
    {
        uint8_t nonce[NWP_NONCE], ct[NWP_PAYLOAD_MAX], tag[NWP_TAG];
        char a[300], b64[260], h[65];
        frame("WNBEGIN", NULL, r, sizeof r, 0);
        for (int i = 0; i < NWP_NONCE; i++) { unsigned v; sscanf(r + 13 + i * 2, "%2x", &v); nonce[i] = (uint8_t)v; }
        n = payload(p, NWP_OP_ADD, 0, 255, &s, 1);
        seal(SECRET, nonce, p, n, ct, tag);
        ct[3] ^= 1;
        nl_b64_encode(ct, n, b64, sizeof b64);
        snprintf(a, sizeof a, "o=0 d=%s", b64);
        frame("WNDATA", a, r, sizeof r, 0);
        tohex(tag, 32, h);
        snprintf(a, sizeof a, "t=%s", h);
        frame("WNEND", a, r, sizeof r, 0);
        CK(!strcmp(r, "ERR WNEND auth") && PT.n == 1, "tampered: %s", r);

        /* replay of a good envelope against a new nonce */
        frame("WNBEGIN", NULL, r, sizeof r, 0);
        ct[3] ^= 1;
        nl_b64_encode(ct, n, b64, sizeof b64);
        snprintf(a, sizeof a, "o=0 d=%s", b64);
        frame("WNDATA", a, r, sizeof r, 0);
        snprintf(a, sizeof a, "t=%s", h);
        frame("WNEND", a, r, sizeof r, 0);
        CK(!strcmp(r, "ERR WNEND auth") && PT.n == 1, "replay: %s", r);
    }

    p_reset();
    frame("WNBEGIN", NULL, r, sizeof r, 0);
    CK(frame("WNDATA", "o=0 d=AAAA", r, sizeof r, NWP_TTL_MS + 1) == 1 && !strcmp(r, "ERR WNDATA expired"), "expiry: %s", r);
    frame("WNBEGIN", NULL, r, sizeof r, 0);
    CK(frame("WNDATA", "o=7 d=AAAA", r, sizeof r, 1) == 1 && !strcmp(r, "ERR WNDATA format"), "wrong offset: %s", r);
    CK(frame("WNDATA", "o=0 d=AAAA", r, sizeof r, 1) == 1 && !strcmp(r, "ERR WNDATA expired"), "a broken transfer is over: %s", r);
    frame("WNBEGIN", NULL, r, sizeof r, 0);
    CK(frame("WNDATA", "o=0 d=!!!!", r, sizeof r, 1) == 1 && !strcmp(r, "ERR WNDATA format"), "bad base64: %s", r);
    frame("WNBEGIN", NULL, r, sizeof r, 0);
    CK(frame("WNEND", "t=xyz", r, sizeof r, 1) == 1 && !strcmp(r, "ERR WNEND format"), "bad tag: %s", r);
    frame("WNBEGIN", NULL, r, sizeof r, 0);
    {
        size_t off = 0;
        char a[300], b64[260];
        uint8_t z[180];
        memset(z, 0, sizeof z);
        nl_b64_encode(z, sizeof z, b64, sizeof b64);
        for (int i = 0; i < 8; i++) {
            snprintf(a, sizeof a, "o=%zu d=%s", off, b64);
            frame("WNDATA", a, r, sizeof r, 1);
            off += 180;
            if (strncmp(r, "OK", 2)) break;
        }
        CK(!strcmp(r, "ERR WNDATA full"), "more than the largest envelope: %s", r);
    }
    frame("WNBEGIN", NULL, r, sizeof r, 0);
    CK(frame("WNABORT", NULL, r, sizeof r, 1) == 1 && !strcmp(r, "OK WNABORT") && !P.open, "abort");
    CK(frame("WNBEGIN", "x=1", r, sizeof r, 1) == 1 && !strcmp(r, "ERR WNBEGIN format"), "no arguments");
    p_key_val = "";
    CK(frame("WNBEGIN", NULL, r, sizeof r, 1) == 1 && !strcmp(r, "ERR WNBEGIN nokey"), "no OTA key: no provisioning");
    CK(frame("PRESCFG", NULL, r, sizeof r, 1) == 0, "other commands pass through");

    /* semantic refusals keep everything */
    p_reset();
    p_active = 0;
    n = payload(p, NWP_OP_REPLACE, 0, 0, &s, 1);
    uart_send(p, n, SECRET, r, sizeof r, 1);
    CK(!strcmp(r, "ERR WNEND active") && PT.n == 1 && !strcmp(PT.net[0].ssid, HOME), "replace cannot lock out: %s", r);
    n = payload(p, NWP_OP_ADD, NWP_FL_UPDATE, 255, &h2, 1);
    uart_send(p, n, SECRET, r, sizeof r, 1);
    CK(!strcmp(r, "ERR WNEND active") && !strcmp(PT.net[0].psk, HOMEPW), "a typo cannot lock out: %s", r);
    n = payload(p, NWP_OP_REPLACE, NWP_FL_FORCE, 0, &s, 1);
    uart_send(p, n, SECRET, r, sizeof r, 1);
    CK(!strncmp(r, "OK WNEND n=1", 12) && !strcmp(PT.net[0].ssid, SPOT), "force: %s", r);
    CK(!strcmp(kv_str("ssid"), SPOT) && !strcmp(kv_str("psk"), SPOTPW), "the mirror follows");

    p_reset();
    n = payload(p, NWP_OP_ADD, 0, 255, &s, 1);
    KV.writes = 0; KV.die_after = 0;
    uart_send(p, n, SECRET, r, sizeof r, 1);
    CK(!strcmp(r, "ERR WNEND store") && PT.n == 1 && p_changed == 0, "NVS refused: %s", r);
    KV.die_after = -1;

    /* malformed payloads, authentically sealed */
    p_reset();
    static const uint8_t bad[][12] = { { 2, 1, 0, 0, 0 }, { 1, 9, 0, 0, 0 }, { 1, 1, 4, 0, 0 }, { 1, 1, 0, 0, 9 }, { 1, 1, 0, 0, 1, 40, 'a' }, { 1, 1, 0, 0, 1, 1, 'a', 3, 'a', 'b', 'c', 0 } };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        uart_send(bad[i], 12, SECRET, r, sizeof r, 1);
        CK(!strncmp(r, "ERR WNEND ", 10) && PT.n == 1, "payload %zu: %s", i, r);
    }
    n = payload(p, NWP_OP_ADD, 0, 255, &s, 1);
    p[n] = 0;
    uart_send(p, n + 1, SECRET, r, sizeof r, 1);
    CK(!strcmp(r, "ERR WNEND format"), "trailing byte: %s", r);
    n = payload(p, NWP_OP_DEL, 0, 0, (nw_net[]){ s, s }, 2);
    uart_send(p, n, SECRET, r, sizeof r, 1);
    CK(!strcmp(r, "ERR WNEND format"), "delete takes one: %s", r);
    n = payload(p, NWP_OP_REPLACE, 0, 0, NULL, 0);
    uart_send(p, n, SECRET, r, sizeof r, 1);
    CK(!strcmp(r, "ERR WNEND empty"), "empty replace: %s", r);
    srand(2);
    for (int k = 0; k < 20000; k++) {              /* parser fuzz: bounds are what ASan watches here */
        size_t m = 5 + (size_t)rand() % 120;
        nwp_op o;
        for (size_t i = 0; i < m; i++) p[i] = (uint8_t)(rand() & 0x7f);
        p[0] = 1; p[1] = (uint8_t)(1 + rand() % 3); p[2] = 0; p[4] = (uint8_t)(rand() % 3);
        nwp_parse(p, m, &o);
    }

    /* HTTP envelope */
    p_reset();
    {
        uint8_t body[NWP_HTTP_MAX + 8], nonce[NWP_NONCE];
        char hx[40], js[64];
        CK(nwp_http_nonce(&P, hx, sizeof hx, 5) == NW_OK && strlen(hx) == 32, "nonce");
        memcpy(nonce, P.nonce, sizeof nonce);
        n = payload(p, NWP_OP_ADD, 0, 255, &s, 1);
        memcpy(body, nonce, NWP_NONCE);
        seal(SECRET, nonce, p, n, body + NWP_NONCE, body + NWP_NONCE + n);
        size_t bl = NWP_NONCE + n + NWP_TAG;
        CK(nwp_http_post(&P, body, bl, 6, js, sizeof js) == 200 && strstr(js, "\"ok\":1") && PT.n == 2, "POST /wifi: %s", js);
        log_add(js);
        CK(nwp_http_post(&P, body, bl, 7, js, sizeof js) == 400 && strstr(js, "expired") && PT.n == 2, "replayed POST: %s", js);
        nwp_http_nonce(&P, hx, sizeof hx, 8);
        CK(nwp_http_post(&P, body, bl, 9, js, sizeof js) == 400 && strstr(js, "expired"), "stale nonce in the body: %s", js);
        nwp_http_nonce(&P, hx, sizeof hx, 8);
        memcpy(nonce, P.nonce, sizeof nonce);
        memcpy(body, nonce, NWP_NONCE);
        seal("ffffffffffffffffffffffffffffffff", nonce, p, n, body + NWP_NONCE, body + NWP_NONCE + n);
        CK(nwp_http_post(&P, body, bl, 9, js, sizeof js) == 403 && strstr(js, "auth"), "wrong key: %s", js);
        CK(nwp_http_post(&P, body, 10, 9, js, sizeof js) == 400, "too short");
        CK(nwp_http_post(&P, body, NWP_HTTP_MAX + 1, 9, js, sizeof js) == 400, "too long");
        p_key_val = "";
        CK(nwp_http_nonce(&P, hx, sizeof hx, 8) == NW_E_NOKEY, "no key");
        p_key_val = SECRET;
    }

    /* WIFISET / WIFICLR */
    p_reset();
    CK(nwp_wifiset(&P, HOME, HOMEPW) == NW_OK && p_changed == 0, "same login: no write, no reconnect");
    CK(nwp_wifiset(&P, "new-net", "new-password") == NW_OK && PT.n == 2 && p_primary == 1, "new primary, old one kept");
    CK(!strcmp(kv_str("ssid"), "new-net") && !strcmp(kv_str("psk"), "new-password"), "mirror follows");
    CK(nwp_wifiset(&P, "x", "short") == NW_E_FORMAT, "refused");
    CK(nwp_clear(&P) == NW_OK && PT.n == 0 && !kv_find(&KV, "ssid") && !kv_find(&KV, "psk"), "WIFICLR takes everything");
    nw_table after;
    CK(nw_load(&after, &KVH) == NW_LD_BLOB && after.n == 0, "and nothing comes back");
    kv_reset();
    kv_legacy(HOME, "short");
    nw_load(&PT, &KVH);
    CK(nwp_clear(&P) == NW_OK && !kv_find(&KV, "ssid") && !kv_find(&KV, "psk"), "even legacy keys that were never usable");
}

static void t_redaction(void)
{
    /* every reply and every buffer the transaction leaves behind */
    uint8_t p[NWP_PAYLOAD_MAX];
    char r[320];
    nw_net s = mk(SPOT, SPOTPW), h2 = mk(HOME, "SECRET-TYPO-PASSWORD");
    size_t n;
    LOG[0] = 0;
    t_provision();
    p_reset();
    n = payload(p, NWP_OP_ADD, 0, 255, &s, 1);
    uart_send(p, n, SECRET, r, sizeof r, 1);
    n = payload(p, NWP_OP_ADD, NWP_FL_UPDATE, 255, &h2, 1);
    uart_send(p, n, SECRET, r, sizeof r, 1);
    CK(!strstr(LOG, SPOT) && !strstr(LOG, SPOTPW) && !strstr(LOG, HOME) && !strstr(LOG, HOMEPW) && !strstr(LOG, "TYPO") && !strstr(LOG, SECRET), "replies carry no credential");
    static const uint8_t zero[sizeof P.op] = { 0 };
    CK(!memcmp(&P.op, zero, sizeof P.op), "decoded request wiped");
    CK(!memcmp(&P.next, zero, sizeof P.next < sizeof zero ? sizeof P.next : sizeof zero) && !memcmp(&P.old, zero, sizeof P.old < sizeof zero ? sizeof P.old : sizeof zero), "working tables wiped");
    CK(!P.open && P.len == 0, "session closed");
    for (size_t i = 0; i < sizeof P.buf; i++) CK(P.buf[i] == 0, "ciphertext buffer wiped");
    for (int c = NW_E_EXPIRED; c <= NW_SAME; c++) { const char *w = nw_err_word(c); CK(w && strlen(w) < 10, "word %d", c); }
}

int main(void)
{
    t_migration();
    t_validation();
    t_ops();
    t_blob();
    t_power();
    t_rollback_interplay();
    t_priority();
    t_wrong_password();
    t_no_dhcp();
    t_hotspot_comes_and_goes();
    t_stable_fallback();
    t_reconnect();
    t_ip_loss();
    t_ota();
    t_hidden();
    t_scan_failure();
    t_stale_disconnect();
    t_wrap();
    t_table_change();
    t_crypto();
    t_provision();
    t_redaction();
    printf("test_wifinet: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
