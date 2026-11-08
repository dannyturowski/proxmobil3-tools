# Provenance of the accessory snapshot

The firmware, shared libraries, simulator, tools and tests under `accessory/` are a snapshot of the author's private accessory repository at **wt32-r5.1.3**. The corresponding private release was subsequently OTA-deployed on the author's unit and marked valid. Home-network, serial-reconnect and ToF checks passed; physical offline/hotspot testing remains pending. The snapshot was taken on 2026-11-07, after the r5.1.3 work and its review fixes had finished and the private tree's own checksum list verified completely.

- 91 files were copied from an explicit list. Each was checked against the private tree's checksum list before and after copying.
- Not copied: deployment scripts, release and rollback binaries, logs, device receipts, the private application bridge and its test suites, and internal documents.
- New here: `host/wt32-monitor.c`, `tools/build_firmware.sh`, `tests/run_firmware_tests.sh`, `tests/testkit.py`, `requirements.txt`, this file.

## Changes made to copied files

| File | Change |
|---|---|
| `firmware/src/main.cpp` | Two comments: an example address replaced by a documentation address, and an application-specific phrase made generic. Code unchanged |
| `shared/nori_link/src/nori_link.h` | One comment on the `trk5`/`trk1` event names made generic. The names stay for wire compatibility |
| `firmware/platformio.ini` | Comment naming the build script |
| `tools/verify_build.sh` | Takes the PlatformIO core from `$PLATFORMIO_CORE_DIR` and the images from an output directory argument. Comparisons with copies of earlier releases replaced by their pinned SHA-256 |
| `tools/gen_sdkconfig_defaults.py` | Default base path follows `$PLATFORMIO_CORE_DIR` instead of a private cache path |
| `tests/test_serial_log.py` | Imports `testkit` instead of the private integration harness |

The rest is byte-for-byte as in the private tree. `wt32-snapshot-SHA256SUMS` in the private task report lists the original hashes.

## Build check

A clean copy of this repository built with `tools/build_firmware.sh`, using an already downloaded PlatformIO core at the pinned versions. `verify_build.sh` passed every check. The image was 988,240 bytes, the same size as the private r5.1.3 release. It differed from that release in 72 bytes: the compile time and date, and the two SHA-256 digests that depend on them.

The images embed the build machine's absolute path to the PlatformIO core, from `__FILE__` in the Arduino core. Review that before you publish any binary.
