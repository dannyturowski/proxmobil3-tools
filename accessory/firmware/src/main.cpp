// Nori accessory firmware for the Wireless-Tag WT32-ETH01 (original ESP32, LAN8720A Ethernet PHY).
// wt32-r5.1.3 = wt32-r5.1.2 plus loop() on the task watchdog (nori_lwdt.h; ArduinoOTA transfers guarded), mDNS off and
// ArduinoOTA no longer recreated on address changes, bounded HTTP replies, a retained phase record (nori_phase.h, DIAG
// p=ph), WDTTEST.  Same partitions, sdkconfig, bootloader, network role, Wi-Fi behaviour and pins (docs/watchdog-r5.1.3.md).
// wt32-r5.1.2 = wt32-r5.1.1 plus DIAG p=zg, the 4x4 depth frame for a host-side debug overlay; plain DIAG unchanged
// wt32-r5.1.1 = wt32-r5.1.0 plus short and long selector gestures (EV tsh5/tsh1/tlg5/tlg1, ni_gesture); trk5/trk1 unchanged
// wt32-r5.1.0 = wt32-r5.0.1 plus saved Wi-Fi networks: up to 8 in priority order with automatic fallback
// (shared/nori_wifi; README "Saved Wi-Fi networks").  Same partitions, sdkconfig and bootloader.
// wt32-r5.0.0 = wt32-r4.0.1 plus the three-way track selector on GPIO2/GPIO15 (EV trk5 / trk1); nothing else
// changes: same network role, OTA, serial link, partitions and sdkconfig.
// wt32-r4.0.1 = wt32-r3.0.0 (VL53L5CX presence task, boot/reset/network diagnostics, the proven-management
// mark-valid rule, Digest HTTP + ArduinoOTA bound to all interfaces) with a new network role: the WT32 is
// the PM3's router.
//  - Wi-Fi station = upstream, always on while provisioned (WIFISET + WIFIEN on=1, as before): DHCP
//    client, default route.  (r1/r3 ran it only as a fallback while Ethernet had no DHCP address.)
//  - Ethernet = private point-to-point LAN to the PM3: static 192.168.77.1/24, no DHCP client.  A small
//    DHCP server (shared/nori_router/nori_dhcps.c + nori_lan.c), bound to the Ethernet netif only, leases
//    192.168.77.2 to the PM3 with router .1 and the upstream DNS servers.  It stands down if the
//    Ethernet segment turns out to be a real LAN (another DHCP server or the upstream gateway seen).
//  - Real IPv4 NAPT (esp-lwip ip4_napt.c, from wt32-r2's reviewed router build): the PM3 reaches the
//    LAN/Internet with the WT32's Wi-Fi address; one DNAT port map <Wi-Fi IP>:2222/tcp ->
//    192.168.77.2:2222 (the PM3's sshd); an inbound guard drops Wi-Fi packets addressed to 192.168.77.0/24.
//  - The serial SSH recovery tunnel (UART link + nori-linkd) moves to TCP 2223, independent of routing.
//  - Management (HTTP 80, ArduinoOTA 3232) stays bound to all interfaces: reachable from the Wi-Fi LAN
//    and from the PM3 at 192.168.77.1.  The mark-valid rule needs Wi-Fi management to be proven.
// Threading: every lwIP netif/NAPT/DHCP-server action runs in the tcpip thread (nat_cb, posted with
// tcpip_try_callback; the DHCP pcb's receive callback); loop() never waits on the stack.
//
// UART0 carries nothing but protocol frames: no logging, no debug prints.  ESP-IDF logging is off;
// the only non-frame text that can ever appear is the ROM boot banner and a panic dump after a crash
// (outside normal operation; tools/nori_serial_log.py keeps them on the PC).
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ETH.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_ota_ops.h>
#include <esp_rom_sys.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <lwip/dns.h>
#include <lwip/netif.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <stdarg.h>

extern "C" {
#include "nori_dev.h"
#include "nori_evlog.h"
#include "nori_imgvalid.h"
#include "nori_input.h"
#include "nori_lwdt.h"
#include "nori_phase.h"
#include "nori_presence.h"
#include "nori_rst.h"
#include "nori_tofsm.h"
}
#include "nori_dhcps.h"
#include "nori_http.h"
#include "nori_lan.h"
#include "nori_nat.h"
#include "nori_tof.h"
#include "nori_wnet.h"
#include "nori_wnprov.h"
#include "nori_wnsm.h"

#ifndef NORI_SENSOR
#define NORI_SENSOR 0
#endif
#ifndef NORI_HOSTNAME
#define NORI_HOSTNAME "WT32-ETH01"
#endif
#ifndef NORI_TUNNEL_PORT
#define NORI_TUNNEL_PORT 2223      // serial SSH recovery tunnel (r1/r3: 2222, which is now the DNAT port)
#endif
#ifndef NORI_DNAT_PORT
#define NORI_DNAT_PORT 2222        // Wi-Fi TCP port forwarded to the PM3's sshd
#endif
#ifndef NORI_LAN_DHCP
#define NORI_LAN_DHCP 1            // the Ethernet DHCP server (0: PM3 must be configured statically, as for r2)
#endif
#ifndef NORI_OTA_MDNS
// wt32-r5.1.3: off (was 1, WT32-ETH01.local).  ArduinoOTA's end()/begin() then call MDNS.end()/begin(), whose
// ESP-IDF code waits without a timeout for the mDNS task (mdns_free, mdns_hostname_set), from loop(), on every address
// change.  The uploaders address the board by IP.  1 restores the advertisement and the restart on address changes.
#define NORI_OTA_MDNS 0
#endif
#ifndef NORI_HTTP_PORT
#define NORI_HTTP_PORT 80
#endif
#ifndef NORI_ETH_ADDR
#define NORI_ETH_ADDR 1            // WT32-ETH01: LAN8720A at PHY address 1
#endif
#ifndef NORI_ETH_POWER
#define NORI_ETH_POWER 16          // PHY / 50 MHz oscillator enable
#endif
#ifndef NORI_ETH_MDC
#define NORI_ETH_MDC 23
#endif
#ifndef NORI_ETH_MDIO
#define NORI_ETH_MDIO 18
#endif
#ifndef NORI_TOF_DEFAULT_EN
#define NORI_TOF_DEFAULT_EN 1      // SENSOR en=0 (persisted) turns the sensor task's I2C off
#endif

// The uploader (tools/nori_http_ota.py) reads the expected version from this tag.
extern const char nori_fw_tag[];
const char nori_fw_tag[] = "NORI_FW=" NORI_FW_VERSION;
static const char *const FW = nori_fw_tag + 8;

// ---- pin sanity, at compile time -------------------------------------------
// Reserved on the WT32-ETH01: 0-3 (GPIO0 = RMII 50 MHz clock in + boot strap, UART0), 5/12/15
// (straps), 6-11 (flash), 16 (PHY/oscillator enable), 17 (kept free as on r5), and the RMII/SMI
// pins 18 MDIO, 19 TXD0, 21 TX_EN, 22 TXD1, 23 MDC, 25 RXD0, 26 RXD1, 27 CRS_DV.
// SDA 33 is the header pin silk-screened "485_EN", SCL 32 the one silk-screened "CFG".
// wt32-r5, the one narrow exception: the track selector uses straps GPIO2 and GPIO15.  They stay "reserved"
// below, so nothing else can claim them; these two are approved only as inputs whose contact goes to ground.
// Neither is ever driven.  Held low through reset: GPIO2 low is what a normal boot wants anyway (it matters
// only with GPIO0 low, for download mode, where low is also correct); GPIO15 low only silences the ROM boot
// log on UART0, which the link treats as noise anyway.  A contact to 3V3 would be wrong on both.
constexpr bool exists(int p) { return p >= 0 && p <= 39 && p != 20 && p != 24 && !(p >= 28 && p <= 31); }
constexpr bool rmii(int p) { return p == 18 || p == 19 || p == 21 || p == 22 || p == 23 || p == 25 || p == 26 || p == 27; }
constexpr bool reserved(int p) { return p <= 3 || p == 5 || (p >= 6 && p <= 11) || p == 12 || p == 15 || p == 16 || p == 17 || rmii(p); }
constexpr bool io_ok(int p) { return exists(p) && !reserved(p) && p < 34; }      // needs output / pull-up
static_assert(io_ok(NORI_PIN_SDA) && io_ok(NORI_PIN_SCL), "I2C pins: not a strap/flash/RMII/UART0 pin, and < 34");
static_assert(NORI_PIN_BTN1 < 0 || io_ok(NORI_PIN_BTN1), "button 1 pin needs an internal pull-up (< 34, not reserved)");
static_assert(NORI_PIN_BTN2 < 0 || io_ok(NORI_PIN_BTN2), "button 2 pin needs an internal pull-up (< 34, not reserved)");
static_assert(NORI_PIN_SDA != NORI_PIN_SCL && NORI_PIN_BTN1 != NORI_PIN_BTN2, "pins must differ");
static_assert(NORI_PIN_TOG_FIVE == 15 && NORI_PIN_TOG_ONE == 2, "wt32-r5: only GPIO2 and GPIO15 are approved strap inputs, ground-only");
static_assert(reserved(NORI_PIN_TOG_FIVE) && reserved(NORI_PIN_TOG_ONE), "the exception is for the selector alone: the straps stay reserved");
static_assert(NORI_PIN_BTN1 != NORI_PIN_SDA && NORI_PIN_BTN1 != NORI_PIN_SCL && NORI_PIN_BTN2 != NORI_PIN_SDA && NORI_PIN_BTN2 != NORI_PIN_SCL, "pins must differ");
static_assert(NORI_ETH_POWER == 16 && NORI_ETH_MDC == 23 && NORI_ETH_MDIO == 18, "the reserved-pin table above assumes the WT32-ETH01 PHY wiring");
static_assert(NORI_SENSOR == 1, "wt32-r4: NORI_SENSOR=1 is the VL53L5CX");
static_assert(ND_TAP_AXIS == 0 && ND_I2CSCAN == 0 && ND_GAME == 0, "motion-build options need a motion sensor");
static_assert(ND_PRESENCE == 1 && ND_EXT == 1 && ND_DIAG_JOIN == 1, "wt32-r4 core flags (as wt32-r3)");
// The DNAT port is translated before lwIP looks for local listeners: nothing local may use it.
static_assert(NORI_TUNNEL_PORT != NORI_DNAT_PORT && NORI_HTTP_PORT != NORI_DNAT_PORT && NORI_DNAT_PORT != 3232,
              "local listeners must not share the DNAT port");
static_assert(ESP_RST_POWERON == 1 && ESP_RST_SW == 3 && ESP_RST_BROWNOUT == 9 && ESP_RST_SDIO == 10 && NR_NAMES == ESP_RST_SDIO + 1,
              "nori_rst.h names follow esp_reset_reason_t of ESP-IDF 4.4");

static const size_t TX_BUF = 4096;
static const size_t RX_BUF = 8192;
static Preferences prefs;
static nd_dev dev;
static const char *const SENSOR_NAME = "vl53l5cx";

// ---- reset diagnostics and the transition log (RAM / RTC RAM only, never NVS) ----------
RTC_NOINIT_ATTR static nr_rtc rst_rtc;            // survives resets that keep RTC memory powered; validated by nr_boot()
RTC_NOINIT_ATTR static nr_trace rst_trace;        // this boot's trace; the previous boot's is copied to prev_trace
static nr_trace prev_trace;
static int prev_ok;
static unsigned rst_reason, rst_hw;               // esp_reset_reason(), raw PRO CPU reset cause
static int rst_kept;                              // 1: rst_rtc held a valid history from earlier boots
static portMUX_TYPE lg_mux = portMUX_INITIALIZER_UNLOCKED;
static nlg_ring lg;                               // guarded by lg_mux (network event task, loop, ToF task)

static void log_ev(uint8_t code, uint8_t a, uint16_t b)
{
    nlg_ev e = { (uint32_t)millis(), code, a, b };
    portENTER_CRITICAL(&lg_mux);
    nlg_add(&lg, e.t_ms, code, a, b);
    nr_trace_ev(&rst_trace, &e);
    portEXIT_CRITICAL(&lg_mux);
}
static void log_copy(nlg_ring *out)
{
    portENTER_CRITICAL(&lg_mux);
    *out = lg;
    portEXIT_CRITICAL(&lg_mux);
}

// ---- wt32-r5.1.3: what loop() is doing (RTC, nori_phase.h) and loop() on the task watchdog (nori_lwdt.h) ------------
RTC_NOINIT_ATTR static nph_rec ph_rtc;            // this boot's; the previous boot's is copied to ph_prev
static nph_rec ph_prev;
static int ph_prev_ok;
// Enter a phase; returns the one left, so a call inside another phase can restore it.  loop() context only.
static uint32_t ph_go(uint32_t p)
{
    uint32_t d, was = nph_enter(&ph_rtc, p, millis(), &d);
    if (d >= NPH_SLOW_MS) log_ev(NLG_SLOW, (uint8_t)was, (uint16_t)(d > 65535 ? 65535 : d));
    return was;
}
static TaskHandle_t loop_task;
static esp_timer_handle_t guard_timer, alive_timer;
static nwd lwd;
static int wd_add(void *) { return esp_task_wdt_add(loop_task) == ESP_OK ? 0 : -1; }
static int wd_del(void *) { return esp_task_wdt_delete(loop_task) == ESP_OK ? 0 : -1; }
static void wd_reset(void *) { esp_task_wdt_reset(); }
// Stop, then start: a failed start leaves NO guard running (nwd_progress() fails closed on that).
static int wd_arm(void *, uint32_t ms)
{
    if (!guard_timer) return -1;
    esp_timer_stop(guard_timer);                  // not running: ESP_ERR_INVALID_STATE, harmless
    return esp_timer_start_once(guard_timer, (uint64_t)ms * 1000) == ESP_OK ? 0 : -1;
}
static void wd_disarm(void *) { if (guard_timer) esp_timer_stop(guard_timer); }
static void wd_fail(void *)                       // loop(): the guard could not be re-armed nor the watchdog restored
{
    uint32_t t = millis();
    ph_rtc.guard_ms = t ? t : 1;                  // reads like the guard firing (pgd>0), in phase ota_xfer
    esp_system_abort("nori: ArduinoOTA guard lost");
}
static const nwd_hal WD_HAL = { nullptr, wd_add, wd_del, wd_reset, wd_arm, wd_disarm, wd_fail };
static void wdt_feed() { nwd_feed(&lwd); }        // every pass, and inside every bounded wait loop
static void ota_hole_begin() { nwd_hole_begin(&lwd, millis()); ph_rtc.wdt = (uint32_t)lwd.state; }
static void ota_hole_end() { nwd_hole_end(&lwd, millis()); ph_rtc.wdt = (uint32_t)lwd.state; }
static void guard_cb(void *)                      // esp_timer task: an ArduinoOTA transfer stopped making progress
{
    if (!nwd_guard_due(&lwd)) return;
    uint32_t t = millis();
    ph_rtc.guard_ms = t ? t : 1;
    esp_system_abort("nori: ArduinoOTA transfer stalled");   // panic reset: no shutdown handlers that could hang too
}
static void alive_cb(void *) { nph_alive(&ph_rtc, millis()); }   // esp_timer task, every NPH_ALIVE_MS
static uint32_t wdt_test_at;                      // WDTTEST: when loop() stops feeding on purpose, 0 = never

