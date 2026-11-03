# Headset voice-recognition activation and optional call control

Status: implemented and host-tested; no board result qualifies it. Physical
activation latency, and whether the target headset stays connected to an
audio gateway that advertises no call feature, need a board run.

The board is the Hands-Free Profile (HFP) Audio Gateway (AG) so that it can
record a headset microphone for the mimus speech recognizer. Pressing the
headset's voice button starts one recognition segment, using the HFP 1.9
voice-recognition activation procedure (section 4.25, `AT+BVRA`). Call control
exists only for HFP conformance and has no board use, so it is a build option
that is off by default.

## Behavior

**Call control.** [a733-bluetooth.cmake](../../cmake/a733-bluetooth.cmake)
defines `SUNSTONE_BT_HFP_CALL_CONTROL` from a CMake option, default OFF, with
one `target_compile_definitions` on `sunstone-a733-bluetooth`. That target
compiles `bt_hfp_ag.c`, `bt_sco.c` and `apps/bluetooth/headset.c` together;
the OS image target compiles its own copies without the definition. No link
combines objects built with different settings, and
[bt_hfp_ag.h](../../include/bt_hfp_ag.h) needs only an `#ifdef`.

ON provides `bt_hfp_ag_init_calls`, `bt_hfp_ag_call`, RING, call indicators,
AG features 352 and SDP Network 1. OFF compiles out answering, dialing,
hang-up, hold, DTMF, RING, call-state indicators and `bt_hfp_ag_call`,
advertises no call feature and sets Network to 0. The queries headsets send
after setup get the same answers in both settings, because none depends on a
call: `AT+CLCC`, `AT+COPS`, `AT+CNUM`, `AT+CLIP`, `AT+CCWA` and `AT+CMEE`.
`AT+BCC`, a codec-connection request, is outside the call handler.

`bt_hfp_ag_init(q, rfcomm, indicators, setup_deadline, timeout_us, codecs,
voice)` is the single initializer for a gateway without call control.

**Voice recognition in the AG.** With `voice` set, the AG advertises bit 2
(value 4) in `+BRSF`, and the application sets the same SDP SupportedFeatures
bit, which the SDP server cross-checks. Recognition is committed on once the
UART accepts the complete `OK` for `AT+BVRA=1` or the complete `+BVRA: 1`.

- `AT+BVRA=1`: `OK` only when the application admits an activation, the
  headset set bit 3 (value 8) in `AT+BRSF`, the service-level connection is
  ready, recognition is not committed on and no `+BVRA` response is queued or
  in flight; otherwise `ERROR`.
- `AT+BVRA=0`: `OK` only while recognition is committed on; otherwise
  `ERROR`. No `+BVRA: 0` follows an accepted one (HFP section 5).
- `AT+BVRA=2`: `ERROR`; enhanced status is not advertised.
- `bt_hfp_ag_voice(q, 1)` and `(q, 0)` queue `+BVRA: 1` and `+BVRA: 0`, and
  return 0 while a response owns the serial stream.

The application admits `AT+BVRA=1` by writing the AG field `voice_admit`
before each AG step. One rule computes the field on every poll from the first
entry into idle through CLOSE and DONE; a stop persists until DONE, so it
closes admission at the next AG step.

**One request, one activation.** The repeated modes 2 (CVSD) and 3 (mSBC)
capture only on request. Mode 5, mSBC with caller playback, keeps continuous
capture and does not advertise voice recognition, because its playback pacing
and cancellation depend on an open capture. The finite modes 0, 1 and 4 use
`bluetooth_headset_copy` through `usb0.c`; profile version 3 admits mode 1,
which the finite-capture branch of the mimus `recognition.cc` and
`checks/bluetooth-recognition.cc` exercise.

A request word holds 0, ACCEPT (admit the next `AT+BVRA=1`), START (start
now) or ACTIVE. The bound consumer may change 0 to ACCEPT or START, or ACCEPT
to START; the owner changes ACCEPT or START to ACTIVE when it starts an
activation, on an admitted `AT+BVRA=1` or by itself on START. On START the
owner sends `+BVRA: 1` when the headset set bit 3, and otherwise opens an
audio connection without a call (HFP section 4.11). When the activation ends,
the owner publishes the segment if it holds at least 2 bytes, stores 0 and
notifies the consumer. A headset cancel never restarts capture; the consumer
chooses the next request.

- Bluetooth image: the recognizer requests START first, so the unattended
  60-second supervised session records one segment and exercises both `+BVRA`
  responses; the host tool defaults to one transcript for modes 2 and 3.
  Afterwards the recognizer requests ACCEPT whenever the word is 0.
