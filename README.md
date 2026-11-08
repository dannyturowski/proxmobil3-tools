# proxmobil3-tools

Tools, hardware interfaces, ESP32 connectivity, and enclosure CAD for repurposing INIT PROXmobil3 transit readers.

By whim.ski. Independent community project; not affiliated with INIT or a transit agency.

## Scope

This is an additive toolkit: small Linux tools that run on the reader's existing system, plus an optional ESP32 accessory. It is not a replacement firmware image for the PM3, and it does not initiate payment transactions or expose card contents.

| Part | What it does | Docs |
|---|---|---|
| `pm3-nfc-presence` | Anonymous NFC presence from the contactless reader module: cards, plus phones and payment cards as an anonymous "something was tapped". JSON lines, no card data | [docs/software/pm3-nfc-presence.md](docs/software/pm3-nfc-presence.md) |
| `pm3-pic32-read` | Ambient-light and input-voltage readings from the PIC32 board controller, read-only | [docs/software/pm3-pic32.md](docs/software/pm3-pic32.md) |
| `pm3-scanner-light` | Barcode-scanner illumination LEDs off, or set to a level, with readback | [docs/software/pm3-scanner-light.md](docs/software/pm3-scanner-light.md) |
| `wt32-monitor` | JSON monitor for the ESP32 accessory's serial link | [docs/software/wt32-monitor.md](docs/software/wt32-monitor.md) |
| `accessory/` | WT32-ETH01 firmware: Wi-Fi/Ethernet connectivity, ToF sensing, buttons and lever, diagnostics, OTA. Also its PC tools and host tests | [docs/software/wt32-firmware.md](docs/software/wt32-firmware.md) |
| `hardware/`, `docs/photos/` | Enclosure, wiring, parts and photos | [hardware/](hardware/) |

TransitGotchi and its games, artwork, and audio remain private and are not part of this repository.

## Quick start

```sh
make && make test          # native build and host tests, no hardware needed
make arm                   # static ARM binaries for the reader, in build/armhf/
```

Building, installing on a reader and the firmware bootstrap are in [docs/software/building.md](docs/software/building.md). What the tests cover, and what they do not, is in [docs/software/testing.md](docs/software/testing.md).

## Status

Initial public source snapshot. Native tools, static ARM tools and the WT32 firmware build from a clean source copy; host tests pass. All four ARM utilities ran their version checks on a PM3. Their standalone hardware interfaces still need validation. The corresponding WT32 r5.1.3 private build was deployed and marked valid, with Wi-Fi, PM3 Ethernet, serial reconnect and live ToF checked. Offline/hotspot recovery remains a physical test to do.

The enclosure rebuilt as six valid solids, and its STL meshes passed closed-mesh checks. Read the [known fit issues](hardware/enclosure/README.md#known-fit-issues-and-checks-before-printing), especially the unresolved corner screw length; digital checks do not prove assembly fit.

- [Parts and Amazon/manufacturer links](hardware/parts.md)
- [Wiring](hardware/wiring.md)
- [Editable enclosure CAD and printable parts](hardware/enclosure/README.md)
- [Actual prototype photos](docs/photos/README.md)

## License

No license has been chosen yet for the original work in this repository. Third-party components keep their own licenses: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## References and acknowledgments

Kevin Wallace's [PM3 research](https://kevin.wallace.seattle.wa.us/pm3/) and [pm3.dev workbench](https://git.doof.net/pm3.dev/) are related upstream resources, as is the community wiki at [wiki.pm3.dev](https://wiki.pm3.dev/). This project is not presented as a fork of that Go workbench and contains no code from it. Any reused third-party code must retain its license and attribution.