// ---- UART ---------------------------------------------------------------------
static size_t u_room(void *) { return (size_t)Serial.availableForWrite(); }
static size_t u_queued(void *)
{
    size_t room = (size_t)Serial.availableForWrite();
    return room >= TX_BUF ? 0 : TX_BUF - room;
}
static void u_write(void *, const char *s, size_t n) { Serial.write((const uint8_t *)s, n); }

// ---- Ethernet: the private LAN to the PM3 (static address, DHCP server, NAPT) ----------
static const IPAddress ETH_IP(192, 168, 77, 1), ETH_MASK(255, 255, 255, 0), PM3_IP(192, 168, 77, 2);
// Flags and counters are set from the network event task; loop() only reads them.
static bool eth_begin_ok, eth_cfg_ok;
static volatile bool eth_started, eth_link;
static volatile uint32_t eth_link_ups, eth_link_downs, eth_ips;
static uint32_t eth_cfg_fixes, eth_noaddr_since;   // static address re-applied by net_service()

// ---- routing + LAN service (lwIP state lives in the tcpip thread) ----------------------
static const nn_cfg NAT_CFG = { (uint32_t)PM3_IP, NORI_DNAT_PORT, NORI_DNAT_PORT, (uint32_t)ETH_IP, (uint32_t)ETH_MASK };
static void lan_on_dhcp(const ndh_out *o);
static void lan_on_lan(int what, int arg);
// One address (.2, the DNAT target): the cable is point-to-point, and a second address could not be reached
// through <Wi-Fi IP>:2222 anyway (r4 review).  120 s while no upstream DNS is known.  wt32-r5.1: lease 10 min (was 1 h):
// DNS comes from the upstream, and a lease is the only way to tell the PM3 it moved, so after a change of network its
// resolver is stale for at most T1 = 5 min instead of 30.
static const nla_cfg LAN_CFG = { { (uint32_t)ETH_IP, (uint32_t)ETH_MASK, (uint32_t)PM3_IP, 1, 600, 120 }, NORI_LAN_DHCP,
                                 lan_on_dhcp, lan_on_lan };
static nn_state nat_tc;                           // written only by nat_cb() in the tcpip thread
static nla_state lan_tc;                          // idem
static uint32_t nat_tc_dns[2];                    // idem: lwIP DNS servers (shared by every netif)
static nn_state nat;                              // loop()'s copies, refreshed when no call is in flight
static nla_state lan;
static uint32_t nat_dns[2];
static struct netif *nat_arg_wifi, *nat_arg_eth;  // arguments of the call in flight
static uint32_t nat_arg_events;
static volatile bool nat_busy;                    // a nat_cb() is queued or running
static volatile bool nat_kick;                    // an event asked for an early reconcile
static uint32_t nat_next, nat_posted, nat_post_fails, nat_slow;   // nat_slow: calls that took > 5 s
static bool nat_slow_seen;
static const uint32_t NAT_PERIOD_MS = 1000;
static const char *const RT_WORD[] = { "init", "nonapt", "nowifi", "noguard", "nolink", "noethip", "defroute", "nodnat", "ok" };
static int rt_last = -1;

static void lan_on_dhcp(const ndh_out *o)         // tcpip thread
{
    const uint8_t *ip = (const uint8_t *)&o->yiaddr;
    log_ev(NLG_DHCP, (uint8_t)o->ev, (uint16_t)(o->mac[5] << 8 | ip[3]));
}
static void lan_on_lan(int what, int arg) { log_ev(NLG_LAN, (uint8_t)what, (uint16_t)arg); }   // tcpip thread

static void nat_cb(void *)                        // tcpip thread
{
    nn_reconcile(nat_arg_wifi, nat_arg_eth, &NAT_CFG, &nat_tc);
    for (int i = 0; i < 2; i++) {
        const ip_addr_t *d = dns_getserver((u8_t)i);
        nat_tc_dns[i] = d && IP_IS_V4(d) ? ip4_addr_get_u32(ip_2_ip4(d)) : 0;
    }
    nla_reconcile(nat_arg_eth, nat_arg_wifi, &LAN_CFG, &lan_tc, nat_tc_dns, nat_arg_events);
    __sync_synchronize();
    nat_busy = false;
}
static struct netif *lwip_netif_of(const char *ifkey)
{
    esp_netif_t *e = esp_netif_get_handle_from_ifkey(ifkey);
    return e ? (struct netif *)esp_netif_get_netif_impl(e) : nullptr;
}
static void nat_service(uint32_t now)
{
    if (nat_busy) {                               // never wait for the stack from loop()
        if (now - nat_posted > 5000 && !nat_slow_seen) { nat_slow_seen = true; nat_slow++; }
        return;
    }
    __sync_synchronize();
    nat = nat_tc;                                 // the last call has finished: its result is stable
    lan = lan_tc;
    nat_dns[0] = nat_tc_dns[0];
    nat_dns[1] = nat_tc_dns[1];
    const char *w = nn_route_state(&nat);
    int k = 0;
    while (k < 9 && strcmp(RT_WORD[k], w)) k++;      // 8 = "ok", 9 = unknown word (never ok)
    if (nat.reconciles && (k == 8) != (rt_last == 8)) log_ev(NLG_ROUTE, k == 8, (uint16_t)k);
    if (nat.reconciles) rt_last = k;
    if (!nat_kick && (int32_t)(now - nat_next) < 0) return;
    nat_kick = false;
    nat_next = now + NAT_PERIOD_MS;
    nat_arg_wifi = lwip_netif_of("WIFI_STA_DEF"); // nn_reconcile / nla_reconcile ignore a netif that left netif_list
    nat_arg_eth = lwip_netif_of("ETH_DEF");
    nat_arg_events = eth_link_ups;
    nat_busy = true;
    nat_slow_seen = false;
    __sync_synchronize();
    nat_posted = now;
    if (tcpip_try_callback(nat_cb, nullptr) != ERR_OK) {   // mailbox full: try again next period
        nat_busy = false;
        nat_post_fails++;
    }
}

static bool eth_up() { return eth_link && nat.eth_up && nat.eth_ip == (uint32_t)ETH_IP; }
static const char *eth_state()
{
    if (!eth_begin_ok) return "fail";
    if (!eth_started) return "init";
    if (!eth_link) return "nolink";
    return eth_up() ? "up" : "noaddr";
}
static const char *ip_s(uint32_t a, char *b) { return nn_ip_str(a, b); }
// The PM3's lease: the first pool address (192.168.77.2), bound.
static const ndh_lease *pm3_lease() { return ndh_primary(&lan.dhcp) == 0 ? &lan.dhcp.l[0] : nullptr; }

// ---- Wi-Fi station: the upstream, always on while provisioned + one TCP client (recovery tunnel) ----
static WiFiServer server(NORI_TUNNEL_PORT);      // bound to all interfaces (INADDR_ANY)
static WiFiClient client;
static bool listening = false, wifi_started = false;
static bool ota_busy = false;                     // an authenticated OTA transfer owns the network
static nw_table wn;                               // wt32-r5.1 saved networks, priority order; never printed, logged or reported
static int wn_load_how;                           // NW_LD_*: what nw_load() found at boot
static nws wsm;                                   // which of them to be on (shared/nori_wifi/nori_wnsm.c)
static nwp_ctx wprov;                             // changes from outside (nori_wnprov.c), UART and HTTP alike
static volatile uint8_t wifi_reason;              // last disconnect reason (802.11 / esp_wifi codes)
static volatile bool wifi_assoc;                  // associated; WL_CONNECTED only comes with the address
static volatile uint32_t wifi_conns, wifi_discs, wifi_ips, wifi_ip_losses;
static uint32_t wifi_starts, wifi_joins;
static int wifi_matches, wifi_best_rssi = -127, wifi_best_auth = -1;   // last scan: known networks seen, the strongest
static int wifi_begin_result;
static uint32_t tun_accepts;

static bool wifi_up() { return wifi_started && WiFi.status() == WL_CONNECTED && (uint32_t)WiFi.localIP() != 0; }
// Management (HTTP, ArduinoOTA) runs while either side has an address; Wi-Fi is the upstream.
static bool net_up() { return wifi_up() || eth_up(); }
static IPAddress net_ip() { return wifi_up() ? WiFi.localIP() : eth_up() ? ETH.localIP() : IPAddress(); }
static int net_kind() { return wifi_up() ? 2 : eth_up() ? 1 : 0; }
static const char *const NET_NAME[] = { "none", "eth", "wifi" };

// The state machine decides; these only do what it asks.  The core's auto-reconnect is off: it would rejoin the last
// network behind the machine's back.  (Its one first_connect retry cannot be turned off; nori_wnsm.c ignores the
// reason-8 disconnect that retry and our own leave produce.)
static void wr_on(void *)
{
    if (wifi_started) return;
    uint32_t was = ph_go(NPH_WIFI_ON);
    WiFi.persistent(false);                       // credentials live in our own NVS namespace only
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(NORI_HOSTNAME);
    WiFi.setSleep(false);                         // no modem sleep: latency and throughput for the router
    WiFi.setAutoReconnect(false);
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);    // also what lets a join find a hidden network: a directed probe
    WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
    wifi_started = true;
    wifi_starts++;
    log_ev(NLG_WIFI_ON, 0, 0);
    ph_go(was);
}
static void wr_off(void *)
{
    if (!wifi_started) return;
    uint32_t was = ph_go(NPH_WIFI_OFF);
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    wifi_assoc = false;
    wifi_started = false;
    nat_kick = true;
    log_ev(NLG_WIFI_OFF, 0, 0);
    ph_go(was);
}
static int wr_scan_start(void *, int bg)          // async: loop() keeps running.  In the background, shorter per channel
{
    uint32_t was = ph_go(NPH_SCAN_START);
    int r = WiFi.scanNetworks(true, true, false, bg ? 150 : 300) == WIFI_SCAN_RUNNING ? 0 : -1;
    ph_go(was);
    return r;
}
static int wr_scan_result(nws_seen *seen);
static int wr_scan_poll(void *, nws_seen *seen)
{
    uint32_t was = ph_go(NPH_SCAN_POLL);
    int r = wr_scan_result(seen);
    ph_go(was);
    return r;
}
static int wr_scan_result(nws_seen *seen)
{
    int found = WiFi.scanComplete();
    if (found == WIFI_SCAN_RUNNING) return 0;
    if (found < 0) { WiFi.scanDelete(); return -1; }
    wifi_matches = 0; wifi_best_rssi = -127; wifi_best_auth = -1;
    for (int i = 0; i < found; ++i) {
        String s = WiFi.SSID(i);
        int k = nw_find(&wn, s.c_str(), s.length());
        if (k < 0) continue;
        int rssi = WiFi.RSSI(i);
        ++wifi_matches;
        if (!seen[k].vis || rssi > seen[k].rssi) { seen[k].vis = 1; seen[k].rssi = (int8_t)rssi; }
        if (rssi > wifi_best_rssi) { wifi_best_rssi = rssi; wifi_best_auth = (int)WiFi.encryptionType(i); }
    }
    WiFi.scanDelete();
    return 1;
}
static void wr_scan_stop(void *)
{
    uint32_t was = ph_go(NPH_SCAN_STOP);
    esp_wifi_scan_stop();
    WiFi.scanDelete();
    ph_go(was);
}
static int wr_join(void *, int i)
{
    uint32_t was = ph_go(NPH_JOIN);
    const nw_net &e = wn.net[i];
    wifi_begin_result = (int)WiFi.begin(e.ssid, e.psk_len ? e.psk : nullptr);
    wifi_joins++;
    ph_go(was);
    return wifi_begin_result == WL_CONNECT_FAILED ? -1 : 0;
}
static void wr_leave(void *)
{
    uint32_t was = ph_go(NPH_LEAVE);
    WiFi.disconnect(false, false);
    ph_go(was);
}
static void wr_note(void *, int what, int i, int cls, int reason)     // indices and codes only
{
    log_ev(NLG_WNET, (uint8_t)(what << 4 | i), (uint16_t)(cls << 8 | (reason & 0xff)));
}
static const nws_hal WSM_HAL = { nullptr, wr_on, wr_off, wr_scan_start, wr_scan_poll, wr_scan_stop, wr_join, wr_leave, wr_note };

// NVS through Preferences, which commits every write.
static int kv_get_blob(void *, const char *k, uint8_t *b, size_t cap)
{
    if (!prefs.isKey(k)) return -1;
    size_t n = prefs.getBytes(k, b, cap);
    return n ? (int)n : -2;
}
static int kv_put_blob(void *, const char *k, const uint8_t *b, size_t n) { return prefs.putBytes(k, b, n) == n ? 0 : -1; }
static int kv_get_str(void *, const char *k, char *out, size_t cap)
{
    if (!prefs.isKey(k)) return -1;
    return prefs.getString(k, out, cap) ? (int)strlen(out) : -2;   // getString counts the NUL: 0 is a failure
}
static int kv_put_str(void *, const char *k, const char *v) { return prefs.putString(k, v) == strlen(v) ? 0 : -1; }
static int kv_del(void *, const char *k) { return !prefs.isKey(k) || prefs.remove(k) ? 0 : -1; }
static const nw_kv WN_KV = { nullptr, kv_get_blob, kv_put_blob, kv_get_str, kv_put_str, kv_del };

