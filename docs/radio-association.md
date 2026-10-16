# Native station association and disconnection control

SunstoneOS implements bounded AIC8800D80 station association and disconnection
through its [Wi-Fi management worker](radio-wifi.md). The source ABI comparison
and integrated host model pass. Physical association and network operation remain
unqualified. Neither command acceptance nor successful association establishes
usable networking.

The initial profiles are open association and WPA2-PSK with CCMP association.
The WPA2 profile stops at `SECURITY_PENDING`: no supplicant handshake, credentials,
key installation, or protected data path is implemented by this component.
Open association also requires the remaining network data path before use.
Full authentication, encryption, network delivery, roaming, and Bluetooth support
remain part of the OS objective.

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
station limit, and its width must match the admitted 20 MHz profile.
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
enterprise profiles, roaming, and the network data stack. Credentials and key
material are absent from the present API and evidence.

## Focused source and host evidence

[checks/aic8800-association.py](../checks/aic8800-association.py) verifies the
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
python3 checks/aic8800-association.py \
  --reference-dir /path/to/pinned/aic8800_fdrv \
  --evidence /path/outside/repository/radio-association-evidence
```

No physical radio connection, firmware upload replay, or performance measurement
occurred. The source references below use kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.

[abi]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/lmac_msg.h
[tx]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/rwnx_msg_tx.c
