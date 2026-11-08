# pm3-scanner-light: barcode scanner LEDs off

Some PROXmobil3 units have a Code CR8200-family barcode engine behind an FTDI USB serial bridge (115200 8N1). Its illumination LEDs stay on, which is bright and wasteful if you are not scanning. `pm3-scanner-light` sets the illumination level with Code's runtime command and reads it back.

**Validation status:** host-tested against a simulated engine. The command `RDILPMB0` and the readback format come from one real unit and the author's earlier scripts. A boot script sent `RDILPMB0` at every start. A camera experiment read back `MB` 0 after it. Another session's `RDILG` query returned `<RD><IL LO="0" MB="25" AC="0" /></RD>`. This tool itself has not yet talked to a real scanner.

## Usage

```sh
DEV='/dev/serial/by-id/usb-FTDI_FT231X_USB_UART_<SERIAL>-if00-port0'
pm3-scanner-light --device "$DEV" off          # RDILPMB0, then reads the level back
pm3-scanner-light --device "$DEV" level 25
pm3-scanner-light --device "$DEV" get
```

```json
{"event":"scanner_light","action":"set","requested":0,"readback":0,"confirmed":true}
```

| Option | Default | |
|---|---|---|
| `--device PATH` | required | Use the `/dev/serial/by-id/` name. `ttyUSB` numbers can swap between boots |
| `--usb-id VID:PID` | `0403:6015` | Required USB identity (FTDI FT231X) |
| `--any-usb-id` | | Skip the identity check (other bridges, pty tests) |
| `--no-readback` | | Set only |
| `--wait S` | 2 | Readback deadline |
| `--force-shared` | | Open even if another process has the port (not recommended) |

Exit codes: 0 done and confirmed, 1 no readback or a different level, 2 usage, 3 port refused, 4 I/O error.

## Why the checks

- **USB identity.** An ESP32 accessory adapter (CP2102, CH340) is also a `ttyUSB`. Sending scanner commands to it would be harmless text, but an open could toggle its reset lines. The tool refuses any USB identity but the expected one.
- **Port in use.** If a program is already reading barcodes from the port, the readback would be split between it and this tool. Run this first.

## Make it stick

`P` in `RDILPMB` means a runtime setting. It is not saved, and the engine returns to its stored configuration when it loses power. Apply it at every boot, before the program that reads barcodes opens the port. [examples/scanner-leds-off-at-boot.sh](../../examples/scanner-leds-off-at-boot.sh) waits up to 10 s for the device to appear and then does this.

Code's configuration reference also has persistent variants. This toolkit does not use them, so a mistake goes away with a power cycle.

## Not supported

- Units with a Honeywell N5680-family engine. They use a different command set.
- The older community note's `CDOPPMD0`. It is said to turn the lights off by disabling code scanning altogether. This tool does not send it.
