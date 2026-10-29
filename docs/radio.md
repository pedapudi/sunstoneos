# Radio implementation dependencies

SunstoneOS implements native AIC8800 SDIO and firmware transport, Wi-Fi station
and network paths, Bluetooth HCI/pairing, headset audio and bounded A2DP capture.
These paths have native builds and software checks. Physical radio operation
remains unqualified. Fitted supply routing and the reset/LPO pin function still
need evidence for these boards. HCI is the command and packet interface between
a Bluetooth host stack and its controller.

This document is for driver developers. It identifies the board wiring,
firmware protocol, host responsibilities, and remaining evidence needed before
radio bring-up. Downloaded source and firmware stay outside Git.

## Published sources identify an AIC8800 path; the installed chip is unconfirmed

Schematic V1.2, sheet 14, labels module U5 `CDW-20800D8-00`. That module name
and its generic pin symbol do not establish the installed silicon revision.
The [board build configuration][board-build] loads `aic8800_fdrv` and
`aic8800_btlpm`. Its [A733 kernel configuration][kernel-config] enables the
AIC SDIO interface, Classic Bluetooth, BLE, and HCI UART H4 support.
The [pinned SDIO match table][sdio] distinguishes these boot identities:

| SDIO manufacturer:device | Source chip identity | Relevance |
| --- | --- | --- |
| `544a:0146` | AIC8800D | Different supported chip; do not select D80 firmware |
| `c8a1:c18d` | AIC8800DC | Different supported chip with a separate function-2 path |
| `c8a1:0182` | AIC8800D80 | Concrete candidate for the board's D8 module |

The D80 loader accepts U01, U02, U03, and D80 U04 revisions, plus an H variant
flag. Confirm the SDIO identity, revision, H flag, and subrevision on the
attached board before choosing images. The published build and module marking
support the D80 hypothesis; they do not constitute that confirmation.

## The radio shares power, SDIO bootstrap, and a PCM audio connection

The [schematic][schematic], sheets 9 and 14, and [board device tree][dts]
provide the following routes. Directions use the A733 as the host.

| Function | A733 route | Module route or control |
| --- | --- | --- |
| SDIO clock, command | PG0, PG1; SDC1 | `WL-SDIO-CLK`, `WL-SDIO-CMD` |
| SDIO data 0–3 | PG2–PG5; SDC1 | `WL-SDIO-D0` through `WL-SDIO-D3` |
| UART transmit, receive | PG6, PG7; UART1 | Module RX, TX respectively |
| UART flow control | PG8 RTS, PG9 CTS | Module CTS, RTS respectively |
| Wi-Fi enable | PM1, active high in device tree | `WL-REG-ON`, symbol `WL_REG_ON` |
| Wi-Fi wake interrupt | PM0, active high | `WL-WAKE-AP`, symbol `WL_HOST_WAKE` |
| Bluetooth reset | PM2, active low in device tree | `AP_WAKE_WL` net, symbol `BT_RST_N` through R61 |
| Host wakes Bluetooth | PM3, active high | `AP-WAKE-BT`, symbol `BT_WAKE` |
| Bluetooth wakes host | PM4, active high | `BT-WAKE-AP`, symbol `BT_HOST_WAKE` |
| PCM bit/frame clocks | PG11, PG12; I2S1 | `PCM_CLK`, `PCM_SYNC` |
| PCM transmit/receive | PG13 output, PG14 input | Module `PCM_IN`, `PCM_OUT` |

The [SoC device tree][soc-dts] places SDC1 at `0x04021000` and UART1 at
`0x02501000`. Controller clocks, resets, interrupts, DMA, and PG/PM pin
configuration need native bindings. Existing console access does not qualify
UART1 flow control or radio transport.

