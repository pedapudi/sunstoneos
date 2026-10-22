# Headset voice-recognition activation and optional call control

Status: implemented, with three differences from the text below.

- The finite-capture branch of `recognition.cc` remains. Profile version 3
  admits mode 1 (`bluetooth_profile_valid` and `prepare.py`), and
  `checks/bluetooth-recognition.cc` exercises that finite recognition. Its
  copied capture is staged in the recognizer's internal scratch, so
  `retained.pcm` is still removed.
- The application admits `AT+BVRA=1` by writing the AG field `voice_admit`
  between steps, which keeps the call-control build within its size budget.
- A packet between activations costs 198 instructions for CVSD and 201 for
  mSBC, above the 188 and 191 targets. The 171 and 174 baselines were measured
  on the DONE path, which skips the capture window, deadline and stop checks
  and silence scheduling that the open link still needs.

The board is the Hands-Free Profile (HFP) Audio Gateway (AG) so that it can
record a headset microphone for mimus. Pressing the headset's voice button
starts one recognition segment, using the HFP 1.9 voice-recognition activation
procedure (section 4.25, `AT+BVRA`). Call control exists only for HFP
conformance and has no board use, so it becomes a build option that is off by
default.

## Behavior

**Call control.** [a733-bluetooth.cmake](../../cmake/a733-bluetooth.cmake)
defines `SUNSTONE_BT_HFP_CALL_CONTROL` from a CMake option, default OFF, with
one `target_compile_definitions` on `sunstone-a733-bluetooth`. That target
compiles `bt_hfp_ag.c`, `bt_sco.c` and `apps/bluetooth/headset.c` together;
the OS image target compiles its own copies without the definition. No link
therefore combines objects built with different settings, and
[bt_hfp_ag.h](../../include/bt_hfp_ag.h) needs only an `#ifdef`.

ON keeps today's behavior: `bt_hfp_ag_init_calls`, `bt_hfp_ag_call`, RING,
call indicators, AG features 352 and SDP Network 1. OFF compiles out
answering, dialing, hang-up, hold, DTMF, RING, call-state indicators and
`bt_hfp_ag_call`, advertises no call feature and sets Network to 0. The
queries headsets send after setup keep today's answers in both settings,
because none depends on a call: `AT+CLCC`, `AT+COPS`, `AT+CNUM`, `AT+CLIP`,
`AT+CCWA` and `AT+CMEE`. `AT+BCC`, a codec-connection request, moves out of
the call handler. A board run must still show that the target headset stays
connected to an AG without call features.

**One initializer.** `bt_hfp_ag_init(q, rfcomm, indicators, setup_deadline,
timeout_us, codecs, voice)` replaces `bt_hfp_ag_init` (called only by
`checks/bt-hfp-ag.c`) and `bt_hfp_ag_init_codecs` (no caller).

**Voice recognition in the AG.** With `voice` set, the AG advertises bit 2
(value 4) in `+BRSF`, and the application sets the same SDP SupportedFeatures
bit, which the SDP server already cross-checks. Recognition is *committed on*
once the UART accepts the complete `OK` for `AT+BVRA=1` or the complete
`+BVRA: 1`.

- `AT+BVRA=1`: `OK` only when the application admits an activation, the
  headset set bit 3 (value 8) in `AT+BRSF`, the service-level connection is
  ready, recognition is not committed on and no `+BVRA` response is queued or
  in flight; otherwise `ERROR`.
- `AT+BVRA=0`: `OK` only while recognition is committed on; otherwise
  `ERROR`. No `+BVRA: 0` follows an accepted one (HFP section 5).
- `AT+BVRA=2`: `ERROR`; enhanced status is not advertised.
- `bt_hfp_ag_voice(q, 1)` and `(q, 0)` queue `+BVRA: 1` and `+BVRA: 0`, and
  return 0 while a response owns the serial stream.

