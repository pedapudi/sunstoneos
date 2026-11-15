# The radio service runs in stages and records the stage it reached

Status: **implemented**. The platform guard, power-on, enumeration, firmware
start, passive scan and normal shutdown have finite board qualification
([radio status](../status/radio.md), [records](../evidence/radio.jsonl)).
Association remains host-tested. Registered failure return covers partial
startup and scan-only shutdown; its physical qualification awaits reviewed USB
launch admission facts.

This document is for driver developers and the board coordinator. It states how
the shipped Wi-Fi service in the OS image behaves, from supply power-up to
association, an IPv4 lease and one name lookup, and what it reports to the host.

## The stages run in order and each one is reported

| Stage | Reached when | Facts recorded |
| --- | --- | --- |
| `rails-observed` | DCDC1, BLDO5, CLDO1 and, in the first-run guard, DCDC8 are read through the PMIC provider | Enable byte, selector and decoded voltage of each |
| `guard-passed` | The first-run guard (below) holds | `GPIO_POW_MOD_SEL`, `GPIO_POW_VAL`, `SGPIO_POW_MOD_SEL`, `SGPIO_POW_VAL` |
| `powered` | BLDO5 is on at 1.8 V, read back, and the 10 ms and PM1 steps of the [vendor sequence](radio-vendor-power-sequence.md) are done | Whether BLDO5 was powered on by this run |
| `enumerated` | CMD5, CMD3, CMD7 and the CCCR and tuple reads complete | OCR offered and selected, RCA, CCCR, SDIO and SD revisions, capabilities, function count, each function's manufacturer and device identity, bus width and card clock |
| `firmware` | The firmware is loaded and started | Firmware flags |
| `identified` | The chip and subrevision words are read | Both words |
| `scanned` | A scan completes | Every access point found: BSSID, channel, frequency and RSSI, one entry per BSSID (the latest sighting), the count dropped for lack of memory, and the scan counts below |
| `associated` | The four-way handshake reaches authorized and secured state | Stage, link status, supplicant and RSN states, EAPOL counts, selected frequency and timing; no credential bytes |

A failure keeps the stage reached and names the stage that did not complete. A
later stage implies every earlier one.

The service keeps a report object (`include/a733_radio_result.h`). Its scan list
is allocated from memory, through an allocator the build supplies (the FreeRTOS
heap in the radio build), and grows with what the scan finds. The only counts
fixed in the record are protocol or topology counts: SDIO function numbers 0 to
7, and the four observed supplies. A report encodes to a variable-length record:
a 232-byte head that gives the total length and the access point count, then one
44-byte entry per access point, then the 16-byte scan counts when head flags
bit 10 is set, then the 48-byte association tail when bit 9 is set, then the
16-byte RSN summary when bit 11 is set, then the 16-byte fault site when bit 12
is set, then the 28-byte transport trace when bit 13 is set, then the 64-byte address section when bit 14 is set. The head
carries the record version (4 since the address section). The image exports the version it
writes as `os_radio_result_version`, and admission refuses an image whose
version the runner's `tools/radio_result.py` does not read, naming both
(`checks/radio-record-version.py`). The scan counts are:

- **Sightings:** scan results delivered to the owner as events.
- **Firmware count:** the count in the firmware's scan-end event
  (`result_cnt`, which the vendor header describes as the number of scan
  results available; the vendor driver does not read it).
- **Merges:** sightings of a BSSID already listed, which replace its entry.
- **Set aside:** scan result records that the dispatcher retired without an
  event, because they were for another interface or arrived after the scan-end
  event.

The listed count equals sightings minus merges minus dropped, and the decoder
refuses a record where it does not. The head's `aps_reported` is the larger of
the sightings and the firmware count; records from images before bit 10 carry
only that, which cannot separate repeated BSSIDs from results that never became
events. `tools/radio_result.py` reads the length from the
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
and association 8 s, the security stage 14 s, each supplicant command 6 s. A normal run should need about 3 s of scan, well
under 1 s to join, and tens of milliseconds per handshake message. Each bound also stops at the session deadline.

