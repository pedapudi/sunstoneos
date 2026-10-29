# A2DP sink signaling and SBC media transport

SunstoneOS can act as an Advanced Audio Distribution Profile (A2DP) sink: a
phone or other audio source streams SBC-encoded audio to the board. The
implementation is the Audio/Video Distribution Transport Protocol (AVDTP) layer
in [bt_avdtp.c](../drivers/bluetooth/bt_avdtp.c), with its interface in
[bt_avdtp.h](../include/bt_avdtp.h). It runs over the existing
[L2CAP worker](bluetooth-l2cap.md) and is published through the optional sink
record of the [local SDP server](bluetooth-sdp-server.md). This document assumes
familiarity with those components.

Status: the optional native Bluetooth image decodes real SBC media into a
bounded PCM capture and delivers it through the existing USB observation path.
Composed host checks cover the complete native producer and host capture.
Physical source interoperability and audio output remain unqualified.

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

- Content protection, delay reporting and the initiator role, if a source
  requires them.
- Pinned copies of the AVDTP 1.3 and A2DP 1.3 specifications.
- Physical qualification with a real source device.

## Native finite capture design

The optional Bluetooth image uses profile version 2, mode 6 for a finite SBC
capture. The existing authenticated ACL owner admits SDP and AVDTP, then copies
PCM through the existing post-disconnect USB result. The sample count counts
interleaved signed 16-bit samples; it must be even and cannot exceed 24,000.
The result carries the decoded sampling rate and channel count. This mode
observes real decoded audio; speaker playback remains a separate integration.

The decoder reuses the pinned LGPL-2.1-or-later BlueZ libsbc 2.0 source in
`apps/bluetooth/msbc-source.json`. A separate compilation redirects only its
allocation calls to one fixed 6,144-byte arena. The pinned generic decoder
requests 5,615 bytes including alignment padding. Initialization occurs once;
no packet allocates memory. The image retains the source license and relinkable
objects with its external build artifacts.

Each owner poll consumes at most one media packet, with at most 15 SBC frames.
The 1 kHz owner task permits four polls per tick. This covers 3,000 shortest
frames per second even when the source sends one frame per packet.
It decodes directly into the 48,000-byte capture buffer. The pinned decoder
limits a final partial frame to complete sample groups within the remaining
capacity. The existing four 1,024-byte compressed slots bound backlog. The
first decoded frame fixes rate and channels independently of later AVDTP state.
A changed geometry, bad CRC, sequence gap, invalid packet or full-slot drop
fails capture and retains resources. The owner closes media and signaling and
waits for confirmed closure before publishing immutable PCM. Capturing ends
within the existing profile deadline. There is no sustained playback claim.

The design budget is 15 decoder calls and 7,680 decoded bytes per poll, one
compressed-packet copy in AVDTP, zero intermediate PCM copies, and one USB
copy per requested result chunk. The existing owner stack is 32 KiB. Native compiler stack-use records bound the
owner-to-decoder call chain at 1,088 bytes, excluding interrupt nesting and the
unchanged lower-stack branches. Finite capture has 1,500 packet records, enough
for 48,000 PCM bytes in the smallest mono frames sent one per packet.
The inherited application also reserves two unused repeated-capture buffers
and its HFP workers; the complete `voice` object is 456,320 bytes. This is
separate from the finite PCM, compressed-slot and decoder budgets.
An independent review accepted the design after requiring explicit mode
admission, independent geometry, bounded packet draining and loss refusal.


## Native build and accepted host evidence

Select `SUNSTONE_BLUETOOTH_APPLICATION=ON` and `SUNSTONE_BLUETOOTH_A2DP=ON`,
with `SUNSTONE_BT_SECURITY_DIR` and `SUNSTONE_BT_SBC_DIR` pointing to the pinned
security and SBC sources. A2DP excludes Mimus recognition and mSBC. The image
accepts only version 2, mode 6 profiles with an incoming, authenticated peer.
The finite sample count counts interleaved samples, so stereo duration is
`samples / (rate * 2)`. SDP publishes only the recorder sink, with no AG record.

The PCM response retains its 160-byte header. For mode 6, the final three
32-bit header fields carry sampling rate, channel count and codec value 1
(SBC). Every 24-byte packet record carries compressed bytes, decoded bytes
and `(rate << 8) | channels` in its reserved field. The host checks these
fields, the full PCM digest and the immutable result identity. It writes a
file named for the actual rate and channels. No device output or acoustic
quality is inferred from copied PCM.

`checks/bluetooth-a2dp.py` compiles the actual application, security and
transport workers with address and undefined-behavior sanitizers. It extracts
the production USB serialization function directly from `apps/bluetooth/usb0.c`.
The passing evidence is under
`~/.cache/sunstoneos/evidence/bluetooth-a2dp-20261028/final/`:

- Stereo capture trims the final decoded frame to 1,200 bytes.
- Short-frame capture produces 48,000 bytes from 100 packets.
- Single-frame packets produce 48,000 bytes from 1,500 packets. Three packets
  arrive per modeled millisecond and fit within the owner's four-poll budget.
- Damaged CRC, fragmented media, sequence gaps, encryption revocation and early
  media closure retain incomplete capture without publishing success.
- SDP queries return the sink and omit AG. PCM stays pending until both AVDTP
  channels have confirmed closure; later polls preserve completed capture.
- Actual native USB result frames pass profile admission, host parsing,
  durable PCM copying and hash validation. Damaged geometry and record metadata
  are refused.

The native image has 717,516 text bytes, 153 initialized data bytes and
6,333,904 BSS bytes. Against the baseline Bluetooth components linked with the
same unchanged providers, A2DP adds 23,948 text bytes and 20,480 BSS bytes;
initialized data is unchanged. The comparison recompiles only the five changed
baseline components from `fe7bfb81`; it does not rerun an unchanged HFP workload.
The commands, baseline image and stack-use files accompany the frozen native
image. The runtime cost remains bounded by 60 decoder calls per tick, with no
per-frame allocation or intermediate PCM copy. Board throughput is unmeasured.

Physical qualification requires a reviewed phone or other SBC source, its peer
identity and security policy, the existing radio/UART scheduling evidence and
a supervised session with the recorded recovery procedure. A useful first test
captures one finite stream, verifies the copied PCM digest and format, and
confirms media/signaling closure. Speaker playback requires a separate audio
consumer and remains outside this observation mode.