- OS image: in workload 0 the capture client requests ACCEPT while a
  generation waits for audio. In workload 4 a new generation, started by the
  Enter key, requests START. If that activation ends without a segment, as
  after a headset cancel, the client requests ACCEPT: the button then records
  that generation, and the next Enter upgrades ACCEPT to START for the new
  one. A START refused because an earlier activation is still running is
  requested again if that activation ends without a segment.

A segment ends at the profile's sample count (at most 24,000 samples: 3 s of
CVSD or 1.5 s of mSBC) or at an accepted `AT+BVRA=0`; an early end publishes
the largest even byte count. `first_sample` counts samples in earlier
published segments, so [observe.py](../../apps/bluetooth/observe.py) bounds a
single frame's `first_sample` by (ticket − 1) × samples, and the supervised
host tool, which sees every ticket, requires it to equal the sum of the input
samples of earlier transcripts. The ticket check (ticket equals tail + 1)
orders segments. A published segment never holds a lost packet, because a loss
stops capture and discards the segment; consumers of repeated segments do not
test `loss_packets`.

## Deadlines and failures

- Setup, through codec selection, keeps its stage deadlines and `fail()` with
  transport retention on a stop or the window, because ACL, authentication,
  SDP, RFCOMM or codec commands may be in flight.
- With the AG ready and the codec selected, the owner enters an idle stage
  without a stage deadline; AG line and response timeouts bound each
  exchange. A stop, or the supervised window 38 s after the session origin,
  ends idle at the first poll with no AG response or codec offer in flight,
  bounded by `bluetooth_close_deadline`, and reaches DONE with
  `segment_disconnected` set and no retention. Idle, and DONE with no audio
  link, run `control_step` (SDP discovery, SDP server, AG or RFCOMM,
  security) and `bt_sco_step`, and skip only the stream lookup.
- Every path to DONE follows a stop or the window, and none leaves the
  headset recognizing. When recognition is committed on, the owner queues
  `+BVRA: 0` first; DONE follows once no AG response is in flight, bounded by
  the AG response timeout and the close bound.
- An activation is admitted only while now plus the line timeout is within
  the window; the owner computes that limit once, on entering idle.
- The first link open, from activation start, is bounded by two line timeouts
  (one AG response still in flight and one codec reselection) plus two
  response timeouts (the link open and its mSBC-to-CVSD retry). In the
  supervised lifetime the bound is also capped at the close bound, 40 s after
  the origin. Exceeding it fails with retention, and in the resident lifetime
  the watchdog then resets the board.
- From the first entry into idle, every stage through CAPTURE handles a stop
  as idle does, including the AG and FALLBACK stages that open the link after
  a headset cancel ended the activation. A stop during an open waits for the
  result: an open link closes through the usual sequence, a confirmed setup
  failure ends cleanly, and `BT_SCO_RETAINED` fails.
- `capture_deadline` is armed at activation start and disarmed at its end;
  between activations silence packets get now plus the response timeout,
  capped by the window. With the link open, a stop or the window closes it
  and discards a partial segment.
- The owner never closes the AG. From the first entry into idle through
  CAPTURE, an AG that leaves the ready state, as when the headset closes the
  service-level connection, is a release the owner did not request. It calls
  `fail()` in the same poll, in either lifetime, unless a stop or the window
  is already ending the session. Without this rule a resident owner with only
  ACCEPT armed would wait idle forever, with no link and no failure.
- Any release of the audio link that the owner did not request calls
  `fail()` in either lifetime. The owner retains its transports, the OS
  client calls `board_fail`, and in the resident image the watchdog resets the
  board.
- `storage_digest_add` returns 0 only for a null state or more than 2^61
  bytes; a 0 return calls `fail()`. AT refusals leave the session usable. A
  consumer call that finds the counters inconsistent returns −1, and the
  consumer fails.

## Data flow and ownership

The Bluetooth owner task alone advances UART, H4, HCI, RFCOMM, the AG, the
mSBC decoder and the audio link. In the poll in which the AG parses
`AT+BVRA`, the application updates admission and starts or ends the
activation.