The command bound is the reference's: `RWNX_80211_CMD_TIMEOUT_MS` is 6000 ms for SDIO (`aic8800_fdrv/rwnx_cmds.h`),
multiplied by the command manager's queue size, which is 1 while one command is outstanding, as it always is here. The
bounds nest so that a full wait ends at the security worker's own deadline: the security stage holds a handshake of up to
8 s and then the pairwise key add's 6 s, and the runtime lease renews 7 s before its end, so no lease end falls inside a
command's wait. `checks/radio-stage-limit.py` checks these relations on the prepared profile, and the `key-bound` case of
`checks/aic8800-supplicant.py` accepts a confirmation 5.9 s after the write and stops at the deadline for one at 6.1 s.

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
authorization times. It holds no SSID, key or credential byte.

No stage's access point list names an SSID. A scan hears the user's network
among its neighbours, and the record reaches the host's result files; the list
keeps each BSSID, frequency and RSSI. `a733_radio_report_ap()` takes no SSID,
so each entry's SSID length and bytes stay 0, and `tools/radio_result.py`
refuses an entry where they are not. `checks/board-radio.c` checks that the
scan-only record holds no name although the scan heard two, and
`checks/radio-result-decode.py` checks the refusal.

An association miss (a refused association, a timeout or a handshake that never
completes) retains the radio, which runs the failure shutdown in the vendor
order. When every step reads back, the platform is clean, and the radio quiesce
closes the station: it retires the association stage's CE, waits for the
credential wipe and completes the return, so the host reads the record with its
association tail. `checks/board-radio.c` checks that both timeouts end clean
with the tail written and no credential byte left, and
`checks/os-quiesce-radio.py` checks the close. When a step does not read back,
the fault stays and the return ends at the watchdog; the first failure record
then carries the tail's codes: bits 31:16 the association status code, bit 15
set, bits 14:12 the link, 11:8 the EAPOL frames sent and 7:4 those received
(each at most 15), and 3:0 the stage.

The RSN summary describes the RSN element of the scan result that the
association stage judged: the passing candidate, or while none passes, the
strongest result with the credential's SSID. It holds the group cipher, the
pairwise and key-management suites, the RSN capabilities (with the MFP capable
and required bits), the parser's result, and the first condition of the PSK
candidate test that failed: no element, an element longer than the
supplicant's copy, a malformed element, a group cipher other than CCMP, no CCMP
pairwise cipher, MFP required, no PSK key management, or a refusal with every
RSN condition met. It holds no address or SSID. `checks/board-radio.c` checks
each condition, and `checks/radio-rsn-summary.py` checks the classifier against
the pinned parser and the supplicant's cipher conditions
(`include/aic8800_supplicant_rsn.h`).

The fault site (`include/aic8800_fault_site.h`) is written when the
association stage fails after the security stage attached, before the failure
shutdown changes any state. It holds codes, states and counts only:

- the source site (file code and line) and error of the transport's first
  fault, and the site of the supplicant's first stop;
- the data worker's state and error;
- the security worker's state when it first stopped, its operation, key slot
  and the confirmation it waited for, with the parameter length and status byte
  of the confirmation that matched before it was judged;
- the supplicant's state, queued actions, and whether the head action installs
  a key, is in flight, the port open is pending, the handshake completed or an
  EAPOL frame is deferred;
- the clock in milliseconds when the stage saw the failure.

A key add whose confirmation never came records the security worker stopped
while waiting, with no matched confirmation; a refused key add records the
confirmation's length and status. `checks/aic8800-supplicant.py` covers both
cases (`key-timeout` and `key-failure`), and `checks/radio-result-decode.py`
covers the decoding.

The transport trace (`struct aic8800_transport_trace`) records what the bus
did after the last command write, so an unanswered confirmation says whether
anything came back. The image writes it with the fault site, and also alone
when the security stage ends authorized, so a slow confirmation that still
arrived in time leaves its latency in the record. It holds:

- the command's message id, whether its CMD53 write was submitted and
  completed, and the flow-control value that admitted it;
- the time from that write to the transport's last clock read;
- since that write: receive records by transport type, receive reads,
  function 1 status reads, and the id of the last command response;
- whether the transport waits for the function interrupt, the last function 1
  status value and the transport's phase;
- the SD controller's SDIO interrupt latch and mask, read once at capture
  (`a733_radio_platform_sdio_irq_view`, a register read that changes nothing),
  whether the host saw the latch, and the radio's GIC interrupts and owner
  wakes since the write;
- the dispatcher's queued events and diagnostic records by kind, the held
  record's transport type, and the status of the last data confirmation;
