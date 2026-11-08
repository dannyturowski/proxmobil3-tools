#!/bin/sh
# Host tests of the accessory firmware's portable code, built natively with ASan/UBSan.  No ESP32, no hardware.
# Adapted from the private repository's focused wt32-r5.1.3 run; leaves out the suites that drive the private
# application bridge.
#   sh accessory/tests/run_firmware_tests.sh
set -eu
cd "$(dirname "$0")/.."
S=shared/nori_link/src
H=shared/hostio
W=shared/nori_wifi
N=shared/nori_sense/src
RT=shared/nori_router
R3="-DND_DIAG_JOIN=1 -DND_PRESENCE=1 -DND_EXT=1"
SAN="-g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
CC="gcc -O1 -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-missing-field-initializers $SAN"
CC2="gcc -O2 -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror $SAN"      # the flags the original suites use for these
CORE="$S/nori_link.c $S/nori_input.c $S/nori_motion.c $S/nori_dev.c $S/nori_host.c"
mkdir -p build/tests

$CC -pedantic -I$N -o build/tests/test_lwdt tests/test_lwdt.c
echo "== loop watchdog vs a model task watchdog: ordinary passes, HTTP waits, every ArduinoOTA ending"
build/tests/test_lwdt
$CC -pedantic -I$N -o build/tests/test_phase tests/test_phase.c $N/nori_evlog.c
echo "== retained phase record: validity, timing, stuck vs reset, DIAG/JSON bounds"
build/tests/test_phase
$CC $R3 -I$S -I$W -o build/tests/test_offline tests/test_offline.c $CORE $W/nori_wnet.c $W/nori_wnsm.c -lm
echo "== buttons and selector without Wi-Fi: away, hotspot flapping, off, no networks"
build/tests/test_offline
echo "== source checks: fed waits, OTA hole lifecycle, replies, mDNS, untouched settings"
python3 tests/check_wdt_source.py
for h in $N/nori_lwdt.h $N/nori_phase.h $N/nori_rst.h $N/nori_imgvalid.h $RT/nori_dhcps.h; do
    g++ -std=gnu++17 -Wall -Wextra -Werror -fsyntax-only -I$N -I$RT -x c++ $h
done
echo "== firmware headers compile as C++17"

$CC2 -I$S -o build/tests/test_shared tests/test_shared.c $CORE -lm
echo "== link framing, host session, input debounce, device core"
build/tests/test_shared
$CC2 -Wno-missing-field-initializers -DND_DIAG_JOIN=1 -I$S -o build/tests/test_wt32 tests/test_wt32.c $S/nori_link.c $S/nori_input.c $S/nori_motion.c $S/nori_dev.c -lm
echo "== WT32 device core (joined DIAG)"
build/tests/test_wt32
$CC $R3 -I$S -o build/tests/test_wt32r3 tests/test_wt32r3.c $CORE -lm
echo "== DIAG pages, extension commands"
build/tests/test_wt32r3
$CC $R3 -I$S -o build/tests/test_toggle tests/test_toggle.c $CORE -lm
echo "== three-way lever and gestures"
build/tests/test_toggle
$CC2 -pedantic -I$S -I$N -o build/tests/test_sense tests/test_sense.c $N/nori_presence.c $N/nori_tofsm.c $N/nori_evlog.c $S/nori_link.c
echo "== presence, ToF supervisor, depth grid, event log, boot trace"
build/tests/test_sense
$CC2 -I$S -I$N -o build/tests/test_rst tests/test_rst.c $N/nori_evlog.c $S/nori_link.c
echo "== reset records"
build/tests/test_rst
$CC2 -I$W -I$S -o build/tests/test_wifinet tests/test_wifinet.c $W/nori_wnet.c $W/nori_wnsm.c $W/nori_wnprov.c $S/nori_link.c
echo "== saved networks: store, power cuts, state machine, provisioning"
build/tests/test_wifinet
$CC2 -pedantic -I$RT -o build/tests/test_dhcps tests/test_dhcps.c $RT/nori_dhcps.c
echo "== private-LAN DHCP server"
build/tests/test_dhcps
gcc -O2 -std=c11 -Wall -Wextra -Werror $SAN -I$H -o build/tests/test_hostio tests/test_hostio.c $H/nori_hostio.c
echo "== host port checks"
build/tests/test_hostio
g++ -O1 -std=c++17 -Wall -Wextra -Werror $SAN -Ifirmware/src -o build/tests/test_http tests/test_http.cpp firmware/src/nori_http.cpp
echo "== HTTP parser"
build/tests/test_http

# Python suites: tools against the firmware's own C code.
CF="-O2 -std=c11 -Wall -Wextra -Werror"
gcc $CF -I$S -o build/tests/frame_check tests/frame_check.c $CORE
$CC $R3 -I$W -I$S -o build/tests/wn_dev tests/wn_dev.c $W/nori_wnet.c $W/nori_wnprov.c \
    $S/nori_link.c $S/nori_input.c $S/nori_motion.c $S/nori_dev.c -lm
gcc $CF -Wno-missing-field-initializers $R3 -I$S -I$N -o build/tests/nori-esp-sim-r3 host/sim/nori-esp-sim.c $CORE $N/nori_presence.c -lm
echo "== ArduinoOTA uploader (fake device on localhost)"
python3 tests/test_ota_updater.py
echo "== HTTP OTA updater (firmware parser behind a fake device)"
python3 tests/test_http_ota.py
echo "== saved-network provisioning over UART and HTTP"
python3 tests/test_wifi_provision.py
echo "== serial logger against the simulator"
python3 tests/test_serial_log.py
echo "== all accessory firmware host suites passed"
