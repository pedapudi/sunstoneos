# SunstoneOS provides the Audio Gateway role for a headset

For headset microphone capture, SunstoneOS is the Hands-Free Profile (HFP)
Audio Gateway (AG). The headset is the Hands-Free unit (HF), which supplies
remote audio input and output. SunstoneOS receives and interprets the headset's
AT commands and sends responses. It does not become an HF merely because it
initiates the Bluetooth or RFCOMM connection.

This role follows HFP sections 2.2 and 2.4. The intended application is microphone
capture for mimus, rather than cellular telephony. Section 4.11 permits an audio
connection without an active call. The implementation must report actual call
and service state; microphone capture does not justify inventing an active call.

The implementation provides [SDP discovery](bluetooth-sdp.md),
[local AG service discovery](bluetooth-sdp-server.md),
[incoming ACL admission](bluetooth-incoming.md),
[RFCOMM serial transport](bluetooth-rfcomm.md) and an initial AG command engine.
Actual security admission and synchronous audio transport remain required.
Successful AT exchange establishes control state only. Headset audio requires
a separate controller and application path.

## Either device can initiate the control connection

HFP sections 4.2 and 6.1 require both AG and HF to support RFCOMM initiation,
acceptance, client and server roles. Section 6.2 requires both L2CAP directions.
The [RFCOMM worker](bluetooth-rfcomm.md) supports an outbound client selected by
remote SDP discovery and an incoming listener on an existing authorized link.
One data channel is admitted per session. Legacy flow control and multiple data
channels remain limitations of that transport implementation.

SunstoneOS discovers the headset's Hands-Free service, UUID `0x111e`. For a
headset-initiated connection, SunstoneOS must publish the AG service, UUID
`0x111f`, with its local RFCOMM server channel. The record also needs the Generic
Audio class, protocol descriptors, Hands-Free profile descriptor, Network and
SupportedFeatures attributes specified by HFP section 6.3. The advertised feature
bits must agree with implemented behavior and the AT feature exchange.

The local SDP server answers discovery requests and retains response and
continuation ownership. Its profile configuration must match the active AG
listener. Incoming ACL handling reserves an authorized peer and processes actual
connection request, command status and connection completion events. Existing
finite ACL peer and handle lifetimes still constrain reconnect behavior.

The RFCOMM registry can delegate unrelated PSM authorization to the SDP server.
Both components must preserve the actual link and service security policy.
Authentication, encryption state, key storage and controller generation are real
inputs to that decision. No default callback may substitute for pairing or a
qualified security provider.

## The command engine establishes initial AG control state

The runtime in [bt_hfp_ag.c](../drivers/bluetooth/bt_hfp_ag.c) implements the AG
side of the service-level connection, the HFP control state established over
RFCOMM. Its interface is [bt_hfp_ag.h](../include/bt_hfp_ag.h). Without call
control or codec negotiation, the supported AG feature bitmap is zero, and the
local SDP record must also use zero supported features and Network zero.
The bitmap names only implemented behavior and makes no qualification claim.

The caller binds an authorized open RFCOMM session. An incoming session must
serve AG UUID `0x111f`; an outgoing session must target HF UUID `0x111e`.
The engine retains the session, registry, link and channel identity. One task
owns the stack, and the engine exclusively consumes that session's application
RX and TX until release. Other services may advance their own lower workers
serially. Existing receive data can be adopted, including an immediately sent
`AT+BRSF`; existing application transmit ownership is refused.

The initial command sequence is:

1. `AT+BRSF=<features>` records the headset's 32-bit feature value and returns
   `+BRSF: <AG features>` followed by `OK`. The value is 0 for the basic
   engine, 512 with codec negotiation, 352 with call control and 864 with both.
2. `AT+CIND=?` returns the seven supported indicators and their order.
3. `AT+CIND?` returns their current values.
4. `AT+CMER=3,0,0,1` enables indicator reporting. The engine enters `READY`
   after the entire final `OK` has been accepted by the UART transport.

The indicator order is service, call, call setup, held call, signal, roaming and
battery charge. The caller supplies actual state; initialization has no invented
network, active call or battery default. Caller updates produce `+CIEV` reports
when reporting and that indicator are enabled. Call changes precede call-setup
changes. Updates can coalesce while another response occupies the serial stream.

`AT+BIA` changes individual reporting states. Omitted entries retain their state,
valid excess entries are ignored, and the call, call-setup and held-call
indicators remain enabled. Disabling `CMER` suppresses reports without losing the
BIA mask. Omitted CMER parameters retain accepted values; irrelevant empty
fields are supported. Mode three must be established before enabling reports.
`CIND?` still returns every indicator. The engine retains valid
`AT+VGM` and `AT+VGS` headset gain reports, with values from zero through fifteen.
Those reports do not change a local codec or establish an audio path.