**One request, one activation.** The repeated modes 2 (CVSD) and 3 (mSBC)
capture only on request. Mode 5, mSBC with caller playback, keeps continuous
capture and does not advertise voice recognition, because its playback pacing
and cancellation depend on an open capture.

A request word holds 0, ACCEPT (admit the next `AT+BVRA=1`), START (start
now) or ACTIVE. The bound consumer may change 0 to ACCEPT or START, or ACCEPT
to START; the owner changes ACCEPT or START to ACTIVE when it starts an
activation, on an admitted `AT+BVRA=1` or by itself on START. On START the
owner sends `+BVRA: 1` when the headset set bit 3, and otherwise opens an
audio connection without a call (HFP section 4.11). When the activation ends,
the owner publishes the segment if it holds at least 2 bytes, stores 0 and
notifies the consumer. A headset cancel therefore never restarts capture; the
consumer chooses the next request.

- Bluetooth image: the recognizer requests START first, so the unattended
  60-second supervised session records one segment and exercises both `+BVRA`
  responses (that run passes `--segments 1` to the host tool). Afterwards it
  requests ACCEPT whenever the word is 0.
- OS image: in workload 0 the capture client requests ACCEPT while a
  generation waits for audio. In workload 4 a new generation, started by the
  Enter key, requests START. If that activation ends without a segment, as
  after a headset cancel, the client requests ACCEPT: the button then records
  that generation, and the next Enter upgrades ACCEPT to START for the new
  one. The next published segment becomes the newest generation's WAV.

A segment ends at the profile's sample count (at most 24,000 samples: 3 s of
CVSD or 1.5 s of mSBC) or at an accepted `AT+BVRA=0`; an early end publishes
the largest even byte count. `first_sample` counts samples in earlier
published segments, so [observe.py](../../apps/bluetooth/observe.py) accepts
`first_sample` up to (ticket − 1) × samples. The existing ticket check
(ticket equals tail + 1) orders segments.

## Deadlines and failures

- Setup, through codec selection, keeps its stage deadlines and `fail()` with
  transport retention on a stop or the window, because ACL, authentication,
  SDP, RFCOMM or codec commands may be in flight.
- With the AG ready and the codec selected, the owner enters a new idle stage
  without a stage deadline; AG line and response timeouts still bound each
  exchange. A stop, or the supervised window 38 s after the session origin,
  ends idle at the first poll with no AG response or codec offer in flight,
  bounded by `bluetooth_close_deadline`, and reaches DONE with
  `segment_disconnected` set and no retention. Idle, and DONE with no audio
  link, run the existing `control_step` (SDP discovery, SDP server, AG or
  RFCOMM, security) and `bt_sco_step`, and skip only the stream lookup.
- An activation is admitted only while now plus the response timeout is
  within the window, which also bounds the first link open and the
  mSBC-to-CVSD retry. A stop during an open waits for the result: an open link
  closes through the existing sequence, a confirmed setup failure ends
  cleanly, and `BT_SCO_RETAINED` fails as today.
- `capture_deadline` is armed at activation start and disarmed at its end;
  between activations silence packets get now plus the response timeout,
  capped by the window.
- With the link open, a stop or the window closes it through the existing
  sequence and discards a partial segment.
- Any release of the audio link that the owner did not request, in either
  lifetime, calls `fail()`, as a release during capture does today. The owner
  retains its transports, the OS client calls `board_fail`, and in the
  resident image the watchdog resets the board. This holds until `bt_sco` can
  reopen a link (Left out).
- `storage_digest_add` returns 0 only for a null state or more than 2^61
  bytes; the owner passes its own state and at most 48,000 bytes per
  activation. A 0 return calls `fail()`.
- AT refusals leave the session usable. A consumer call that finds the
  counters inconsistent returns −1, and the consumer fails as today.

## Data flow and ownership

