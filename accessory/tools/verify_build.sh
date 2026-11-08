#!/bin/sh
# wt32-r5 (wt32-r2's verifier, extended): prove from the build products (not from our own sources) that the firmware really
# forwards and translates, serves DHCP only through an Ethernet-bound pcb, carries the r3 sensor/diagnostics code,
# and that the images fit the r1/r3 OTA layout.  Run by tools/build_firmware.sh;
# writes <out dir>/VERIFY.txt.   sh tools/verify_build.sh <pio build dir> <out dir>
# proxmobil3-tools: the PlatformIO core comes from $PLATFORMIO_CORE_DIR (set by build_firmware.sh), the images and the
# generated sdkconfig.wt32 from <out dir>; comparisons with earlier releases use pinned hashes instead of local copies.
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
B=$1
P=${PLATFORMIO_CORE_DIR:?set PLATFORMIO_CORE_DIR (tools/build_firmware.sh does)}/packages
TC=$P/toolchain-xtensa-esp32/bin
NM=$TC/xtensa-esp32-elf-nm
OBJDUMP=$TC/xtensa-esp32-elf-objdump
R=${2:?usage: verify_build.sh <pio build dir> <out dir>}
E=$B/firmware.elf
OUT=$R/VERIFY.txt
fail=0
: > "$OUT"
say() { echo "$*" | tee -a "$OUT"; }
ok() { say "ok   $*"; }
bad() { say "FAIL $*"; fail=1; }
need() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }
ver() { sed -n 's/.*"version": *"\([^"]*\)".*/\1/p' "$1" | head -1; }

say "wt32-r5 build verification  $(date -u +%Y-%m-%dT%H:%M:%SZ)"
say "framework-espidf $(ver $P/framework-espidf/package.json) (ESP-IDF $(cat $P/framework-espidf/version.txt))"
say "framework-arduinoespressif32 $(ver $P/framework-arduinoespressif32/package.json)  toolchain-xtensa-esp32 $(ver $P/toolchain-xtensa-esp32/package.json)"

S=$R/sdkconfig.wt32
H=$B/config/sdkconfig.h
say "--- generated sdkconfig (firmware/sdkconfig.wt32)"
for kv in CONFIG_LWIP_IP_FORWARD=y CONFIG_LWIP_IPV4_NAPT=y CONFIG_LWIP_L2_TO_L3_COPY=y CONFIG_LWIP_STATS=y \
          CONFIG_FREERTOS_HZ=1000 CONFIG_ESPTOOLPY_FLASHMODE_DIO=y CONFIG_ESPTOOLPY_FLASHFREQ_40M=y \
          CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y CONFIG_PARTITION_TABLE_CUSTOM=y 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"' \
          CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y CONFIG_AUTOSTART_ARDUINO=y CONFIG_ETH_USE_ESP32_EMAC=y \
          CONFIG_ETH_RMII_CLK_INPUT=y CONFIG_ETH_RMII_CLK_IN_GPIO=0 CONFIG_LWIP_IRAM_OPTIMIZATION=y \
          CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER=y CONFIG_COMPILER_OPTIMIZATION_PERF=y CONFIG_ARDUHAL_LOG_DEFAULT_LEVEL_NONE=y \
          CONFIG_LOG_DEFAULT_LEVEL_NONE=y CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=64 CONFIG_ETH_DMA_RX_BUFFER_NUM=16 CONFIG_ETH_DMA_TX_BUFFER_NUM=16 \
          CONFIG_ESP32_WIFI_DYNAMIC_TX_BUFFER_NUM=32 CONFIG_ESP32_WIFI_STATIC_RX_BUFFER_NUM=8 CONFIG_ESP32_WIFI_DYNAMIC_RX_BUFFER_NUM=32 \
          CONFIG_ESP32_WIFI_TX_BA_WIN=6 CONFIG_ESP32_WIFI_RX_BA_WIN=6 CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=4096; do
    need "sdkconfig $kv" "grep -qx '$kv' '$S'"
done
for k in CONFIG_BT_ENABLED CONFIG_ESP32_SPIRAM_SUPPORT; do
    need "sdkconfig $k off" "grep -qx '# $k is not set' '$S'"