- the request id of the slowest security command that was confirmed, and the
  time from its write to its confirmation in milliseconds.

The `window:events` variants of `checks/aic8800-supplicant.py` give three
distinct traces for a key add: `silent` (nothing came: no records, latch
clear), `unserviced` (the reply sits in the card behind a raised interrupt the
owner never serviced: no records, latch set) and `other` (another message came:
records counted, the last response id named). The `key-bound` case holds the
key add's confirmation for a given time: at 5900 ms it is accepted and the
authorized trace names it as the slowest, and at 6100 ms the stage stops with
reason deadline.

### Confirmations are read for the fields used

The reference's command manager (`rwnx_cmds.c`, lines 475 to 482, and
`aic_bsp_driver.c`, lines 161 to 168, for the firmware loader) copies a
confirmation's parameters for the length the firmware sends, up to 1024 bytes
(`IPC_E2A_MSG_SIZE_BASE` of 256 words, with `CONFIG_RWNX_TL4` off), and the
handlers read only their fields. The indication handlers in `rwnx_msg_rx.c`
read their fields the same way, with no length check. This driver does the
same: each message is refused only when it is shorter than the bytes read, and
a longer one is read for the same fields.

| Message | Reference struct | Fields read | Shortest accepted |
|---|---|---|---|
| MM_SET_RF_CALIB_CFM | 16 bytes | the four gain table addresses | 16 |
| MM_GET_MAC_ADDR_CFM | 6 bytes | `mac_addr` | 6 |
| MM_VERSION_CFM | 28 bytes, the last one padding | the version words, `features`, `max_sta_nb`, `max_vif_nb` | 27 |
| MM_ADD_IF_CFM | 2 bytes | `status`, `inst_nbr` | 2 |
| SCANU_START_CFM | 3 bytes | `vif_idx`, `status`, `result_cnt` | 3 |
| SM_CONNECT_CFM | 1 byte | `status` | 1 |
| SM_CONNECT_IND | 852 bytes | every field through `ac_param` | 852 |
| SM_DISCONNECT_IND | 6 bytes, the last one padding | `reason_code`, `vif_idx`, `ft_over_ds`, `reassoc` | 5 |
| MM_KEY_ADD_CFM | 4 bytes, the last two alignment | `status`, `hw_key_idx` | 2 |
| DBG_MEM_READ_CFM, DBG_MEM_WRITE_CFM | 8 bytes | `memaddr`, `memdata` | 8 |
| the firmware loader's other confirmations | 4 bytes | the 32-bit status | 4 |
| DBG_BINDING_CFM | 1 byte | the status | 1 |
| DBG_BINDING_IND | 16 bytes | `enc_data` | 16 |

Confirmations whose parameters are not read (MM_RESET_CFM, MM_START_CFM,
ME_CONFIG_CFM, ME_CHAN_CONFIG_CFM, SCANU_START_CFM_ADDTIONAL,
SM_DISCONNECT_CFM, MM_KEY_DEL_CFM and ME_SET_CONTROL_PORT_CFM) are accepted at
any length. The codec refuses a message longer than 1024 bytes, the reference's
bound. The security worker keeps the first 64 bytes of a confirmation for the
record, and the station event keeps the first 852 bytes of an indication. A
data confirmation (transport type 0x12, a 12-byte record) is not a message and
is not covered here.

`checks/aic8800-wifi.c` checks each management and key confirmation at its
shortest accepted length and one byte shorter, and two mutants that restore an
exact length (MM_KEY_ADD_CFM at 4 bytes, SM_CONNECT_IND at 852 bytes) must
fail it. `checks/aic8800-firmware.c` runs the firmware load with every
confirmation four bytes longer, and with the first one a byte short;
`checks/aic8800-runtime.c`, `checks/aic8800-sdio-codec.c` and
`checks/aic8800-dispatch.c` cover the binding messages, the memory
confirmations and the dispatcher's matching rules.

Three board runs reached the pairwise key add. In run 5 (entry
1794738427185556712-c450b701) no confirmation came within the 1 s bound the
image then had; that remains unexplained. In run 6 (entry
1794745255109339882-f869332f) the confirmation came at once as two bytes with
status 0, and the image refused it because it then required exactly 4 bytes.
In run 7 (entry 1794751437798567373-be2dcf1b), with the lengths above, the
stage ended authorized: connect 4.13 s after arming, both key adds and the port
open confirmed, the link secured 0.42 s after connect, the orderly shutdown
matched and the board returned 4.66 s after arming.

