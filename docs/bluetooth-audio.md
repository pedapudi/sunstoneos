# Bluetooth headset audio over HCI

The synchronous audio worker binds an established Hands-Free Profile (HFP)
audio-gateway session to SCO or eSCO transport. It exchanges signed 16-bit,
8 kHz PCM samples through standard HCI commands and H4 UART packets. CVSD is
the radio codec. The implementation is host-model work; controller acceptance,
headset interoperability and microphone capture require physical qualification.

The Orange Pi is the audio gateway and the headset is the hands-free device.
HFP 1.9 §4.11 permits an audio connection without an active call. Audio admission
requires the actual AG service-level connection, retained RFCOMM/L2CAP/ACL
identity and a current security provider. It does not change call or service
indicators. Pairing, keys and encryption remain required platform work.

## Controller configuration precedes every connection

The default Classic configuration has eight commands. Selecting
`bt_hci_incoming_config.synchronous_audio` inserts four commands before page
scan: synchronous flow-control enable/readback and erroneous-data-reporting
enable/readback. Scan and its readback remain last. Synchronous flow control can
change only when the controller has no connections.

Audio admission requires reported HCI version 4.1 through 5.4, SCO and CVSD
features, nonzero synchronous buffers, and the supported-command bits for every
additional operation. Unsupported identity is refused before configuration
mutation. A successful write without the matching readback cannot establish
`synchronous_ready`.

| Operation | HCI opcode | Ownership/result |
| --- | --- | --- |
| Write/read synchronous flow control | `0x0c2f` / `0x0c2e` | Complete and enabled readback before any ACL |
| Write/read erroneous-data reporting | `0x0c5b` / `0x0c5a` | Complete and enabled readback before any ACL |
| Read remote supported features | `0x041b` | Status followed by the matching ACL feature event |
| Enhanced synchronous setup | `0x043d` | Status followed by synchronous Connection Complete |
| Enhanced synchronous accept | `0x043e` | Actual incoming request, then Status and Connection Complete |
| Reject synchronous request | `0x042a` | Actual incoming request, then Status and failed Connection Complete |
| Disconnect | `0x0406` | Status followed by the matching synchronous disconnect event |

Local feature bits do not establish remote capabilities. The audio worker
queries the peer through the same HCI command owner before setup or acceptance.
An incoming request during that query is copied and retains its response bound.
The scheduling authority must prove that classification and response fit the
read-back controller acceptance timeout.

## Audio uses the standard HCI data path

Enhanced setup and accept select input/output data path zero and transport-unit
size zero, as Core 5.4 §7.1.45 requires for HCI. Both air coding formats are CVSD;
both host coding formats are linear PCM with 16-bit two's-complement samples.
Radio bandwidth is 8,000 octets/s in each direction; host PCM bandwidth is
16,000 octets/s in each direction. The CVSD frame-size fields are 60 bytes.

The fixed profiles follow HFP 1.9 §6.7:

| Profile | Radio packets | Maximum latency | Retransmission effort |
| --- | --- | --- | --- |
| S1 | EV3 | 7 ms | 1 |
| S4 | 2-EV3 | 12 ms | 2 |
| D1 | HV3 | 5 ms | 0 |
| D0 | HV1 | 4 ms | 0 |

An ordinary CVSD link uses S1 when both controllers report eSCO. A confirmed
terminal setup failure permits D1, then D0, when SCO is legal. A command refusal
or uncertain timeout never enters that fallback. A security provider that
reports AES-CCM Secure Connections requires S4 and forbids SCO fallback.
Incoming SCO under that security mode is rejected with reason `0x0e`.

Successful Connection Complete must report the admitted transport and CVSD air
format. SCO interval and radio packet lengths must be zero. An eSCO connection
must report a nonzero interval and packet lengths. Successful remote eSCO
reconfiguration updates the reported interval, retransmission window and packet
lengths; these are separate from HCI packet lengths.

