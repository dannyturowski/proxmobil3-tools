# WT32-ETH01 accessory firmware

`accessory/` holds the firmware for an optional ESP32 accessory, a Wireless-Tag WT32-ETH01 board connected to the PM3. It also holds the board's host-side tools and its tests. Snapshot: **wt32-r5.1.3**. The corresponding private build was deployed on the author's unit on 2026-11-07 and marked valid; the public source build was verified separately. Where it came from and what was changed for publication: [accessory/PROVENANCE.md](../../accessory/PROVENANCE.md).

## What it does

| Function | Detail |
|---|---|
| Serial link to the PM3 | UART0 (TX0 GPIO1, RX0 GPIO3) at 460800 baud through a 3.3 V USB-UART adapter (CP2102 on the author's unit). CRC-framed text protocol, see `accessory/shared/nori_link/src/nori_link.h`. `wt32-monitor` is the host side |
| Inputs | Two buttons (GPIO14, GPIO4), and a centre-off three-way lever (GPIO15, GPIO2), all to GND with internal pull-ups. Debounced on the board; each move is reported once |
| Presence | VL53L5CX time-of-flight sensor on I2C (SDA GPIO33, SCL GPIO32, 400 kHz), 4x4 zones at 5 Hz. Debounced presence events and a depth-grid diagnostic page |
| Network | Wi-Fi station upstream, up to 8 saved networks in priority order. The Ethernet LAN8720A (address 1, power GPIO16, MDC GPIO23, MDIO GPIO18) is a private point-to-point link to the PM3: 192.168.77.1/24, with a DHCP server for the PM3 at 192.168.77.2. IPv4 NAPT from the PM3 to Wi-Fi, one port forward (Wi-Fi TCP 2222 to the PM3's 2222), and an inbound guard |
| Updates | ArduinoOTA and an authenticated HTTP push updater, both keyed by an OTA secret provisioned over the serial link. Default Arduino partition layout with two app slots. A new image rolls back unless it proves itself: Wi-Fi management reachable plus the PM3 holding its lease |
| Diagnostics | `DIAG` pages over the link (`tof net rst ev prev rt dhcp tog zg ph`), reset history and a retained phase record in RTC memory, an event log, authenticated `/status` and `/diag` over HTTP |
| r5.1.3 | Main loop on the task watchdog (5 s). ArduinoOTA transfers guarded by a timer instead. mDNS off. ArduinoOTA no longer recreated on address changes. Bounded HTTP writes |

Pins and build options live in `accessory/firmware/src/CMakeLists.txt`. GPIO2 and GPIO15 are strap pins, used here as ground-only inputs. A contact to 3V3 would be wrong on both. Wiring and parts: see the hardware documents.

## Bootstrap a build machine

Linux or WSL2, with Python 3.8 or later and about 2 GB free.

```sh
python3 -m venv ~/.venvs/platformio
~/.venvs/platformio/bin/pip install platformio==6.2.0     # the version the snapshot was built with
export PIO=~/.venvs/platformio/bin/pio
sh accessory/tools/build_firmware.sh
```

The first run downloads the platform, frameworks and toolchain into `~/.cache/proxmobil3-tools/pio-core`. They are pinned in `accessory/firmware/platformio.ini`: platform `espressif32@6.12.0`, Arduino-ESP32 2.0.17 (`3.20017.241212`) built as an ESP-IDF 4.4.7 (`3.40407.240606`) component. This setup exists so that lwIP is compiled from source with IP forwarding and NAPT. The download is about 1.5 GB. To reuse a core you already have, set `PM3TOOLS_PIO_CORE=/path/to/core`.

The build copies the sources to a scratch directory without spaces, because ESP-IDF 4.4 needs that. It writes `build/firmware/` and runs `accessory/tools/verify_build.sh`. That script checks the built configuration against pinned hashes, checks that the router, watchdog and HTTP code really are linked into the ELF, checks the image headers, and checks the partition layout. Expect `ALL CHECKS PASSED`.

## Host tests

```sh
sh accessory/tests/run_firmware_tests.sh
```

These run natively with ASan/UBSan: the link core and debouncers, the lever, presence and ToF supervision, saved networks and provisioning, the DHCP server, the HTTP parser, the watchdog state machine, the phase record, offline inputs, and source checks. They also run the Python uploaders and the serial logger against the firmware's own C code. Needs gcc, g++, Python 3 and `requests` (`accessory/requirements.txt`).

## First install and provisioning

Nothing in this repository has flashed a board. The commands below are the author's tools, documented by their own `--help`. Read them before you use them.

1. **First flash** over the serial adapter, with the board in download mode (GPIO0 to GND at reset). Use esptool from the PlatformIO core at the standard offsets: bootloader `0x1000`, partitions `0x8000`, boot_app0 `0xe000`, application `0x10000`. The application slot is 0x140000 bytes.
2. **OTA secret.** `accessory/tools/nori_wt32_provision.py newpass FILE` creates a random password in a user-only file. `--port PORT otakey --password-file FILE` stores its MD5 on the board. The password never appears on a command line or in output.
3. **Wi-Fi.** `nori_wt32_provision.py --port PORT wifi --wifi-file W.json` takes one network. `nori_wifi_profiles.py apply --catalog C.json ...` takes several, in priority order. Keep those JSON files outside the repository. The `.gitignore` excludes common names, but check before you commit anything.
4. **Updates.** `nori_http_ota.py upload --host BOARD_IP --password-file FILE build/firmware/firmware.bin`. HTTP Digest protects the secret, not the image, so use it on a trusted LAN only.
5. **Watching it.** `wt32-monitor` on the PM3, or `nori_serial_log.py --port PORT --host` on a PC. The logger keeps a bounded, sanitized log of everything the board sends, ROM banners and panic dumps included.

Keep the lever at centre and the PM3 connected while a new image proves itself. Until it is marked valid, any reset rolls it back to the previous slot.

## Status and limits

- **Initial r5.1.3 hardware checks passed:** image marked valid, task watchdog enabled, Wi-Fi/private Ethernet restored, host serial reconnected, and fresh ToF data observed. These do not reproduce the original incident. It targets a suspected network-induced input stall seen on 2026-11-07, which was never reproduced. Brownouts were also recorded around then, and their role is unknown. r5.1.3 does not claim to fix them.
- What host tests cannot show: real Arduino and ESP-IDF call durations, the radio, the power path, and the hardware watchdog.
- The physical tests the author still has to do for r5.1.3: a watchdog test (`WDTTEST stall=1` resets about 5 s later), an offline boot with no known network, and 20 minutes of phone-hotspot flapping with inputs in use. Only then should r5.1.3 be called validated.
- The lever direction labels `trk5`/`trk1` are wire names only. Which physical direction is which was checked once, on one unit.
- Power: the author's unit uses a separate battery 5 V USB power injector while the PM3 supplies USB data through the CP2102. Do not flash a board on a supply that is not known to be good.
- A built image contains the build machine's absolute path to the PlatformIO core. Check that before you publish binaries.