The schematic supplies module VBAT from `VCC-3V3-WIFI` and its I/O from
`VCCIO_1V8_WIFI`. The device tree names `axp8191-bldo5` and `axp8191-cldo1`
as 1.8 V radio supplies, with comments identifying PG/PM I/O power.
These descriptions concern different supply paths; a driver must preserve
shared I/O rails and coordinate Wi-Fi and Bluetooth power references.

PM2 is particularly ambiguous: the schematic calls its net `AP_WAKE_WL`,
connects it to the symbol's Bluetooth reset, and also routes it through R1130
to the symbol's `LPO` pin. The nearby `AP-CLK32K-OUT` label does not establish
a connected 32 kHz input. Treat reset polarity, pin-symbol interpretation,
and any low-frequency clock requirement as board-validation dependencies.

The device tree enables SDC1 with four data lines, SDIO interrupts, retained
power during suspend, and timing capabilities up to 208 MHz. Those are Linux
configuration settings. They do not establish SunstoneOS performance or a safe
initial clock choice. Start enumeration at the SDIO initialization rate and qualify
faster modes later.

I2S1 has board pin definitions but its platform and sound-card nodes are
disabled. The card describes DSP_A framing, CPU clock ownership, two 16-bit
slots, and inverted bit clock. Its selected data-pin indices also need
reconciliation with the wired DIN0/DOUT0 pins. Wiring alone does not prove a
usable Bluetooth PCM configuration.

## Bluetooth patch loading precedes the UART host stack

The [D80 bootstrap][bootstrap] uses the shared SDIO command channel to load
Bluetooth data and patch code before Wi-Fi firmware startup. A Bluetooth-only
application therefore still needs the radio bootstrap and SDIO host path.
The Linux probe waits for function 2 to enumerate, then uses function 1 for
D80 bootstrap traffic. Its separate function-2 data handling applies to DC;
copying that path into the D80 driver would be incorrect.

The [transport source][sdio] initializes function 1 with 512-byte blocks,
enables it, and programs chip-specific function-0 registers `0xf2` and,
conditionally, `0xf1`. D80 uses its V3 register map: interrupt enable at
`0x00`, pending at `0x01`, device wake at `0x02`, queue credit at `0x03`,
receive FIFO at `0x0f`, and transmit FIFO at `0x10`. Receive length decoding,
byte/block transfer padding, interrupt acknowledgement, and credit limits
must follow the source; a raw FIFO write is insufficient.

The [command serializer][commands] uses a four-byte transport header,
a four-byte dummy word, and little-endian 16-bit message ID, destination,
source, and payload length fields. D80 adds CRC-8 over the first three header
bytes. Firmware memory uploads use confirmed block-write commands with at
most 1,024 payload bytes per chunk. The response decoder must validate lengths
and correlate the expected confirmation before reusing a command buffer.

The source performs a memory write of `0x40` to `0x40500058` before reading
identity from `0x40500000` and subrevision from `0x00000004`. Consequently,
the extended identity sequence is not a hardware read-only probe. SDIO card
identification can precede that sequence; chip setup writes require a qualified
recovery path and explicit inclusion in the coordinator's board test.

After identity selection, bootstrap applies system settings, loads Bluetooth
ADID data and patch code, applies the patch table, loads Wi-Fi firmware at
`0x00120000`, configures its patch pointers, and starts the application.
Every device address in these artifacts belongs to the radio controller's
address space. It is not an A733 virtual or physical address.

The firmware may request binding verification. The [published verifier][verify]
checks a 16-byte challenge and computes a 16-byte response using SHA-256 and
byte transformations. It also requires a successful SoC-ID callback, although
the returned ID bytes do not enter the response calculation in this source.
Implement and validate this protocol if requested; returning success without
a response does not establish a working controller.

For SDIO builds, Bluetooth defaults are UART at 1,500,000 baud, RTS/CTS flow
control, low-power signaling enabled, and shared-antenna coexistence mode.
The patch-table mode fields are overwritten with these settings.
`aichw.conf` can override them; its load failure leaves the source defaults.
UART H4 is the first transport candidate, supported by the published kernel
configuration. The inspected build files do not establish a board-specific
attach command, so verify framing and controller responses before claiming HCI
support. The unrelated AW859A startup script is not evidence for this module.

