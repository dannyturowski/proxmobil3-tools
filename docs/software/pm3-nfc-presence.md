# pm3-nfc-presence: anonymous NFC presence

`pm3-nfc-presence` talks to the PROXmobil3's contactless reader module on its UART and reports, as JSON lines, *that* something was presented and when it left. It never reports *what*: no UIDs, no card contents, no reader log text.

**Validation status:** host-tested only, against a simulated reader (see [testing.md](testing.md)). This standalone tool has not yet been run against a reader module. The protocol handling is the same as the author's private application, which runs on one unit. Treat the timings below as starting values.

## Before you run it

Only one program can own the reader UART. The tool refuses to open the port while any other process has it open, and says which one. Stop that program first:

- On a stock unit the reader belongs to the stock software. The community notes on [wiki.pm3.dev](https://wiki.pm3.dev/) describe how to stop it (`systemctl stop nx`). They warn that the PIC32 board controller powers the unit off about 60 s later unless its watchdog is also disabled (`NxExe watchdog 0`). This toolkit never changes the watchdog. Whether and how you do that is your decision.
- On a unit running your own application, stop that application.

`--force-shared` skips this check. Do not use it while another program is talking to the reader: two hosts on one UART corrupt each other's sessions.

## Usage

```sh
pm3-nfc-presence                          # /dev/ttymxc3, ISO + DESFire + Ultralight, phone presence on
pm3-nfc-presence --duration 120 --health-every 10 > nfc.jsonl
pm3-nfc-presence --no-phone --funcs 7     # DESFire only, as in Kevin Wallace's ORCA replica
```

| Option | Default | Meaning |
|---|---|---|
| `--device PATH` | `/dev/ttymxc3` | Reader UART (115200 8N1) |
| `--funcs LIST` | `6,7,10` | Card functions to enable: 6 ISO 14443-4, 7 DESFire, 10 Ultralight/NTAG |
| `--no-phone` | | Do not report `0xD1` frames as phone/payment presence |
| `--no-release` | | Never send a card release (`0x32`) after a card report |
| `--no-rearm-abort` | | Never send abort card handling (`0x46`) after a card presence ends |
| `--frames` | | One metadata line per received frame: type, length, sequence numbers. Never payload |
| `--health-every S` | 30 | Health line interval; 0 = only at exit |
| `--duration S` | run until signal | Exit after S seconds |

SIGINT, SIGTERM or SIGHUP end it cleanly. On exit it sends abort, then release, so the next program finds the reader in its usual state.

Exit codes: 0 normal end, 2 usage, 3 port busy or unusable, 4 I/O error.

## What it sends

Only these message types can be transmitted (enforced in `pm3_nfc_send`):

| Type | Name | When |
|---|---|---|
| `0x46` | abort card handling | Setup; once per card presence after it ends (unless `--no-rearm-abort`); exit |
| `0x32` | card release | Setup; once per card presence, answering the report (unless `--no-release`); exit |
| `0x02` | version request | Setup |
| `0x04` | status request | Setup, then every 5 s |
| `0xE4` | function enable `00 <fn> 01 01` | Setup, once per function until acknowledged |

No reset (`0x10`), no APDUs, no EMV or payment commands, no LED or buzzer commands. Nothing it sends is persistent as far as is known. One community note says the reader remembers enabled functions across restarts; this tool only enables.

Setup runs after 1.2 s of listening, and again whenever the module announces a restart (`0x0F`). A cold module can take about a minute before it acknowledges enables. The tool keeps retrying: every second for the first ten tries, then every 4 s.

## Output

Every line is one JSON object with `t` (seconds since start, monotonic) and `event`. The `start` line also carries the UTC wall time.

| Event | Fields | Meaning |
|---|---|---|
| `start` | `tool version utc funcs phone_presence release_cards rearm_abort shared` | Configuration |
| `setup` | `stage` | `listen`, `abort`, `release`, `probe`, `enable` |
| `function_enabled` / `function_rejected` | `function tries` / `function status` | Enable acknowledged (`0000`) or refused |
| `ready` | `elapsed setups resync` | All functions acknowledged |
| `presence` | `kind frame type outcome since_last_s` or `quiet_s` | Something was presented, see below |
| `absent` | `kind reason present_s` | `removal`, `timeout` or `module-restart` |
| `phone_ignored` | `reason` | A phone/payment frame that did not count, see below |
| `rearm_abort` | `count` | Abort sent after a card presence ended |
| `status` | `code late` | First status reply, then only changes. `0000` is normal |
| `status_missing` / `status_restored` | `consecutive heartbeats` / `missed` | Three status requests in a row went unanswered / answered again |
| `status_malformed` | `len` | A status reply that was not exactly 2 bytes |
| `module_startup` | `count` | The module restarted itself; setup runs again |
| `reader_message` | `level len format text` | A reader log frame (`0xED`). `text` is always `"omitted"` |
| `version_reply` | `len` | Answer to the version request (content not printed) |
| `link` | `state since_s` | `silent` after 10 s without any valid frame, `alive` again |
| `parse_error` | `bad_header bad_crc oversize` | Line noise or corruption; the parser resynchronises |
| `health` | counters | Link, configured, command path, presence counters |
| `frame` | `type name len seq reply_to` | Only with `--frames` |
| `exit` | `code` | Last line |

### Cards

A card report is `0xB1` ISO read, `0xB9` DESFire read, `0xBE` PICC read, or an unsolicited `0xD5` Ultralight report with first byte 0. While a card stays in the field the reader repeats its report about every 0.2 s.

- The first report of a presence is `accepted`. If it comes less than 0.35 s after the previous accepted one, it is `cooldown` instead.
- Repeats are `held`. They are counted in `health`, not printed.
- The presence ends 0.3 s after a removal frame (`0xBB`, or `0xD5` with first byte 1) with no new report: `absent reason=removal`. Otherwise it ends after 3 s without a report: `absent reason=timeout`.
- Within 3 s after a module restart, a report is `resync`. It is most likely the card that was already there.

Some readers stopped reporting after the first card until they were re-armed. In the author's tests the reliable fix was one `0x46` after the card left, which is what the rearm abort does. See *Known limits*.

### Phones and payment cards

Bank cards and phone wallets go through the module's own EMV entry point and are not handed to the host as card reads. What the host does see is an unsolicited `0xD1` "EMV status" frame, 63 bytes long, with replyTo 0. The tool reports such a frame as `kind=phone_or_payment`. This is a heuristic based on what one reader did. It cannot tell a phone from a bank card, and it does not prove anything about the payment application.

| `phone_ignored` reason | Why |
|---|---|
| `setup` | Setup is still running |
| `startup-grace` | Within 3 s of setup completing |
| `resync` | Within 3 s of a module restart |
| `abort-grace` | Within 1.5 s of the tool's own abort |
| `recent-card` | Within 0.4 s of a card report: the EMV entry point also looked at that card |
| `cooldown` | Within 0.4 s of the last accepted presence |
| `burst` | Within 0.4 s of the previous `0xD1`: the same phone, still held. Printed only with `--frames` |

A `0xD1` of any other length, or with replyTo set, is never presence.

The tool never reads, stores or prints the content of these frames and sends nothing in reply. It does not interact with payment in any way. Do not use it for anything that matters financially.

## Health line

```json
{"t":30.0,"event":"health","link":"alive","configured":true,"stage":"ready","cmd":"verified", ...}
```

- `link`: any valid frame within 10 s. Heartbeats (`0x07`, about every 2-3 s) keep it alive.
- `configured`: every enable acknowledged since the last setup.
- `cmd`: `verified` if a status reply matched one of our requests within the last 15 s, otherwise `stale` or `unknown`.
- No card presented for a long time is normal. Nothing the reader offers proves that detection works, so the tool never treats quiet as a fault.

## Known limits

- **Not run on hardware in this form.** Deadlines and gaps come from one unit's traces.
- The tool does not attempt recovery beyond re-running setup after a module restart. If `cmd` goes `stale` with the link alive, restart the tool. If the link goes `silent`, check the module's power.
- Whether the rearm abort is needed, or harmful, on other reader firmware is not known. `--no-rearm-abort` turns it off.
- Reader firmware with a different `0xD1` behaviour gives wrong phone results. Use `--no-phone`.
- A new tap within the 3 s resync window after a module restart is dropped, by design.

## Protocol references

- [wiki.pm3.dev](https://wiki.pm3.dev/), the cvend and pic32 pages: message types and framing.
- Kevin Wallace, [PM3 research](https://kevin.wallace.seattle.wa.us/pm3/) and the [pm3.dev workbench](https://git.doof.net/pm3.dev/).

This implementation was written independently. No code from those projects is included.