The module also has wired PCM signals on I2S1 pins PG11–PG14. Board wiring and
an enabled PCM build option do not identify the controller's routing command or
framing. This worker therefore does not configure that physical path. Controller-specific physical PCM routing and LC3-SWB remain unimplemented.
The optional [native mSBC path](bluetooth-native.md#wideband-capture-through-the-existing-recognizer)
uses transparent HCI data, explicit codec negotiation and copied16kHz PCM.
Controller support and physical operation remain unqualified.

## Credits and retained records have separate owners

ACL and synchronous packets use separate controller credit pools. The shared
Number Of Completed Packets dispatcher validates every handle, duplicate and
count in an entire event before mutating either pool. Synchronous packets
retire only through their own handle and generation. UART acceptance does not
return controller credits or prove peer playback.

A successful disconnect returns credits for fully submitted synchronous
packets and leaves their copied results marked `FLUSHED`. That result records
uncertain delivery. Queued packets become `NOT_SENT`. A partially transmitted
H4 frame makes disconnect uncertain and retains the complete stack lifetime.
Unknown successful synchronous connections fault with their event retained;
unrelated failure and late records remain owned without assigning a new handle.

The worker copies up to eight TX and eight RX records. TX accepts complete
16-bit samples, with even length from 2 through the lesser of the reported
controller capacity and 254 bytes. The caller supplies actual playback or
silence samples. RX preserves HCI packet segmentation, including odd lengths,
and packet status: good, possibly invalid, lost, or partly lost. The consumer
must carry incomplete samples across packet boundaries and preserve loss
metadata. Acquisition serials are host ordering information, not RF timestamps.

When copied RX storage is full, the exact already-classified H4 record remains
owned. Known held records acquired before a confirmed disconnect can still
drain afterward; the disconnect serial excludes newly acquired late packets. The worker retries only those recorded serials, in order. An unrelated
old packet cannot acquire a later audio handle merely because capacity returns.
Generic lower record consumers must leave these synchronous records to the
audio owner. RTS/CTS, UART service latency and buffer capacity need an actual platform
scheduling proof; a scalar supplied by a caller does not establish it.

## The owner task retains the complete lifetime

One task serializes all Bluetooth workers. Register one audio manager after
optional configuration and ACL handoff. The manager, AG sessions, callbacks,
security contexts and all lower backing remain alive through controller
recovery. They must occupy disjoint storage. Public fields are read-only.

Each audio poll performs one lower step, bounded fixed-size scans and at most
one new command or data submission. Other serialized upper workers may advance
HCI. ACL invokes the audio preparation callback before and after HCI advancement
so a completed synchronous UART submission becomes visible before credit
processing. A pending synchronous acknowledgement prevents another ACL command
from replacing its HCI ownership before classification.

Close the synchronous link before locally closing its parent AG or ACL.
Security revocation, parent loss, deadline expiry and transport failure retain
all unresolved ownership. A late observation cannot publish a usable audio
link. Confirmed closure retains terminal TX results and received audio until
explicit release.

Four peer/audio identities are available per controller lifetime. Closed peers
and handles remain quarantined. This finite bound prevents ambiguous delayed
handle reuse; sustained reconnect support requires a separate proven identity
policy or actual controller recovery. Audio control does not complete the
remaining mandatory HFP call-control functions or establish headset compliance.

## Source and validation scope

The original Apache-2.0 runtime implements wire semantics from Bluetooth Core
5.4, Volume 4, Part E §§5.4.3, 7.1.28, 7.1.45–46, 7.3.36–37 and 7.7.35–36,
and HFP 1.9 §§4.11 and 6.7. Supported-command positions come from the Core HCI
command table. It contains no copied Linux implementation code.

The pinned Linux headers provide independent packed layout comparisons in the
external fixture. Their original license notices remain with those extracts.
The published `hci_conn.c` CVSD setup provides a second reference for codec
fields and safe profiles. The fixture uses zero transport-unit sizes for the
standard HCI path required by the primary specification.

The focused composition uses the actual UART, H4, configured HCI, incoming ACL,
SDP, RFCOMM and AG workers. It covers audio configuration/readback, complete
enhanced-command bytes, incoming and outgoing setup, security-dependent
fallback, typed mixed credits, loss metadata, retained buffers and faults.
Native compilation covers changed runtime objects and consumers of the changed
HCI/ACL layout. Earlier Bluetooth scenario suites are reused without replay.
Physical headset and board checks are pending coordinator-owned recovery and
security/scheduling prerequisites.

Primary references: [Core 5.4 HCI specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host-controller-interface/host-controller-interface-functional-specification.html),
[HFP 1.9](https://www.bluetooth.org/DocMan/handlers/DownloadDoc.ashx?doc_id=574215),
and the pinned [published CVSD setup](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/net/bluetooth/hci_conn.c).


The native recognition application also supports [successive copied CVSD
segments](bluetooth-native.md#repeated-headset-capture-and-recognition) within
one bounded supervised session. Two input slots and exact transcript delivery
acknowledgments provide backpressure; capture stops on backlog or loss. This
application extension keeps the existing CVSD negotiation and disconnect path.
Physical capture and wideband mSBC remain unqualified.