## Published firmware covers a subset of the loader's revisions

The [build firmware stage][build-firmware] overlays the Orange Pi firmware
repository onto its base Linux firmware package. The inspected overlay is
pinned at `dc163b0ed2e0272d9ee31977a0e0a04884f0ae61`.
All names below are relative to its [aic8800d80 directory][firmware].

| Loader selection | Bluetooth artifacts | Wi-Fi image | Overlay availability |
| --- | --- | --- | --- |
| Non-H U01 | `fw_adid_8800d80.bin`, `fw_patch_8800d80.bin`, `fw_patch_table_8800d80.bin` | `fmacfw_8800d80.bin` | Absent |
| Non-H U02/U03 | Corresponding `_u02.bin` files | `fmacfw_8800d80_u02.bin` | Present |
| H flag, accepted revision | Same `_u02.bin` Bluetooth files | `fmacfw_8800d80_h_u02.bin` | Present |
| Non-H D80 U04 | Corresponding `_u04.bin` Bluetooth files | `fmacfw_8800d80_u02.bin` | Bluetooth files absent |

Some tables can name extra patch images as
`fw_patch_8800d80_u02_ext<ID>.bin` or the U04 equivalent. None are present
in this overlay. The inspected U02 table has no extension requirement.
The overlay also lacks `aichw.conf`; its `aic_userconfig_8800d80.txt` is a
different Wi-Fi configuration artifact. The extra `fmacfwbt_8800d80_u02.bin`
image is not selected by this pinned D80 loader. Neither that image nor the
RF-test image should substitute for the selected normal firmware.

The overlay has no license file or radio-specific redistribution grant in
its inspected tree; its README only identifies it as Orange Pi firmware.
The Linux driver declares GPL module licensing, which does not establish
license terms for separate firmware binaries. Artifact availability and
redistribution rights remain separate dependencies. No firmware is added to
this repository. Determine applicable firmware terms before distributing OS
images containing it.

### The inspected U02 table overrides the default patch address

The Bluetooth table begins with a 16-byte tag area. Each record contains a
16-byte name, little-endian type and pair count, and that many eight-byte
address/value pairs. The first information record can override image addresses
and declare extension images. Mode records require nine writable values;
version records contain text instead of writes.

The published table has seven records, with types 0 through 6 and pair counts
4, 2,793, 48, 9, 3, 23, and 8. It selects ADID destination `0x00201940` and
patch destination `0x001e0000`, overriding the source's U02 patch default of
`0x0020b43c`. Its version text identifies July 11, 2024 and source commit
`73d2ce5`. These facts identify the artifact; they do not prove compatibility
with the attached controller.

[tools/inspect-radio-patch.py](../tools/inspect-radio-patch.py) validates
record bounds, mode payload capacity, and information-record extension counts.
It reports hashes and addresses without executing writes. Unknown record types
are retained as unknown; unspecified padding is accepted. Structural validation
does not authorize the listed target addresses or establish firmware integrity
against a trusted signing key.

```sh
python3 tools/inspect-radio-patch.py /path/to/fw_patch_table_8800d80_u02.bin \
  --sha256 5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60
```

## Native host software must supply authentication and Bluetooth profiles

The Linux Wi-Fi driver uses its own firmware messages alongside Linux networking
and wireless configuration APIs. Its [message definitions][messages] and
[request construction][wifi-commands] cover scan, connect, disconnect,
key installation, and external authentication. A FreeRTOS implementation needs
bounded message ownership, receive-buffer validation, transmit completion and
credit handling, link state, Wi-Fi authentication, and an Ethernet-frame adapter.
A TCP/IP stack alone does not provide those functions.

