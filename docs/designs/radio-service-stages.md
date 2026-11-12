# The radio service runs in stages and records the stage it reached

Status: **implemented**. The platform guard, power-on, enumeration, firmware
start, passive scan and normal shutdown have finite board qualification
([radio status](../status/radio.md), [records](../evidence/radio.jsonl)).
Association remains host-tested. Registered failure return covers partial
startup and scan-only shutdown; its physical qualification awaits reviewed USB
launch admission facts.

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
| `associated` | The four-way handshake reaches authorized and secured state | Stage, link status, supplicant and RSN states, EAPOL counts, selected frequency and timing; no credential bytes |

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

The PG condition refuses 3.3 V because a different supply could conflict with
BLDO5 when it is enabled. The manual defines `00` and `01` as invalid; neither
proves that the pins are unpowered. Qualified observations report PG at 1.8 V
with BLDO5 both enabled and disabled, so this detector does not identify its
supply ([power design](radio-vendor-power-sequence.md)). The PG bits are recorded
either way.

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
record carries neither. During the registered return, the owner erases its independent credential copies at EL1 on every
terminal path. Positive module shutdown also permits erasing the session's target, connect command, held records, queued
events, supplicant keys, key store and transport buffers. An uncertain stop retains those accepted-transfer and protocol
buffers unchanged; it cannot authorize their erasure. Each permitted erase uses stores the compiler cannot remove and
cleans the bytes to DRAM. `checks/board-radio.c` checks credential residue after completed cleanup and separately verifies
that a retained active transfer preserves its bytes and ownership while independent owner copies are erased.

The record carries a 48-byte association tail after the access point list (head
flags bit 9). It records the stage, link state, association status, supplicant
and RSN states, and counts of received and sent EAPOL frames. Messages 1 and 3
are received; messages 2 and 4 are sent. Flags identify unqualified entropy,
SSID match, PSK and CCMP support, target construction, entropy readiness,
credential presence and entropy failure. The tail also records the number of
matching scan results, selected frequency and RSSI, and connection and
authorization times. It holds no SSID, key or credential byte. The access point
list in this stage also omits SSIDs.

## Entropy and credentials gate association only

Enumeration, firmware loading and the scan need neither entropy qualification
nor a WPA2 profile. Association needs keys, so entropy qualification and the
credential (`ss_wifi_credential()` in `include/sunstone_wifi_credential.h`)
gate that stage alone. A session with no credential stops after the scan and
returns normally; its result is the record up to `scanned`.

## Registered return and admission limits

The returning supervisor requests the radio owner before thermal and PMIC stop.
The owner closes admission, drains the accepted SDIO transfer and advances
partial startup or scan shutdown through the native stop sequence. It uses the
same absolute deadline as every other owner; existing shorter operation bounds
still apply. Shared lock waiters and pin callbacks reject access after that
deadline. No further firmware or protocol setup begins during return.

When Bluetooth shares the module, its protocol owner retains access until it
publishes release. The module owner then asserts and verifies PM2 using explicit
version 4 authority and reports peer stop, after which Bluetooth closes UART.
Without that authority, Bluetooth retains FAULT. Returning headset admission
remains excluded pending physical reset-route and accessory qualification
([Bluetooth return](bluetooth-return.md)).

A scan-only return reports DONE after DOWN readbacks, transport and PMIC
retirement, and credential cleanup. Full-station, association and entropy-only
sessions retain FAULT value `0x905`: their crypto-engine ownership has no complete
retirement proof. Returning admission therefore remains limited to the scanning
station. A registered owner fault records its diagnostic and permits independent
cleanup to continue, but prevents per-core terminal release
([owner audit](failure-return-audit.md)).

The service copies each scan result into its allocated report and publishes the
encoded record through a fixed descriptor containing its address and length.
The runner reads that descriptor and then the complete variable-length record.
The scan-only profile constructs the platform and session without CE entropy or
a stored network credential. The preparation and runner tools support the guard
and stage limit.

Physical failure injection through scan awaits reviewed USB launch admission
facts. Existing normal scan returns do not qualify this failure path. Association,
full-station CE retirement, the Bluetooth reset route and physical headset
closure remain separate qualification requirements.
