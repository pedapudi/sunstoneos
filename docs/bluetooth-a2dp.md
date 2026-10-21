# A2DP sink signaling and SBC media transport

SunstoneOS can act as an Advanced Audio Distribution Profile (A2DP) sink: a
phone or other audio source streams SBC-encoded audio to the board. The
implementation is the Audio/Video Distribution Transport Protocol (AVDTP) layer
in [bt_avdtp.c](../drivers/bluetooth/bt_avdtp.c), with its interface in
[bt_avdtp.h](../include/bt_avdtp.h). It runs over the existing
[L2CAP worker](bluetooth-l2cap.md) and is published through the optional sink
record of the [local SDP server](bluetooth-sdp-server.md). This document assumes
familiarity with those components.

Status: the protocol layer is implemented and passes composed host checks. No
native application uses it yet, so no image decodes or delivers A2DP audio, and
no physical source device has been tested.

## One SBC sink endpoint accepts a source's commands

The sink exposes one stream endpoint, SEID 1, with media type audio and the
SBC codec. It is an AVDTP acceptor only: the source sends every command and the
sink replies. The sink sends no AVDTP commands, so it cannot start, suspend or
reconfigure a stream itself.

The caller supplies the SBC capability element at initialization. It must
include every value that A2DP 1.3 makes mandatory for a sink: 44.1 and 48 kHz,
all four channel modes, all four block lengths, 4 and 8 subbands, both
allocation methods, and a bitpool range with 2 ≤ minimum ≤ maximum ≤ 250. The
16 and 32 kHz rates are optional.

The sink answers these signals:

- `DISCOVER` returns the endpoint and whether it is in use.
- `GET_CAPABILITIES` and `GET_ALL_CAPABILITIES` return media transport and the
  SBC capability element. No delay reporting is advertised.
- `SET_CONFIGURATION` accepts exactly media transport and one SBC
  configuration. Each SBC field must select one supported value, and the bitpool
  range must lie inside the advertised range. Refusals name the offending
  service category and the AVDTP or A2DP error code, for example
  `INVALID_SAMPLING_FREQUENCY` for two selected rates. Content protection,
  delay reporting and the other optional categories are refused as
  `UNSUPPORTED_CONFIGURATION`.
- `GET_CONFIGURATION` returns the accepted configuration.
- `OPEN`, `START`, `SUSPEND`, `CLOSE` and `ABORT` follow the AVDTP stream states
  idle, configured, open, streaming, closing and aborting. A command in the
  wrong state receives `BAD_STATE`. `START` and `SUSPEND` rejects name the first
  failing endpoint. An `ABORT` for another endpoint receives no reply.
- `RECONFIGURE` and `SECURITY_CONTROL` are always refused. Any other signal,
  including `DELAYREPORT`, receives a general reject.

Signaling messages must be single packets. Every supported message fits the
L2CAP minimum MTU, so a source fragments only messages the sink does not
support. A fragmented packet faults the sink and retains its receive record.
Responses from the source are discarded because the sink never sends a command.

## Two L2CAP channels carry signaling and media

The application installs `bt_avdtp_admit` beneath its L2CAP authorization
callback, in the same way as the SDP server's admission callback. PSM `0x0019`
is admitted only for incoming channels, and only after the application's
security policy returns success for that link. The first channel becomes the
signaling channel and fixes the peer link. A second channel on the same link
becomes the media transport channel, but only after the sink has accepted
`OPEN`. Every other PSM 25 request is refused, as is any request after a local
close. Admission reserves the exact next L2CAP channel generation, so the sink
never adopts a channel by a reused wire CID.

Security is checked again on every step while the sink holds either channel.
Before it handles a signaling command, sends a response or publishes a media
slot, the step calls the security policy for PSM 25 on the admitted link. Any
result other than success, for example after a key replacement, a loss of
encryption or a revoked link, faults the sink with `BT_AVDTP_SECURITY`. The
faulted sink handles no further commands, sends no response and publishes no
media. It retains both channels and their unread receive records. During a
local close the sink accepts no input and publishes nothing, so the step skips
this check and the close can complete.

A sink fault refuses only PSM 25. The admission callback still passes requests
for other PSMs, such as SDP and RFCOMM, to the application's policy, so a fault
caused by a malformed AVDTP packet does not deny the other services on the link.