| Component | Available primary source | Integration consequence |
| --- | --- | --- |
| TCP/IP | [FreeRTOS+TCP][tcp], MIT | Natural first IP-stack candidate; requires a native network interface and buffer ownership rules |
| Alternative TCP/IP | [lwIP][lwip], BSD-style license | Portable Ethernet interface; requires an OS abstraction and the same radio/authentication work |
| Wi-Fi authentication | [wpa_supplicant 2.12][wpa], BSD license | Port the driver API, event loop, timers, entropy, crypto and EAPOL packet transport; Linux nl80211 is not available |
| BLE host | [Apache NimBLE][nimble], Apache-2.0 | Includes a FreeRTOS abstraction; BLE coverage does not implement Classic headset profiles |
| Dual-mode Bluetooth host and profiles | [BlueKitchen BTstack][btstack] | Includes a FreeRTOS run loop, UART H4 transport, and HFP audio-gateway code; its default license restricts use to personal, noncommercial purposes |

These candidates have not been adopted as dependencies. BTstack's
[license][btstack-license] offers separate commercial licensing. A product-wide
stack choice must resolve those terms or provide a different Classic Bluetooth
implementation. The Linux radio driver supplies protocol details but requires substantial
OS adaptation before it can run under FreeRTOS.

For an ordinary Bluetooth headset microphone, SunstoneOS needs the hands-free
profile's audio-gateway role, pairing and bond storage, SDP service discovery,
RFCOMM control, and an SCO/eSCO voice connection. HCI ACL traffic or BLE GATT
alone cannot capture headset audio. Determine whether this firmware exposes
voice packets over UART or requires the wired PCM bus, including its routing
command and clock/framing settings. Wideband voice also requires codec
negotiation and an appropriate mSBC path. LE Audio is a separate feature set;
the board's advertised Bluetooth version does not establish its support.

## First implementation should stop at confirmed radio identity

Implement a bounded SDIO transport and D80 identity worker before importing a
network stack. Use one radio owner for the shared enable/reset lines, SDIO
bootstrap, and Wi-Fi/Bluetooth power references. Expose status and a retained
terminal error; do not automatically reset or re-upload after transport loss.

1. Bind SDC1 clocks, reset, 1.8 V pins and power controls. Implement SDIO
   enumeration with CMD5, CMD3 and CMD7, plus bounded reads of common/function
   registers and card information tuples. Reject unexpected function counts or
   identities before chip-specific operations.
2. Add CMD52 register operations and CMD53 fixed-address FIFO transfers, with
   explicit byte/block lengths, bounded buffers, interrupt handling, and
   monotonic timeouts. Select the D80 register map only after its SDIO ID matches.
3. Implement the command header/CRC, memory-read request, and confirmation
   decoder. Include the reference's required setup writes in a separate,
   reviewable identity sequence. Retain the observed revision and H flag;
   stop without loading images.
4. Check one scripted identity exchange, one malformed/incorrect confirmation,
   and one transport timeout with retained state on the host. Run a single
   coordinator-owned board identity check after watchdog recovery and SDC1
   bindings are qualified. Reuse those checks until relevant behavior changes.

The following implementation can then consume matched, hashed firmware from
immutable byte ranges; no runtime filesystem is required. It must validate all
patch-table writes and image bounds before publication, retain transfer state,
and confirm each command. Firmware startup should precede passive capability
queries, scan/connect/authentication, TCP/IP, Bluetooth discovery/pairing, and
headset audio. Power management, coexistence, and all supported radio modes
remain hardware-support work even when mimus uses another input source.

The [native SDIO identity application](../apps/radio-sdio-id/README.md) already
connects the controller, card, radio platform and shared providers. It retains
resources on success and failure; reusable shutdown is unqualified. Its
[return design](../apps/radio-sdio-id/RETURNING.md) records the fitted-supply and
PM2 reset/LPO uncertainties and missing disposal proof. Independent watchdog
qualification is not a prerequisite for unrelated work. Radio performance
measurements remain deferred until functional hardware integration.

