# Third-party notices

This file covers the software parts of the repository. Hardware, CAD and photo sources are listed with them.

## Included in this repository

### STMicroelectronics VL53L5CX Ultra Lite Driver

- Files: `accessory/firmware/lib/vl53l5cx_uld/src/vl53l5cx_api.c`, `vl53l5cx_api.h`, `vl53l5cx_buffers.h`, copied unmodified, plus `changelog-ST.txt`.
- Source: <https://github.com/STMicroelectronics/stm32-vl53l5cx>, tag `v1.0.7` (commit `1e6b8b35b25d59f2f256d9b6b1280b026132ab85`), driver revision `VL53L5CX_1.3.9`. Per-file SHA-256 in `accessory/firmware/lib/vl53l5cx_uld/SOURCE.md`.
- License: BSD-3-Clause, Copyright 2022 STMicroelectronics. Full text: `accessory/firmware/lib/vl53l5cx_uld/LICENSE-ST.md`.
- `platform.c` and `platform.h` in the same directory are this project's ESP32 port. They keep ST's function prototypes; their bodies are new.

### Configuration derived from Arduino-ESP32

- `accessory/firmware/partitions.csv` is Arduino-ESP32's `default.csv`, unchanged.
- `accessory/firmware/sdkconfig.defaults` is generated: the Arduino-ESP32 2.0.17 SDK's `sdkconfig` with project overrides (`sdkconfig.r4-overrides`) applied. `sdkconfig.wt32` is the configuration the build produces from it.
- Arduino-ESP32 (Espressif Systems), package `framework-arduinoespressif32` 3.20017.241212. Its package metadata declares LGPL-2.1-or-later. The LGPL 2.1 text is included at [LICENSES/LGPL-2.1.txt](LICENSES/LGPL-2.1.txt).

## Downloaded at build time, not included

The firmware build fetches these through PlatformIO, at the exact versions pinned in `accessory/firmware/platformio.ini`. Their licenses apply to firmware images built from this repository. Anyone distributing such an image must meet those terms, LGPL-2.1 included.

| Package | Version | License (package metadata or license file where the package has one, otherwise the upstream project's license) |
|---|---|---|
| PlatformIO platform `espressif32` | 6.12.0 | Apache-2.0 |
| `framework-arduinoespressif32` (Arduino-ESP32 2.0.17, includes ArduinoOTA and WebServer) | 3.20017.241212 | LGPL-2.1-or-later |
| `framework-espidf` (ESP-IDF v4.4.7, includes lwIP, FreeRTOS, mbedTLS and others, each under its own license) | 3.40407.240606 | Apache-2.0 for ESP-IDF; lwIP BSD-3-Clause (Swedish Institute of Computer Science); see each component |
| `toolchain-xtensa-esp32` (GCC) | 8.4.0+2021r2-patch5 | GPL with the GCC runtime library exception |
| `tool-esptoolpy` | 2.40900.250804 | GPL-2.0 |
| `tool-cmake`, `tool-ninja`, `tool-scons`, `toolchain-esp32ulp`, `tool-esp-rom-elfs` | 3.30.2, 1.7.1, 4.41101.0, 1.23800.240113, 0.0.1+20241011 | Their own licenses; build tools only |

The table was compiled from the downloaded package cache on 2026-11-07. Check it again before you distribute firmware binaries.

Python packages for the PC-side accessory tools (`accessory/requirements.txt`): `requests` 2.32.3 (Apache-2.0) and `pyserial` 3.5 (BSD-3-Clause). They are not included.

## References, no code included

- Kevin Wallace, PM3 research (<https://kevin.wallace.seattle.wa.us/pm3/>) and the pm3.dev workbench (<https://git.doof.net/pm3.dev/>).
- The community wiki at <https://wiki.pm3.dev/>: reader message types and framing, PIC32 registers, scanner and NX notes.
- Code Corporation CR8200-family configuration documentation: illumination commands.

The PM3 tools in `pm3/` were written independently from these protocol descriptions and the author's own traces.

## Original work

The rest of the repository is original work by the project author. **No license has been chosen for it yet.** See the README.
