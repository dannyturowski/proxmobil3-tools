# pm3-pic32-read: ambient light and input voltage

The PROXmobil3's PIC32 board controller exposes registers that the stock utility reads with `NxExe prox2 read <addr>`. `pm3-pic32-read` runs that utility, checks its answer strictly, and prints one JSON line per reading. It only reads. It never writes PIC32 registers, and never touches the watchdog, power or backlight.

**Validation status:** host-tested against fake `NxExe` scripts. The command and the output format (`Addr <decimal address>: <decimal value>`) are those the author's private application has used on one unit since October 2026. This tool has not yet read a real PIC32.

## Usage

```sh
pm3-pic32-read ambient
pm3-pic32-read --count 0 --interval 5 ambient input-voltage     # until Ctrl-C
pm3-pic32-read --list
```

```json
{"t":0.000,"event":"pic32_read","register":"ambient","addr":"0x500b","value":48,"elapsed":0.412}
{"t":0.413,"event":"pic32_error","register":"input-voltage","addr":"0x5040","reason":"timeout","elapsed":3.001}
```

| Register | Address | Value |
|---|---|---|
| `ambient` | `0x500b` | Raw light-sensor counts, **not lux**. One bench unit read 0 covered, about 48 in a normal room, about 3273 under a flashlight |
| `input-voltage` | `0x5040` | Supply input in millivolts (one unit: about 12100 on a 12 V supply). Not a battery state of charge |

| Option | Default | |
|---|---|---|
| `--count N` | 1 | Rounds; 0 = until SIGINT/SIGTERM |
| `--interval S` | 5 | Seconds between rounds, at least 1 |
| `--timeout S` | 3 | Per-read deadline; the child is killed when it passes |
| `--nxexe PATH` | `/usr/bin/NxExe` | The stock utility |
| `--workdir DIR` | `/run` | Its working directory |

Error reasons: `spawn`, `timeout`, `exit=N` (`exit=127`: utility missing), `signal=N`, `parse` (unexpected output). Exit code 0 if every read succeeded, 1 otherwise, 2 for usage errors.

## Notes

- Each read starts a process; the author's application reads ambient light no faster than every 5 s. Keep `--interval` at a few seconds.
- Other registers are documented on the [wiki.pm3.dev pic32 page](https://wiki.pm3.dev/pic32): power-on counters, watchdog counters, ignition state, and more. They are not in the table on purpose: only registers that have been read on a real unit are listed. Add more in `pm3/src/pm3_pic32.c` once you have checked them. Keep the list read-only.
- The stock utility also offers toggles: Ethernet PHY and NFC resets, audio amplifier, barcode, USB, WLAN, backlight. They are deliberately not wrapped here.
- Light-based dimming in software (shading the framebuffer, not the backlight) is a simple consumer of `ambient`: map 0..48 raw to a floor..100 % brightness, smooth over a few seconds, and return to full brightness when readings are older than about 12 s.