static bool wifi_wanted() { return dev.cfg.wifi_on && wn.n > 0; }
static bool wifi_no_bg();
static void wifi_tick(uint32_t now)
{
    nws_in in = { wifi_wanted(), wifi_assoc, wifi_up(), wifi_discs, wifi_reason, ota_busy, wifi_no_bg() };
    nws_tick(&wsm, &wn, &in, now);
}

static uint32_t net_next, net_changes;
static int net_last = -1;
static void net_service(uint32_t now)
{
    if ((int32_t)(now - net_next) < 0) return;
    net_next = now + 250;
    wifi_tick(now);
    // Ethernet keeps its static address (esp_netif restores it on every link-up); re-apply it if the
    // stack still shows none a few seconds into a link.
    if (eth_link && nat.eth_netif && nat.eth_ip != (uint32_t)ETH_IP) {
        if (!eth_noaddr_since) eth_noaddr_since = now ? now : 1;
        else if (now - eth_noaddr_since > 3000) {
            uint32_t was = ph_go(NPH_ETH_CFG);
            eth_cfg_ok = ETH.config(ETH_IP, IPAddress(0, 0, 0, 0), ETH_MASK);   // no gateway, no DNS: Wi-Fi keeps both
            ph_go(was);
            eth_cfg_fixes++;
            eth_noaddr_since = 0;
            nat_kick = true;
        }
    } else {
        eth_noaddr_since = 0;
    }
    int k = net_kind();
    if (k != net_last) {
        if (net_last >= 0) net_changes++;
        net_last = k;
        log_ev(NLG_NET, (uint8_t)k, 0);
    }
}
static const char *wifi_state()
{
    if (!dev.cfg.wifi_on) return "off";
    if (!wn.n) return "nocfg";
    if (!wifi_started) return "start";
    if (wsm.state == NWS_SCAN) return "scan";
    return wifi_up() ? "up" : "joining";
}
static int wifi_channel()
{
    wifi_ap_record_t ap;
    return wifi_up() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.primary : 0;
}

// HELLO/HB "wifi=" field: the upstream station (the address the recovery tunnel and DNAT answer on).
static int w_status(void *, char *ip, size_t cap, int *rssi)
{
    if (wifi_up()) {
        snprintf(ip, cap, "%s", WiFi.localIP().toString().c_str());
        *rssi = WiFi.RSSI();
        return ND_WIFI_UP;
    }
    if (wifi_wanted()) return ND_WIFI_JOINING;
    if (dev.cfg.wifi_on) return ND_WIFI_NOCFG;
    return ND_WIFI_OFF;
}
// WIFISET: see nw_op_primary().  -2 = table full (ERR WIFISET full).
static int w_set(void *, const char *ssid, const char *psk)
{
    int r = nwp_wifiset(&wprov, ssid, psk);
    return r == NW_OK ? 0 : r == NW_E_FULL ? -2 : -1;
}
static int w_clear(void *) { return nwp_clear(&wprov) == NW_OK ? 0 : -1; }   // every network and the legacy keys, see nori_wnet.h
static void wn_changed(void *, const nw_table *old, int primary_named)
{
    nws_remap(&wsm, old, &wn, millis());
    if (primary_named) nws_kick(&wsm);            // WIFISET has always meant "use this one now"
    net_next = millis();
    log_ev(NLG_WNET, 0, wn.n);
}
static int wn_active(void *) { return nws_active(&wsm); }
static void wn_rnd(void *, uint8_t *b, size_t n) { esp_fill_random(b, n); }
static void w_power(void *, int on) { dev.cfg.wifi_on = (uint8_t)on; net_next = millis(); }
static void t_listen(void *, int on)
{
    if (on && !listening) { server.begin(); server.setNoDelay(true); listening = true; }
    else if (!on && listening) { if (client) client.stop(); server.end(); listening = false; }
}
static int t_accept(void *)
{
    if (ota_busy) return 0;
    WiFiClient c = server.available();
    if (!c) return 0;
    client = c;
    client.setNoDelay(true);
    tun_accepts++;
    log_ev(NLG_TUN, 1, 0);
    return 1;
}
static int t_read(void *, uint8_t *b, size_t max)
{
    if (!client) return -1;
    int fd = client.fd();
    if (fd < 0) return -1;
    int n = recv(fd, b, max, MSG_DONTWAIT);
    if (n > 0) return n;
    if (n == 0) return -1;                        // orderly close
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}
static int t_write(void *, const uint8_t *b, size_t n)
{
    if (!client) return -1;
    int fd = client.fd();
    if (fd < 0) return -1;
    int w = send(fd, b, n, MSG_DONTWAIT);
    if (w >= 0) return w;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
}
static void t_close(void *) { if (client) { client.stop(); log_ev(NLG_TUN, 0, 0); } }

// Tap tuning keys (kept so CFG behaves as on r5.2; there is no tap source in this build).
static const char *const K_THS = "ths", *const K_WAIT = "wait", *const K_REFR = "refr";
static nd_cfg saved;                              // what NVS holds; build defaults for absent keys

// r4: write only what changed.
static int c_save(void *, const nd_cfg *c)
{
    if (c->wifi_on != saved.wifi_on) prefs.putUChar("wifi_on", c->wifi_on);
    if (c->tap_ths != saved.tap_ths) prefs.putUChar(K_THS, c->tap_ths);
    if (c->dtap_wait_ms != saved.dtap_wait_ms) prefs.putUShort(K_WAIT, c->dtap_wait_ms);
    if (c->refractory_ms != saved.refractory_ms) prefs.putUShort(K_REFR, c->refractory_ms);
    saved = *c;
    return 0;
}

// ---- presence sensor configuration (NVS: written only by an explicit PRESCFG / SENSOR command) -----
static np_cfg pres_cfg, pres_saved;
static bool tof_en = NORI_TOF_DEFAULT_EN;
static const char *const PK[] = { "p_near", "p_far", "p_min", "p_zones", "p_on", "p_off", "p_stale" };
static void pres_load()
{
    np_defaults(&pres_cfg);
    np_cfg c = pres_cfg;
    c.near_mm = prefs.getUShort(PK[0], c.near_mm);
    c.far_mm = prefs.getUShort(PK[1], c.far_mm);
    c.min_mm = prefs.getUShort(PK[2], c.min_mm);
    c.min_zones = prefs.getUChar(PK[3], c.min_zones);
    c.on_frames = prefs.getUChar(PK[4], c.on_frames);
    c.off_frames = prefs.getUChar(PK[5], c.off_frames);
    c.stale_ms = prefs.getUShort(PK[6], c.stale_ms);
    if (np_cfg_valid(&c)) pres_cfg = c;           // a damaged / inconsistent record falls back to the defaults
    pres_saved = pres_cfg;
    tof_en = prefs.getUChar("tof_en", NORI_TOF_DEFAULT_EN) != 0;
}
static void pres_save()
{
    const np_cfg &c = pres_cfg, &o = pres_saved;
    if (c.near_mm != o.near_mm) prefs.putUShort(PK[0], c.near_mm);
    if (c.far_mm != o.far_mm) prefs.putUShort(PK[1], c.far_mm);
    if (c.min_mm != o.min_mm) prefs.putUShort(PK[2], c.min_mm);
    if (c.min_zones != o.min_zones) prefs.putUChar(PK[3], c.min_zones);
    if (c.on_frames != o.on_frames) prefs.putUChar(PK[4], c.on_frames);
    if (c.off_frames != o.off_frames) prefs.putUChar(PK[5], c.off_frames);
    if (c.stale_ms != o.stale_ms) prefs.putUShort(PK[6], c.stale_ms);
    pres_saved = c;
}

// ---- authenticated ArduinoOTA ---------------------------------------------------------
// Runs only while a network (Wi-Fi upstream, or the private Ethernet) has an address AND an OTA
// password hash is in NVS (provisioned with OTAKEY over the link; there is no default, no AP,
// no unauthenticated mode).  App image only: filesystem uploads are refused, so NVS, the
// partition table and spiffs are never written.  The transfer blocks loop(); the tunnel is
// closed first so the two never compete, the sensor task stops using I2C, and the UART stays
// silent (the host sees heartbeats stop, then TCLOSE or a new boot id).
static ArduinoOTAClass *ota;                      // fresh per start: ArduinoOTA takes the hash only before begin()
static char ota_hash[33];                         // effective OTA secret; never printed
static IPAddress ota_ip;
static const char *ota_last = "none";             // last OTA failure, for DIAG
static bool ota_fs, ota_hold, img_pending;
static uint32_t ota_next, ota_hold_until, ota_auth_fails, ota_starts, ota_errors;
// wt32-r3 mark-valid rule (replaces r1's "both OTA paths up, or 10 minutes at the latest"), r4: the network
// that must be proven is the Wi-Fi upstream (the only way the PC reaches the WT32 and the PM3):
// a pending image is marked valid only once management is proven: after IMG_MIN_MS, either an
// authenticated HTTP request was served on this boot while Wi-Fi + both OTA paths are up, or a Wi-Fi address
// + ArduinoOTA + HTTP were all up continuously for IMG_HEALTHY_MS.  Without that it stays pending
// forever, so the next reset (power cycle, crash, watchdog) boots the previous image.
static const uint32_t IMG_MIN_MS = 30000, IMG_HEALTHY_MS = 60000;
static niv_state img_health;                      // management health (nori_imgvalid.h)
static uint32_t img_retry_at, img_mark_fail;      // a failed mark-valid is retried, counted, never reported as valid
static uint32_t http_auth_ok, http_auth_bad, http_uploads, http_fails;
static uint32_t http_auth_wifi;                  // r4: authenticated requests that arrived on the Wi-Fi address (mark-valid)
static const char *img_why = "-";                 // why the image is (still) pending / how it became valid

static bool wifi_no_bg() { return img_pending || client; }   // nothing optional while a new image proves itself or the tunnel is open

static void ota_stop()
{
    if (!ota) return;
    ota->end();
    delete ota;
    ota = nullptr;
}
// wt32-r5.1.3: the transfer blocks loop() inside ArduinoOTA, which cannot feed the watchdog often enough: it runs in the
// guarded hole (nori_lwdt.h), opened here and closed by ota_on_error() or when handle() returns, whichever comes first.
static void ota_on_start()
{
    ph_go(NPH_OTA_XFER);
    ota_busy = true;
    ota_starts++;
    wifi_tick(millis());                          // stops a background scan now: loop() is about to block
    tof_pause(true);
    if (client) client.stop();                    // the core notices and sends TCLOSE once loop() runs again
    if (listening) server.end();
    ota_fs = ota->getCommand() != U_FLASH;
    if (ota_fs) Update.abort();                   // nothing written yet: Update.end() fails, the uploader is told
    log_ev(NLG_OTA, 1, ota_fs ? 1 : 0);
    ota_hole_begin();
}
static void ota_on_progress(unsigned int, unsigned int)
{
    uint32_t rf = lwd.rearm_fails;
    nwd_progress(&lwd, millis());
    ph_rtc.wdt = (uint32_t)lwd.state;             // a failed re-arm puts loop() back under the watchdog: "on"
    if (lwd.rearm_fails != rf) log_ev(NLG_OTA, 3, (uint16_t)lwd.state);
}
static void ota_on_error(ota_error_t e)
{
    static const char *const name[] = { "auth", "begin", "connect", "receive", "end" };
    ota_hole_end();                               // back under the watchdog first (idempotent: connect errors come twice)
    ota_last = ota_fs ? "fs" : (unsigned)e < 5 ? name[e] : "other";
    ota_fs = false;
    ota_errors++;
    log_ev(NLG_OTA, 2, (uint16_t)e);
    if (e == OTA_AUTH_ERROR) {                    // slow down guessing; stopped by ota_service(), not from here
        uint32_t back = 2000u << (ota_auth_fails < 5 ? ota_auth_fails : 5);
        ota_auth_fails++;
        ota_hold = true;
        ota_hold_until = millis() + (back < 60000 ? back : 60000);
    }
    if (ota_busy) {
        ota_busy = false;
        tof_pause(false);
        if (listening) { server.begin(); server.setNoDelay(true); }
    }
}
// ---- authenticated HTTP push OTA + diagnostics (r2.0.1 / wt32-r3) ----------------------------
// Same preconditions and secret as ArduinoOTA.  HTTP Digest (RFC 2617, qop=auth, MD5) with
// user "nori" and the stored hash as the password, i.e. the uploader keys it with
// md5(password); neither is ever sent in the clear.  The uploader connects to us, so it needs
// no inbound port.  The listener is bound to all interfaces (the Wi-Fi upstream and the PM3 side
// at 192.168.77.1 alike; the DNAT port is the only Wi-Fi port forwarded to the PM3).
//   GET  /status   {"fw","part","img","ota","http","uptime_s","net","ip", + wt32-r3 summary}, no secrets
//   GET  /diag     wt32-r3: everything (reset, previous boot, network, link, sensor, event log), no secrets
//   POST /update   multipart/form-data, one part name="firmware" (app image)
//   GET  /wifi/nonce, POST /wifi   wt32-r5.1: saved networks, a sealed envelope (shared/nori_wifi/nori_wnprov.h)
// handleClient() is ours (see r5.2): bounded head read, digest checked before any body byte,
// streamed multipart body (firmware/src/nori_http.cpp, host-tested) with idle/total deadlines.
static const char *http_last = "none";            // last HTTP update failure, for DIAG
static const uint32_t HTTP_HEAD_MS = 3000, HTTP_IDLE_MS = 10000, HTTP_TOTAL_MS = 300000;
// wt32-r5.1.3: replies.  WiFiClient::write would wait up to 10 x 1 s per stall (a /diag reply is larger than the
// TCP send buffer); ours gives up after 2 s without progress or 10 s in all, and feeds the watchdog meanwhile.
static const uint32_t HTTP_TX_IDLE_MS = 2000, HTTP_TX_MS = 10000;
static uint32_t http_tx_fails;

static const char *ota_state();

static void tunnel_pause()                        // as ota_on_start(): the update owns the network
{
    ota_busy = true;
    wifi_tick(millis());
    tof_pause(true);
    if (client) client.stop();
    if (listening) server.end();
}
static void tunnel_resume()
{
    ota_busy = false;
    tof_pause(false);
    if (listening) { server.begin(); server.setNoDelay(true); }
}

// recv() on the raw socket: n > 0 bytes, 0 nothing yet, -1 closed or failed.
static int sock_read(int fd, void *b, size_t max)
{
    int n = recv(fd, b, max, MSG_DONTWAIT);
    if (n > 0) return n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;
}

