#!/bin/sh
# Build the WT32-ETH01 accessory firmware from this checkout and verify the result.
#
#   sh accessory/tools/build_firmware.sh
#
# Output: build/firmware/{firmware.bin,firmware.elf,bootloader.bin,partitions.bin,boot_app0.bin,sdkconfig.h,
#         sdkconfig.wt32,VERIFY.txt} at the repository root.  Nothing is flashed or uploaded.
#
# Environment:
#   PIO                 PlatformIO executable (default: pio, then platformio, from PATH)
#   PM3TOOLS_PIO_CORE   PlatformIO core directory: platform, frameworks, toolchain and the ESP-IDF Python venv.
#                       Default ~/.cache/proxmobil3-tools/pio-core.  Kept apart from ~/.platformio so other
#                       projects' packages cannot change this build.  The first build downloads about 1.5 GB
#                       into it, at the exact versions pinned in accessory/firmware/platformio.ini.
#   PM3TOOLS_FW_OUT     output directory (default build/firmware)
#
# ESP-IDF 4.4's CMake build does not support spaces in paths, so the sources are copied to a scratch
# directory under $TMPDIR (default /tmp) and built there.
set -eu
cd "$(dirname "$0")/.."
ACC=$PWD
REPO=$(cd .. && pwd)
OUT=${PM3TOOLS_FW_OUT:-$REPO/build/firmware}
export PLATFORMIO_CORE_DIR="${PM3TOOLS_PIO_CORE:-$HOME/.cache/proxmobil3-tools/pio-core}"
if [ -z "${PIO:-}" ]; then
    PIO=$(command -v pio || command -v platformio || true)
fi
[ -n "$PIO" ] || { echo "build_firmware: PlatformIO not found; see docs/software/wt32-firmware.md (Bootstrap)" >&2; exit 2; }

W=$(mktemp -d "${TMPDIR:-/tmp}/wt32-fw-XXXXXX")
case "$W$PLATFORMIO_CORE_DIR" in *' '*) echo "build_firmware: paths with spaces are not supported by ESP-IDF 4.4: $W / $PLATFORMIO_CORE_DIR" >&2; exit 2 ;; esac
trap 'rm -rf "$W"' EXIT
export PLATFORMIO_WORKSPACE_DIR="$W/work"
mkdir -p "$W/src" "$PLATFORMIO_CORE_DIR"
cp -R "$ACC/firmware" "$ACC/shared" "$W/src/"
rm -f "$W/src/firmware/sdkconfig.wt32"          # always regenerated from sdkconfig.defaults; compared below

echo "build_firmware: $("$PIO" --version)"
echo "build_firmware: core $PLATFORMIO_CORE_DIR, scratch $W"
"$PIO" run -d "$W/src/firmware" -e wt32

B="$PLATFORMIO_WORKSPACE_DIR/build/wt32"
mkdir -p "$OUT"
cp "$B/firmware.bin" "$B/firmware.elf" "$B/bootloader.bin" "$B/partitions.bin" "$OUT/"
cp "$PLATFORMIO_CORE_DIR/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin" "$OUT/"
cp "$B/config/sdkconfig.h" "$OUT/sdkconfig.h"
cp "$W/src/firmware/sdkconfig.wt32" "$OUT/sdkconfig.wt32"
if cmp -s "$OUT/sdkconfig.wt32" "$ACC/firmware/sdkconfig.wt32"; then
    echo "build_firmware: generated sdkconfig.wt32 matches the committed one"
else
    echo "build_firmware: WARNING generated sdkconfig.wt32 differs from accessory/firmware/sdkconfig.wt32" >&2
fi
sh "$ACC/tools/verify_build.sh" "$B" "$OUT"