done
say "--- wt32-r5 changes no configuration: byte-identical to r4.0.1's (the image running today)"
sha() { sha256sum "$1" | cut -c1-64; }
need "sdkconfig.wt32 = r4.0.1's" "[ \"\$(sha '$S')\" = bb9249be5c134a0d0641fa56ac0b3b5cac9659159b62652cd37fee5393476423 ]"
need "sdkconfig.defaults = r4.0.1's" "[ \"\$(sha '$ROOT/firmware/sdkconfig.defaults')\" = 1113d91e48c9ff4179bd3dfba8ef31b1c14809b36d9782b389488ca0b8c2f17d ]"
need "sdkconfig.r4-overrides = r4.0.1's" "[ \"\$(sha '$ROOT/firmware/sdkconfig.r4-overrides')\" = 1ee2204c0bc69455d39e588daa80aeef4299275a0973de5e9a5d5c6437d5a23a ]"
need "partitions.csv = r4.0.1's" "[ \"\$(sha '$ROOT/firmware/partitions.csv')\" = 2683fd26b661bc909dcb8a9f05723d36c4000f9396995941a278fda409b753f7 ]"
need "sdkconfig.h = r4.0.1's" "[ \"\$(sha '$H')\" = 830dc77908e7b3f9645c3d3ba286ed1c28cf3dee960b2775439042ea3348f114 ]"
say "--- sdkconfig.h (what the compiler saw)"
for d in 'CONFIG_LWIP_IP_FORWARD 1' 'CONFIG_LWIP_IPV4_NAPT 1' 'CONFIG_LWIP_STATS 1' 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1'; do
    need "#define $d" "grep -q '^#define $d\$' '$H'"
done
need "#define CONFIG_APP_ROLLBACK_ENABLE (alias used by Arduino's initArduino rollback hook)" "grep -q '^#define CONFIG_APP_ROLLBACK_ENABLE ' '$H'"

say "--- lwIP library built from source"
LIB=$B/esp-idf/lwip/liblwip.a
need "liblwip.a built in this tree ($LIB)" "[ -f '$LIB' ]"
need "liblwip.a contains ip4_napt.o" "$TC/xtensa-esp32-elf-ar t '$LIB' | grep -q 'ip4_napt'"
for s in ip_napt_forward ip_napt_recv ip_portmap_add ip_portmap_remove ip_portmap_get ip_napt_enable_no; do
    need "liblwip.a defines $s" "$NM --defined-only '$LIB' 2>/dev/null | grep -q ' T $s\$'"
done

say "--- firmware.elf (linked code)"
for s in ip_napt_forward ip_napt_recv ip_portmap_add ip_portmap_remove ip_napt_enable_no ip4_input nn_reconcile nori_fw_tag; do
    need "ELF defines $s" "$NM '$E' | grep -q ' [TtDdRr] $s\$'"
done
# The call sites that make it a router.  tools/elf_calls.py also follows IRAM -> flash calls
# through their l32r literals (ip4_input is in IRAM with CONFIG_LWIP_IRAM_OPTIMIZATION).
# ip4_forward is static and inlined into ip4_input at -O2, so its callees show up there.
calls() { python3 "$ROOT/tools/elf_calls.py" "$E" "$OBJDUMP" "$NM" "$@" > /dev/null; }
need "ip4_input calls ip_napt_recv, ip_napt_forward and ip4_route_src (inbound translation + forward path)" \
     "calls ip4_input ip_napt_recv ip_napt_forward ip4_route_src"
need "nn_reconcile calls ip_napt_enable_no, ip_portmap_add, ip_portmap_remove, netif_set_default (ip_portmap_get via portmap_is)" \
     "calls nn_reconcile ip_napt_enable_no ip_portmap_add ip_portmap_remove netif_set_default portmap_is && calls portmap_is ip_portmap_get"
need "Wi-Fi inbound guard linked: nn_reconcile installs guard_input (loads its address), guard_next saved" \
     "python3 '$ROOT/tools/elf_calls.py' --refs '$E' '$OBJDUMP' '$NM' nn_reconcile guard_input > /dev/null && $NM '$E' | grep -q ' b guard_next$'"