Commands arrive as a stream terminated by carriage returns. The parser retains
incomplete lines across RFCOMM records and handles successive lines in one
record. Command names are case insensitive and spaces are ignored. A line holds
at most 128 non-space bytes. An overlong or malformed line is consumed through
its terminator and receives `ERROR`; its prefix cannot execute as a command.
Unknown commands and unsupported codec or voice requests also receive
`ERROR`. Without call control, call commands also receive `ERROR`; the next
section describes the procedures that call control adds.

Each response uses at most 256 bytes and splits at the negotiated RFCOMM MTU.
Only one fragment is owned by RFCOMM at a time; the next waits for controller
retirement. This bounds storage and preserves response order under credit
backpressure. UART acceptance establishes local response progress. Controller
retirement releases copied transport storage; neither observation proves that
the headset application processed a response.

Each worker call advances the lower stack once, consumes at most the requested
one to 512 input bytes, handles at most one complete command and submits at most
one response fragment. The UART budget remains one to 64 bytes per lower call.
The caller supplies an absolute setup deadline and a line/response duration from
one to 60,000,000 microseconds. Both use the UART clock. Expiry retains partial
input, responses and lower-owned bytes. It does not permit a late `READY` result.

Explicit close uses the existing bounded RFCOMM close. Received and transmitted
storage remains owned until confirmed lower closure and retirement. The AG then
detaches; its caller separately releases RFCOMM. Security revocation or lower
failure retains all unresolved backing for recovery. No default authorization,
call action, voice-recognition result or audio completion is supplied.

## Call control adds the mandatory call procedures

HFP Table 3.1 makes several call procedures mandatory for an AG: answering,
terminating and placing calls, last-number redial, memory dialing, call waiting
and calling-line notification activation, DTMF, subscriber number, current call
list, operator query and extended error codes. `bt_hfp_ag_init_calls` enables
them with a nonzero RING interval of at most 60 seconds. The AG then advertises
`BT_HFP_AG_CALL_FEATURES`, 352: ability to reject a call (bit 5), enhanced call
status (bit 6) and extended error result codes (bit 8). The local SDP record must
set Network to 1, which HFP section 6.3 defines as the ability to reject a call.
Three-way calling, in-band ringing, voice recognition and echo cancellation are
not advertised.

The engine holds at most one call. It owns the call, call-setup and held-call
indicators from initialization onward; all three must start at zero. The
held-call indicator stays zero because no second call can exist. A later
`bt_hfp_ag_indicators` update must repeat the engine's current call values or
it is refused. Service, signal, roaming and battery remain caller-supplied.

The application reports events that originate outside the headset through
`bt_hfp_ag_call`:

- `INCOMING` starts an incoming call with call setup 1. The caller may supply
  a number of 1 to 32 dial characters (`0`–`9`, `*`, `#`, `+` and the
  uppercase DTMF letters `A`–`D`) and a type from 128 through 175.
- `ALERTING` moves an outgoing call from setup 2 to setup 3.
- `ACTIVE` answers an incoming call at the AG or connects an outgoing call.
- `END` ends any call or call setup.

Each event returns 0 while a response occupies the serial stream, because a
response's state commits only after its UART acceptance. Invalid transitions
and malformed numbers are refused without a state change.

The headset drives the same call through these commands:

- `ATA` answers an incoming call. `AT+CHUP` rejects an incoming call, cancels
  an outgoing setup or ends an active call.
- `ATD<number>;` and `AT+BLDN` start an outgoing call with setup 2 and record
  the last dialed number. An `ATD` number uses the same dial characters as an
  incoming number. Both require service indicator 1 and no existing call.
  `ATD>` memory dialing always receives `ERROR` because the AG has no phone book.
- `AT+VTS=<digit>` is accepted only during an active call. The engine counts
  the digit for the application, which owns any call audio.
- `AT+CLCC` lists the call with index 1, direction, status, voice mode, no
  multiparty membership and its quoted number when known.
- `AT+COPS=3,0` selects the long alphanumeric format. `AT+COPS?` returns
  `+COPS: 0`: no network operator exists, so the format and name are omitted.
- `AT+CNUM` returns `OK` alone because no subscriber number exists.
- `AT+CLIP`, `AT+CCWA` and `AT+CMEE` accept 0 or 1. Call waiting never produces
  `+CCWA` because a second call cannot exist.
- `AT+BCC`, with codec negotiation, requests an audio connection. The engine
  replies `OK` and counts the request when no codec offer is outstanding; the
  application must then select a codec and open synchronous audio.

Indicator changes are reported after the command's `OK`, with call before call
setup. During an incoming call the engine sends `RING` after the call-setup
report, then once per configured interval until the call is answered or ends.
When calling-line notification is enabled and the number is known, `+CLIP`
follows each `RING`. After `AT+CMEE=1`, refusals caused by call state use
`+CME ERROR: 3` (operation not allowed) and dialing without service uses
`+CME ERROR: 30` (no network service). Syntax errors still use `ERROR`.