The Bluetooth owner task alone advances UART, H4, HCI, RFCOMM, the AG, the
mSBC decoder and the audio link. In the poll in which the AG parses
`AT+BVRA`, the application updates admission and starts or ends the
activation.

During an activation, CVSD bytes go from the SCO packet into a slot with one
`memcpy`, as today, and mSBC frames decode into the slot. The owner hashes the
slot with `storage_digest_add` as it writes, over the even-length prefix, and
calls `storage_digest_end` at publication. Between activations it releases
each packet without copying, decoding or hashing it, and keeps the next
activation aligned:

- **CVSD.** [bt_sco.h](../../include/bt_sco.h) allows odd packet lengths, so a
  packet can split a 16-bit sample. The owner keeps the parity of all bytes
  received since the stream opened, one exclusive-or per packet, and an
  activation that starts after an odd count skips one byte.
- **mSBC.** The decoder in [msbc.c](../../apps/bluetooth/msbc.c) carries its
  H2 sequence lock and a partial frame across packets. A new
  `bluetooth_msbc_resync`, called at activation start, clears the partial
  frame, the lock, the expected sequence and the discard count; the existing
  search then locks onto the first valid H2 header. The libsbc synthesis
  history is kept, so only the first decoded frame of a segment can carry
  audio from before the gap.

A consumer borrows the oldest segment with
`bluetooth_headset_segment_take(app, &pcm, &snapshot)`, which returns 0 when
nothing is published. Otherwise it checks the calling task, the argument
ranges and that head, tail and ticket agree, copies the snapshot and returns a
read-only pointer to slot *tail* mod 2, valid until
`bluetooth_headset_segment_ack`. The producer writes slot *head* mod 2 only
while *head − tail* < 2, and *tail* advances only at the acknowledgement, so
that check alone keeps the borrowed slot unchanged. Each consumer converts
the slot into its WAV buffer with the existing `bluetooth_pcm_wav` or
`bluetooth_pcm16_wav` and acknowledges; the recognizer transcribes after the
acknowledgement. Holding a slot for a conversion of about a millisecond cannot
cause backlog within a 1.5 to 3 s segment. The reported PCM digest is the
owner's.

After the H4 record, each PCM byte is copied by `bt_sco` into its packet, by
the owner or decoder into the slot, and by the WAV conversion. The conversion
stays because CVSD needs 8-to-16 kHz interpolation and because releasing the
slot lets the next segment record during transcription. Today each byte also
passes through a 1 KiB chunk and a 48,000-byte consumer buffer.

## Interrupt and polling

The owner keeps its 1 ms poll, which the UART and audio receive-latency
contract requires; before the first press those 1,000 wake-ups per second do
no audio work.

Consumers wait in `ulTaskNotifyTake(pdTRUE, timeout)` on index 0 and re-read
the counters and the request word after every return, so a lost or spurious
notification changes only latency. The owner gives to the bound consumer when
an activation ends and after a stop or disconnection. The Bluetooth
recognizer's per-tick waits for a segment and for USB collection become
100 ms waits, also given by `bluetooth_recognition_segment_ack` and the
recognizer's failure path. The OS client waits 250 ms, inside its 1 s
progress window, and `os_headset_capture` gives to it. All handles are set
before the scheduler starts.

## Memory placement and cache policy

No buffer is added. Slots and records keep their static placement in Normal
write-back, inner-shareable memory; the UART path is programmed I/O, so DMA
ownership and cache maintenance do not change. `retained.pcm` in
[recognition.cc](../../apps/bluetooth/recognition.cc), `headset.pcm` in
[apps/os/headset.c](../../apps/os/headset.c) (48,000 bytes each) and the
consumers' 1 KiB stack chunks are removed. New state: the request word, the
published-sample total, the parity bit, a 112-byte digest state and a few AG
words; OFF drops the call fields. Hashing each packet right after its copy
reads it from L1, where today's publication re-reads 48,000 bytes.

## Synchronization across cores