One serialized task owns the sink and the lower workers. Each step advances the
lower stack once, maintains both channels, handles at most one signaling command
and copies at most one media packet. One response is outstanding at a time.
Its deadline is the caller's response interval, from 1 microsecond to 60
seconds. The L2CAP transmission keeps the independent ERTX transport bound, so
an expired response faults the sink while the lower stack keeps ownership of
any accepted bytes.

## Media is copied only when every SBC frame matches

While streaming, each media packet is parsed as an RTP packet. The parser skips
CSRC entries and a header extension, removes RTP padding, and reads the A2DP
media payload header. Fragmented SBC frames are not supported. Every SBC frame
header must carry the sync word and the configured rate, block length, channel
mode, allocation method and subband count, with a bitpool inside the configured
range. The frame lengths computed from A2DP 1.3 section 12.9 must exactly cover
the payload.

A valid packet is copied into one of four 1024-byte slots with its RTP sequence
number and timestamp, its frame count and its L2CAP receive serial. The `gap`
field counts the packets the application never received before this one,
whether they were lost on the link, invalid or dropped. When all four slots
are published, the packet is dropped and counted. Invalid packets are also
counted and never published. Packets that arrive while the stream is not
streaming are discarded. The L2CAP receive record is released as soon as the
packet is copied or discarded, so media cannot exhaust the shared L2CAP pool.

The application reads the oldest slot with `bt_avdtp_media` and frees it with
`bt_avdtp_release_media`. The copied bytes are whole SBC frames, ready for a
decoder such as the pinned BlueZ libsbc already used for mSBC.

## Closure releases channels only after confirmation

A source normally sends `CLOSE` or `ABORT` and then disconnects the media
channel. When the media channel's disconnection is confirmed, the sink releases
it and returns to idle with no configuration. If the source disconnects the
signaling channel, the sink closes any media channel, releases both channels
and can then admit a new source. A media channel that disappears while
streaming also ends the stream.

`bt_avdtp_close` performs a local shutdown with an absolute deadline. The sink
stops replying, disconnects the media channel, waits for that confirmation, and
then disconnects the signaling channel after any outstanding response retires.
After both channels are released, the step returns 0 and no new stream is
admitted. A quarantined channel, an identity mismatch or an expired deadline
retains all resources as a fault.

## Validation composes the actual lower stack and pinned SBC codec

[bt-avdtp.py](../checks/bt-avdtp.py) builds the sink with the actual UART, H4,
HCI, ACL, L2CAP and SDP server workers under address and undefined-behavior
sanitizers. The source side of each exchange is encoded independently in
[bt-avdtp.c](../checks/bt-avdtp.c). The cases cover:

- SDP discovery of the sink record through the same composed stack.
- Discovery, capabilities, 21 configuration refusals and the in-use endpoint.
- The complete open, start, suspend, close and abort sequence, including
  refused extra channels and commands in the wrong state.
- SBC media encoded by the pinned libsbc: copied frames, RTP sequence gaps,
  decoding of the copied frames back to the input sine with more than 25 dB
  signal-to-noise ratio, invalid packets and slot exhaustion.
- Frame-length agreement with libsbc for 128 combinations of rate, channel mode,
  block length and subband count.
- Peer signaling closure, ordered local closure, denied security, fragmented
  signaling and response expiry.
- Security revoked while streaming: a pending `SUSPEND` receives no response,
  a pending media packet fills no slot, and both channels and both receive
  records are retained.
- After a fragmented-signaling fault, PSM 25 is refused while SDP admission
  still reaches the policy.

The runner verifies the libsbc sources against `apps/bluetooth/msbc-source.json`
and compares the SBC configuration bit layout with libsbc's A2DP definitions.
It also compiles the sink for freestanding AArch64. The AVDTP and A2DP signal
identifiers and error codes come from the AVDTP 1.3 and A2DP 1.3
specifications; those specification documents are not yet pinned in the source
cache, so the check encodes them independently but has no pinned copy to
compare against.

## Remaining work

- A native application mode that publishes the sink record, admits PSM 25,
  decodes SBC with libsbc and delivers PCM to its consumer.
- Content protection, delay reporting and the initiator role, if a source
  requires them.
- Pinned copies of the AVDTP 1.3 and A2DP 1.3 specifications.
- Physical qualification with a real source device.