## Source and artifact identities

The board kernel revision is `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
Kernel paths below are under `bsp/drivers/net/wireless/aic8800/`, except the
device trees. The board build revision is
`a95129bf30ea11886cc35f08ebb3e69695a26656`. SHA-256 values identify inspected
bytes, including artifacts whose compatibility remains unconfirmed.

| Source | SHA-256 |
| --- | --- |
| Schematic V1.2 PDF | `4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0` |
| `sun60i-a733-orangepi-zero3w.dts` | `31145367f604fd756f2c6a67447e5134e4be0ad5fae87672cfaa9b7c6d055a39` |
| `sun60iw2p1.dtsi` | `1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa` |
| Build `external/config/boards/orangepizero3w.conf` | `fe2083e66ee1d852b1c201a8f6d3c0987c1ccada865104aa1c95bb538641b13c` |
| Build `external/config/kernel/linux-sun60iw2-current-a733.config` | `ae50f80a9f46cf06adfd79aa5c754809fc9c5f59d3ded9863ebe0dc783b1e3dc` |
| Build `scripts/compilation.sh` | `580bd859798b57e59c5a17c989577828ed3bd3cb7d8d64e31720d4ed6b084dcb` |
| `aic8800_bsp/aicsdio.c` | `f08bcfa63f9b7226f4b26832ee9b6088c057fd5ee06389f3ad872f13f9246f0c` |
| `aic8800_bsp/aicsdio.h` | `af520385376df1843a6e24c01cc7128a151b148fea18d73c0d20f1a345bc6571` |
| `aic8800_bsp/aic_bsp_8800d80.c` | `fcf647395a7f007f22cd19f4e15dcc821e924da55b72528395a9972e5c6290e8` |
| `aic8800_bsp/aic_bsp_driver.c` | `43439d69cd1c3f537ee90f50d4bc52d2a54379f41092f625ded080daacf7de6e` |
| `aic8800_bsp/aic_bsp_driver.h` | `9f72ebdd743ac0af134df50ebc36ea25876b9a361a89b82e17a4a8c2e4d918b3` |
| `aic8800_bsp/aic_bsp_verify.c` | `76ccd57c0ddb2f2c5b074575f709d4816a0dd7a0fbb3ba9cfa763d69d1b76ed1` |
| `aic8800_btlpm/aic8800_btlpm.c` | `f802d8e4004c216478b4fa54519bab9c600ed32e12889e8ba6dd3a06ce26b1a6` |
| `aic8800_fdrv/lmac_msg.h` | `916df5d5a2346a6504b35bad2fc221233e959a4a68de3d1bd61b1df6dfaf12d3` |
| `aic8800_fdrv/rwnx_msg_tx.c` | `755c5c635cbf157f0f5363550ca1023c7f05eeca1b0cb2e0db53dcf9d38dbd91` |

Firmware paths are relative to `aic8800d80/` at the overlay revision above.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `fw_adid_8800d80_u02.bin` | 1,708 | `a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56` |
| `fw_patch_8800d80_u02.bin` | 24,744 | `4ad4eede4d347c45ed61b23508e7843dc884f0e411bf957f95410001d0cfc4d0` |
| `fw_patch_table_8800d80_u02.bin` | 23,288 | `5ff184105fdc4c4679e026418f4da293efec8df02a3a3ca22cee723c17df4a60` |
| `fmacfw_8800d80_u02.bin` | 341,212 | `55b5a20865aca7cd2edaae1733a36da45d2b0b29060bc243d8e9ec212e139f6c` |
| `fmacfw_8800d80_h_u02.bin` | 341,260 | `ae8d548df96c132c445f888b91eab5ef03c19991b8bdef4fc12ebfcbf9f953e7` |
| `aic_userconfig_8800d80.txt` | 2,683 | `995dbb9e4f2e7b19db838291f52be39af9ab673ad51b270c988216ccff1a720e` |

Candidate stack revisions and inspected license-file hashes:

| Candidate | Revision | License SHA-256 |
| --- | --- | --- |
| FreeRTOS+TCP | `b86ad086e9870099bb0ac18a75dcb7f6dd508cc7` | `508a77d2e7b51d98adeed32648ad124b7b30241a8e70b2e72c99f92d8e5874d1` |
| lwIP | `d08f4773edd0182b7910fc8f046eed82ffcd67c9` | `ef4aac92e05e87cd1cdc140870ed52206ba03d4a7fe46c1e11d7ffa6c87d252b` |
| Apache NimBLE | `5d1d5bcbc0d7f67b8a46c20512d5cb6db51a3d04` | `4398a18aeb845a13a0aec2ad41695a21e45ac4e3ae02d732d1c29338258cc500` |
| BTstack | `e38553977a25fb0b55b383c72c289be0975f422c` | `10aa5f952979a9fa2f58bf63c1390c9795b41f8eca405ab975ab926e4d14fc5a` |
| wpa_supplicant | Release 2.12, `COPYING` | `a87ac4e333d0f120408a9d814e40c3672cd27f365af89b4f2f6631f7a9338953` |

The [wpa_supplicant release archive][wpa-archive] SHA-256 is
`08e23937e16d0155e55cab2b51f51fbe10d80a1aa91c4e15442645059b737ef6`.
Its README contains the BSD terms; that file's SHA-256 is
`29631e03453491eeefe2ae894b8af47d7e38e64ef3495837e7b8529c6bbf2e2f`.

Host evidence is in `~/.cache/sunstoneos/evidence/radio-20261016/`.
The focused [check](../checks/radio-patch.py) passed against the published
table, six malformed structures, an extended information record, unknown-type
retention, and unspecified padding. `results.json` records tool/check hashes;
`published-table.json` records decoded artifact metadata. Source manifests and
cached downloads are in `~/.cache/sunstoneos/sources/radio/`.

[schematic]: https://drive.google.com/file/d/1layhtSvMpQo-z_iMJjNfNVvhPhObpsvW/view
[board-build]: https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/external/config/boards/orangepizero3w.conf
[kernel-config]: https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/external/config/kernel/linux-sun60iw2-current-a733.config
[build-firmware]: https://github.com/orangepi-xunlong/orangepi-build/blob/a95129bf30ea11886cc35f08ebb3e69695a26656/scripts/compilation.sh
[dts]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60i-a733-orangepi-zero3w.dts
[soc-dts]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi
[sdio]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aicsdio.c
[bootstrap]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_8800d80.c
[commands]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_driver.c
[verify]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_bsp/aic_bsp_verify.c
[messages]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/lmac_msg.h
[wifi-commands]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/rwnx_msg_tx.c
[firmware]: https://github.com/orangepi-xunlong/firmware/tree/dc163b0ed2e0272d9ee31977a0e0a04884f0ae61/aic8800d80
[tcp]: https://github.com/FreeRTOS/FreeRTOS-Plus-TCP/tree/b86ad086e9870099bb0ac18a75dcb7f6dd508cc7
[lwip]: https://github.com/lwip-tcpip/lwip/tree/d08f4773edd0182b7910fc8f046eed82ffcd67c9
[nimble]: https://github.com/apache/mynewt-nimble/tree/5d1d5bcbc0d7f67b8a46c20512d5cb6db51a3d04
[btstack]: https://github.com/bluekitchen/btstack/tree/e38553977a25fb0b55b383c72c289be0975f422c
[btstack-license]: https://github.com/bluekitchen/btstack/blob/e38553977a25fb0b55b383c72c289be0975f422c/LICENSE
[wpa]: https://w1.fi/wpa_supplicant/
[wpa-archive]: https://w1.fi/releases/wpa_supplicant-2.12.tar.gz
