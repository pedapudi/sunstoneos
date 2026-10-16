# RFCOMM serial channels over native Bluetooth

RFCOMM carries serial-port data over Bluetooth. The client implements one
outbound serial data channel over the native
[L2CAP worker](bluetooth-l2cap.md). It selects an advertised server channel from a
completed [SDP query](bluetooth-sdp.md), establishes the RFCOMM multiplexer,
negotiates credit flow control, and exchanges modem status before accepting data.
This is software implementation work. Bluetooth headset operation, authentication,
HFP control, voice transport and physical qualification remain outstanding.

## Admission binds the service and transport

A caller initializes `bt_rfcomm_registry` before L2CAP and installs
`bt_rfcomm_admit` as the L2CAP authorization callback. The registry reserves one
PSM 3 transport per link before opening it. A protocol/service multiplexer
(PSM) selects the service carried by an L2CAP channel. Only that serialized acquisition can
pass the PSM 3 callback. Incoming PSM 3 sessions, including simultaneous connection
attempts, are refused. Other PSMs retain the supplied policy's result, so an
RFCOMM session does not exclude SDP or unrelated L2CAP services.

The registry and its callback context must outlive the L2CAP worker, even after
all RFCOMM sessions close. The sole task owner serializes every worker and public
operation. Callback reentry is forbidden. Session, registry, discovery, lower
workers, retained buffers and caller input/output storage must be disjoint.
A fault preserves the session reservation. Confirmed channel close and explicit
release are required before another session can use it.

The separate service authorization callback must establish the actual profile's
security requirements on the current link. There is no default authorization.
The worker checks authorization before opening, sending and stepping transport.
Revocation faults the session before another lower step. Already accepted lower
copies remain owned and need qualified recovery. SDP identifies a remote service;
it does not establish authentication, encryption or permission to use it.

The client accepts a matching SDP service record and a server channel from 1 to 30.
The descriptor must specify PSM 3 or omit the standard PSM parameter. It copies
the selected identity; the SDP result can then be released. The lower link's
immutable generation and channel generation bind all subsequent operations.
The existing ACL peer/handle quarantine and finite L2CAP identifiers still apply.

## Setup and frame acceptance follow the negotiated mode

Setup sends the channel-opening command SABM on the multiplexer channel,
negotiates parameters, opens the data channel, and exchanges Modem Status Commands
in both directions. SABM and its acknowledgement UA establish a channel; they do not retire controller transmit buffers.
A response is eligible only after complete UART acceptance of its request and
acquisition of a newer H4 record. L2CAP records the acquisition boundary and
monotonic timestamp in microseconds at complete acceptance. These retained values also support
another serialized service advancing the transport before RFCOMM next runs.
The check establishes acquisition order. It cannot
identify arbitrary delayed duplicate peer responses that carry no transaction ID.
The admitted lower transport must preserve order without duplicate delivery.

The client emits one RFCOMM frame per L2CAP packet. It checks the frame's address,
command/response direction, control value, encoded length and frame check sequence
(FCS). Unnumbered Information with Header check (UIH) data frames cover address and control in the FCS; channel-control
frames also cover length. The implementation uses the protocol polynomial
directly. It does not copy the Linux FCS table into runtime code.

The negotiated maximum data length is 23 to 512 bytes. L2CAP must accommodate that
length plus framing. Two-byte lengths support data larger than 127 bytes. The
initial component requires credit flow; a legacy flow negotiation is reported as
unsupported. Extra data channels receive the Disconnected Mode response DM, leaving the selected
channel operational. Full legacy flow, incoming sessions, multiple data channels
and channel renegotiation that changes an open channel's size remain required.
These limits prevent a claim of complete RFCOMM conformance.

TEST requests are echoed. Remote Port Negotiation reports fixed virtual-port
settings of 9600 baud, eight data bits, no parity, one stop bit and no flow control.
Its response mask accepts only supported settings. These virtual settings do not
change the physical Bluetooth UART. Remote Line Status is acknowledged and
retained for the application. Unknown multiplexer commands receive NSC. Global
legacy flow commands are unsupported in the negotiated credit mode.

## Credits reserve buffers independently at each layer

