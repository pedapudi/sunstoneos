# The radio service runs in stages and records the stage it reached

Status: **the platform stages, the guard and the result record are host-tested.
The firmware, scan and association stages, the returning lifetime and the host
launch path are open.** No board has run any of it.

This document is for driver developers and the board coordinator. It states how
the shipped Wi-Fi service in the OS image behaves, from supply power-up to
association, and what it reports to the host.

## The stages run in order and each one is reported

| Stage | Reached when | Facts recorded |
| --- | --- | --- |
| `rails-observed` | DCDC1, BLDO5, CLDO1 and, in the first-run guard, DCDC8 are read through the PMIC provider | Enable byte, selector and decoded voltage of each |
| `guard-passed` | The first-run guard (below) holds | `GPIO_POW_MOD_SEL`, `GPIO_POW_VAL`, `SGPIO_POW_MOD_SEL`, `SGPIO_POW_VAL` |
| `powered` | BLDO5 is on at 1.8 V, read back, and the 10 ms and PM1 steps of the [vendor sequence](radio-vendor-power-sequence.md) are done | Whether BLDO5 was powered on by this run |
| `enumerated` | CMD5, CMD3, CMD7 and the CCCR and tuple reads complete | OCR offered and selected, RCA, CCCR, SDIO and SD revisions, capabilities, function count, each function's manufacturer and device identity, bus width and card clock |
| `firmware` | The firmware is loaded and started | Firmware flags |
| `identified` | The chip and subrevision words are read | Both words |
| `scanned` | A scan completes | Up to 16 access points: BSSID, SSID, channel, frequency and RSSI, one per BSSID with the strongest kept when the list is full, and the count reported by the firmware |
| `associated` | Association completes | Open work |

A failure keeps the stage reached and names the stage that did not complete. A
later stage implies every earlier one. The result is the fixed 928-byte record
in `include/a733_radio_result.h`, which `tools/radio_result.py` decodes.
`a733_radio_result_platform()` fills the platform's part from its own state;
the later stages add their facts through the setters in the same header.

## The first-run guard stops before any write unless the board matches

On a board whose supply route is unconfirmed, the platform is asked for the
guard (`a733_radio_platform_first_run_guard()`, before the first poll). After the
rails and pad power words are read, and before any PMIC write, it proceeds only
if all of these hold:

- BLDO5 is off;
- PG reads unsupplied (`GPIO_POW_VAL` bits 13:12 are `00` or `01`);
- CLDO1 is on at 1.8 V;
- DCDC1 is on at selector code 23.

Otherwise the platform keeps every observation, writes nothing and retains. The
record carries the observed values either way. DCDC8 is observed and recorded
but never required.

## Entropy and credentials gate association only

Enumeration, firmware loading and the scan need neither entropy qualification
nor a WPA2 profile. Association needs keys, so entropy qualification and the
credential (`ss_wifi_credential()` in `include/sunstone_wifi_credential.h`)
gate that stage alone. A session with no credential stops after the scan and
returns normally; its result is the record up to `scanned`.

## What remains before a board run

1. **Scan results.** The management layer holds one scan result at a time. The
   session must copy each result into the record through
   `a733_radio_result_ap()` as it arrives, then call
   `a733_radio_result_scan_done()` with the firmware's reported count.
2. **A radio build without entropy.** `board_radio_startup()` refuses
   construction without CE entropy qualification, and the radio profile carries
   the WPA2 target. A scan-only profile must construct the platform and session
   without the entropy source, and `board_radio_poll()` must stop at `scanned`
   when no credential was uploaded.
3. **A returning lifetime with the radio.** `cmake/a733-os-cpu-return.cmake`
   refuses `SUNSTONE_A733_OS_RADIO`, and the radio platform has no shutdown. A
   session that returns normally must first put the module in reset (PM1 low),
   stop the SDC1 clock and mask its interrupt, with the register readbacks that
   prove it, before the CPU-return path retires. That sequence needs its own
   design and a host check; until it exists a radio run ends by watchdog reset
   and power removal, as the supervised build does.
4. **A host channel for the record.** The radio result is not part of the USB0
   output today. The record must travel in the generic application result
   (`ss_finish()`), or the OS must add an operation for it.
5. **Host preparation.** `apps/os/prepare.py` and `radio-assets.py` require the
   entropy facts and a WPA2 target in the profile; a scan-only profile needs the
   same admission without them, and the preparation must accept the first-run
   guard request.