struct http_upload_ctx {
    const char *md5;
    size_t slot;
    bool begun;
};
// Only reachable from NoriHttp::update(), i.e. after the digest was verified.
static bool up_start(void *ctx, size_t n)
{
    http_upload_ctx *u = (http_upload_ctx *)ctx;
    if (n > u->slot) return false;                // larger than the inactive app slot
    if (!Update.begin(n, U_FLASH)) return false;  // U_FLASH: the next app slot, never NVS/spiffs
    u->begun = true;
    return !u->md5[0] || Update.setMD5(u->md5);
}
static bool up_data(void *, const uint8_t *p, size_t n)
{
    return Update.write(const_cast<uint8_t *>(p), n) == n;   // first byte must be 0xE9 (app image magic)
}

class NoriHttp : public WebServer {
public:
    using WebServer::WebServer;
    void handleClient() override;

private:
    void reply(int code, const char *type, const char *body);
    void status();
    void diag();
    void update(const nh_req &r, const uint8_t *rest, size_t nrest);
    void wifi_nonce();
    void wifi_post(const nh_req &r, const uint8_t *rest, size_t nrest);
    void drain(long left);
    bool tx(const char *p, size_t n);
    bool tx_failed = false;                       // this request's connection: once failed, later writes fail at once

protected:
    // wt32-r5.1.3: every WebServer write (send(), sendContent(), requestAuthentication()'s 401) goes through tx().
    size_t _currentClientWrite(const char *b, size_t l) override { return tx(b, l) ? l : 0; }
    size_t _currentClientWrite_P(PGM_P b, size_t l) override { return tx(b, l) ? l : 0; }
};

void NoriHttp::reply(int code, const char *type, const char *body)
{
    sendHeader("Cache-Control", "no-store");
    send(code, type, body);
}

bool NoriHttp::tx(const char *p, size_t n)
{
    if (tx_failed) return false;
    uint32_t was = ph_go(NPH_HTTP_TX);
    int fd = _currentClient.fd();
    uint32_t t0 = millis(), last = t0;
    bool ok = fd >= 0;
    while (ok && n) {
        wdt_feed();
        uint32_t now = millis();
        if (now - t0 > HTTP_TX_MS || now - last > HTTP_TX_IDLE_MS) { ok = false; break; }   // every pass, short writes too
        int w = lwip_send(fd, p, n, MSG_DONTWAIT); // not send(): inside WebServer that name is the member
        if (w > 0) { p += w; n -= (size_t)w; last = millis(); continue; }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) ok = false;
        else delay(2);
    }
    if (!ok) { http_tx_fails++; tx_failed = true; }   // handleClient() closes the socket
    ph_go(was);
    return ok;
}

void NoriHttp::handleClient()
{
    static char head[NH_HEAD_MAX];
    static nh_req r;
    WiFiClient c = _server.available();
    if (!c) return;
    int fd = c.fd(), pr = 0;
    size_t got = 0;
    uint32_t t0 = millis();
    while ((pr = nh_parse_head(head, got, &r)) == 0) {           // bounded: 2 KB, 3 s
        wdt_feed();
        int k = sock_read(fd, head + got, sizeof head - got);
        if (k < 0 || millis() - t0 > HTTP_HEAD_MS) break;
        if (k == 0) delay(2);
        got += (size_t)(k > 0 ? k : 0);
    }
    if (pr == 0) { c.stop(); return; }
    ph_go(NPH_HTTP_REQ);                                         // loop() sets the next phase when this returns

    // Just enough request state for authenticate()/requestAuthentication()/send().
    _currentClient = c;
    tx_failed = false;
    _currentVersion = 1;
    _chunked = false;
    _contentLength = CONTENT_LENGTH_NOT_SET;
    _responseHeaders = "";
    collectHeaders(nullptr, 0);                                  // forgets the previous Authorization
    bool get = !strcmp(r.method, "GET"), post = !strcmp(r.method, "POST");
    if (pr < 0) {
        reply(400, "text/plain", "bad request\n");
    } else if (!get && !post) {
        reply(405, "text/plain", "method not allowed\n");
    } else {
        _currentMethod = get ? HTTP_GET : HTTP_POST;
        if (r.auth[0]) _collectHeader("Authorization", r.auth);
        char uri[sizeof r.path];
        bool ok = ota_hash[0] && r.auth[0] && authenticate("nori", ota_hash) &&
                  nh_auth_param(r.auth, "uri", uri, sizeof uri) && !strcmp(uri, r.path);   // digest bound to this target
        if (ok) http_auth_ok++;
        if (ok && wifi_up() && c.localIP() == WiFi.localIP()) http_auth_wifi++;   // r4: proves the upstream path
        else if (r.auth[0]) { http_auth_bad++; log_ev(NLG_HTTP, 3, 0); }   // a wrong / stale credential, not the first challenge
        if (!ok) requestAuthentication(DIGEST_AUTH, "nori-ota");  // fresh nonce; no body byte was read
        else if (get && !strcmp(r.path, "/status")) status();
        else if (get && !strcmp(r.path, "/diag")) diag();
        else if (post && !strcmp(r.path, "/update")) update(r, (const uint8_t *)head + r.head_len, got - r.head_len);
        else if (get && !strcmp(r.path, "/wifi/nonce")) wifi_nonce();
        else if (post && !strcmp(r.path, "/wifi")) wifi_post(r, (const uint8_t *)head + r.head_len, got - r.head_len);
        else reply(404, "text/plain", "not found\n");
    }
    collectHeaders(nullptr, 0);
    _currentClient.stop();
    _currentClient = WiFiClient();
}

// Bounded JSON / token writer: appends while it fits, remembers that something did not.
struct sbuf {
    char *p;
    size_t cap, n;
    bool full;
    sbuf(char *b, size_t c) : p(b), cap(c), n(0), full(false) { if (c) b[0] = 0; }
    void add(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        if (full || n >= cap) { full = true; return; }
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(p + n, cap - n, fmt, ap);
        va_end(ap);
        if (k < 0 || (size_t)k >= cap - n) { p[n] = 0; full = true; return; }   // whole items only
        n += (size_t)k;
    }
};

static ni_toggle tog;                             // wt32-r5 track selector, see tog_service()
static ni_gesture ges;                            // wt32-r5.1.1 what each move turned out to be
static const char *const TOG_WORD[] = { "center", "five", "one", "both" };
static tof_snap tof_last;                         // refreshed by loop() every 50 ms
static int pres_eff = 0;
static uint32_t heap_low_logged;
static const char *pres_tok() { return !tof_last.known ? "-" : pres_eff ? "1" : "0"; }
static const char *tof_stage_name(const tof_snap &t)
{
    if (!t.task_ok) return "notask";
    if (!t.enabled) return "off";
    if (t.sm.paused) return "paused";
    if ((t.sm.stage == NT_S_PROBE || t.sm.stage == NT_S_BACKOFF) && !t.sm.present && t.sm.nacks) return "absent";
    return NT_STAGE_NAME[t.sm.stage];
}

void NoriHttp::status()
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    static char out[1280];                        // r3 worst case ~620 B + r4 routing/lease ~260 B
    char rst[NR_JSON_MAX], wip[16], mac[18];
    nr_json(&rst_rtc, dev.boot_id, rst_reason, rst_hw, rst_kept, rst, sizeof rst);
    const ndh_lease *pl = pm3_lease();
    sbuf b(out, sizeof out);
    b.add("{\"fw\":\"%s\",\"part\":\"%s\",\"img\":\"%s\",\"img_why\":\"%s\",\"ota\":\"%s\",\"http\":\"ready\",\"uptime_s\":%lu,"
          "\"net\":\"%s\",\"ip\":\"%s\",%s,",
          FW, run ? run->label : "?", img_pending ? "pending" : "valid", img_why, ota_state(), (unsigned long)(millis() / 1000),
          NET_NAME[net_kind()], net_ip().toString().c_str(), rst);
    b.add("\"heap\":%lu,\"heap_min\":%lu,\"eth\":\"%s\",\"wifi\":\"%s\",\"wifi_disc\":%lu,\"wifi_reason\":%u,\"host\":%d,",
          (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(), eth_state(), wifi_state(),
          (unsigned long)wifi_discs, (unsigned)wifi_reason, dev.host_alive);
    b.add("\"wifi_n\":%u,\"wifi_i\":%d,\"wifi_gen\":%lu,\"wifi_st\":\"%s\",\"wifi_why\":\"%s\",\"wifi_fail\":\"%s\",", wn.n, wsm.cur,
          (unsigned long)wn.gen, NWS_STATE_NAME[wsm.state], NWW_NAME[wsm.why], NWF_NAME[wsm.last_cls]);
    b.add("\"rt\":\"%s\",\"wip\":\"%s\",\"eip\":\"192.168.77.1\",\"dnat\":%d,\"fwd\":%lu,\"lan\":\"%s\",\"pm3_ip\":\"%s\","
          "\"pm3_mac\":\"%s\",\"pm3_lease_s\":%lu,\"tunnel_port\":%u,",
          nn_route_state(&nat), wifi_up() ? ip_s(nat.wifi_ip, wip) : "", nat.pm_ok, (unsigned long)nat.fw, nla_state_word(&lan),
          pl ? "192.168.77.2" : "", pl ? ndh_mac_str(pl->mac, mac) : "", (unsigned long)(pl ? ndh_remaining_ms(pl, sys_now()) / 1000 : 0),
          (unsigned)NORI_TUNNEL_PORT);
    b.add("\"tog\":\"%s\",", TOG_WORD[tog.stable]);
    b.add("\"wdt\":\"%s\",\"ph_prev\":\"%s\",\"ph_prev_in_ms\":%lu,", nph_wdt_name(ph_rtc.wdt), ph_prev_ok ? nph_name(ph_prev.ph) : "",
          (unsigned long)(ph_prev_ok ? nph_in_ms(&ph_prev, ph_prev.alive_ms) : 0));   // wt32-r5.1.3
    b.add("\"sensor\":\"%s\",\"tof\":\"%s\",\"presence\":%s,\"nearest_mm\":%d,\"tof_frames\":%lu,\"tof_restarts\":%lu}\n",
          SENSOR_NAME, tof_stage_name(tof_last), tof_last.known ? (pres_eff ? "1" : "0") : "null",
          tof_last.known ? tof_last.np.nearest_mm : -1, (unsigned long)tof_last.sm.frames, (unsigned long)tof_last.sm.restarts);
    if (b.full) { reply(500, "text/plain", "status overflow\n"); return; }
    reply(200, "application/json", out);
}

static void diag_json(sbuf &b);

void NoriHttp::diag()
{
    static char out[10240];                       // worst case ~8.4 KB (r3 5.5 KB + r4 router/LAN + r5.1 profiles); overflow -> HTTP 500, never a cut JSON
    sbuf b(out, sizeof out);
    diag_json(b);
    if (b.full) { reply(500, "text/plain", "diag overflow\n"); return; }
    reply(200, "application/json", out);
}

// After a refused upload, read (and drop) the rest of an authenticated body so
// closing the socket does not reset the connection before the client reads why.
void NoriHttp::drain(long left)
{
    static uint8_t junk[512];
    uint32_t was = ph_go(NPH_HTTP_DRAIN);
    int fd = _currentClient.fd();
    uint32_t t0 = millis(), last = t0;
    while (left > 0 && millis() - last < 2000 && millis() - t0 < 30000) {
        wdt_feed();                              // bounded by the 2 s / 30 s above, never by the peer
        int k = sock_read(fd, junk, left < (long)sizeof junk ? (size_t)left : sizeof junk);
        if (k < 0) break;
        if (k == 0) { delay(2); continue; }
        left -= k;
        last = millis();
    }
    ph_go(was);
}

void NoriHttp::update(const nh_req &r, const uint8_t *rest, size_t nrest)
{
    _snonce = _getRandomHexString();             // one upload per challenge: a captured header cannot be replayed
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
    if (!r.multipart || r.content_length < 0) {
        http_last = "format";
        http_fails++;
        reply(400, "text/plain", "ERR need multipart/form-data with Content-Length\n");
        drain(r.content_length - (long)nrest);
        return;
    }
    if (!slot || r.content_length > (long)slot->size + NH_PART_HEAD_MAX + 8 + NH_BOUNDARY_MAX) {
        http_last = "size";
        http_fails++;
        reply(413, "text/plain", "ERR larger than the app slot\n");  // refused unread: the client may see a reset
        return;
    }
    if (r.expect_continue) tx("HTTP/1.1 100 Continue\r\n\r\n", 25);

    ph_go(NPH_HTTP_UP);
    http_uploads++;
    log_ev(NLG_HTTP, 1, 0);
    tunnel_pause();
    http_upload_ctx u = { r.md5, slot->size, false };
    const nh_sink sink = { &u, up_start, up_data };
    static uint8_t buf[1460];
    nh_body b;
    nh_body_init(&b, r.boundary, r.content_length);
    if ((long)nrest > r.content_length) nrest = (size_t)r.content_length;   // ignore anything pipelined after the body
    int st = nrest ? nh_body_feed(&b, rest, nrest, &sink) : NH_B_PART;
    int fd = _currentClient.fd();
    const char *why = nullptr;
    uint32_t t0 = millis(), last = t0;
    while (st != NH_B_DONE && st != NH_B_ERROR) {
        wdt_feed();                              // bounded by HTTP_IDLE_MS / HTTP_TOTAL_MS: no watchdog hole for HTTP
        int k = sock_read(fd, buf, sizeof buf);
        uint32_t now = millis();
        if (k < 0) { why = "disconnect"; break; }
        if (now - t0 > HTTP_TOTAL_MS || (k == 0 && now - last > HTTP_IDLE_MS)) { why = "timeout"; break; }
        if (k == 0) { delay(1); continue; }
        last = now;
        long left = r.content_length - b.consumed;
        st = nh_body_feed(&b, buf, (size_t)k < (size_t)left ? (size_t)k : (size_t)left, &sink);
    }
    if (st == NH_B_ERROR) why = b.err;
    wdt_feed();
    if (!why && !(Update.end() && Update.isFinished())) why = "end";  // MD5 (if sent), image verify, boot slot switch
    if (!why) {
        reply(200, "text/plain", "OK rebooting\n");
        delay(200);                              // let the response leave before the socket closes
        _currentClient.stop();
        delay(100);
        ESP.restart();
    }
    char msg[96];
    snprintf(msg, sizeof msg, "ERR %s %s\n", why, u.begun ? Update.errorString() : "");
    if (u.begun) Update.abort();                 // nothing is committed; the running slot stays the boot slot
    http_last = why;
    http_fails++;
    log_ev(NLG_HTTP, 2, 0);
    tunnel_resume();
    if (strcmp(why, "disconnect") && strcmp(why, "timeout")) {
        reply(strcmp(why, "end") && strcmp(why, "write") ? 400 : 500, "text/plain", msg);
        drain(r.content_length - b.consumed);
    }
}