# nat_service() is inlined into loop() at -O2; nat_cb is a real function (its address is posted).
NATCB=$($NM "$E" | awk '$2 ~ /^[tT]$/ && $3 ~ /nat_cb/ {print $3; exit}')
need "loop() posts nat_cb with tcpip_try_callback; nat_cb runs nn_reconcile (lwIP work in the tcpip thread)" \
     "[ -n '$NATCB' ] && calls _Z4loopv tcpip_try_callback && python3 '$ROOT/tools/elf_calls.py' --refs '$E' '$OBJDUMP' '$NM' _Z4loopv '$NATCB' > /dev/null && calls '$NATCB' nn_reconcile"
CALLERS=$("$OBJDUMP" -d "$E" | awk '/^[0-9a-f]+ <[^>]*>:$/ {f=$2; next} /call[0-9x]*[ \t].*<nn_reconcile>/ {print f}' | tr -d '<>:' | sort -u | tr '\n' ' ')
need "nn_reconcile's only direct caller is nat_cb (callers: $CALLERS)" "[ '$(echo $CALLERS)' = '$NATCB' ]"

say "--- wt32-r4 private-LAN DHCP server"
for s in ndh_handle ndh_tick nla_reconcile nla_sniff udp_bind_netif udp_sendto_if; do
    need "ELF defines $s" "$NM '$E' | grep -q ' [TtDdRr] $s\$'"
done
need "nat_cb runs nla_reconcile (DHCP pcb and Ethernet sniffer managed in the tcpip thread)" "calls '$NATCB' nla_reconcile"
LCALLERS=$("$OBJDUMP" -d "$E" | awk '/^[0-9a-f]+ <[^>]*>:$/ {f=$2; next} /call[0-9x]*[ \t].*<nla_reconcile>/ {print f}' | tr -d '<>:' | sort -u | tr '\n' ' ')
need "nla_reconcile's only direct caller is nat_cb (callers: $LCALLERS)" "[ '$(echo $LCALLERS)' = '$NATCB' ]"
need "the DHCP pcb is bound to a netif: udp_bind_netif called from nla_reconcile or pcb_ensure" \
     "calls nla_reconcile udp_bind_netif || calls pcb_ensure udp_bind_netif"
RCB=$(python3 "$ROOT/tools/elf_calls.py" "$E" "$OBJDUMP" "$NM" recv_cb 2>/dev/null | tr '\n' ' ')
need "DHCP replies leave only through udp_sendto_if on the Ethernet netif (recv_cb calls: $RCB)" \
     "calls recv_cb ndh_handle udp_sendto_if && ! echo ' $RCB ' | grep -q ' udp_sendto \| udp_send \| udp_sendto_if_src '"
need "Ethernet sniffer linked: nla_reconcile or sn_install installs sn_input" \
     "python3 '$ROOT/tools/elf_calls.py' --refs '$E' '$OBJDUMP' '$NM' nla_reconcile sn_input > /dev/null || python3 '$ROOT/tools/elf_calls.py' --refs '$E' '$OBJDUMP' '$NM' sn_install sn_input > /dev/null"
say "--- wt32-r3 sensor and diagnostics code carried over"
for s in vl53l5cx_init vl53l5cx_start_ranging vl53l5cx_get_ranging_data np_frame_update nt_next nt_result nlg_add nlg_json i2c_master_cmd_begin; do
    need "ELF defines $s" "$NM '$E' | grep -q ' [TtWw] $s\$'"
done
need "ELF defines tof_begin (C++)" "$NM '$E' | grep -q ' T _Z9tof_begin'"

say "--- wt32-r5 track selector"
for s in ni_toggle_update ni_toggle_init nd_selector ni_gesture_update ni_gesture_cancel; do   # nd_toggle, nd_gesture: inlined into nd_selector
    need "ELF defines $s" "$NM '$E' | grep -q ' [Tt] $s\$'"
