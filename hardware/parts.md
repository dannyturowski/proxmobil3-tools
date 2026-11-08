# Parts and purchase links

Links checked 2026-11-07. These are ordinary, non-affiliate links. Listings, sellers and variants can change; match the specifications and wiring, not only the product title. No price or stock guarantee is implied.

**Used** means that type of hardware is in the prototype. **Candidate** means a purchasing example, not a tested substitute. Exact seller matches are called out separately.

| Part / quantity | Purpose and selection notes | Purchase / reference | Evidence |
|---|---|---|---|
| INIT PROXmobil3 / 1 | Surplus reader, existing Linux system. This is not a Proxmark3. Access and peripheral variants may differ. | [PM3 research and identification](https://kevin.wallace.seattle.wa.us/pm3/) | Used; no reliable retail source selected |
| Wireless-tag WT32-ETH01 / 1 | ESP32 plus LAN8720A Ethernet, photographed board v1.4. Match this board for the supplied pin mapping. | [Manufacturer documentation](https://wiki.wireless-tag.com/docs/en/WT32-ETH01/board_features.html) | Used; seller not recorded |
| CP2102 USB-to-UART breakout / 1 | 3.3 V UART logic, TX/RX/GND and optional USB 5 V output. Do not substitute an RS-232 voltage-level adapter. | [HiLetgo Amazon example](https://www.amazon.com/dp/B00LODGRV8), [Addicore alternative](https://www.addicore.com/products/usb-to-serial-converter-module-cp2102) | CP2102 used; these exact seller units are **candidates**, not verified as the installed adapter |
| AITRIP VL53L5CX V2 six-pin breakout / 1 | 25 x 10 mm nominal board with mounting ears; green board and black optical cover. Seller calls it VL53L5X/VL53L5CX. | [Exact user-supplied Amazon listing](https://www.amazon.com/dp/B0GSF96HHH) | Used; exact listing supplied by builder |
| Tact switches / 2 | Nominal 6 x 6 x 7 mm total height. Compare body, actuator and pin dimensions with the enclosure. | Generic assortment; exact seller unknown | Used; no exact purchase link established |
| Centre-off momentary SPDT toggle / 1 | (ON)-OFF-(ON), common plus two throws; approximately 12 mm barrel. Ground-only GPIO contact use here. | [DaierTek KN3C-123 family reference](https://www.daierswitches.com/products/15a-250v-momentary-3-way-toggle-switch-with-dustproof-cover) | Owned family; exact installed suffix and keyed-barrel fit remain unverified |
| Ethernet patch cable / 1 | WT32 to PM3 private Ethernet link in the wireless accessory configuration. | Standard Cat5e/Cat6 patch cable | Used |
| USB data cable / as needed | Connects PM3 host USB to the CP2102. Must carry data. | Match the adapter connector | Used |
| Separate regulated 5 V supply / 1 | Powers WT32; prototype uses a battery USB output and a USB power injector while preserving PM3 USB data. | Exact injector model not identified | Used configuration; do not assume an arbitrary USB Y cable isolates power correctly |
| 12 V battery / optional | Prototype enclosure models a nominal 145 x 85 x 28 mm pack with a rocker and USB output. Verify polarity, protection, charging arrangement and actual dimensions. | Exact model not established | Used; no substitute fit claim |
| M2, M2.5, M3, M4 hardware; M6 inserts / enclosure-dependent | See [enclosure notes](enclosure/README.md) for roles and the unresolved M4 grip-length issue. | Match actual head, thread, pilot and engagement requirements | Lengths and insert outside dimensions are not a finalized shopping list |

## Optional bench PoE power

A PoE **injector** puts power onto Ethernet; a **splitter** extracts it and provides a DC output. Neither is the USB power injector used for the WT32.

| Item | Example | Status / limits |
|---|---|---|
| Standards-based PoE source | [TP-Link TL-POE160S PoE+ injector](https://www.omadanetworks.com/us/business-networking/omada-accessory-poe-adapter/tl-poe160s/), or a compatible PoE switch | Candidate; manufacturer specifies 802.3af/at. Match splitter and measured load. |
| 12 V PoE splitter | [REVOTECH PS5712TG on Amazon](https://www.amazon.com/dp/B08HS4NT13) | Candidate. Listing specifies 12 V, up to 2 A, 802.3af/at and 5.5 x 2.1 mm barrel. Available power depends on the upstream PoE budget. Exact installed splitter is not identified. |

Verify the PM3's actual DC inlet/polarity and current requirements before connecting a splitter. The WT32 requires regulated 5 V at its 5V pin; **do not feed a splitter's 12 V into it**. The battery/WT32 wireless configuration and a PM3 wired bench-PoE configuration are different wiring arrangements.

## Electrical references

The CP2102's 3.3 V regulator is not an appropriate WT32 power supply. [SparkFun's CP2102 guide](https://learn.sparkfun.com/tutorials/cp2102-usb-to-serial-converter-hook-up-guide/hardware-layout) describes that output as limited to 100 mA. Use a suitable separate 5 V supply and common ground. A power selector on a breakout does not by itself establish its UART signal voltage.