During an activation, CVSD bytes go from the SCO packet into a slot with one
`memcpy`, and mSBC frames decode into the slot directly; mSBC sample counts
are multiples of 120, so a whole frame always fits. After each write, the
owner passes the 64-byte SHA-256 blocks that the write completed to
`storage_digest_add`, which compresses them in place from the slot. At
publication the owner hashes the rest of the even-length prefix, fewer than
64 bytes, and calls `storage_digest_end`. Hashing only whole blocks keeps the
digest's 64-byte staging block empty between calls, so each PCM byte is
copied once. Between activations the owner releases each packet without
copying, decoding or hashing it. It counts the bytes released since the
stream opened, and an activation starts by skipping to the next sample or
frame boundary:

- **CVSD.** [bt_sco.h](../../include/bt_sco.h) allows odd packet lengths, so a
  packet can split a 16-bit sample. An activation that starts after an odd
  count skips one byte.
- **mSBC.** The decoder in [msbc.c](../../apps/bluetooth/msbc.c) carries its
  H2 sequence lock and a partial frame across packets.
  `bluetooth_msbc_resync`, called at activation start, clears the partial
  frame, the lock, the expected sequence and the discard count. Each decoded
  frame records the stream position at its end. Frames are 60 bytes, so an
  activation after the first decoded frame on the link skips the bytes up to
  the next frame boundary, and the decoder starts at a frame. Only the first
  activation on a link uses the header search, which can accept a misaligned
  window as a header and end the session; on random payload that has
  probability about 10⁻⁵. If an HCI packet between activations were lost
  without a reported status, the skip would end inside a frame, and the header
  search would find the next one. The libsbc synthesis history is kept, so
  only the first decoded frame of a segment can carry audio from before the
  gap.

A consumer borrows the oldest segment with
`bluetooth_headset_segment_take(app, &pcm, &snapshot)`, which returns 0 when
nothing is published. Otherwise it checks the calling task, the argument
ranges and that head, tail and ticket agree, copies the snapshot and returns a
read-only pointer to slot *tail* mod 2, valid until
`bluetooth_headset_segment_ack`. The producer writes slot *head* mod 2 only
while *head − tail* < 2, and *tail* advances only at the acknowledgement, so
that check alone keeps the borrowed slot unchanged. Each consumer converts the
slot into its WAV buffer with `bluetooth_pcm_wav` or `bluetooth_pcm16_wav` and
acknowledges; the recognizer transcribes after the acknowledgement. Holding a
slot for a conversion of about a millisecond cannot cause backlog within a 1.5
to 3 s segment. The reported PCM digest is the owner's.

After the H4 record, each PCM byte is copied by `bt_sco` into its packet, by
the owner or decoder into the slot, and by the WAV conversion. The conversion
stays because CVSD needs 8-to-16 kHz interpolation and because releasing the
slot lets the next segment record during transcription.

## Interrupt, polling and synchronization

The owner keeps its 1 ms poll, which the UART and audio receive-latency
contract requires; before the first press those 1,000 wake-ups per second do
no audio work. Both 4-poll latencies (`AT+BVRA=1` to commit, and commit to the
synchronous-connection setup accepted by the UART) are the heavier form that section 1.3 of `docs/performance-architecture.md` describes. The owner advances RFCOMM, L2CAP, ACL, HCI and the UART one step per poll, and no
qualified UART receive interrupt wakes it sooner.

Consumers wait in `ulTaskNotifyTake(pdTRUE, timeout)` on index 0 and re-read
the counters and the request word after every return, so a lost or spurious
notification changes only latency. The owner gives to the bound consumer when
an activation ends and after a stop or disconnection. The Bluetooth
recognizer waits 100 ms for a segment and for USB collection; the
notification is also given by `bluetooth_recognition_segment_ack` and the
recognizer's failure path. The OS client waits 250 ms, inside its 1 s progress
window, and `os_headset_capture` gives to it. All handles are set before the
scheduler starts.

The owner and a consumer may run on different cores. Head and tail keep their
release and acquire ordering. Every change of the request word is an
acquire-release compare-and-exchange, except the owner's final store of 0. At
the end of an activation the owner stores *head* with release, then 0 in the
word with release, then notifies.

`bluetooth_headset_segment_request(app, request, published)` takes the
published count the consumer last observed: its status read, or the head a
take saw when it returned 0. It loads the word with acquire, then *head*. A 0
from an activation end therefore comes with that activation's segment, and the
request is refused when *head* differs from `published`; otherwise the
exchange from the loaded word succeeds unless an activation started or ended
in between. It returns 1 when the word holds the request, including when it
already did, so a consumer can tell that its START is pending. Only the owner
touches AG and decoder state.

