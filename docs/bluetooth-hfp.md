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

The current implementation provides [SDP discovery](bluetooth-sdp.md) and
[RFCOMM serial transport](bluetooth-rfcomm.md). HFP command processing, actual
security admission and synchronous audio transport remain required. Successful
AT exchange will establish control state only. It cannot establish that headset
audio reaches the application.

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

A local SDP server must answer actual discovery requests and retain response and
continuation ownership. An RFCOMM listener alone does not make its service
visible. Incoming ACL connection request/accept/reject handling is also required
before a headset can establish the underlying link itself. Existing finite ACL
peer and handle lifetimes still constrain reconnect behavior.

The RFCOMM registry can delegate unrelated PSM authorization to the SDP server.
Both components must preserve the actual link and service security policy.
Authentication, encryption state, key storage and controller generation are real
inputs to that decision. No default callback may substitute for pairing or a
qualified security provider.

## The Audio Gateway responds to headset commands

After RFCOMM opens, the HF starts the service-level initialization with
`AT+BRSF`. The AG returns its supported features. The HF then discovers indicator
names and values through `AT+CIND`, and enables updates with `AT+CMER`.
Negotiated optional features add codec, call-control or HF-indicator exchanges.
The next control component must implement the AG side of this sequence.

A bounded parser must retain incomplete lines across RFCOMM records, accept
multiple complete lines in a record and preserve response ordering under credit
backpressure. Responses use the specified carriage-return/line-feed framing.
Unknown or unsupported commands receive an appropriate error. A command's receipt
and successful response transmission are distinct state transitions.

Control state needs the exact link and channel generations. Disconnect, security
revocation and uncertain transmission must retain owned responses and invalidate
readiness. Feature masks must exclude functionality without an actual provider.
For example, advertising voice recognition requires the application and audio
path to carry out the resulting request. A parser recognizing `AT+BVRA` alone
does not satisfy that feature.

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

The next implementation sequence is an AG command engine over the serial
transport, then a controller-bound synchronous audio service and application
capture. Local SDP serving, incoming ACL handling and security complete the
connection paths. Model checks qualify software behavior; headset interoperability
and real microphone samples require physical testing after board recovery and
radio initialization are qualified.

## Primary and implementation references

The primary reference is
[Hands-Free Profile 1.9](https://www.bluetooth.org/DocMan/handlers/DownloadDoc.ashx?doc_id=574215),
particularly sections 2.2–2.4, 4.2, 4.11, 5, 6.1–6.3, 6.7 and 7.2.
The cached PDF has SHA256
`56f5fcce59cc4342c38d13ed44cc87b28ce73d1f2214dc909d121c378575fa41`.
It is stored outside the repository in
`~/.cache/sunstoneos/sources/bluetooth-hfp/`.

BlueZ revision `0845b8f6ef2ac004b1c953cf4fe4ca3458cd8e36` supplies published
`src/shared/hfp.h`, `src/shared/hfp.c` and `unit/test-hfp.c` references. Their
notices and source hashes remain in the same external cache. They provide
implementation comparisons; the source references do not establish SunstoneOS
HFP qualification or justify advertising unfinished capabilities.