The trace counts milliseconds from the transport's clock read at the start of
each owner pass. A confirmation read in the same pass as its write completed
reads 0 ms, as every confirmation did in run 7, so the trace resolves no
latency below one pass and one millisecond. The 6000 ms command bound stays
until a run measures a latency it can be tightened against.

## The address stage leases an IPv4 address and resolves one name

Status: **host-tested**. No board run has reached it, and the IPv4 stack has not yet run on the board.

`--radio-stage-limit 6` (`BOARD_RADIO_STAGE_ADDRESS`) runs the association stage and then this one. It needs `--wifi
NAME` as stage 5 does. The prepared profile sets the network's DHCP flag and the bounds below; the image refuses a stage-6
profile without them, and a profile that sets DHCP at any other stage. The sequence after authorization is:

1. Authorization ends association as in stage 5: the transport trace is captured and the PMK copy is erased. The address
   stage's bound starts.
2. The owner starts the station network, the pinned FreeRTOS+TCP stack over the AIC8800 data path, once the transport
   is idle and the IP task's random-number ring (below) holds four words. The stack's DHCP client sends its first
   discover 250 ms later.
3. The owner's notification wakes the radio client task, which submits one `SUNSTONE_NET_RESOLVE` for `example.com`.
   The network service holds the request until a lease is published, then sends one A query to the first DNS server the
   lease names and waits for the response. The client zeroes any returned address before it stores the reply.
4. On the client's reply, if the lease is still held, the owner takes the network offline (`sunstone_net_offline`). The
   IP task runs the stack's network-down processing, which stops DHCP's timer and closes its socket, and then
   acknowledges. The owner returns the session from the network to the management layer
   (`a733_radio_session_park_network`), which discards any held data frames, and the stage ends at `scanned`. The module
   then shuts down as after association.

Every wait is a blocked task. The owner wakes on radio interrupts and on the client's notification. The IP task wakes on
its receive queue and the stack's timers. The client wakes on its reply notification.

The image sends no DHCPRELEASE. The pinned stack has no release message, and the image does not patch it. Stopping DHCP
and shutting down in order leaves the lease on the server until its lease time ends. No DHCP message carries a host
name: the stack's `ipconfigDHCP_REGISTER_HOSTNAME` stays disabled, and the record flags any sent message that holds
option 12.

The stage fails, and the failure shutdown runs, in three cases:

- **No lease:** the client's reply comes while no lease is held, as it does after no offer, or after a NAK with no
  later ACK.
- **Link lost:** the access point disconnects the station.
- **Bound passed:** the stage's bound ends first.

The network faults in each case, which also stops DHCP. Each path writes the address section.

The stack draws its DHCP transaction identifiers, its DNS query identifier and its socket port numbers on the IP task.
The CE entropy reader serves only the owner task, so the owner fills a 16-word single-producer ring from the CE
reservoir on every pass and the IP task takes words from it. The stack draws the first DHCP transaction identifier once
at its start, and DHCP never starts when that draw fails; the network therefore starts only with four words ready.
`checks/radio-network-dhcp-dns.py` measures four words drawn through a lease, its renewal, one socket and one lookup. A
draw that finds the ring empty sets the record's entropy-short flag. The ring is erased with the other secrets.

The IP task is an OS service created without an affinity, so it runs on the service core, CPU0, as the radio owner
does. The record's placement fields show the cores both ran on and how often the IP task woke.

### The address section holds counts, codes, flags and times only

Head flags bit 14 marks a 64-byte address section, the last section of the record (version 4). Its fields
(`include/aic8800_address_trace.h`) are:

- **Counts:** DHCP discovers, offers, requests, ACKs and NAKs, DNS queries and responses, and the frames ignored because
  their transaction identifier, query identifier or hardware address belongs to another station.
- **Times:** the first frame of each kind, in milliseconds since the network started.
- **Placement:** the IP task's wakes since the network started, and those that carried no event (the stack's timer
  waits ending, and the driver's two bare wake-ups at the stack's start and the offline marker); the cores the IP task
  woke on, and the cores the radio owner polled the network on, one bit per core.
- **Codes:** the lease time in seconds, the number of DNS servers the lease names, the DNS response code and answer
  count, and the resolve's reply code.