done
need "firmware.elf carries EV names trk5/trk1" "strings -a '$E' | grep -qx trk5 && strings -a '$E' | grep -qx trk1"
need "firmware.elf carries EV names tsh5/tsh1/tlg5/tlg1" "strings -a '$E' | grep -qx tsh5 && strings -a '$E' | grep -qx tsh1 && strings -a '$E' | grep -qx tlg5 && strings -a '$E' | grep -qx tlg1"
need "loop() samples the selector (nd_selector, directly or through tog_service)" \
     "calls _Z4loopv nd_selector || calls _ZL11tog_servicej nd_selector"

say "--- wt32-r5.1 saved Wi-Fi networks"
for s in nw_load nw_commit nw_op_primary nws_tick nws_remap nwp_cmd nwp_open nwp_http_post; do
    need "ELF defines $s" "$NM '$E' | grep -q ' [Tt] $s\$'"
done
need "firmware.elf carries the WN frames and the HTTP route" "strings -a '$E' | grep -qx WNBEGIN && strings -a '$E' | grep -qx WNEND && strings -a '$E' | grep -qx /wifi/nonce"
need "the core's auto-reconnect is not left on: wr_on calls setAutoReconnect" "calls _ZL5wr_onPv _ZN12WiFiSTAClass16setAutoReconnectEb"

say "--- wt32-r5.1.2 depth grid"
need "ELF defines np_grid_format" "$NM '$E' | grep -q ' [Tt] np_grid_format\$'"
need "firmware.elf carries the grid page's format" "strings -a '$E' | grep -q 'zf=%lu za=%lu zg='"

say "--- wt32-r5.1.3 loop watchdog, ArduinoOTA guard, phase record"
for s in esp_task_wdt_add esp_task_wdt_delete esp_task_wdt_reset esp_timer_create esp_timer_start_once esp_system_abort; do
    need "ELF defines $s" "$NM '$E' | grep -q ' [TtWw] $s\$'"
done
WDADD=$($NM "$E" | awk '$2 ~ /^[tT]$/ && $3 ~ /wd_add/ {print $3; exit}')
WDRST=$($NM "$E" | awk '$2 ~ /^[tT]$/ && $3 ~ /wd_reset/ {print $3; exit}')
GUARD=$($NM "$E" | awk '$2 ~ /^[tT]$/ && $3 ~ /guard_cb/ {print $3; exit}')
need "the watchdog HAL subscribes, feeds and is installed by setup() (wd_add -> esp_task_wdt_add, wd_reset -> esp_task_wdt_reset)" \
     "[ -n '$WDADD' ] && [ -n '$WDRST' ] && calls '$WDADD' esp_task_wdt_add && calls '$WDRST' esp_task_wdt_reset && python3 '$ROOT/tools/elf_calls.py' --refs '$E' '$OBJDUMP' '$NM' _Z5setupv '$WDADD' > /dev/null"
need "setup() creates and starts the esp_timers (phase clock, ArduinoOTA guard); guard_cb ends in esp_system_abort" \
     "[ -n '$GUARD' ] && calls _Z5setupv esp_timer_create esp_timer_start_periodic && calls '$GUARD' esp_system_abort"   # its address sits in a rodata struct
need "firmware.elf carries the phase names, the DIAG p=ph format and WDTTEST" \
     "strings -a '$E' | grep -qx ota_xfer && strings -a '$E' | grep -qx http_drain && strings -a '$E' | grep -q ' pwph=%s pwms=%lu' && strings -a '$E' | grep -qx WDTTEST && strings -a '$E' | grep -qx slow"
need "every WebServer write is bounded: NoriHttp overrides _currentClientWrite and _currentClientWrite_P" \
     "$NM '$E' | grep -q ' [TtWw] _ZN8NoriHttp19_currentClientWriteEPKcj\$' && $NM '$E' | grep -q ' [TtWw] _ZN8NoriHttp21_currentClientWrite_PEPKcj\$'"
need "mDNS off: firmware/src/main.cpp defaults NORI_OTA_MDNS to 0 and CMakeLists.txt does not override it" \
     "tr -d '\r' < '$ROOT/firmware/src/main.cpp' | grep -qx '#define NORI_OTA_MDNS 0' &&! grep -q '^[[:space:]]*NORI_OTA_MDNS=' '$ROOT/firmware/src/CMakeLists.txt' && ! grep -q NORI_OTA_MDNS '$ROOT/firmware/platformio.ini'"