The owner and a consumer may run on different cores. Head and tail keep their
release and acquire ordering. Every change of the request word is an
acquire-release compare-and-exchange, except the owner's final store of 0. At
the end of an activation the owner stores *head* with release, then 0 in the
word with release, then notifies. A consumer that loads 0 with acquire and
then loads *head* sees any segment that activation published; an unchanged
*head* means it ended without one. Only the owner touches AG and decoder
state.

## Reuse and removal

Reused: the AG response and commit rules, the `bt_sco` link and silence
cadence, the two-slot queue and ticket check, the stop flags, the close
sequence, the incremental `storage_digest` API, the WAV conversions and the
mSBC header search. The SDP server is unchanged.

Removed:

- `bluetooth_headset_segment_copy`, which take replaces in `recognition.cc`,
  `apps/os/headset.c`, `checks/bluetooth-headset-repeated.c`,
  `checks/bluetooth-msbc.c` and the models in
  `checks/bluetooth-recognition*.cc` and `checks/os-image-headset.c`.
  `bluetooth_headset_copy` stays only for the finite modes 0, 1 and 4
  through `usb0.c`.
- The finite-capture branch of `recognition.cc`, which cannot run because
  recognition starts only for profile version 3.
- The unused initializer and wrapper, and call control in default builds.
- The consumers' 2-byte peek, chunk loops, per-chunk snapshot comparison, PCM
  buffers, second SHA-256 and per-tick waits; the OS client's discard path and
  ticket arithmetic; the hash burst at publication; and owner work on
  unrequested audio.

## Performance budgets

Sizes are text plus read-only data, and static data, from `size` on AArch64
`-O2` objects built at the base commit with `SUNSTONE_BLUETOOTH_APPLICATION`
and `SUNSTONE_A733_OS_HEADSET` on. Instruction counts come from a Unicorn
harness that runs the functions built with the image's flags (`-O2`,
`-mgeneral-regs-only`) and stays in the package build cache as
before-and-after measurement only. Poll, wake-up and byte counts come from the
`bluetooth-headset.py` and `os-image-headset.py` host models. The implementing
commit message records the after value of every row and every AT command.

| Metric | Baseline | Target |
| --- | --- | --- |
| `bt_hfp_ag.c` code, OFF | 15,066 B; 11,938 B with all call handling removed | ≤ 13,000 B |
| `bt_hfp_ag.c` code, ON | 15,066 B | ≤ 15,800 B |
| `apps/bluetooth/headset.c` code; static data | 11,879 B; 440,320 B | ≤ 13,400 B; ≤ 160 B more |
| `msbc.c` code | 1,012 B | ≤ 1,076 B |
| `apps/os/headset.c` code; static data | 2,069 B; 548,000 B | ≤ 2,100 B; ≤ 500,000 B |
| `recognition.cc` static data | includes `retained.pcm`, 48,000 B | 48,000 B smaller |
| `bt_sdp_server.c` code | 10,333 B | unchanged |
| Existing AT line, both settings | per command, below | ≤ 2% over each |
| `AT+BVRA` line | none | ≤ 1,400 instructions |
| Activation start: word exchange, digest restart, parity, resync | none | ≤ 400 instructions |
| Owner step, packet between activations | 171 CVSD, 174 mSBC | ≤ 188 CVSD, ≤ 191 mSBC |
| Owner step, packet in a segment | 422 CVSD, 36,811 mSBC, unhashed; SHA-256 1,683,514 per 48,000 B | ≤ 3,000 CVSD, ≤ 46,500 mSBC, hashed |
| Owner step, publishing packet | 1,684,087 CVSD; 1,715,924 mSBC | ≤ 8,000 CVSD, ≤ 52,000 mSBC |
| Consumer work per 48,000 B segment | 47 chunks × 1,699 plus SHA-256 (about 1,763,000) plus WAV conversion | ≤ 600 plus WAV conversion; no PCM copy |
| Owner wake-ups per second, idle | 1,000 | 1,000; ≤ 10 plus 1 per HCI packet after the UART receive interrupt |
| Consumer wake-ups while waiting | 1,000 per second | ≤ 10 per second per recognizer wait, ≤ 4 in the OS client, plus 1 per event |
| `AT+BVRA=1` line to committed on | none | ≤ 2 owner polls |
| Commit to HCI synchronous-connection setup accepted by the UART, link closed | none | ≤ 2 owner polls |
| Link open to first PCM byte in a slot | none | the poll holding the first CVSD packet or completing the first valid mSBC frame |
| mSBC bytes discarded after resync, intact stream | none | ≤ 59 |
| Publication to `+BVRA: 0` queued | none | first owner poll with the serial stream free |