- **Flags:**
  - the lease was published;
  - the leased address is in an RFC 1918 private range;
  - the ACK named a router, and the first router is private-range;
  - the offer and the ACK name the same address, and the same server;
  - a sent DHCP message carried a host-name option;
  - the ACK carried a lease time;
  - a DNS response carried the query's identifier, and the resolve returned an address;
  - the IP task acknowledged the offline fence;
  - the stage's bound passed;
  - the ring was empty at a draw.

The section holds no address, MAC, host name or DNS name. The owner's frame observer keeps the transaction identifiers
and the offered address in memory to compare with later frames; they never reach the record. `tools/evidence.py`
refuses any record in `docs/evidence/radio.jsonl` that holds an IPv4 dotted quad or a 48-bit MAC in colon or hyphen form
(`checks/evidence-addresses.py`).

### The stage's bounds nest inside the startup deadline and the session

Ticks are milliseconds. The bounds start at three points:

- **Stage, 17 s from authorization.** It covers the wait for an idle transport and four ring words, the network's
  start, the resolve and the offline fence: the resolve's 16 s plus 1 s for the rest.
- **DHCP acquisition, 12 s from the network's start.** The stack sends its first discover at 250 ms
  (`dhcpINITIAL_TIMER_PERIOD`) and waits 5 s for an offer (`dhcpINITIAL_DHCP_TX_PERIOD`), then sends a second discover
  and waits 10 s; a request waits 5 s for its ACK. 12 s holds two discovers, or one discover and the request with its
  full wait.
- **Resolve, 16 s from the client's submit.** The resolve waits for the lease and then has at least 4 s for the query
  when the lease came within 12 s.

Each frame has 2 s to transmit and 3 s in the queue. The lease cap is 10 minutes, far beyond the stage. Every bound
also stops at the 50 s startup deadline (`bounded_deadline` in `apps/board/radio.c`).

Three later deadlines follow in session time. The radio client application must finish by 55 s: the 60 s session end
less the 5 s return reserve in `apps/radio-scan/manifest.json`, which is earlier than its 52 s run bound from its start.
The radio owner's deadline is 10 ms before the session end, and the session ends at 60 s.

- **Nominal.** On the board, firmware is confirmed about 2 s into the session and association authorizes about 4.5 s
  in. The lease should follow within 1 to 2 s and the DNS answer within tens of milliseconds; with the fence and the
  normal shutdown, the stage ends near 8 s.
- **Worst case.** With firmware confirmed at the 16.7 s the association budget allows, the association bounds (scan
  10 s, join 8 s, security 14 s) can carry authorization to 48.7 s, and
  the stage's 17 s then stops at the 50 s startup deadline. The failure shutdown starts there, the client must finish by
  55 s, and the return has the 5 s reserve before the 60 s session end.

`checks/radio-stage-limit.py` checks the relations between the acquisition, resolve, frame and lease bounds and the
association budget on the prepared profile. `checks/radio-network-dhcp-dns.py` runs the stack against a scripted
server for an offer and ACK, a NAK, and no reply, and `checks/board-radio.c` covers the fenced end, no lease and the
bound.

## Entropy and credentials gate association only

Enumeration, firmware loading and the scan need neither entropy qualification
nor a WPA2 profile. Association needs keys, so entropy qualification and the
credential (`ss_wifi_credential()` in `include/sunstone_wifi_credential.h`)
gate that stage and the address stage after it. A session with no credential stops after the scan and
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
retirement, and credential cleanup. An association return also retires the
crypto engine before DONE (`os_ce_retire_step` in `apps/os/ce_probe.c`): a TRNG
task still in flight is polled to completion and its block zeroed, the
controller must be idle, and `a733_ce_clock_retire` takes the CE down by the
reverse of its bring-up (bus gate, MBUS gate, clock gate, module gate, both
resets, then the clock source, each read back under the register lock). The
clock token is then released, and the descriptor page is returned from the
device, zeroed, cleaned and freed to the ledger. A CE that cannot be proved down
keeps FAULT value `0x905` with the unmet step in bits 31:16 (1 clock, 2
deadline, 3 engine fault or failed task, 4 not idle, 5 clock refused after a
write, 6 token, 7 page). Full-station and entropy-only sessions retain FAULT
value `0x905`: the full station's own entropy source and the entropy stage have
no retirement path.

A registered owner fault records its diagnostic and permits independent
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
