# Native Wi-Fi management initialization and passive scan

SunstoneOS implements the normal AIC8800D80 management startup sequence and a
bounded passive-scan operation through its existing SDIO transport. A host model
passes initialization, command serialization, scan-result retention, and fault
handling. Physical calibration, scanning, and radio behavior remain unqualified.

Management readiness means that the worker received the required initialization
confirmations and a station-interface index. It does not establish association,
authentication, encryption, network data delivery, or Bluetooth operation.
[Station association and disconnection](radio-association.md) use a separately
admitted target and preserve WPA2 security as pending.
The [runtime receiver](radio-runtime.md) continues to preserve unrelated control
and opaque data records throughout management work.

## RF profile and channel authority are explicit inputs

[include/aic8800_wifi.h](../include/aic8800_wifi.h) defines one admitted RF profile
and an owned channel catalogue. Initialization requires nonzero caller records
for both the module RF profile and channel policy. These records identify the
caller's authorization; their numeric values do not establish regulatory
permission or grant physical board access.

RF profile 1 follows the pinned source's normal D80 defaults: the published
version-3 transmit-power table, disabled power-adjustment/offset overrides,
disabled crystal-capacitance overrides, and an actual firmware calibration
exchange. The table is a source configuration, not measured calibration data or
a country-specific power authorization. The caller must approve its use for the
installed module and retain the same power/reset lifetime.

The caller supplies every permitted channel's frequency, band, maximum power,
and flags. Initialization copies the list into owned storage; that copy remains
immutable throughout the worker's lifetime. It rejects duplicate frequencies,
invalid band/frequency encodings, oversized lists, and channels lacking `NO_IR`.
The source ABI permits up to 14 channels in 2.4 GHz and 28 in 5 GHz. Optional
radar-required flags are preserved. No country, default regulatory domain, or
permitted channel list is inferred from chip identity or a MAC address.

Passive behavior follows the source's explicit `CHAN_NO_IR` flag, which means
that the channel cannot initiate radiation. The published scan sender applies
that flag under `RADAR_OR_IR_DETECT`; its Makefile defaults that option off.
The native profile follows the enabled flag-handling branch and sets `NO_IR`
on every configured and scanned channel. It also sends zero SSIDs, no additional
probe-request information elements, and the broadcast BSSID filter.
Zero SSIDs alone would not establish passive behavior in the default source
build. Actual firmware suppression of probe transmissions requires physical
qualification; host checks establish the requested flag and packet bytes.

## Initialization preserves calibration, identity, and version evidence

The [D80 RF entry point][rf], [normal initialization caller][main], and
[message sender][tx] establish this order:

| Step | Request / confirmation | Implemented behavior |
| --- | --- | --- |
| Power table | `0x0077` / `0x0078` | Send the source's 69-byte default version-3 table |
| Calibration | `0x0069` / `0x006a` | Send the source's 24-byte D80 calibration request and preserve four returned radio addresses |
| MAC read | `0x0073` / `0x0074` | Request the firmware MAC address; require six bytes and a nonzero unicast address |
| Management reset | `0x0000` / `0x0001` | Perform the required initial management reset once |
| Version | `0x0004` / `0x0005` | Preserve firmware, MAC/PHY versions, feature bits, station limit, and interface limit |
| Management configuration | `0x1400` / `0x1401` | Configure a legacy management profile with the admitted station power-save mode |
| Channels | `0x1402` / `0x1403` | Publish the admitted channel list with its power limits and `NO_IR` flags |
| Management start | `0x0002` / `0x0003` | Send source PHY/start parameters |
| Station interface | `0x0006` / `0x0007` | Use the read MAC, require success, and retain the returned interface index |

The calibration fields are `0x0f8f` for 2.4 GHz, `0x0f0f` for 5 GHz,
`0x0c34c008` for alpha, Bluetooth calibration disabled, Bluetooth parameter
`0x264203`, and zero crystal override bytes. These are the source request values.
The 16-byte confirmation supplies four radio addresses; the worker preserves
them without inventing a success flag, dereferencing them on the A733, or
substituting fabricated calibration results.

The source's PHY-configuration call is disabled in this branch, leaving its
64-byte configuration buffer zero. Management start therefore uses that zero
buffer, the source's UAPSD timeout of 300, and its clock-accuracy value of 20 ppm.
The admitted management profile uses 20 MHz, no HT/VHT/HE capability claims
and packet lifetime 512. Future networking must configure and qualify the full
capability set.

