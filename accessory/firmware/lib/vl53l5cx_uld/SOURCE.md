# VL53L5CX ULD (vendored)

| Item | Value |
|---|---|
| Vendor | STMicroelectronics |
| Repository | https://github.com/STMicroelectronics/stm32-vl53l5cx |
| Tag / commit | `v1.0.7` / `1e6b8b35b25d59f2f256d9b6b1280b026132ab85` ("Release v1.0.7") |
| Driver revision | `VL53L5CX_API_REVISION "VL53L5CX_1.3.9"` (changelog: 05/29/2023, "Improved glare filter tuning") |
| License | BSD-3-Clause, full text in `LICENSE-ST.md` (ST changed the ULD from dual to full BSD-3 in 1.3.6, see `changelog-ST.txt`) |
| Fetched | 2026-10-08 with `git clone` into a scratch directory |

Files copied **byte for byte** from `modules/` (SHA-256):

| File | SHA-256 |
|---|---|
| `src/vl53l5cx_api.c` | `4710ba2f0e840340d459d112f668fdf4df41134ad9e5bd28dbe5909c76ce5253` |
| `src/vl53l5cx_api.h` | `0e62695e1b42fcb9aa8237cd6acc5ef76b9ffc49e81ccc90e5c648993c419e52` |
| `src/vl53l5cx_buffers.h` | `86880245e6898709f8478c7cf6b13e387fe9796c2c6326a6f230ed7e55d88b87` |
| `LICENSE-ST.md` (from `LICENSE.md`) | `56f87432a0d96c233fec5a5782221e2a8b67d81d907e4ec0bea67e459e8bb70b` |
| `changelog-ST.txt` (from `modules/changelog.txt`) | `e38876f67d6efb3a6f750e42acbce15274934a3fb4c7194df36519d09ff367c6` |

Not copied: ST's `porting/platform.[ch]` (an STM32 HAL example with a busy-wait `WaitMs`), the BSP
wrapper `vl53l5cx.[ch]` and the xtalk / motion / threshold plugins (unused).

`src/platform.h` and `src/platform.c` are nori's ESP32 port, written for this build. They keep ST's
prototypes and replace the bodies:

- the ESP-IDF 4.4 legacy I2C master driver (Arduino 2.0.17's SDK) on port 0, not Arduino `Wire`
  (`Wire` buffers 128 bytes; the ULD writes 32 KB blocks);
- every block is split into transactions of at most 256 data bytes, with the register index advanced
  (the sensor auto-increments it), each staged through a RAM bounce buffer;
- `WaitMs` is `vTaskDelay` (at least one tick), so polling never busy-waits;
- failures are counted per kind (NACK, bus timeout, other).

The output selection in `platform.h` keeps only distance, target status and the number of targets per
zone (`VL53L5CX_NB_TARGET_PER_ZONE` = 1). The same set of data is what the presence detector reads.

Cross-checks made when vendoring:

- `adafruit/Adafruit VL53L5CX` 1.0.1 (PlatformIO registry) bundles the same revision string, `VL53L5CX_1.3.9`,
  and names this repository as its source.
- `stm32duino/STM32duino VL53L5CX` 1.2.3 bundles the older `VL53L5CX_1.1.2`.
- `sparkfun/SparkFun VL53L5CX Arduino Library` 1.0.3 bundles the older `VL53L5CX_1.1.1`.
