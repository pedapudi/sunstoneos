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

- BLDO5 is off, or on at 1.8 V (a reset left it on);
- PG does not read 3.3 V: `GPIO_POW_VAL` bits 13:12 are not `10`. A reading of `11` (1.8 V), `00` or `01` passes;
- CLDO1 is on at 1.8 V;
- DCDC1 is on at selector code 23.

The PG condition refuses only 3.3 V for three reasons. The manual defines no
"unsupplied" reading: `00` and `01` are "Invalid", and the earlier condition that
required them refused the run of 2026-11-03. The board reads `11` for PG with BLDO5
both off and on, so the word does not track BLDO5. And a 3.3 V reading would mean a
different supply that could fight BLDO5 once it is switched on, while the vendor
driver enables BLDO5 unconditionally. The PG bits are recorded either way.

Otherwise the platform keeps every observation, writes nothing and ends without
retaining anything; the code `0x400 | PG bits << 4 | unmet mask` (1 PG, 2 DCDC1,
4 BLDO5, 8 CLDO1) is the record's failure line. The record carries the observed values either way. DCDC8 is observed and recorded
but never required.

## The association stage joins the stored network and stops at the four-way handshake

`--radio-stage-limit 5` runs the full radio path through association (`BOARD_RADIO_STAGE_ASSOCIATE`). It needs
`--wifi NAME`; `board.py submit` refuses a run without a name, and refuses a name with no stored credential, before the
device is touched. The sequence after the passive 2.4 GHz scan is:

1. The owner picks the strongest scan result whose SSID equals the credential's and whose RSN element offers PSK and CCMP
   (and does not require protected management frames). None found ends the stage as a clean radio failure.
2. It waits for the CE reservoir (at least 128 bytes), which fills beside the radio from the first poll. The entropy is
   unqualified and bring-up only ([entropy](../entropy.md)); the profile declares this in `network_reserved[1]`.
3. It builds the target (BSSID, SSID, the scan channel's index) and the supplicant configuration (the PMK from
   `ss_wifi_credential()` at EL1, the AP's RSN element) and calls connect. The PMK copy in the board's configuration is
   zeroed once the supplicant has taken it; the supplicant struct is zeroed when the module is down.
4. Link `SECURITY_PENDING` starts the supplicant, which runs the four-way handshake. Supplicant `AUTHORIZED` with link
   `SECURED` ends the stage; the owner then shuts the module down as after a scan. No address is requested: DHCP is a later
   stage.

Time budget, against the 50 s startup deadline (firmware confirmed near 16.7 s of session time): scan 10 s, authentication
and association 8 s, four-way handshake 8 s, supplicant command 1 s. A normal run should need about 3 s of scan, well under
1 s to join, and tens of milliseconds per handshake message. Each bound also stops at the session deadline.

Credential residue. The runner backs up image RAM before it delivers the credential, so the backups hold none. After the
return it reads only named blocks: the return report, the output record, the radio descriptor and the record it names, and
the device-range list; it then restores image RAM from the backup. Nothing the runner reads holds the SSID or key, and the
record carries neither. The owner still wipes every credential-derived byte at EL1 as soon as the session is over, on every
exit that keeps DRAM (the module down after success, and the end of a failure, deadline, refusal or board failure): the SSID
in the target, the connect command, held records and queued events; the PMK, PTK and GTK in the supplicant and its slots (its
only heap), the key store and the transport buffers; and the entropy reservoir. Each is zeroed with stores the compiler cannot
remove and cleaned to DRAM. A watchdog reset loses DRAM and needs no wipe. `checks/board-radio.c` runs six exit paths with a
test credential and scans the whole `struct board_radio` and the report arena for any 8-byte run of the SSID or PMK.

The record carries a 48-byte association tail after the access point list (head flags bit 9): the stage reached, the link
state, the association status code, the supplicant and RSN states, the EAPOL frames received and sent (messages 1 and 3 are
received, 2 and 4 sent), flags (unqualified entropy, SSID matched, RSN offers PSK and CCMP, target built, entropy ready,
credential present, entropy failed), the count of matching scan results, the chosen frequency and RSSI, and connect and
authorize times. It holds no SSID, key or credential byte, and the access point list in this stage carries no SSIDs.

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
3. **A returning lifetime with the radio.** Only the scanning station shuts
   the module down on return, with a readback at each step
   (`apps/os/radio_scan.c`). Admission refuses the radio in any other returning
   image (`apps/os/grants.c`), so a full-station run still ends by watchdog
   reset and power removal, as the supervised build does.
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