void NoriHttp::wifi_nonce()
{
    char h[NWP_NONCE * 2 + 1], js[64];
    if (nwp_http_nonce(&wprov, h, sizeof h, millis()) != NW_OK) { reply(403, "application/json", "{\"ok\":0,\"err\":\"nokey\"}\n"); return; }
    snprintf(js, sizeof js, "{\"nonce\":\"%s\"}\n", h);
    reply(200, "application/json", js);
}

// Bounded like the request head: 845 bytes at most, 3 s.  The body is sealed, so nothing here sees a password.
void NoriHttp::wifi_post(const nh_req &r, const uint8_t *rest, size_t nrest)
{
    static uint8_t body[NWP_HTTP_MAX];
    char js[48];
    _snonce = _getRandomHexString();             // as update(): one change per challenge
    if (r.content_length < 1 || r.content_length > (long)sizeof body) {
        nwp_abort(&wprov);
        reply(413, "application/json", "{\"ok\":0,\"err\":\"size\"}\n");
        return;
    }
    if (r.expect_continue) tx("HTTP/1.1 100 Continue\r\n\r\n", 25);
    size_t want = (size_t)r.content_length, got = nrest < want ? nrest : want;
    memcpy(body, rest, got);
    int fd = _currentClient.fd();
    uint32_t t0 = millis();
    while (got < want && millis() - t0 <= HTTP_HEAD_MS) {
        wdt_feed();
        int k = sock_read(fd, body + got, want - got);
        if (k < 0) break;
        if (k == 0) delay(2);
        got += (size_t)(k > 0 ? k : 0);
    }
    if (got < want) {
        nwp_abort(&wprov);
        reply(408, "application/json", "{\"ok\":0,\"err\":\"short\"}\n");
        return;
    }
    int code = nwp_http_post(&wprov, body, got, millis(), js, sizeof js);
    reply(code, "application/json", js);
}

static NoriHttp *http;

static void http_stop()
{
    if (!http) return;
    http->close();
    delete http;
    http = nullptr;
}
static const char *http_state()
{
    if (!ota_hash[0]) return "nokey";
    return http ? "ready" : "nonet";
}

static void ota_service(uint32_t now)
{
    if ((int32_t)(now - ota_next) < 0) return;
    ota_next = now + 500;
    if (ota_hold && (int32_t)(now - ota_hold_until) >= 0) ota_hold = false;
    bool up = net_up();
    IPAddress ip = net_ip();
    bool want = up && ota_hash[0] && !ota_hold;
    // wt32-r5.1.3: dropped or held only.  Its UDP socket is bound to INADDR_ANY (WiFiUDP::begin(port)) and it answers
    // the uploader's address, so like the HTTP listener it needs no restart when the address changes (every hotspot
    // join and loss: Ethernet keeps net_up() true).  Only mDNS needed one.
    if (ota && (!want || (NORI_OTA_MDNS && ip != ota_ip))) {
        uint32_t was = ph_go(NPH_OTA_STOP);
        ota_stop();
        ph_go(was);
    }
    if (want && !ota) {
        uint32_t was = ph_go(NPH_OTA_BEGIN);
        ota = new ArduinoOTAClass();
        ota->setHostname(NORI_HOSTNAME).setPasswordHash(ota_hash).setMdnsEnabled(NORI_OTA_MDNS).setRebootOnSuccess(true);
        ota->onStart(ota_on_start).onProgress(ota_on_progress).onError(ota_on_error);
        ota->begin();
        ota_ip = ip;
        ph_go(was);
    }
    // wt32-r3: the HTTP listener is bound to INADDR_ANY, so an address change (DHCP renewal, Wi-Fi
    // up/down while Ethernet stays) needs no restart; it runs whenever some network has an address and a key exists.
    bool want_http = up && ota_hash[0];          // not paused by the ArduinoOTA auth hold
    if (http && !want_http) {
        uint32_t was = ph_go(NPH_HTTP_STOP);
        http_stop();
        ph_go(was);
    }
    if (want_http && !http) {
        uint32_t was = ph_go(NPH_HTTP_BEGIN);
        http = new NoriHttp(NORI_HTTP_PORT);
        http->begin();
        ph_go(was);
    }
    // Mark-valid rule (see IMG_MIN_MS above; nori_imgvalid.h, host-tested): never on time alone.
    const char *why;
    // r4: Ethernet-only health (the PM3 side) does not count, nor does an authenticated request from the PM3 side:
    // only the Wi-Fi upstream proves the PC can reach us (review fix for the request path).  And the router
    // must work: routing "ok" and the PM3 holding .2 (niv_router_net), continuously for the 60 s path.
    const char *net_why;
    int net = niv_router_net(wifi_up(), !strcmp(nn_route_state(&nat), "ok"), pm3_lease() != nullptr, &net_why);
    int how = niv_step(&img_health, now, IMG_MIN_MS, IMG_HEALTHY_MS, net, ota_hash[0] != 0, ota != nullptr, http != nullptr,
                       http_auth_wifi, &why);
    if (how == NIV_WAIT && !strcmp(why, "no-network")) why = net_why;   // route-wait / lease-wait say which part is missing
    if (!img_pending) return;
    img_why = why;
    if (how == NIV_WAIT || (int32_t)(now - img_retry_at) < 0) return;
    ph_go(NPH_IMG_MARK);                          // loop() sets the next phase when this returns
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    bool valid = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_VALID;
    if (e == ESP_OK && valid) {                   // report "valid" only once the otadata says so
        img_pending = false;
        log_ev(NLG_IMG_OK, 0, (uint16_t)how);
    } else {                                      // otadata write failed: stay pending, retry in 5 s
        img_mark_fail++;
        img_why = "mark-failed";
        img_retry_at = now + 5000;
        log_ev(NLG_IMG_OK, 1, (uint16_t)(e & 0xffff));
    }
}
static const char *ota_state()
{
    if (!ota_hash[0]) return "nokey";
    if (ota) return "ready";
    return ota_hold ? "hold" : "nonet";
}
static int o_key(void *, const char *h)
{
    if (prefs.putString("otah", h) != 32) return -1;
    memcpy(ota_hash, h, sizeof ota_hash);
    ota_auth_fails = 0;
    ota_hold = false;
    nwp_abort(&wprov);                            // an envelope in progress was sealed with the old key
    ota_stop();                                   // next ota_service() pass starts with the new key
    ota_next = millis();
    return 0;
}
extern "C" bool verifyRollbackLater() { return true; }   // ota_service() decides when a new image is good

// ---- buttons ----------------------------------------------------------------------
static ni_button btn[2];
static const int BTN_PIN[2] = { NORI_PIN_BTN1, NORI_PIN_BTN2 };
static uint32_t next_btn;

static int pressed(int k) { return BTN_PIN[k] >= 0 && digitalRead(BTN_PIN[k]) == LOW; }

// ---- track selector (wt32-r5): debounced in nori_input.c, sampled with the buttons -------
static uint32_t tog_tx, tog_drop, tog_seen_both, tog_seen_locked;
static int tog_low(int pin) { return digitalRead(pin) == LOW; }
static void tog_service(uint32_t now)
{
    int ok = 0;
    int pos = nd_selector(&dev, &tog, &ges, tog_low(NORI_PIN_TOG_FIVE), tog_low(NORI_PIN_TOG_ONE), now, 30, &ok);
    if (pos) {
        if (ok) tog_tx++; else tog_drop++;
        log_ev(NLG_TOG, (uint8_t)pos, (uint16_t)ok);
    }
    if (tog.n_both != tog_seen_both) { tog_seen_both = tog.n_both; log_ev(NLG_TOG, NI_TOG_BOTH, 0); }
    if (tog.n_locked != tog_seen_locked) { tog_seen_locked = tog.n_locked; log_ev(NLG_TOG, tog.stable, 0); }
}