The owner loads `segment_stop` and the word once per poll and passes them to
admission, recognition and the repeated-capture step; its own stops update its
copy. A consumer's stop stored during a poll is handled in the next poll, 1 ms
later. The words the consumer writes sit on their own cache line, apart from
the owner's per-poll fields.

## Memory placement

Slots and records are static, in Normal write-back inner-shareable memory; the
UART path is programmed I/O, so DMA ownership and cache maintenance do not
apply. The consumers hold no separate PCM buffers or stack chunks. Added state
is the request word, the published-sample total, the released-byte count, the
end of the last decoded mSBC frame, a 112-byte digest state and a few AG
words; OFF drops the call fields.

## Qualified figures

Instruction counts come from a Unicorn harness that runs the functions built
with the image flags (`-O2`, `-mgeneral-regs-only`); sizes come from AArch64
`-O2` objects. Neither is a board result.

| Figure | Value | Record |
| --- | --- | --- |
| `bt_hfp_ag.c` code, call control OFF; ON | 11,572 B; 15,800 B | [budgets](../evidence/bluetooth-voice-recognition.jsonl) `bluetooth-voice-recognition-budgets` |
| `AT+BVRA` line, OFF; ON | 808; 1,240 instructions | same |
| Owner step, packet in a CVSD segment (at most); publishing packet | 2,993; 6,025 instructions | same |
| Owner instructions per 48,000 B CVSD segment | 2,090,169 | `bluetooth-voice-recognition-measure` in the same file |
| Consumer work per segment, excluding WAV conversion | 372 instructions | same |
| `AT+BVRA=1` to committed on; commit to setup accepted | 4 polls; 4 polls (13 after a codec list change) | `bluetooth-voice-recognition-budgets` |

The records hold the complete budget table, the baseline of each row and the
measurement method.

## Host tests

The host models `bt-hfp-ag.py`, no host check and
`os-image-headset.py` run in both call-control settings, for the Bluetooth
image with recognition (with and without mSBC) and the OS headset image. They
cover a window or stop at each setup and link stage, requests around
publication and `+BVRA: 0`, a workload 4 headset cancel followed by a button
press and by a new Enter, zero-sample and odd-byte early ends against the digest of the even prefix, odd-length CVSD packets between activations, and mSBC activations separated by an odd number of released frames. They also cover every unrequested peer release (each reports a failure and, when committed on, sends
`+BVRA: 0` before DONE).

## Limitations and open work

- Enhanced voice-recognition status, recognition text, voice tags, echo
  cancellation and in-band ringing have no board use and are not offered. Call
  control is not offered in the OS image.
- The audio link stays open from the first press, because closing it between
  segments needs `bt_sco` to reopen a link to the same peer
  ([bt_sco.c](../../drivers/bluetooth/bt_sco.c)). For CVSD the open link costs
  about 34,100 UART bytes per second in both directions (11.4% of the 1.5
  Mbaud link), 266.7 packet releases and silence sends per second, and the
  radio's eSCO slots. The same change would let a headset that releases the
  link after `+BVRA: 0` record again, and would allow lending the H4 record
  instead of copying it into a `bt_sco` packet. Reading the peer's remote
  features when the owner enters idle would remove polls after the button press.
- Waking the owner from the UART receive interrupt
  ([performance findings](../audit/performance.md), UART and SPI), or repeating
  transport steps while they make progress (the radio finding "Repeat transport
  steps while they progress" in the same document), would remove most of the 4
  polls from `AT+BVRA=1` to commit and from commit to setup accepted.
- The OS recognizer's capture wait in
  [recognition.patch](https://github.com/pedapudi/mimus/blob/port/sunstoneos-split/sunstoneos/apps/os/recognition.patch)
  wakes every tick; a 250 ms notification wait needs `apps/mimus-board` to
  expose the recognizer's task handle. The finite mode-1 path of the mimus
  `recognition.cc` waits with `vTaskDelay(1)` and copies and hashes its capture
  again; the owner could lend the capture in place.
- libsbc is built with `-mgeneral-regs-only`, so mSBC decoding costs about
  36,800 instructions per 60-byte frame, about 4.9 million per second during a
  segment; NEON decoding needs FP/SIMD context for the owner task. The scalar
  `storage_digest.c` could use ARMv8 SHA2 instructions or the crypto engine.
  The runtime `memcpy` copies byte by byte when source and destination differ in
  alignment modulo 8 (about 264 instructions for half of the 60-byte CVSD
  packets); word copies would return the packet-in-segment budget from 3,030 to
  3,000 instructions.
- Board run: pair the target headset against an audio gateway without call
  features and measure physical activation latency.