say "--- images"
ESPTOOL="python3 $P/tool-esptoolpy/esptool.py"
SZ=$(stat -c %s "$R/firmware.bin")
need "firmware.bin $SZ bytes fits the 0x140000 app slot ($(( (0x140000 - SZ) / 1024 )) KiB free, $(( SZ * 100 / 0x140000 ))% used)" "[ $SZ -le $((0x140000)) ]"
# image header bytes: e9 <segments> <flash mode: 02 = DIO> <size|freq: 20 = 4MB, 40m>
for img in firmware.bin bootloader.bin; do
    HDR=$(od -An -tx1 -N4 "$R/$img" | tr -d ' \n')
    MAGIC=$(echo "$HDR" | cut -c1-2)
    MODE=$(echo "$HDR" | cut -c5-8)
    need "$img header $HDR: magic e9, DIO, 4MB, 40m" "[ '$MAGIC' = e9 ] && [ '$MODE' = 0220 ]"
done
$ESPTOOL --chip esp32 image_info "$R/firmware.bin" > "$R/.imginfo" 2>&1 || true
need "firmware.bin checksum and SHA-256 validation hash valid (esptool image_info)" \
     "grep -q 'Checksum: .*(valid)' '$R/.imginfo' && grep -q 'Validation Hash: .*(valid)' '$R/.imginfo'"
rm -f "$R/.imginfo"
need "firmware.bin carries NORI_FW=wt32-r5.1.3" "grep -qa 'NORI_FW=wt32-r5.1.3' '$R/firmware.bin'"
GEN=$P/framework-espidf/components/partition_table/gen_esp32part.py
python3 "$GEN" "$R/partitions.bin" > "$R/.parts.csv" 2>/dev/null || true
need "partitions.bin = nvs 0x9000 / otadata 0xe000 / app0 0x10000 1280K / app1 0x150000 1280K / spiffs 0x290000 / coredump 0x3f0000" \
     "grep -q '^nvs,data,nvs,0x9000,20K' '$R/.parts.csv' && grep -q '^otadata,data,ota,0xe000,8K' '$R/.parts.csv' && grep -q '^app0,app,ota_0,0x10000,1280K' '$R/.parts.csv' && grep -q '^app1,app,ota_1,0x150000,1280K' '$R/.parts.csv' && grep -q '^spiffs,data,spiffs,0x290000,1408K' '$R/.parts.csv' && grep -q '^coredump,data,coredump,0x3f0000,64K' '$R/.parts.csv'"
rm -f "$R/.parts.csv"
# Pinned: the bootloader, partition table and boot_app0 every release from r4.0.1 to r5.1.3 shipped, so an OTA
# keeps the layout and rollback between them works.  Pinned so no copy of an older release is needed.
need "partitions.bin = the r4.0.1..r5.1.3 table (148b959c...)" \
     "[ \"\$(sha '$R/partitions.bin')\" = 148b959cbff1c38aa8e1d5c0ba9d612c54997b945e56a63f41223eef650653a1 ]"
need "bootloader.bin = the r4.0.1..r5.1.3 bootloader (0b5259c8...)" \
     "[ \"\$(sha '$R/bootloader.bin')\" = 0b5259c8b5fb7ee04efe59c3b960ca55067a10e49e29eca33933fc37bea04fac ]"
need "boot_app0.bin = the r4.0.1..r5.1.3 boot_app0 (f94c5d78...)" \
     "[ \"\$(sha '$R/boot_app0.bin')\" = f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0 ]"
need "boot_app0.bin is 8 KiB" "[ $(stat -c %s "$R/boot_app0.bin") -eq 8192 ]"

say "--- hashes"
( cd "$R" && sha256sum bootloader.bin partitions.bin boot_app0.bin firmware.bin firmware.elf ) | tee -a "$OUT"
if [ $fail -eq 0 ]; then say "ALL CHECKS PASSED"; else say "SOME CHECKS FAILED"; fi
exit $fail
