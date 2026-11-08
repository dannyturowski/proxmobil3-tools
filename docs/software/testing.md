# Tests and what they do not cover

All tests run on a PC. None needs, or touches, a PM3, a reader module, a scanner or an ESP32.

| Command | What runs |
|---|---|
| `make unit` | `tests/unit/test_pm3.c` under ASan/UBSan, 118 checks: IPP CRCs and framing (resync, corrupt header or payload, oversize, byte-by-byte), the full setup sequence and its acknowledgement rules, status correlation, missing and malformed replies, link silence, card presence (accept, held, cooldown, removal, timeout, rearm abort once per episode, Ultralight, `--no-release`), phone/payment presence and every reason it is ignored, module restart and resync, reader messages without text, the transmit allowlist, PIC32 output parsing, scanner commands and the recorded reply format, the USB identity and busy-port checks against fake sysfs and proc trees. Several checks assert that card bytes and reader text never reach the output |
| `make e2e` | `tests/e2e/test_tools.py`, 10 tests: the real binaries against simulated devices on ptys (`tests/e2e/fakes.py`). A reader runs setup, a card, a held card, absence, a phone and clean exit, checking what the reader received. The NFC and scanner tools refuse a port another process holds. `pm3-pic32-read` runs against fake `NxExe` scripts: success, exit codes, signals, timeouts, bad output and the exact arguments. A scanner covers off, level and get, a silent engine, and refusals. A WT32 covers the handshake, inputs, replies, refused commands, `--until-reply` and a silent device |
| `sh accessory/tests/run_firmware_tests.sh` | The accessory firmware's portable code and PC tools, 3,365 checks in this snapshot, see [wt32-firmware.md](wt32-firmware.md) |
| `sh accessory/tools/build_firmware.sh` | Real ESP32 build plus `verify_build.sh` |

## Not covered

- **Hardware interface testing of the standalone tools is still pending.** The PM3 protocol handling, timings and commands come from the author's private application and scripts on one unit. The simulators reproduce what that unit was seen to do, not every reader firmware.
- All four static ARM binaries ran `--version` on a PM3 after SHA-256-verified transfer. This confirms executable compatibility only; no NFC/scanner/serial ports were opened by that smoke test.
- Reader behaviour with phones and bank cards (`0xD1`) is a heuristic from one unit.
- The scanner commands are for Code CR8200-family engines only.
- Accessory firmware r5.1.3: see *Status and limits* in [wt32-firmware.md](wt32-firmware.md).

To try a tool on a unit that runs something else, for example the author's own application, stop that program first and restart it afterwards. The tools refuse ports that are in use for exactly this reason.