AT baselines in instructions, with call control as built today: `AT+BRSF`
1,292; `AT+BAC` 1,205; `AT+CIND=?` 2,441; `AT+CIND?` 1,427; `AT+CMER` 1,567;
`AT+BIA` 1,723; `AT+VGS` 1,190; `AT+VGM` 1,173; `AT+BCS` 1,104; `AT+CMEE`
1,273; `AT+CLCC` 1,163; `AT+COPS?` 1,246; `ERROR` answers 1,781 (`AT+NREC`)
and 2,321 (`AT+XAPL`). The in-segment targets allow one 64-byte SHA-256 block
beyond the bytes added, and publication adds the two-block finalization.
Physical activation latency stays unmeasured until a board run.

New host cases:

- a window or stop during CONNECT, the AG stage, a link open, and idle with
  an AG response in flight;
- a request between publication and acceptance of `+BVRA: 0`, and
  `AT+BVRA=0` while not committed on;
- a workload 4 headset cancel followed by a button press and by a new Enter;
- zero-sample and odd-byte early ends against the digest of the even prefix;
- odd-length CVSD packets released between two activations;
- two activations on one open mSBC link separated by an odd number of
  released frames, with a packet length that does not divide 60;
- a resident peer release after `+BVRA: 0`, which must report a failure;
- both call-control settings in `bt-hfp-ag.py` and `bluetooth-headset.py`.

The builds are the Bluetooth image with recognition, with and without mSBC,
and the OS headset image.

## Left out

- Enhanced voice-recognition status, recognition text, voice tags, echo
  cancellation and in-band ringing have no board use.
- Closing the audio link between segments needs `bt_sco` to reopen a link to
  the same peer, a change for the owner of
  [bt_sco.c](../../drivers/bluetooth/bt_sco.c). Until then the link stays open
  from the first press. For CVSD that costs about 34,100 UART bytes per second
  in both directions (11.4% of the 1.5 Mbaud link), 266.7 packet releases and
  silence sends per second, and the radio's eSCO slots. The same change would
  let a headset that releases the link after `+BVRA: 0` record again instead
  of failing the session. Lending the H4 record instead of copying it byte by
  byte into a `bt_sco` packet is also a `bt_sco.c` change.
- Owner wake-up from the UART receive interrupt is a UART driver change
  ([performance findings](../audit/performance.md), UART and SPI).
- The OS recognizer's capture wait in
  [recognition.patch](../../apps/os/recognition.patch) still wakes every tick.
  A 250 ms notification wait needs `apps/mimus-board` to expose the
  recognizer's task handle.
- libsbc is built with `-mgeneral-regs-only`, so mSBC decoding costs about
  36,800 instructions per 60-byte frame, about 4.9 million per second during a
  segment. NEON decoding needs FP/SIMD context for the owner task and is a
  follow-up.
- `storage_digest.c` is scalar under `-mgeneral-regs-only`; ARMv8 SHA2
  instructions or the crypto engine's hash queue are a follow-up for its
  owner.
- Call control is not offered in the OS image, and ACL loss keeps its
  handling.
