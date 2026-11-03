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
| `scanned` | A scan completes | Every access point found: BSSID, SSID, channel, frequency and RSSI, one entry per BSSID (the latest sighting), the count the firmware reported and the count dropped for lack of memory |
| `associated` | Association completes | Open work |

A failure keeps the stage reached and names the stage that did not complete. A
later stage implies every earlier one.

The service keeps a report object (`include/a733_radio_result.h`). Its scan list
is allocated from memory, through an allocator the build supplies (the FreeRTOS
heap in the radio build), and grows with what the scan finds. The only counts
fixed in the record are protocol or topology counts: SDIO function numbers 0 to
7, and the four observed supplies. A report encodes to a variable-length record:
a 232-byte head that gives the total length and the access point count, then one
44-byte entry per access point. `tools/radio_result.py` reads the length from the
head (`record_length()` on the first 232 bytes tells a reader how much more to
fetch). `a733_radio_report_platform()` fills the platform's part from its own
state; the later stages add their facts through the setters in the same header.

## The first-run guard stops before any write unless the board matches

On a board whose supply route is unconfirmed, the platform is asked for the
guard (`a733_radio_platform_first_run_guard()`, before the first poll). After the
rails and pad power words are read, and before any PMIC write, it proceeds only
if all of these hold:

- BLDO5 is off;
- PG reads `00` or `01` in `GPIO_POW_VAL` bits 13:12 (the manual calls these values "Invalid"; the board read `11` for PG with BLDO5 both off and on, see the vendor power sequence note, so this condition refused the run of 2026-11-03);
- CLDO1 is on at 1.8 V;
- DCDC1 is on at selector code 23.

Otherwise the platform keeps every observation, writes nothing and ends without
retaining anything; the code `0x400 | PG bits << 4 | unmet mask` (1 PG, 2 DCDC1,
4 BLDO5, 8 CLDO1) is the record's failure line. The record carries the observed values either way. DCDC8 is observed and recorded
but never required.

## Entropy and credentials gate association only

Enumeration, firmware loading and the scan need neither entropy qualification
nor a WPA2 profile. Association needs keys, so entropy qualification and the
credential (`ss_wifi_credential()` in `include/sunstone_wifi_credential.h`)
gate that stage alone. A session with no credential stops after the scan and
returns normally; its result is the record up to `scanned`.

## What remains before a board run

1. **Scan results.** The management layer holds one scan result at a time. The
   session must copy each result into the report through
   `a733_radio_report_ap()` as it arrives, then call
   `a733_radio_report_scan_done()` with the firmware's reported count.
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
   output today, and the record's length is known only when the service ends.
   The service copies the encoded record into retained memory sized to its
   length and publishes a small fixed descriptor (magic, address, length) at a
   known symbol. The host reads the descriptor, then the record in transfers of
   the channel's measured maximum, so no record is truncated.
5. **Host preparation.** `apps/os/prepare.py` and `radio-assets.py` require the
   entropy facts and a WPA2 target in the profile; a scan-only profile needs the
   same admission without them, and the preparation must accept the first-run
   guard request.