Version admission requires the source-defined full-MAC feature and a nonzero
interface limit. Interface creation requires status zero and an index below
that limit. A bad MAC address is a terminal failure; there is no generated
address fallback. Confirmations whose source sender ignores the payload retain
their bounded bytes without an invented zero-status or empty-payload rule.

## Station power save follows the source management configuration

The `power_save` field of the Wi-Fi configuration selects one of three modes.
In power save, the firmware lets the station doze between beacons while the
access point buffers its traffic.

| Mode | `ps_on` | `dpsm` | Meaning |
| --- | --- | --- | --- |
| `AIC8800_WIFI_POWER_OFF` (0) | 0 | 0 | The station stays awake |
| `AIC8800_WIFI_POWER_ON` (1) | 1 | 0 | The reference full-MAC default |
| `AIC8800_WIFI_POWER_DYNAMIC` (2) | 1 | 1 | Dynamic power save, which leaves doze while traffic is active |

The worker writes these two booleans into the management configuration
request, as `rwnx_send_me_config_req` does. The source clears `ps_on` when the
version confirmation lacks the power-save feature (`MM_FEAT_PS_BIT`, bit 2 of
the feature word). The worker does the same and records the applied mode in
`power_mode`, so a caller can see that the request had no effect. Values above
2 and nonzero reserved bytes refuse initialization. The field occupies former
tail padding, so the configuration remains 288 bytes and the 1,872-byte board
radio profile keeps its layout; a zero byte keeps power save off.

The pinned driver defines `ME_SET_PS_MODE_REQ` but never sends it, and its
`set_power_mgmt` callback is empty. The native worker likewise has no runtime
mode change; the mode stays fixed for the management lifetime. The firmware
handles doze and wake itself, so the host data path does not change.

`checks/aic8800-wifi-power.py` compiles the source `me_config_req` for each mode
and drives the actual worker through management startup. The request bytes
match for every mode; without the feature bit, both enabled modes send the
power-off bytes and report `power_mode` 0. An out-of-range mode and a nonzero
reserved byte are refused. Physical doze behavior and its effect on latency
remain unqualified.

```sh
python3 checks/aic8800-wifi-power.py \
  --reference-dir /path/to/pinned/aic8800_fdrv \
  --evidence /path/outside/repository/radio-wifi-power
```

## Binding and Wi-Fi share one outstanding command

The transport admits management work only between complete receive aggregates,
with no pending controller request or held record. It retains the Wi-Fi worker,
runtime, firmware artifacts, card, controller, and their buffers as one device
lifetime. Initialization needs fresh zero storage and refuses reuse.

Binding is conditional in the source. Runtime silence is not recorded as binding
success. A binding challenge received while a Wi-Fi command awaits confirmation
is retained; that confirmation completes first. Binding then runs before the
next Wi-Fi command. Both deadlines continue during the pause, and the transport
never submits two outstanding control commands.

The implementation uses fixed command encoders and exposes no arbitrary radio
write interface. It performs the initial management reset required by the source;
errors do not trigger reset, rollback, command retry, or firmware substitution.
The [firmware uploader](radio-firmware.md) and bootstrap evidence remain separate
from this management implementation.

## Scan acceptance, results, and completion remain distinct

`aic8800_transport_scan` admits one scan after management readiness. The request
uses the initialized station index and the immutable admitted channel catalogue.
It has a command deadline and a separate overall scan deadline.

| Event | ID | Meaning |
| --- | --- | --- |
| Scan request | `0x1000` | Submit the 376-byte source request structure |
| Immediate confirmation | `0x1009` | Record command acceptance; the source defines no status layout for its ignored payload |
| Scan result | `0x1004` | Validate and retain one result while its consumer owns it |
| Final confirmation | `0x1001` | Record interface, status, and reported result count; require status zero |

The worker records immediate acceptance and final completion separately and
requires both before reporting a completed scan. Results can arrive between
those messages. A final confirmation for another interface, a duplicate,
or an unrelated command remains an ordinary retained record.