// ---- DIAG (UART) ----------------------------------------------------------------------
// Most useful first: the core cuts the joined token at the end if it is too long.
//   sensor=vl53l5cx tof=ranging pr=0 nm=812 wifi=up wn=0/2 wip=192.0.2.10 eth=up lnk=100F/1 rt=ok lan=lease
//   pm3=001e06421122 rst=3/sw up=345 img=valid ota=ready http=ready part=app0 otalast=none httplast=none
//   btn=00 tog=center heap=130k/118k i2c=on      (linkd's 5 Hz range poll reads nm= here: keep it near the front)
static void lnk_str(char *lnk, size_t cap)
{
    snprintf(lnk, cap, "0");
    if (eth_link) snprintf(lnk, cap, "%u%c/%lu", (unsigned)ETH.linkSpeed(), ETH.fullDuplex() ? 'F' : 'H', (unsigned long)eth_link_ups);
}
// "001e06421122" (12 hex digits; DIAG tokens stay short)
static const char *mac_hex(const uint8_t *m, char *b)
{
    snprintf(b, 13, "%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}
// nm= for DIAG and DIAG p=zg: linkd reads either, and the gesture gate must not see a difference
static void nm_str(char *nm, size_t cap)
{
    snprintf(nm, cap, "-");
    if (tof_last.known && tof_last.np.nearest_mm >= 0) snprintf(nm, cap, "%d", tof_last.np.nearest_mm);
}
static void diag(void *, char *out, size_t cap)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    char lnk[16], nm[8], wip[16], mac[13];
    lnk_str(lnk, sizeof lnk);
    nm_str(nm, sizeof nm);
    const ndh_lease *pl = pm3_lease();
    sbuf b(out, cap);
    b.add("sensor=%s tof=%s pr=%s nm=%s", SENSOR_NAME, tof_stage_name(tof_last), pres_tok(), nm);
    b.add(" wifi=%s wn=%d/%u wip=%s eth=%s lnk=%s rt=%s lan=%s pm3=%s", wifi_state(), wsm.cur, wn.n, wifi_up() ? ip_s(nat.wifi_ip, wip) : "-", eth_state(), lnk,
          nn_route_state(&nat), nla_state_word(&lan), pl ? mac_hex(pl->mac, mac) : "-");
    b.add(" rst=%u/%s up=%lu", rst_reason, nr_name(rst_reason), (unsigned long)(millis() / 1000));
    b.add(" img=%s ota=%s http=%s part=%s", img_pending ? "pending" : "valid", ota_state(), http_state(), run ? run->label : "?");
    if (wifi_started && !wifi_up())
        b.add(" ws=%d wr=%u wd=%lu ap=%d sig=%d", (int)WiFi.status(), (unsigned)wifi_reason, (unsigned long)wifi_discs,
              wifi_matches, wifi_best_rssi);
    b.add(" otalast=%s httplast=%s", ota_last, http_last);
    b.add(" btn=%d%d tog=%s heap=%luk/%luk i2c=%s", pressed(0), pressed(1), TOG_WORD[tog.stable], (unsigned long)(ESP.getFreeHeap() / 1024),
          (unsigned long)(ESP.getMinFreeHeap() / 1024), tof_last.bus_ok ? "on" : "off");
}

// DIAG p=<page>: tof net wifi wifip rt dhcp rst ev prev tog zg ph.  Each page is a few dozen tokens, cut at the end if too
// long (about 200 characters reach the host after the DIAG header).
static int diag_page(void *, const char *page, char *out, size_t cap)
{
    sbuf b(out, cap);
    const tof_snap &t = tof_last;
    if (!strcmp(page, "ph")) {                     // wt32-r5.1.3: loop() phases, this boot and the previous one (nori_phase.h)
        char s[NPH_DIAG_MAX];
        nph_diag(&ph_rtc, millis(), &ph_prev, ph_prev_ok, s, sizeof s);
        b.add("%s htx=%lu", s, (unsigned long)http_tx_fails);
        return 0;
    }
    if (!strcmp(page, "zg")) {                     // wt32-r5.1.2: linkd's 5 Hz poll; one snapshot, so nm= and the grid belong together
        char nm[8], grid[NP_GRID_TEXT_MAX];
        nm_str(nm, sizeof nm);
        np_grid_format(&t.np, millis(), grid, sizeof grid);   // the live clock: a stuck task's frozen frame still ages out
        b.add("tof=%s nm=%s %s", tof_stage_name(t), nm, grid);
        return 0;
    }
    if (!strcmp(page, "tof")) {
        char s[200];
        nt_diag(&t.sm, s, sizeof s);
        if (!t.enabled) { char *e = strchr(s, ' '); b.add("tof=off%s", e ? e : ""); }
        else b.add("%s", s);
        b.add(" pr=%s lvl=%d nm=%d vz=%u nz=%u hz=%u", pres_tok(), t.np.level, t.np.nearest_mm, t.np.valid_zones,
              t.np.near_zones, t.np.held_zones);
        b.add(" ri=%lu fa=%lu sf=%lu ff=%lu gf=%lu nbf=%lu zr=%lu", (unsigned long)t.np.rises, (unsigned long)t.np.falls,
              (unsigned long)t.np.stale_falls, (unsigned long)t.np.fault_falls, (unsigned long)t.np.frames,
              (unsigned long)t.np.bad_frames, (unsigned long)t.np.zones_rejected);
        b.add(" ptx=%lu prt=%lu", (unsigned long)dev.pres_tx, (unsigned long)dev.pres_retry);
        b.add(" bus=%s x=%lu nack=%lu tmo=%lu err=%lu skip=%lu le=%d us=%u bcf=%lu", t.bus_ok ? "on" : "off", (unsigned long)t.xfer,
              (unsigned long)t.nack, (unsigned long)t.tmo, (unsigned long)t.err, (unsigned long)t.skipped, t.last_err, t.uld_status,
              (unsigned long)t.sm.busclr_fail);
        b.add(" temp=%d stk=%lu", t.temp_c, (unsigned long)t.stack_free);
        return 0;
    }
    if (!strcmp(page, "net")) {
        char lnk[16];
        lnk_str(lnk, sizeof lnk);
        b.add("net=%s nc=%lu eth=%s lnk=%s eu=%lu ed=%lu ei=%lu", NET_NAME[net_kind()], (unsigned long)net_changes, eth_state(), lnk,
              (unsigned long)eth_link_ups, (unsigned long)eth_link_downs, (unsigned long)eth_ips);
        b.add(" wifi=%s wst=%lu wc=%lu wd=%lu wr=%u wi=%lu wl=%lu", wifi_state(), (unsigned long)wifi_starts, (unsigned long)wifi_conns,
              (unsigned long)wifi_discs, (unsigned)wifi_reason, (unsigned long)wifi_ips, (unsigned long)wifi_ip_losses);
        if (wifi_up()) b.add(" rssi=%d", WiFi.RSSI());
        b.add(" host=%d tun=%lu ota=%s os=%lu oe=%lu http=%s ha=%lu hb=%lu hu=%lu hf=%lu", dev.host_alive, (unsigned long)tun_accepts,
              ota_state(), (unsigned long)ota_starts, (unsigned long)ota_errors, http_state(), (unsigned long)http_auth_ok,
              (unsigned long)http_auth_bad, (unsigned long)http_uploads, (unsigned long)http_fails);
        b.add(" img=%s why=%s heap=%lu hmin=%lu hmax=%lu", img_pending ? "pending" : "valid", img_why, (unsigned long)ESP.getFreeHeap(),
              (unsigned long)ESP.getMinFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
        return 0;
    }
    if (!strcmp(page, "wifi")) {                  // wt32-r5.1: wn=<index in use>/<saved>; no SSID, ever
        b.add("wn=%d/%u gen=%lu st=%s why=%s fail=%s/%u ld=%s", wsm.cur, wn.n, (unsigned long)wn.gen, NWS_STATE_NAME[wsm.state],
              NWW_NAME[wsm.why], NWF_NAME[wsm.last_cls], (unsigned)wsm.last_reason, NW_LD_NAME[wn_load_how]);
        b.add(" sc=%lu bg=%lu sf=%lu jn=%lu sw=%lu ug=%lu uf=%lu rd=%lu hd=%lu", (unsigned long)wsm.scans, (unsigned long)wsm.bg_scans,
              (unsigned long)wsm.scan_fails, (unsigned long)wsm.joins, (unsigned long)wsm.switches, (unsigned long)wsm.upgrades,
              (unsigned long)wsm.upgrade_fails, (unsigned long)wsm.rounds, (unsigned long)wsm.holds);
        b.add(" ap=%d sig=%d auth=%d br=%d", wifi_matches, wifi_best_rssi, wifi_best_auth, wifi_begin_result);
        return 0;
    }
    if (!strcmp(page, "wifip")) {                 // per network: index:rssi/tries/ok/fails/last reason/backoff s
        uint32_t now_w = millis();
        for (int i = 0; i < wn.n; i++) {
            const nws_prof &p = wsm.p[i];
            unsigned long f = 0;
            for (int k = 0; k < NWF_COUNT; k++) f += p.fails[k];
            b.add("%s%d:%d/%lu/%lu/%lu/%u/%lu", i ? " " : "", i, p.vis ? p.rssi : 0, (unsigned long)p.tries, (unsigned long)p.oks, f,
                  (unsigned)p.last_reason, (unsigned long)(p.nb_on ? (p.nb - now_w) / 1000 : 0));
        }
        return 0;
    }
    if (!strcmp(page, "rt")) {                    // wt32-r4: routing (NAPT, DNAT, guard, default route, upstream)
        char s[240], wip[16], gw[16], dns[16];
        nn_format(&nat, &NAT_CFG, 1, s, sizeof s);
        b.add("%s", s);
        b.add(" wip=%s gw=%s dns=%s ch=%d tun=%u:%s pf=%lu sl=%lu ecf=%lu", ip_s(nat.wifi_ip, wip), ip_s(nat.wifi_gw, gw),
              ip_s(nat_dns[0], dns), wifi_channel(), (unsigned)NORI_TUNNEL_PORT, listening ? (client ? "open" : "listen") : "off",
              (unsigned long)nat_post_fails, (unsigned long)nat_slow, (unsigned long)eth_cfg_fixes);
        return 0;
    }
    if (!strcmp(page, "dhcp")) {                  // wt32-r4: the PM3 LAN: server state, leases (MAC/remaining s), counters
        char mac[13];
        const ndh_state &d = lan.dhcp;
        uint32_t now_l = sys_now();
        b.add("lan=%s sd=%s", nla_state_word(&lan), NLA_SD_NAME[lan.standdown < NLA_SD_COUNT ? lan.standdown : 0]);
        for (int i = 0; i < LAN_CFG.dhcp.pool_n; i++) {
            const ndh_lease &l = d.l[i];
            if (l.state == NDH_FREE && !l.has_mac) continue;
            b.add(" l%d=%s/%s/%lu%s", i + 2, NDH_STATE_NAME[l.state < 5 ? l.state : 0], l.has_mac ? mac_hex(l.mac, mac) : "-",
                  (unsigned long)(ndh_remaining_ms(&l, now_l) / 1000), l.stale ? "/st" : "");
        }
        b.add(" ack=%lu off=%lu nak=%lu dis=%lu req=%lu rel=%lu dec=%lu full=%lu oth=%lu bad=%lu ign=%lu exp=%lu",
              (unsigned long)d.ack, (unsigned long)d.offer, (unsigned long)d.nak, (unsigned long)d.discover, (unsigned long)d.request,
              (unsigned long)d.release, (unsigned long)d.decline, (unsigned long)d.full, (unsigned long)d.other_server,
              (unsigned long)d.bad, (unsigned long)d.ignored, (unsigned long)d.expired);
        b.add(" lk=%d lu=%lu ld=%lu q=%lu wif=%lu tx=%lu/%lu pcb=%d sn=%d sds=%lu fd=%lu gws=%lu ls=%lu", lan.link,
              (unsigned long)lan.link_ups, (unsigned long)lan.link_downs, (unsigned long)lan.rx_quiet, (unsigned long)lan.rx_wrong_if,
              (unsigned long)lan.tx_ok, (unsigned long)lan.tx_err, lan.pcb_ok, lan.sniff, (unsigned long)lan.standdowns,
              (unsigned long)lan.sn_foreign_dhcp, (unsigned long)lan.sn_gw_seen, (unsigned long)lan.sn_lan_srcs);
        return 0;
    }
    if (!strcmp(page, "tog")) {                   // wt32-r5: raw = GPIO15/GPIO2 contact (1 = closed to ground) right now
        b.add("tog=%s raw=%d%d lock=%d five=%lu one=%lu ctr=%lu both=%lu ref=%lu tx=%lu drop=%lu evdrop=%lu short=%lu long=%lu gcan=%lu", TOG_WORD[tog.stable],
              tog_low(NORI_PIN_TOG_FIVE), tog_low(NORI_PIN_TOG_ONE), tog.locked, (unsigned long)tog.n_five, (unsigned long)tog.n_one,
              (unsigned long)tog.n_center, (unsigned long)tog.n_both, (unsigned long)tog.n_locked, (unsigned long)tog_tx,
              (unsigned long)tog_drop, (unsigned long)dev.ev_dropped, (unsigned long)ges.n_short, (unsigned long)ges.n_long,
              (unsigned long)ges.n_cancel);
        return 0;
    }
    if (!strcmp(page, "rst")) {
        char s[NR_DIAG_MAX];
        nr_diag(&rst_rtc, rst_reason, s, sizeof s);
        b.add("%s hw=%u rtc=%s boot=%08lx up=%lu", s, rst_hw, rst_kept ? "kept" : "new", (unsigned long)dev.boot_id,
              (unsigned long)(millis() / 1000));
        if (prev_ok)
            b.add(" prev=ok pboot=%08lx pup=%lu pheap=%lu pev=%lu", (unsigned long)prev_trace.boot_id, (unsigned long)prev_trace.up_s,
                  (unsigned long)(prev_trace.heap_min == UINT32_MAX ? 0 : prev_trace.heap_min), (unsigned long)prev_trace.ev_total);
        else b.add(" prev=none");
        return 0;
    }
    if (!strcmp(page, "ev") || !strcmp(page, "prev")) {
        static nlg_ring r;                        // loop() only: no reentrancy
        if (page[0] == 'e') log_copy(&r);
        else {
            nlg_init(&r);
            nlg_ev ev[NR_TRACE_EV];
            unsigned k = prev_ok ? nr_trace_events(&prev_trace, ev) : 0;
            for (unsigned i = 0; i < k; i++) nlg_add(&r, ev[i].t_ms, ev[i].code, ev[i].a, ev[i].b);
        }
        b.add("n=%lu ", (unsigned long)r.total);
        nlg_tail(&r, 16, out + b.n, cap - b.n);
        return 0;
    }
    return -1;
}

// /diag JSON (authenticated).  No SSID, PSK, OTA hash or password anywhere.
static void diag_json(sbuf &b)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const tof_snap &t = tof_last;
    char rst[NR_JSON_MAX], lnk[16];
    nr_json(&rst_rtc, dev.boot_id, rst_reason, rst_hw, rst_kept, rst, sizeof rst);
    lnk_str(lnk, sizeof lnk);
    b.add("{\"fw\":\"%s\",\"part\":\"%s\",\"img\":\"%s\",\"img_why\":\"%s\",\"img_mark_fail\":%lu,\"uptime_ms\":%lu,%s,", FW, run ? run->label : "?",
          img_pending ? "pending" : "valid", img_why, (unsigned long)img_mark_fail, (unsigned long)millis(), rst);
    b.add("\"hostname\":\"%s\",\"wifi_hostname\":\"%s\",\"eth_hostname\":\"%s\",", NORI_HOSTNAME, WiFi.getHostname() ? WiFi.getHostname() : "", ETH.getHostname() ? ETH.getHostname() : "");
    {   // wt32-r5.1.3 (~400 B)
        char s[NPH_JSON_MAX];
        nph_json(&ph_rtc, millis(), &ph_prev, ph_prev_ok, s, sizeof s);
        b.add("%s,\"wdt\":{\"holes\":%lu,\"hole_max_ms\":%lu,\"add_fails\":%lu,\"del_fails\":%lu,\"arm_fails\":%lu,\"rearm_fails\":%lu,"
              "\"guard\":%d,\"mdns\":%d,\"http_tx_fails\":%lu},",
              s, (unsigned long)lwd.holes, (unsigned long)lwd.hole_max_ms, (unsigned long)lwd.add_fails, (unsigned long)lwd.del_fails,
              (unsigned long)lwd.arm_fails, (unsigned long)lwd.rearm_fails,guard_timer != nullptr, NORI_OTA_MDNS, (unsigned long)http_tx_fails);
    }
    b.add("\"heap\":{\"free\":%lu,\"min\":%lu,\"max_alloc\":%lu},", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap(),
          (unsigned long)ESP.getMaxAllocHeap());
    b.add("\"net\":{\"now\":\"%s\",\"ip\":\"%s\",\"changes\":%lu},", NET_NAME[net_kind()], net_ip().toString().c_str(),
          (unsigned long)net_changes);
    b.add("\"eth\":{\"state\":\"%s\",\"ip\":\"%s\",\"link\":\"%s\",\"link_ups\":%lu,\"link_downs\":%lu,\"got_ip\":%lu},", eth_state(),
          eth_up() ? ETH.localIP().toString().c_str() : "", lnk, (unsigned long)eth_link_ups, (unsigned long)eth_link_downs,
          (unsigned long)eth_ips);
    b.add("\"wifi\":{\"state\":\"%s\",\"tunnel_optin\":%d,\"starts\":%lu,\"connects\":%lu,\"disconnects\":%lu,\"last_reason\":%u,"
          "\"got_ip\":%lu,\"lost_ip\":%lu,\"rssi\":%d,\"scan_matches\":%d,\"scan_best_rssi\":%d,\"ip\":\"%s\"},",
          wifi_state(), dev.cfg.wifi_on, (unsigned long)wifi_starts, (unsigned long)wifi_conns, (unsigned long)wifi_discs,
          (unsigned)wifi_reason, (unsigned long)wifi_ips, (unsigned long)wifi_ip_losses, wifi_up() ? (int)WiFi.RSSI() : 0,
          wifi_matches, wifi_best_rssi, wifi_up() ? WiFi.localIP().toString().c_str() : "");
    {   // wt32-r5.1: saved networks by index (worst case ~1.2 KB), never an SSID
        nvs_stats_t ns;
        memset(&ns, 0, sizeof ns);
        nvs_get_stats(nullptr, &ns);
        b.add("\"wnet\":{\"n\":%u,\"i\":%d,\"gen\":%lu,\"pend\":%u,\"state\":\"%s\",\"why\":\"%s\",\"last_fail\":\"%s\",\"last_reason\":%u,\"load\":\"%s\","
              "\"scans\":%lu,\"bg_scans\":%lu,\"scan_fails\":%lu,\"joins\":%lu,\"switches\":%lu,\"upgrades\":%lu,\"upgrade_fails\":%lu,"
              "\"rounds\":%lu,\"holds\":%lu,\"nvs_free\":%lu,\"profiles\":[",
              wn.n, wsm.cur, (unsigned long)wn.gen, wn.pend, NWS_STATE_NAME[wsm.state], NWW_NAME[wsm.why], NWF_NAME[wsm.last_cls],
              (unsigned)wsm.last_reason, NW_LD_NAME[wn_load_how], (unsigned long)wsm.scans, (unsigned long)wsm.bg_scans,
              (unsigned long)wsm.scan_fails, (unsigned long)wsm.joins, (unsigned long)wsm.switches, (unsigned long)wsm.upgrades,
              (unsigned long)wsm.upgrade_fails, (unsigned long)wsm.rounds, (unsigned long)wsm.holds, (unsigned long)ns.free_entries);
        uint32_t now_w = millis();
        for (int i = 0; i < wn.n; i++) {
            const nws_prof &p = wsm.p[i];
            b.add("%s{\"i\":%d,\"hidden\":%d,\"seen\":%u,\"rssi\":%d,\"tries\":%lu,\"ok\":%lu,\"noap\":%lu,\"auth\":%lu,\"assoc\":%lu,\"dhcp\":%lu,"
                  "\"lost\":%lu,\"last\":\"%s\",\"reason\":%u,\"backoff_s\":%lu}", i ? "," : "", i, wn.net[i].flags & NW_F_HIDDEN ? 1 : 0, p.vis, p.rssi,
                  (unsigned long)p.tries, (unsigned long)p.oks, (unsigned long)p.fails[NWF_NOAP], (unsigned long)p.fails[NWF_AUTH],
                  (unsigned long)p.fails[NWF_ASSOC], (unsigned long)p.fails[NWF_DHCP], (unsigned long)p.fails[NWF_LOST], NWF_NAME[p.last_cls],
                  (unsigned)p.last_reason, (unsigned long)(p.nb_on ? (p.nb - now_w) / 1000 : 0));
        }
        b.add("]},");
    }
    {   // wt32-r4: routing (worst case ~560 B) and the PM3 LAN (~420 B + 4 leases x ~120 B)
        char a[16], c[16], d[16], e[16], f[16], g[16], mac[18];
        b.add("\"router\":{\"state\":\"%s\",\"napt\":%u,\"default\":\"%s\",\"dnat\":\"%s:%u>%s:%u\",\"dnat_ok\":%u,\"guard\":%u,"
              "\"guard_drops\":%lu,\"forwarded\":%lu,\"no_route\":%lu,\"dropped\":%lu,\"nat_tcp\":%u,\"nat_udp\":%u,\"nat_icmp\":%u,"
              "\"nat_evictions\":%u,\"portmap_binds\":%lu,\"portmap_fails\":%lu,\"default_fixes\":%lu,\"reconciles\":%lu,",
              nn_route_state(&nat), nat.napt, nn_def_name(nat.def), nat.pm_ok ? ip_s(nat.pm_maddr, a) : "-", (unsigned)NAT_CFG.dnat_mport,
              ip_s(NAT_CFG.dnat_daddr, c), (unsigned)NAT_CFG.dnat_dport, nat.pm_ok, nat.guard, (unsigned long)nat.guard_drops,
              (unsigned long)nat.fw, (unsigned long)nat.rterr, (unsigned long)nat.drop, nat.nat_tcp, nat.nat_udp, nat.nat_icmp,
              nat.nat_evict, (unsigned long)nat.pm_binds, (unsigned long)nat.pm_fails, (unsigned long)nat.def_fixes,
              (unsigned long)nat.reconciles);
        b.add("\"wifi_ip\":\"%s\",\"wifi_mask\":\"%s\",\"wifi_gw\":\"%s\",\"dns\":[\"%s\",\"%s\"],\"eth_ip\":\"%s/%d\",\"channel\":%d,"
              "\"joins\":%lu,\"post_fails\":%lu,\"slow\":%lu,\"eth_cfg_ok\":%d,\"eth_cfg_fixes\":%lu,\"tunnel_port\":%u,\"tunnel\":\"%s\"},",
              ip_s(nat.wifi_ip, a), ip_s(nat.wifi_mask, c), ip_s(nat.wifi_gw, d), ip_s(nat_dns[0], e), ip_s(nat_dns[1], f),
              ip_s(nat.eth_ip, g), nn_prefix_len(nat.eth_mask), wifi_channel(), (unsigned long)wifi_joins, (unsigned long)nat_post_fails,
              (unsigned long)nat_slow, eth_cfg_ok, (unsigned long)eth_cfg_fixes, (unsigned)NORI_TUNNEL_PORT,
              listening ? (client ? "open" : "listen") : "off");
        const ndh_state &s = lan.dhcp;
        b.add("\"lan\":{\"state\":\"%s\",\"enabled\":%d,\"standdown\":\"%s\",\"standdowns\":%lu,\"link\":%u,\"link_ups\":%lu,"
              "\"link_downs\":%lu,\"listening\":%u,\"evidence_window\":%u,\"pcb\":%u,\"sniffer\":%u,\"server\":\"192.168.77.1\",\"pool\":\"192.168.77.2-%u\","
              "\"lease_s\":%lu,\"dns\":[\"%s\",\"%s\"],",
              nla_state_word(&lan), LAN_CFG.enabled, NLA_SD_NAME[lan.standdown < NLA_SD_COUNT ? lan.standdown : 0],
              (unsigned long)lan.standdowns, lan.link, (unsigned long)lan.link_ups, (unsigned long)lan.link_downs, lan.listening, lan.evidence,
              lan.pcb_ok, lan.sniff, 1u + LAN_CFG.dhcp.pool_n, (unsigned long)LAN_CFG.dhcp.lease_s, ip_s(lan.dns[0], a), ip_s(lan.dns[1], c));
        b.add("\"rx\":%lu,\"discover\":%lu,\"request\":%lu,\"offer\":%lu,\"ack\":%lu,\"nak\":%lu,\"release\":%lu,\"decline\":%lu,"
              "\"inform\":%lu,\"full\":%lu,\"other_server\":%lu,\"bad\":%lu,\"ignored\":%lu,\"expired\":%lu,\"quiet_drops\":%lu,"
              "\"wrong_if\":%lu,\"wrong_port\":%lu,\"too_big\":%lu,\"tx_ok\":%lu,\"tx_err\":%lu,\"pcb_fails\":%lu,",
              (unsigned long)s.rx, (unsigned long)s.discover, (unsigned long)s.request, (unsigned long)s.offer, (unsigned long)s.ack,
              (unsigned long)s.nak, (unsigned long)s.release, (unsigned long)s.decline, (unsigned long)s.inform, (unsigned long)s.full,
              (unsigned long)s.other_server, (unsigned long)s.bad, (unsigned long)s.ignored, (unsigned long)s.expired,
              (unsigned long)lan.rx_quiet, (unsigned long)lan.rx_wrong_if, (unsigned long)lan.rx_wrong_port, (unsigned long)lan.rx_too_big,
              (unsigned long)lan.tx_ok, (unsigned long)lan.tx_err, (unsigned long)lan.pcb_fails);
        b.add("\"sniff_frames\":%lu,\"foreign_dhcp\":%lu,\"upstream_gw_seen\":%lu,\"lan_sources\":%lu,\"last_foreign\":\"%s\","
              "\"last\":{\"ev\":\"%s\",\"mac\":\"%s\",\"ip\":\"%s\",\"age_s\":%lu},\"leases\":[",
              (unsigned long)lan.sn_frames, (unsigned long)lan.sn_foreign_dhcp, (unsigned long)lan.sn_gw_seen, (unsigned long)lan.sn_lan_srcs,
              ip_s(lan.sn_last_foreign, a), NDH_EV_NAME[s.last_ev < NDH_EV_COUNT ? s.last_ev : 0], ndh_mac_str(s.last_mac, mac),
              ip_s(s.last_ip, c), (unsigned long)(s.last_ms ? (sys_now() - s.last_ms) / 1000 : 0));
        uint32_t now_l = sys_now();
        for (int i = 0; i < LAN_CFG.dhcp.pool_n; i++) {
            const ndh_lease &l = s.l[i];
            b.add("%s{\"ip\":\"192.168.77.%d\",\"state\":\"%s\",\"mac\":\"%s\",\"remaining_s\":%lu,\"stale\":%u,\"acks\":%lu}", i ? "," : "",
                  i + 2, NDH_STATE_NAME[l.state < 5 ? l.state : 0], l.has_mac ? ndh_mac_str(l.mac, mac) : "",
                  (unsigned long)(ndh_remaining_ms(&l, now_l) / 1000), l.stale, (unsigned long)l.acks);
        }
        b.add("]},");
    }
    b.add("\"link\":{\"host_alive\":%d,\"rx_frames\":%lu,\"noise\":%lu,\"crc\":%lu,\"bad\":%lu,\"long\":%lu,\"ev_dropped\":%lu,"
          "\"tunnels\":%lu},",
          dev.host_alive, (unsigned long)dev.rx.frames, (unsigned long)dev.rx.noise, (unsigned long)dev.rx.bad_crc,
          (unsigned long)dev.rx.bad_syntax, (unsigned long)dev.rx.too_long, (unsigned long)dev.ev_dropped, (unsigned long)tun_accepts);
    b.add("\"toggle\":{\"pos\":\"%s\",\"locked\":%d,\"five\":%lu,\"one\":%lu,\"center\":%lu,\"both\":%lu,\"refused\":%lu,\"sent\":%lu,\"dropped\":%lu,\"short\":%lu,\"long\":%lu,\"cancelled\":%lu},",
          TOG_WORD[tog.stable], tog.locked, (unsigned long)tog.n_five, (unsigned long)tog.n_one, (unsigned long)tog.n_center,
          (unsigned long)tog.n_both, (unsigned long)tog.n_locked, (unsigned long)tog_tx, (unsigned long)tog_drop,
          (unsigned long)ges.n_short, (unsigned long)ges.n_long, (unsigned long)ges.n_cancel);
    b.add("\"ota\":{\"arduino\":\"%s\",\"http\":\"%s\",\"ota_last\":\"%s\",\"http_last\":\"%s\",\"ota_starts\":%lu,\"ota_errors\":%lu,"
          "\"auth_ok\":%lu,\"auth_wifi\":%lu,\"auth_refused\":%lu,\"uploads\":%lu,\"upload_fails\":%lu},",
          ota_state(), http_state(), ota_last, http_last, (unsigned long)ota_starts, (unsigned long)ota_errors, (unsigned long)http_auth_ok,
          (unsigned long)http_auth_wifi,
          (unsigned long)http_auth_bad, (unsigned long)http_uploads, (unsigned long)http_fails);
    b.add("\"tof\":{\"stage\":\"%s\",\"enabled\":%d,\"bus\":%d,\"present_on_bus\":%d,\"probes\":%lu,\"nacks\":%lu,\"bus_errs\":%lu,"
          "\"bus_clears\":%lu,\"inits_ok\":%lu,\"inits_fail\":%lu,\"init_ms\":%lu,\"cfg_fail\":%lu,\"start_fail\":%lu,\"id_fail\":%lu,",
          tof_stage_name(t), t.enabled, t.bus_ok, t.sm.present, (unsigned long)t.sm.probes, (unsigned long)t.sm.nacks,
          (unsigned long)t.sm.bus_errs, (unsigned long)t.sm.busclrs, (unsigned long)t.sm.inits_ok, (unsigned long)t.sm.inits_fail,
          (unsigned long)t.sm.init_ms_last, (unsigned long)t.sm.cfg_fail, (unsigned long)t.sm.start_fail, (unsigned long)t.sm.id_fail);
    b.add("\"frames\":%lu,\"bad_frames\":%lu,\"timeouts\":%lu,\"restarts\":%lu,\"stops_fail\":%lu,\"last_fail\":\"%s/%d\","
          "\"xfer\":%lu,\"nack\":%lu,\"bus_timeouts\":%lu,\"bus_other\":%lu,\"skipped\":%lu,\"busclr_fail\":%lu,\"task\":%d,"
          "\"last_bus_err\":%d,\"uld_status\":%u,\"temp_c\":%d,"
          "\"stack_free\":%lu,\"hz\":%d,\"integ_ms\":%d,\"zones\":16},",
          (unsigned long)t.sm.frames, (unsigned long)t.sm.bad_frames, (unsigned long)t.sm.timeouts, (unsigned long)t.sm.restarts,
          (unsigned long)t.sm.stops_fail, t.sm.last_fail_act >= 0 ? NT_ACTION_NAME[t.sm.last_fail_act] : "-", t.sm.last_fail_res,
          (unsigned long)t.xfer, (unsigned long)t.nack, (unsigned long)t.tmo, (unsigned long)t.err, (unsigned long)t.skipped,
          (unsigned long)t.sm.busclr_fail, t.task_ok, t.last_err, t.uld_status, t.temp_c,
          (unsigned long)t.stack_free, NORI_TOF_HZ, NORI_TOF_INTEG_MS);
    b.add("\"presence\":{\"known\":%d,\"level\":%d,\"effective\":%d,\"sent\":%d,\"nearest_mm\":%d,\"valid_zones\":%u,\"near_zones\":%u,"
          "\"held_zones\":%u,\"rises\":%lu,\"falls\":%lu,\"stale_falls\":%lu,\"fault_falls\":%lu,\"good_frames\":%lu,\"bad_frames\":%lu,"
          "\"zones_rejected\":%lu,\"ev_sent\":%lu,\"ev_retries\":%lu,",
          t.known, t.np.level, pres_eff, dev.pres_sent, t.np.nearest_mm, t.np.valid_zones, t.np.near_zones, t.np.held_zones,
          (unsigned long)t.np.rises, (unsigned long)t.np.falls, (unsigned long)t.np.stale_falls, (unsigned long)t.np.fault_falls,
          (unsigned long)t.np.frames, (unsigned long)t.np.bad_frames, (unsigned long)t.np.zones_rejected, (unsigned long)dev.pres_tx,
          (unsigned long)dev.pres_retry);
    b.add("\"cfg\":{\"near_mm\":%u,\"far_mm\":%u,\"min_mm\":%u,\"min_zones\":%u,\"on_frames\":%u,\"off_frames\":%u,\"stale_ms\":%u}},",
          t.cfg.near_mm, t.cfg.far_mm, t.cfg.min_mm, t.cfg.min_zones, t.cfg.on_frames, t.cfg.off_frames, t.cfg.stale_ms);
    static nlg_ring r;
    log_copy(&r);
    static char ev[48 * 64];
    nlg_json(&r, ev, sizeof ev);
    b.add("\"events_total\":%lu,\"events\":%s,", (unsigned long)r.total, ev);
    nlg_ev pe[NR_TRACE_EV];
    unsigned k = prev_ok ? nr_trace_events(&prev_trace, pe) : 0;
    nlg_json_arr(pe, k, ev, sizeof ev);
    if (prev_ok)
        b.add("\"prev\":{\"valid\":1,\"boot\":\"%08lx\",\"up_s\":%lu,\"heap_min\":%lu,\"events_total\":%lu,\"events\":%s}}\n",
              (unsigned long)prev_trace.boot_id, (unsigned long)prev_trace.up_s,
              (unsigned long)(prev_trace.heap_min == UINT32_MAX ? 0 : prev_trace.heap_min), (unsigned long)prev_trace.ev_total, ev);
    else b.add("\"prev\":{\"valid\":0}}\n");
}

// ---- wt32-r3 commands: PRESCFG, SENSOR -------------------------------------------------
//   PRESCFG                      -> OK PRESCFG near=.. far=.. min=.. zones=.. on=.. off=.. stale=..
//   PRESCFG near=900 far=1200 .. -> same, after validating all keys together and saving the changed ones
//   SENSOR [en=0|1] [re=1]       -> OK SENSOR en=<0|1> st=<stage>     (en is saved; re=1 restarts the sensor)
static int ext_cmd(void *, const nl_frame *f, char *reply, size_t cap)
{
    if (nwp_cmd(&wprov, f, reply, cap, millis())) return 1;   // wt32-r5.1: WNBEGIN WNDATA WNEND WNABORT
    if (!strcmp(f->type, "PRESCFG")) {
        np_cfg c = pres_cfg;
        for (int i = 0; i < f->argc; i++) {
            char key[12];
            const char *eq = strchr(f->argv[i], '=');
            uint32_t v;
            size_t kl = eq ? (size_t)(eq - f->argv[i]) : 0;
            if (!eq || kl == 0 || kl >= sizeof key || nl_parse_u32(eq + 1, &v) != 0) { snprintf(reply, cap, "ERR PRESCFG format"); return 1; }
            memcpy(key, f->argv[i], kl);
            key[kl] = 0;
            if (np_cfg_set(&c, key, v) != 0) { snprintf(reply, cap, "ERR PRESCFG range"); return 1; }
        }
        if (!np_cfg_valid(&c)) { snprintf(reply, cap, "ERR PRESCFG range"); return 1; }
        if (f->argc) {
            pres_cfg = c;
            pres_save();
            tof_set_cfg(&pres_cfg);
        }
        char s[96];
        np_cfg_format(&pres_cfg, s, sizeof s);
        snprintf(reply, cap, "OK PRESCFG %s", s);
        return 1;
    }
    // wt32-r5.1.3, for the physical test only: WDTTEST stall=1 -> OK WDTTEST, then loop() stops feeding the watchdog
    // (phase wdt_test) and the board resets with task_wdt about 5 s later.  Nothing is saved.
    if (!strcmp(f->type, "WDTTEST")) {
        if (f->argc != 1 || strcmp(f->argv[0], "stall=1")) { snprintf(reply, cap, "ERR WDTTEST need stall=1"); return 1; }
        if (lwd.state != NWD_ON) { snprintf(reply, cap, "ERR WDTTEST wdt=%s", nph_wdt_name((uint32_t)lwd.state)); return 1; }
        uint32_t t = millis() + 300;              // the reply goes out first
        wdt_test_at = t ? t : 1;
        snprintf(reply, cap, "OK WDTTEST stall in 300 ms");
        return 1;
    }
    if (!strcmp(f->type, "SENSOR")) {
        uint32_t en = 2, re = 0;
        for (int i = 0; i < f->argc; i++) {
            const char *a = f->argv[i];
            uint32_t v;
            if (!strncmp(a, "en=", 3) && nl_parse_u32(a + 3, &v) == 0 && v <= 1) en = v;
            else if (!strncmp(a, "re=", 3) && nl_parse_u32(a + 3, &v) == 0 && v == 1) re = 1;
            else { snprintf(reply, cap, "ERR SENSOR range"); return 1; }
        }
        if (en <= 1 && (bool)en != tof_en) {
            tof_en = en;
            prefs.putUChar("tof_en", (uint8_t)en);
            tof_enable(tof_en);
        }
        if (re) tof_restart();
        snprintf(reply, cap, "OK SENSOR en=%d st=%s", tof_en ? 1 : 0, tof_en ? tof_stage_name(tof_last) : "off");
        return 1;
    }
    return 0;
}

static const nd_hal HAL = { nullptr, u_room, u_queued, u_write, w_status, w_set, w_clear, w_power,
                            t_listen, t_accept, t_read, t_write, t_close, c_save, diag, o_key, diag_page, ext_cmd };

// ---- main ---------------------------------------------------------------------------
void setup()
{
    rst_reason = (unsigned)esp_reset_reason();
    rst_hw = (unsigned)esp_rom_get_reset_reason(0);
    rst_kept = nr_boot(&rst_rtc, (uint8_t)rst_reason);
    uint32_t boot = esp_random() ^ (uint32_t)esp_timer_get_time() ^ ((uint32_t)rst_reason << 24);
    if (!boot) boot = 1;                           // nd_init would turn 0 into 1; keep the trace identical
    ph_prev_ok = nph_take(&ph_rtc, &ph_prev, boot, millis());   // wt32-r5.1.3: before anything that could hang
    loop_task = xTaskGetCurrentTaskHandle();       // setup() runs in the Arduino loop task
    esp_log_level_set("*", ESP_LOG_NONE);
    nlg_init(&lg);
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
        case ARDUINO_EVENT_ETH_START: ETH.setHostname(NORI_HOSTNAME); eth_started = true; log_ev(NLG_ETH_START, 0, 0); break;
        case ARDUINO_EVENT_ETH_CONNECTED: eth_link = true; eth_link_ups = eth_link_ups + 1; log_ev(NLG_ETH_UP, 0, 0); break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            eth_link = false; eth_link_downs = eth_link_downs + 1; log_ev(NLG_ETH_DOWN, 0, 0); break;
        case ARDUINO_EVENT_ETH_GOT_IP: eth_ips = eth_ips + 1; log_ev(NLG_ETH_IP, 0, 0); break;   // r4: the static address (re)applied
        case ARDUINO_EVENT_ETH_STOP: eth_started = eth_link = false; log_ev(NLG_ETH_STOP, 0, 0); break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED: wifi_assoc = true; wifi_conns = wifi_conns + 1; log_ev(NLG_WIFI_CONN, 0, 0); break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            wifi_assoc = false;
            wifi_reason = info.wifi_sta_disconnected.reason;
            wifi_discs = wifi_discs + 1;
            log_ev(NLG_WIFI_DISC, info.wifi_sta_disconnected.reason, 0);
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP: wifi_reason = 0; wifi_ips = wifi_ips + 1; log_ev(NLG_WIFI_IP, 0, 0); break;
        case ARDUINO_EVENT_WIFI_STA_LOST_IP: wifi_ip_losses = wifi_ip_losses + 1; log_ev(NLG_WIFI_NOIP, 0, 0); break;
        default: break;
        }
        nat_kick = true;                          // re-bind the port map / default route / LAN service on the next pass
    });
    Serial.setRxBufferSize(RX_BUF);
    Serial.setTxBufferSize(TX_BUF);
    Serial.begin(NORI_BAUD);                       // UART0: TX0 GPIO1, RX0 GPIO3
    Serial.setRxFIFOFull(64);
    // (no btStop(): Bluetooth is not compiled into this build, CONFIG_BT_ENABLED is off)

    prefs.begin("nori", false);
    nd_cfg cfg;
    nd_cfg_defaults(&cfg);
    cfg.wifi_on = prefs.getUChar("wifi_on", 0);
    cfg.tap_ths = prefs.getUChar(K_THS, 0);
    cfg.dtap_wait_ms = prefs.getUShort(K_WAIT, cfg.dtap_wait_ms);
    cfg.refractory_ms = prefs.getUShort(K_REFR, cfg.refractory_ms);
    saved = cfg;
    wn_load_how = nw_load(&wn, &WN_KV);            // first boot after r5.0.1: its ssid/psk become profile 0, and stay
    nws_init(&wsm, &WSM_HAL);
    wprov.t = &wn;
    wprov.kv = &WN_KV;
    wprov.key = [](void *) -> const char * { return ota_hash; };
    wprov.rnd = wn_rnd;
    wprov.active = wn_active;
    wprov.changed = wn_changed;
    String h = prefs.getString("otah", "");
    if (h.length() == 32) memcpy(ota_hash, h.c_str(), sizeof ota_hash);
    pres_load();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    img_pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;

    for (int k = 0; k < 2; k++) if (BTN_PIN[k] >= 0) pinMode(BTN_PIN[k], INPUT_PULLUP);
    pinMode(NORI_PIN_TOG_FIVE, INPUT_PULLUP);      // physical test: GPIO15 selects the lower, five-person track
    pinMode(NORI_PIN_TOG_ONE, INPUT_PULLUP);
    prev_ok = nr_trace_take(&rst_trace, &prev_trace, boot);
    log_ev(NLG_BOOT, (uint8_t)rst_reason, (uint16_t)rst_hw);
    log_ev(NLG_WNET, 0, (uint16_t)(wn_load_how << 8 | wn.n));
    // LAN8720A, 50 MHz reference clock from the board oscillator into GPIO0 (enabled with GPIO16).
    // r4: static private address straight away, so the DHCP client never runs on Ethernet (r3 ran one there,
    // and so did the PM3: two clients, no address).  No gateway, no DNS: Wi-Fi keeps the default route.
    ph_go(NPH_ETH_BEGIN);
    eth_begin_ok = ETH.begin(NORI_ETH_ADDR, NORI_ETH_POWER, NORI_ETH_MDC, NORI_ETH_MDIO, ETH_PHY_LAN8720, ETH_CLOCK_GPIO0_IN);
    if (eth_begin_ok) eth_cfg_ok = ETH.config(ETH_IP, IPAddress(0, 0, 0, 0), ETH_MASK);
    ph_go(NPH_SETUP);
    nn_init(&nat_tc);
    nn_init(&nat);
    nla_init(&lan_tc);
    nla_init(&lan);
    delay(5);                                      // let pull-ups settle before the first sample
    uint32_t now = millis();
    net_next = now;                                // Wi-Fi starts from loop(), asynchronously
    nat_next = now;
    for (int k = 0; k < 2; k++) ni_button_init(&btn[k], pressed(k), now);
    ni_toggle_init(&tog, tog_low(NORI_PIN_TOG_FIVE), tog_low(NORI_PIN_TOG_ONE), now);   // the boot position is not a move
    ni_gesture_init(&ges, &tog);
    if (tog.stable == NI_TOG_BOTH) log_ev(NLG_TOG, NI_TOG_BOTH, 0);
    int nbtn = (BTN_PIN[0] >= 0) + (BTN_PIN[1] >= 0);
    nd_init(&dev, &HAL, boot, FW, SENSOR_NAME, nbtn, &cfg, now);
    ph_go(NPH_TOF_BEGIN);
    tof_begin(tof_en, &pres_cfg, log_ev);          // starts probing ~3 s from now, in its own task
    tof_snapshot(&tof_last);
    ph_go(NPH_SETUP);
    // wt32-r5.1.3: the phase record's independent clock, the ArduinoOTA guard, and loop() on the task watchdog.  Nothing
    // here waits for a network: buttons, selector and the serial link work from the first pass, with or without Wi-Fi.
    const esp_timer_create_args_t ta = { alive_cb, nullptr, ESP_TIMER_TASK, "nori_alive", false };
    if (esp_timer_create(&ta, &alive_timer) == ESP_OK) esp_timer_start_periodic(alive_timer, NPH_ALIVE_MS * 1000);
    const esp_timer_create_args_t tg = { guard_cb, nullptr, ESP_TIMER_TASK, "nori_otaguard", false };
    if (esp_timer_create(&tg, &guard_timer) != ESP_OK) guard_timer = nullptr;   // then no ArduinoOTA hole (nori_lwdt.h)
    nwd_init(&lwd, &WD_HAL);
    nwd_start(&lwd);
    ph_rtc.wdt = (uint32_t)lwd.state;
}