Four receive records each retain up to 512 data bytes. Initial parameter
negotiation grants four credits. A nonempty peer data frame consumes one credit;
releasing its record permits a replacement grant. Queued grants reserve
available records. They become issued credits only after
complete UART acceptance. Each issued credit retains that acquisition boundary,
so an earlier received frame cannot consume a later grant.
A credit-only frame needs no peer transmit credit and consumes no receive record.
A data frame carrying a credit byte has one less byte of payload capacity.

Four application transmit records hold copied frames, and four separate control
records hold negotiation, responses and grants. Serial-data submission spends a
peer RFCOMM credit once when the lower layer accepts its copy. Controller ACL
credits remain owned by the lower worker until its completion event. RFCOMM
credits, serial acknowledgements and channel-control responses cannot release
those controller credits.

The caller releases each received record and terminal transmit result explicitly.
A retired transmit result means controller ownership ended. It does not prove
remote application delivery. Queued data can expire without transmission;
already accepted lower copies can finish after the caller's deadline. Their
storage remains retained until actual lower retirement. Counter exhaustion,
malformed frames and uncertain lower results produce retained faults.

## Deadlines bound waiting without releasing uncertain ownership

The control-response timer is 20 seconds; opening the data channel uses 60 seconds.
Both begin after the complete request reaches the UART. There are no automatic
retries. An independent caller deadline bounds the entire opening sequence.
Protocol or opening expiry requests lower channel closure under a separate
20-second close bound. Failure to confirm closure retains the session.

Normal close sends the disconnect command DISC and waits for UA. A peer DISC receives UA. Unsubmitted
application data is cancelled and unsubmitted obsolete control frames are
removed. Submitted frames retain their lower ownership. Already credited peer
data can
arrive before the closing UA and remains readable through close. No replacement
receive credits are granted while closing. The worker waits for all submitted
frames to retire before initiating L2CAP disconnection; changing the
lower channel state earlier would invalidate those active application transfers.
A confirmed lower close also requires lower buffer release before `CLOSED`.
Application receive records and terminal results remain readable afterward.

Each step performs one lower worker step, examines at most one received frame,
and submits at most one new frame. Fixed arrays bound every scan and parse.
Control capacity exhaustion retains the received frame for a later step.
The current platform contract remains task-polled UART with its interrupt source
masked; native scheduling, radio initialization and recovery need qualification.

## Source provenance and focused validation

[RFCOMM1.2](https://www.bluetooth.org/docman/handlers/DownloadDoc.ashx?doc_id=263754)
and its referenced
[GSM07.10 version 6.3](https://www.etsi.org/deliver/etsi_ts/101300_101399/101369/06.03.00_60/ts_101369v060300p.pdf)
define the runtime protocol. RFCOMM sections 4–6 specify framing overrides,
connection establishment, negotiation, timers and credit flow. GSM sections 5.2,
5.4.3 and 5.4.6 supply the basic frame, command/response and control-message rules.
The [adoption page](https://www.bluetooth.com/specifications/specs/rfcomm-1-2/)
also lists mandatory errata for a complete compliance claim.

The published Linux header and core at revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` provide independent wire constants,
packed layouts and an FCS reference. The check extracts those reference portions
outside the repository with their original notices. Original runtime code uses
Apache-2.0; copied reference material keeps its source license. Source hashes,
raw inputs, compiled artifacts and commands belong in the external evidence
manifest.

The focused check composes actual SDP, RFCOMM, L2CAP, ACL, HCI and UART controllers.
It covers source-byte frame comparison, negotiated setup, distinct credit
ownership, data retention, local and peer close, session collision, security
revocation, malformed FCS and deadline expiry. Existing lower test mains are
compiled for reusable helpers and are not executed. The composed model passed
with AddressSanitizer and UndefinedBehaviorSanitizer enabled. AArch64 compilation
passed for RFCOMM, the changed L2CAP layout and its SDP consumer.

The successful evidence is
`~/.cache/sunstoneos/evidence/bluetooth-rfcomm-20261016/model2/results.json`.
It preserves executed inputs, binaries and source reference material. An earlier
compile stopped on two formatting warnings; its separate directory preserves
those inputs. No physical radio operation or performance measurement was run.
