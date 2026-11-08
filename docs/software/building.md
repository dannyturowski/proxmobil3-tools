# Building the tools

Everything builds from a clean checkout with no local caches or older revisions.

## PM3 tools and wt32-monitor

Needs: a C11 compiler, `make`, and Python 3 (standard library only) for the tests. For the reader you also need an ARM hard-float cross compiler. On Debian or Ubuntu that is `sudo apt install build-essential gcc-arm-linux-gnueabihf python3`.

```sh
make            # build/native/: pm3-nfc-presence pm3-pic32-read pm3-scanner-light wt32-monitor
make arm        # build/armhf/: the same, static ARMv7 hard-float (Cortex-A9, VFPv3-D16) for the PM3
make test       # unit tests (ASan/UBSan) and end-to-end tests against simulated devices
make WERROR=1   # warnings as errors, as the checks were run
```

Tested with GCC 13.3 (native and `arm-linux-gnueabihf`) and Python 3.12 on Ubuntu 24.04 under WSL2. The ARM binaries are static, so they need nothing from the reader's libraries. All four binaries also ran their `--version` checks on a PM3. Those checks do not open hardware ports; the standalone tools have not yet been validated against the actual reader/scanner.

## Putting them on a reader

Copy the files from `build/armhf/` to a writable place on the reader, for example over SSH. Then run them from a shell. Nothing is installed as a service. Before running `pm3-nfc-presence`, stop whatever owns the reader UART: see [pm3-nfc-presence.md](pm3-nfc-presence.md). `pm3-scanner-light` belongs at boot, before the barcode program starts: see [examples/scanner-leds-off-at-boot.sh](../../examples/scanner-leds-off-at-boot.sh) and the sample [config](../../config/proxmobil3-tools.conf.example).

Keep a way back. These tools change nothing persistent on the reader, but stopping the stock software can make the board controller power the unit off.

## Accessory firmware

See [wt32-firmware.md](wt32-firmware.md): PlatformIO with pinned packages, built by `accessory/tools/build_firmware.sh`.