No telephony network exists on the board. Calls are application events, for
example a local session that the headset presents as a call. Call audio uses the
separate synchronous audio path below.

## Microphone samples require a separate synchronous audio path

HFP sections 2.3 and 4.11 distinguish the service-level control connection from
the audio connection. The latter requires a synchronous Bluetooth transport,
SCO or eSCO, and valid audio in both directions. The AG normally selects and
establishes the transport when codec negotiation is supported.

CVSD is the baseline codec. Wide Band Speech requires mSBC and its defined
16 kHz configuration; Super Wide Band Speech adds LC3-SWB. Each admitted codec
needs actual controller routing, packet framing, loss handling and conversion to
the application's sample format. The no-call capture path still requires
qualified audio setup, scheduling, buffering and shutdown.

The native application implements controller-bound CVSD capture and optional
[mSBC capture](bluetooth-native.md#wideband-capture-through-the-existing-recognizer)
through the existing repeated recognizer. Model checks establish the described
software behavior. Headset interoperability and real microphone samples require
physical qualification.

## Primary and implementation references

The primary reference is
[Hands-Free Profile 1.9](https://www.bluetooth.org/DocMan/handlers/DownloadDoc.ashx?doc_id=574215),
particularly sections 2.2–2.4, 4.2, 4.11, 5, 6.1–6.3, 6.7 and 7.2.
Omitted AT parameter syntax follows sections 3.1 and 4.1 of
[3GPP TS 27.007 version 6.8.0](https://www.etsi.org/deliver/etsi_ts/127000_127099/127007/06.08.00_60/ts_127007v060800p.pdf),
which HFP references.
The cached HFP 1.9 PDF has SHA256
`56f5fcce59cc4342c38d13ed44cc87b28ce73d1f2214dc909d121c378575fa41`.
It is stored outside the repository in
`~/.cache/sunstoneos/sources/bluetooth-hfp/`.

BlueZ revision `0845b8f6ef2ac004b1c953cf4fe4ca3458cd8e36` supplies published
`src/shared/hfp.h`, `src/shared/hfp.c` and `unit/test-hfp.c` references. Their
notices and source hashes remain in the same external cache. They provide
implementation comparisons; the source references do not establish SunstoneOS
HFP qualification or justify advertising unfinished capabilities.

## Validation composes the actual incoming stack

The focused runner [bt-hfp-ag.py](../checks/bt-hfp-ag.py) composes the UART,
H4, HCI, incoming ACL, L2CAP, local SDP, RFCOMM and AG runtimes. Its fixture
establishes an incoming link, reads the actual local AG record, negotiates the
advertised RFCOMM service and exchanges HFP commands. The expected SDP features
and AT responses are independently encoded from the pinned specifications.

The selected cases cover split and successive commands, MTU fragmentation,
indicator masks and ordering, gain reports, malformed input, response
backpressure, setup refusal, security revocation and retained close ownership.
The call-control case uses a record with Network 1 and checks incoming alerts
with `RING` and `+CLIP`, repeated ringing, answer, reject, termination, dialing,
redial, DTMF, the status queries, extended errors, application refusals and an
audio request with codec negotiation. Physical headset call handling remains
unqualified.
The runner also compiles the AG and lower consumers affected by the incoming
ACL structure changes for AArch64. Earlier component scenario mains are available
as helper sources but are not executed. The selected host assertions and native
compilation passed. Exact inputs, binaries, fixture diagnostics and reused
assertion evidence are preserved under
`~/.cache/sunstoneos/evidence/bluetooth-hfp-ag-20261016/`. Physical Bluetooth
and headset qualification remain outstanding.

The AG runtime is original Apache-2.0 code grounded in the HFP protocol. External
Linux header and RFCOMM checksum fixtures preserve their original notices.
Published BlueZ code is a reading reference and is not copied into this runtime.


The native recognition application also supports [successive copied CVSD
segments](bluetooth-native.md#repeated-headset-capture-and-recognition) within
one bounded supervised session. Two input slots and exact transcript delivery
acknowledgments provide backpressure; capture stops on backlog or loss. This
application extension keeps the existing CVSD negotiation and disconnect path.
Physical capture and wideband mSBC remain unqualified.

The optional native caller-PCM modes require a confirmed mSBC selection and reuse
the existing transparent eSCO connection. They encode actual 16 kHz PCM with the
pinned libsbc implementation; silence and caller audio share encoder history and
H2 sequencing. They do not reinterpret 16 kHz input after a CVSD selection.
Playback-only and capture/recognition duplex use the same authenticated HFP/HCI
owner. [Their copied-input contract](bluetooth-native.md#caller-pcm-playback-and-duplex)
retains the distinction between controller credits, disconnect flushing and
unqualified acoustic delivery.
