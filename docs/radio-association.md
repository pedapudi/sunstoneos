# Native station association and disconnection control

SunstoneOS implements bounded AIC8800D80 station association and disconnection
through its [Wi-Fi management worker](radio-wifi.md). The source ABI comparison
and integrated host model pass. Physical association and network operation remain
unqualified. Neither command acceptance nor successful association establishes
usable networking.

The initial profiles are open association and WPA2-PSK with CCMP association.
This worker ends a WPA2 association at `SECURITY_PENDING`. The
[RSN supplicant](radio-security.md) then runs the four-way handshake, installs
the keys and opens the controlled port, which makes the link `SECURED`. Open
association also requires the remaining network data path before use. Roaming,
WPA3 and Bluetooth support remain part of the OS objective.

## A connection requires an explicit target and transmission authority

The caller supplies a target BSSID, SSID, channel index, security profile,
connection-authority record, and evidence of an observed matching beacon.
The channel index selects the immutable catalogue admitted during management
initialization. The worker copies the target before encoding a request.
Its BSSID must be nonzero and unicast; its SSID contains one to 32 bytes.
Wildcard targets, automatic selection, and automatic reconnect are unsupported.

The authority and beacon records identify independently established platform
facts. A scan result, numeric record, or chip identity cannot itself establish
permission to transmit. The platform must verify that the observed access point,
channel rules, and approved operation allow active association. No physical
network connection is authorized or attempted by the host check.

The request preserves the catalogue's channel frequency, band, maximum power,
and `NO_IR`/radar flags. It does not clear channel restrictions to enable a
connection. Actual firmware behavior under those restrictions remains a physical
qualification requirement.

The transport admits a connection only at a drained receive boundary with no
held message, controller request, or earlier management operation. The station
must be disconnected. The copied target and current interface index remain owned
through completion or failure. Scan and a second connection are refused while
the station is associated, awaiting security, disconnecting, or unresolved.

## Firmware acceptance and association completion are separate

The [published sender][tx] and [message structures][abi] define these exchanges:

| Message | ID | Native handling |
| --- | --- | --- |
| Connect request | `0x1800` | Encode the 320-byte station request into a fixed 512-byte transport packet |
| Connect confirmation | `0x1801` | Require its one-byte status; zero admits the procedure, other values retain a refusal event |
| Connect indication | `0x1802` | Validate the 852-byte association result and retain its status, peer, channel, and association information elements |
| Disconnect request | `0x1803` | Encode the four-byte reason/interface structure |
| Disconnect confirmation | `0x1804` | Retain its bounded opaque payload; the source sender defines no checked status layout |
| Disconnect indication | `0x1805` | Require the six-byte layout and preserve reason, interface, fast-transition, and reassociation fields |

Connect requests use open-system 802.11 authentication, a host-controlled port,
unencrypted control-port frames, and disabled HT capability. The control-port
EtherType is EAPOL `0x888e` in network byte order. Listen interval, multicast-wait,
UAPSD queue mask, and unused fields are zero for this explicit legacy profile.
The management worker has already disabled power saving and HT/VHT/HE claims.

The open profile includes no security information element. The WPA2 profile
also sets the source's WPA/WPA2 flag and includes a canonical RSN information
element: version 1, CCMP group cipher, one CCMP pairwise cipher, one PSK key
management suite, and zero RSN capabilities. Protected management frames are
not advertised. These bytes request a profile; they do not prove negotiation,
authentication, or key establishment.

A successful connect confirmation starts no network-ready state. The worker
waits for the association indication and preserves the confirmation deadline
through the next clock observation. An indication may arrive first; its result
remains retained while the worker waits for the confirmation.

On successful association, the indication must match the selected BSSID,
frequency, band, and interface. Its access-point index must fit the firmware's
station limit. The reported width may be any defined channel width (20, 40, 80,
160 or 80+80 MHz) and is kept in the event: the reference uses it without a check,
and the firmware may report the access point's own width for this legacy profile.
The worker reports `ASSOCIATED_OPEN` or `SECURITY_PENDING` according to the
selected security profile. A nonzero association status is reported with the
link disconnected; no automatic retry occurs. Connect confirmations reporting
busy, operation in progress, or an unknown status leave the link unresolved.
Those statuses cannot establish that firmware has no outstanding procedure.