The [result handler][rx] and [message ABI][messages] supply a frame length, frame-control metadata,
frequency, band, station/interface indices, RSSI, and frame bytes. The native
worker accepts beacon and probe-response layouts with at least the 36-byte
management header/fixed fields. It checks the declared frame length, channel
membership, interface identity, and every information-element boundary before
publishing BSSID, SSID, and RSSI. Results for a different interface remain raw
records for their consumer.

One result of at most 1,000 frame bytes fits owned worker storage. The complete
frame remains available alongside parsed metadata. A held result blocks aggregate
reuse and new transfers; neither another result nor completion can overwrite it.
`aic8800_transport_peek_scan` returns a borrowed view valid until generic
`aic8800_transport_consume`. Consumers must copy asynchronous work before
consuming it. Output views cannot overlap any retained radio object or artifact.
Larger or malformed frames retain the original receive aggregate in a terminal
fault rather than being truncated or silently discarded.

After completion and aggregate drain, `aic8800_transport_take_scan` copies the
summary and acknowledges it. This permits another scan. The summary distinguishes
the firmware's reported count from the count of accepted native results; the
worker does not assume that an eight-bit firmware count cannot wrap.

## Deadlines include completion and retained consumer work

Each initialization operation has a bounded command deadline within the admitted
overall deadline. A scan's immediate confirmation ends its command timeout while
the overall timeout continues through results and final completion. All work
must fit within the runtime receive lease.

A confirmation retains its active deadline through the transport's subsequent
clock observation. Changing to readiness/completion or clearing a command timer
cannot hide late completion. Monotonic-clock checks also apply during binding,
controller progress, and consumer backpressure.

Failures preserve the first error, calibration/MAC/version evidence, outgoing
request, partial transfer, received bytes, and held result. Terminal polling
performs no further backend access. There is no disposal API; external recovery
must quiesce the device before releasing its resources.

## Focused source and host evidence

[checks/aic8800-wifi.py](../checks/aic8800-wifi.py) verifies the pinned sources,
compiles their unmodified full-MAC headers with fixed-width host type shims,
and builds expected command structures outside Git. It extracts the power-table
initializer from the published platform source. Independent framing then produces
complete 512-byte expected packets, including CRC and zero padding.

The native packets match nine initialization commands, a binding request inserted
after version confirmation, and one passive scan. The integrated model uses the
real controller, card, codec, runtime, transport, and management worker under
address/undefined-behavior sanitizers. Its completed-firmware boundary is an
explicit model assumption; it does not execute firmware or replay its upload.

The lifecycle passed calibration/MAC/version retention, late-binding serialization,
foreign-interface result preservation, BSSID/SSID/RSSI decoding, backpressure,
summary acknowledgment, malformed information elements, failed final status,
missing confirmation, and partial transmission. A focused follow-up passes final
and intermediate confirmation deadlines that expire across a state transition.
The already-passing lifecycle and source transcript were reused.

Evidence is under `~/.cache/sunstoneos/evidence/radio-wifi-20261016/`:
`model/` holds the source structures/transcript; `model-corrected/` holds the
passing lifecycle; `completion-deadline/` holds the focused deadline check.
`aarch64/` records successful freestanding compilation of the changed Wi-Fi and
transport sources, with compiler commands and object hashes. `final-inputs.json`
records final file identities and which evidence applies. The earlier binding
vectors and unchanged SDIO/bootstrap/firmware evidence were reused.

```sh
python3 checks/aic8800-wifi.py \
  --reference-dir /path/to/pinned/aic8800_fdrv \
  --binding-vectors /path/to/verified/host/binding-vectors.bin \
  --evidence /path/outside/repository/radio-wifi-evidence
```

All source references use kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The runner records complete source,
header, helper, transcript, and executable hashes. The source files and reference
helper remain outside Git, preserving the downloaded license/copyright notices.

Physical execution still needs qualified recovery, platform bindings, approved
module RF settings, current channel authority, and explicit admission of the
board operation. Full authentication/security, network data paths,
full capability configuration, UART/HCI, sleep/wake, and interrupt service remain
required follow-on work. No physical RF transmission, board access, or performance
measurement occurred during this implementation.

[rf]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/aicwf_8800d80.c
[main]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/rwnx_main.c
[tx]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/rwnx_msg_tx.c
[rx]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/rwnx_msg_rx.c
[messages]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/lmac_msg.h
