/* wt32-r5.1: the saved Wi-Fi networks.  Portable C (no Arduino, no ESP-IDF): the firmware and
 * tests/test_wifinet.c compile this same file.
 *
 * The table is priority ordered: net[0] is the primary, later entries are fallbacks.  It is stored as ONE
 * NVS blob (key "wnet"), so a change is all or nothing: nvs_set_blob writes the new version before it erases
 * the old one (nvs_storage.cpp, Storage::writeItem), and the blob carries its own CRC.
 *
 * The legacy keys "ssid"/"psk" stay as a mirror of the primary, so r5.0.1 and r4.0.1 still find a network
 * after a rollback.  They are two keys and cannot change atomically with the blob, hence the "pend" flag:
 *   1. blob with the new table, pend=1      (the commit point)
 *   2. legacy keys rewritten / removed
 *   3. blob with pend=0
 * A boot that finds pend=1 finishes steps 2-3.  A boot that finds pend=0 and legacy keys that disagree knows an
 * older firmware changed them after a rollback, and honours that.  n=0 is a real table: WIFICLR's tombstone,
 * so a power cut during WIFICLR can never bring a network back from the legacy keys.
 *
 * Secrets: nothing here prints or logs.  nw_wipe() clears every temporary. */
#ifndef NORI_WNET_H
#define NORI_WNET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NW_MAX 8
#define NW_SSID_MAX 32
#define NW_PSK_MAX 64              /* 8..63 passphrase, or exactly 64 hex digits; 0 = open network */
#define NW_F_HIDDEN 0x01           /* may be tried without being seen in a scan (bounded, see nori_wnsm.h) */
#define NW_FLAGS_ALL NW_F_HIDDEN
#define NW_BLOB_VER 1
#define NW_BLOB_MAX (16 + NW_MAX * (3 + NW_SSID_MAX + NW_PSK_MAX))
#define NW_K_BLOB "wnet"
#define NW_K_SSID "ssid"
#define NW_K_PSK "psk"

enum { NW_SAME = 1, NW_OK = 0, NW_E_FORMAT = -1, NW_E_FULL = -2, NW_E_EXISTS = -3, NW_E_ACTIVE = -4, NW_E_EMPTY = -5,
       NW_E_STORE = -6, NW_E_DUP = -7, NW_E_ABSENT = -8, NW_E_AUTH = -9, NW_E_NOKEY = -10, NW_E_EXPIRED = -11 };
/* One word for a reply; never carries a value from the request. */
const char *nw_err_word(int code);

/* how nw_load() found the table */
enum { NW_LD_EMPTY, NW_LD_BLOB, NW_LD_MIGRATED, NW_LD_CORRUPT_LEGACY, NW_LD_CORRUPT_EMPTY, NW_LD_MIRRORED, NW_LD_ADOPTED,
       NW_LD_CLEARED, NW_LD_LEGACY_BAD, NW_LD_COUNT };
extern const char *const NW_LD_NAME[NW_LD_COUNT];

typedef struct {
    uint8_t ssid_len, psk_len, flags;
    char ssid[NW_SSID_MAX + 1];
    char psk[NW_PSK_MAX + 1];
} nw_net;

typedef struct {
    uint8_t n, pend;
    uint32_t gen;                  /* +1 per commit; reported, so a tool can see that its change landed */
    nw_net net[NW_MAX];
} nw_table;

/* get_blob: length, -1 absent, -2 read error.  get_str: length without the NUL, -1 absent, -2 read error.
 * put_*, del: 0 ok (del of an absent key is ok). */
typedef struct {
    void *ctx;
    int (*get_blob)(void *ctx, const char *key, uint8_t *b, size_t cap);
    int (*put_blob)(void *ctx, const char *key, const uint8_t *b, size_t n);
    int (*get_str)(void *ctx, const char *key, char *out, size_t cap);
    int (*put_str)(void *ctx, const char *key, const char *s);
    int (*del)(void *ctx, const char *key);
} nw_kv;

void nw_wipe(void *p, size_t n);
uint32_t nw_crc32(const uint8_t *b, size_t n);

/* Fill *o, validating everything: SSID 1..32 bytes without NUL, password as NW_PSK_MAX says, known flags. */
int nw_net_set(nw_net *o, const char *ssid, size_t ssid_len, const char *psk, size_t psk_len, uint8_t flags);
int nw_find(const nw_table *t, const char *ssid, size_t ssid_len);          /* index or -1 */
int nw_same_login(const nw_net *a, const nw_net *b);                        /* same SSID and password */

/* Table edits.  All work on *t in RAM and leave it untouched on any error, so nothing is ever half applied.
 * active: index of the network in use now, or -1.  Without force, an edit that removes or changes the login of
 * the active network or of the primary is refused (NW_E_ACTIVE): a typo cannot lock the device out. */
/* WIFISET: this network becomes the primary.  Known SSID: password updated, moved to the front.  New SSID:
 * inserted at the front, everything else moves down one and nothing is dropped (NW_E_FULL at NW_MAX).
 * NW_SAME: already the primary, nothing to do. */
int nw_op_primary(nw_table *t, const nw_net *net);
/* Additive.  pos: index to insert new SSIDs at, >= n appends.  A known SSID is NW_E_EXISTS unless update. */
int nw_op_add(nw_table *t, const nw_net *nets, int k, int pos, int update, int active, int force);
/* The whole list, in order.  An empty list is NW_E_EMPTY: forgetting everything is WIFICLR's job. */
int nw_op_replace(nw_table *t, const nw_net *nets, int k, int active, int force);
/* The last network cannot be deleted (NW_E_EMPTY), for the same reason. */
int nw_op_del(nw_table *t, const char *ssid, size_t ssid_len, int active, int force);

int nw_encode(const nw_table *t, uint8_t *b, size_t cap);                   /* bytes, or -1 */
int nw_decode(nw_table *t, const uint8_t *b, size_t n);                     /* NW_OK or NW_E_FORMAT; validates every entry */

/* Boot.  Returns NW_LD_*; *t is the table to use, also if a write failed (it is retried on the next boot). */
int nw_load(nw_table *t, const nw_kv *kv);
/* Make *next the stored table, see the top of this file.  NW_OK once step 1 succeeded (*cur = the new table);
 * NW_E_STORE: nothing changed, *cur is untouched. */
int nw_commit(nw_table *cur, const nw_table *next, const nw_kv *kv);

#ifdef __cplusplus
}
#endif
#endif