static uint32_t next_tof, next_sec;
static int host_last = -1;
static uint32_t pres_tx_seen;

void loop()
{
    wdt_feed();                                    // wt32-r5.1.3: the only feed of a normal pass (see nori_lwdt.h)
    ph_rtc.passes++;
    ph_go(NPH_UART);
    uint32_t now = millis();
    static uint8_t buf[1024];                      // one bulk read per pass
    int avail = Serial.available();
    size_t n = avail > 0 ? Serial.read(buf, (size_t)avail < sizeof buf ? (size_t)avail : sizeof buf) : 0;
    if (n) nd_uart_input(&dev, buf, n, now);

    ph_go(NPH_INPUT);                              // before anything network: inputs never wait for Wi-Fi
    if ((int32_t)(now - next_btn) >= 0) {          // 5 ms sampling, 30 ms debounce
        next_btn = now + 5;
        for (int k = 0; k < 2; k++) if (BTN_PIN[k] >= 0) nd_button(&dev, k, ni_button_update(&btn[k], pressed(k), now, 30), now);
        tog_service(now);
    }
    ph_go(NPH_TOF);
    if ((int32_t)(now - next_tof) >= 0) {          // presence: a snapshot of the sensor task, 20 times a second
        next_tof = now + 50;
        tof_snapshot(&tof_last);
        pres_eff = np_effective(&tof_last.np, &tof_last.cfg, now, 500);
    }
    nd_presence(&dev, tof_last.known, pres_eff);
    dev.cfg_changed = 0;                           // CFG tap keys have no sensor to reconfigure here
    ph_go(NPH_NET);
    if (listening && client && server.hasClient()) server.available().stop();   // one tunnel at a time
    net_service(now);
    ph_go(NPH_NAT);
    nat_service(now);                              // routing + LAN service, posted to the tcpip thread
    ph_go(NPH_OTA_SVC);
    ota_service(now);
    ph_go(NPH_OTA_POLL);
    if (ota) ota->handle();                       // an authenticated transfer blocks, in the guarded hole
    ota_hole_end();                                // whatever way the transfer ended, loop() is watched again
    ph_go(NPH_HTTP);
    if (http) http->handleClient();               // <= 3 s unauthenticated; an authenticated upload blocks, feeding
    ph_go(NPH_LINK);
    nd_poll(&dev, millis());
    if (dev.pres_tx != pres_tx_seen) {             // a pon/poff went out (or was queued) for this host session
        pres_tx_seen = dev.pres_tx;
        log_ev(NLG_PRES, (uint8_t)(dev.pres_sent > 0), 0);
    }
    if (dev.host_alive != host_last) {
        host_last = dev.host_alive;
        log_ev(NLG_HOST, (uint8_t)host_last, 0);
    }
    ph_go(NPH_TICK);
    if ((int32_t)(now - next_sec) >= 0) {          // once a second: previous-boot trace (RTC RAM), heap watch
        next_sec = now + 1000;
        uint32_t fh = ESP.getFreeHeap();
        portENTER_CRITICAL(&lg_mux);
        nr_trace_tick(&rst_trace, now / 1000, fh);
        portEXIT_CRITICAL(&lg_mux);
        if (fh < 32768 && !heap_low_logged) { heap_low_logged = 1; log_ev(NLG_HEAP, 0, (uint16_t)(fh / 1024)); }
    }
    if (wdt_test_at && (int32_t)(millis() - wdt_test_at) >= 0) {   // WDTTEST stall=1: stop feeding, on purpose
        ph_go(NPH_WDT_TEST);
        for (;;) delay(10);                        // yields: only the loop task's watchdog entry can expire
    }
    ph_go(NPH_IDLE);
    delay(1);
}
