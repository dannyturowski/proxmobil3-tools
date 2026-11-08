# wt32-monitor: accessory link monitor

`wt32-monitor` is the host side of the accessory's serial link (protocol 1, `$NL1 <seq> <TYPE> [args]*CRC16`). It shows what the ESP32 accessory reports, as JSON lines on stdout: buttons, lever, presence, heartbeats and command replies. It does not drive any application. Pipe its output wherever you like.

**Validation status:** host-tested against a simulated accessory on a pty. This tool has not been run against a real board. The link code it uses (`accessory/shared`) is the same code the firmware and the author's private bridge run.

## Usage

```sh
wt32-monitor --device usb:2-1.3 --cmd DIAG --until-reply      # one diagnostic snapshot
wt32-monitor --device '/dev/serial/by-id/usb-Silicon_Labs_CP2102_..._<SERIAL>-if00-port0' --heartbeats
wt32-monitor --device usb:2-1.3 --stdin                        # type commands, one per line
```

```json
{"t":0.000,"event":"start","tool":"wt32-monitor","version":"0.1.0","baud":460800}
{"t":0.514,"event":"hello","boot":"1a2b3c4d","fw":"wt32-r5.1.3","sensor":"vl53l5cx","wifi":"up","tun_cap":false}
{"t":0.515,"event":"session","state":"ready"}
{"t":0.530,"event":"reply","status":"OK","what":"DIAG","args":["rx=12","noise=0","crc=0","bad=0","long=0","..."]}
{"t":4.200,"event":"input","name":"b1d","device_ms":834211}
```

| Option | Default | |
|---|---|---|
| `--device PATH\|usb:PORT` | required | The adapter, by path or by USB port (`usb:2-1.3` resolves interface 0's `ttyUSB` under that port only) |
| `--baud N` | 460800 | Must match the firmware |
| `--usb-id VID:PID` | `10c4:ea60` | Required USB identity (CP2102). CH340 adapters are `1a86:7523` |
| `--nfc PATH`, `--scanner PATH` | `/dev/ttymxc3`, none | Devices that must never be opened, even through an alias |
| `--allow-pty` | | Accept a pty (simulator) |
| `--cmd "KIND args"` | | Sent after every HELLO; repeatable up to 8 |
| `--diag-every S` | | `DIAG` every S seconds while connected |
| `--stdin` | | Read commands from stdin |
| `--allow-config` | | Permit commands that change settings (`CFG`, `WIFIEN`, `WIFICLR`, `MODE`, ...) |
| `--heartbeats` | | Print every heartbeat; otherwise only counted |
| `--until-reply` | | Exit after one reply per `--cmd`; exit 5 if none arrives |
| `--duration S` | | Exit after S seconds |

Exit codes: 0 normal, 2 usage, 3 port refused or unusable, 4 I/O error, 5 `--until-reply` timed out.

## Events

| Event | Meaning |
|---|---|
| `hello` / `hello_after_reboot` | Device handshake: boot id, firmware, sensor, Wi-Fi state |
| `reboot` | A heartbeat from a new boot; waiting for its HELLO |
| `session` | `ready`, `waiting-for-hello`, `silent` (nothing for 8 s; the handshake starts over) |
| `input` | `b1d b1u b2d b2u` buttons, `tap dtap` taps, `pon poff` presence, `trk5 trk1` lever moved to the GPIO15 / GPIO2 side, `tsh5 tsh1 tlg5 tlg1` short or long hold of that move |
| `heartbeat` | With `--heartbeats`: device uptime, `wifi`, `ip`, `rssi`, `tun`, `evdrop` |
| `reply` | `OK`/`ERR` with the command name and its arguments |
| `dropped` | A duplicate or stale frame, as the session rules define them |
| `command_sent`, `command_refused`, `command_invalid` | What happened to a command |
| `exit` | Counters: frames, noise, CRC errors, duplicates, stale, reboots |

Useful read-only commands: `DIAG`, `DIAG p=tof`, `DIAG p=net`, `DIAG p=rst`, `DIAG p=ev`, `DIAG p=ph` (r5.1.3 phase record), `IICSCAN`.

## Safety

- Only read-only commands are sent unless you pass `--allow-config`.
- `WIFISET`, `OTAKEY` and the saved-network commands carry credentials. The monitor never sends them, whatever the options. Use the provisioning tools in `accessory/tools/` with a password file: see [wt32-firmware.md](wt32-firmware.md).
- The port is opened with an exclusive lock and with DTR/RTS released and HUPCL cleared. This requests normal operation rather than reset/download mode; verify the reset wiring of your particular adapter.
- The monitor does not relay the accessory's Wi-Fi SSH tunnel.
- One program at a time: if another bridge owns the port, the lock refuses the open.