Both association information-element lengths must fit the source's shared
800-byte area. Each element's declared length must fit its containing sequence.
The complete indication remains available alongside parsed metadata, including
association ID, station/channel indices, QoS fields, and EDCA parameters.
ABI padding bytes are retained without invented zero-value constraints.

## The association path against the reference

This table compares the native association path with the pinned vendor driver
(`aic8800_fdrv` at kernel revision `2ac08e8c7cdc`, `rwnx_msg_tx.c`,
`rwnx_main.c`, `rwnx_tx.c`, `rwnx_msg_rx.c`, `lmac_msg.h`) and wpa_supplicant
2.12 (`src/rsn_supp/wpa.c`). The firmware itself is not available as source, so
its reaction to the differences marked "open" is a board observation.

| Step | Reference | Native | Status |
| --- | --- | --- | --- |
| Connect request layout | 320 bytes; SSID at 0, BSSID 34, channel 40, flags 48, ethertype 52, IE length 54, authentication type 56, interface 61, IEs 64 | The same offsets; unwritten fields zero in both | Same |
| Connect flags | `CONTROL_PORT_HOST` and `WPA_WPA2_IN_USE` (9); `DISABLE_HT` only for WEP or TKIP; `CONTROL_PORT_NO_ENC` only for IEEE 802.1X without WPA | 15: adds `CONTROL_PORT_NO_ENC` and `DISABLE_HT` for the legacy profile | Open. The first handshake is unaffected. With `NO_ENC`, EAPOL stays unencrypted after key installation, which a group or pairwise rekey may need to change |
| Channel flags and power in the connect request and the channel configuration | `get_chan_flags()` returns 0 in the shipped build (`RADAR_OR_IR_DETECT` off); transmit power 0 in the connect request | `NO_IR` on every channel, and the catalogue's maximum power | Open. A firmware that honours `NO_IR` could withhold authentication frames: the connect would then time out or be refused |
| UAPSD queues | Voice queue bit set by default | 0 | Cosmetic for association |
| RSN element | The supplicant's element, copied into the request | 22 bytes: version 1, CCMP group and pairwise, PSK, no capabilities; the same bytes in message 2 | Same for a WPA2-PSK CCMP access point |
| Connect indication | Uses status, peer, channel and width without checks | Requires the selected BSSID, frequency, band and interface and an access-point index below the station limit; accepts any defined width | Same acceptance for a successful association |
| EAPOL transmit | Host flags 0; the firmware encrypts by connect flags and key state; the confirmation status is ignored for EAPOL on SDIO | Host flags 0, confirmation requested by host identifier bit 31; a processed frame completes whether or not it was acknowledged | Same |
| Early message 1 | Delivered to the supplicant by the network stack | Delivered once the supplicant is attached. A frame that arrives between the connect indication and the attachment is consumed as a dispatcher event and dropped; the access point retransmits it after about one second | Open: adds up to one retransmission interval |
| Key installation | Message 4 first, then the pairwise key (`MM_KEY_ADD_REQ`, key index 0, the access point's station index, CCMP 2), then the group key (key index 1 to 3, station index 0xFF) | The same order; the key add waits for message 4's confirmation | Same |
| Controlled port | `ME_SET_CONTROL_PORT_REQ` (0x1404) with the station index and open 1, after the supplicant completes | The same request after every queued action completes | Same |

## Disconnection events preserve ordering and ownership

The caller may request disconnection from an associated or security-pending
station. It supplies a nonzero reason and an absolute deadline. The worker
requires both command confirmation and the disconnect indication before the
requested operation completes, in either order.

An unsolicited disconnect immediately changes the recorded link state and
retains its reason. If a connection or disconnection command is outstanding,
that command retains ownership of its confirmation and deadline. The event does
not silently cancel a partially transmitted request or authorize a replacement.

A disconnect marked as fast transition or reassociation leaves the link
`LINK_UNRESOLVED`. A same-interface connect indication marked as roaming also
leaves the link unresolved and remains a raw retained message. The native worker
cannot infer completion of those unsupported procedures. New scans, connections,
and disconnection requests remain refused until a future implementation resolves
the transition or external recovery establishes a fresh device lifetime.
Ordinary confirmations and indications for an earlier request cannot clear
this unresolved state, even when they arrive after the roaming event is consumed.

Foreign-interface indications and unrelated messages remain raw records for
explicit consumption. Unexpected external-authentication messages also remain
available to a future authentication handler. They do not imply WPA success.

`aic8800_transport_peek_station` returns a borrowed event and payload view.
The caller must copy anything needed asynchronously before generic transport
consumption. One held event blocks aggregate reuse and further transfers.
Consumption acknowledges storage ownership only; it does not authenticate the
peer or permit protected traffic. Output views cannot alias retained radio
objects or firmware artifacts.

## Failures retain state and outstanding resources

Every command has a deadline inside its operation deadline and the runtime
receive lease. A successful acceptance ends only the command timeout; the
operation remains bounded while awaiting its final indication. Consumer
backpressure and binding pauses continue to count against those deadlines.

Response completion preserves the active deadline through the transport's
post-processing clock observation. A late final indication therefore cannot
escape timeout by changing the worker to its idle state. Malformed lengths,
contradictory acceptance, unexpected successful peer/channel data, partial
transmission, or deadline failure retain the first fault, copied target,
transmitted request, received aggregate, and any held event.

A faulted transport performs no further backend access. It offers no reset,
retry, automatic reconnect, credential fallback, or live-object reuse.
External recovery must prove that firmware and controller activity have stopped
before disposing of retained state.

## Security and network work remains explicit

The pinned ABI defines separate key-add and key-delete operations. Key addition
contains the cipher, station/interface identity, pairwise/group selection, and
key material; its confirmation supplies status and a hardware key index.
The Linux caller installs keys separately from station association. The source
also uses external authentication for SAE and separate responses for fast
transition. An association indication cannot substitute for those operations.

Required follow-on work includes the supplicant and EAPOL path, cryptographic
handshakes, validated key installation/deletion and lifetime, replay handling,
controlled-port authorization, protected management frames, supported WPA3 and
enterprise profiles, firmware-initiated roaming and fast transition, and the
network data stack. The [radio session](radio-session.md#explicit-healthy-reconnect-preserves-the-existing-service)
moves between access points of one network with an ordinary disconnection and
a new association. Credentials and key
material are absent from the present API and evidence.

## Focused source and host evidence

no host check verifies the
same pinned source hashes as Wi-Fi initialization. It compiles the unmodified
full-MAC headers with fixed-width type shims and constructs source-layout
requests and an association indication outside Git. Independent framing produces
three complete expected packets: open association, WPA2 association, and
disconnection. The indication includes nonzero unspecified padding.

The integrated model uses the real controller, card, runtime, transport, and
Wi-Fi worker under address and undefined-behavior sanitizers. It starts from an
explicit model assumption that enumeration, bootstrap, firmware startup, and
management initialization have completed. Those earlier sequences are not
replayed, and no firmware executes in this model.

The composition passes target copying, refusal without beacon evidence, both
association profiles, confirmation/indication ordering, foreign-interface
preservation, disconnect ordering, result ownership, refusal status, association
failure, malformed lengths, mismatched peer, partial transmission, missing final
indication, and late completion. A focused follow-up passes unresolved fast
transition, reassociation, and roaming admission. Earlier initialization, scan,
binding, and firmware evidence is reused.

Evidence is under `~/.cache/sunstoneos/evidence/radio-association-20261016/`.
`abi/` records source structure sizes and offsets; `model/` contains the source
comparison and passing composition; `transition/` contains the focused
unsupported-transition cases. `busy/` covers firmware busy, operation-in-progress,
and unknown refusals. `unresolved/` covers roaming followed by a refused or
successful earlier connection request. `aarch64/` records successful freestanding compilation of
the changed worker and transport. `final-inputs.json` ties final file identities
to the retained evidence. Source and artifact hashes accompany each result.

```sh
python3 no host check \
  --reference-dir /path/to/pinned/aic8800_fdrv \
  --evidence /path/outside/repository/radio-association-evidence
```

No physical radio connection, firmware upload replay, or performance measurement
occurred. The source references below use kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.

[abi]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/lmac_msg.h
[tx]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/rwnx_msg_tx.c
