# Headset voice-recognition activation and optional call control

Status: design for review; no code is implemented yet.

The board is the Hands-Free Profile (HFP) Audio Gateway (AG) so that it can
record a headset microphone for mimus. Two changes follow. Pressing the
headset's voice button starts one recognition segment, using the HFP 1.9
voice-recognition activation procedure (section 4.25, `AT+BVRA`). Call control,
which exists only for HFP conformance and has no board use, becomes a build
option that is off by default.

## Behavior

**Call control.** The CMake option `SUNSTONE_BT_HFP_CALL_CONTROL` (default
OFF) is defined for every target that compiles
[bt_hfp_ag.c](../../drivers/bluetooth/bt_hfp_ag.c). When it is ON, the
headset application keeps today's call-control initializer, advertises AG
features 352 and sets the SDP Network attribute to 1. When it is OFF, the
call-control command handler and the RING path are compiled out, a nonzero
RING interval is refused, no call feature is advertised and Network is 0.
Without call control, the headset's post-connection call queries (`AT+CLCC`,
`AT+COPS?`, `AT+CNUM`) receive `ERROR`. A headset that drops the connection
after such an `ERROR` needs the ON setting.

**Voice recognition in the AG.** The AG advertises the voice-recognition
function: bit 2 (value 4) in `+BRSF` and in the SDP SupportedFeatures
attribute. The AG accepts activation only from a headset that set bit 3
(value 8) in its own `AT+BRSF`. It handles:

- `AT+BVRA=1`: `OK` when the application has admitted an activation and the
  service-level connection is ready; otherwise `ERROR`. A repeat while
  recognition is already on receives `OK` with no change.
- `AT+BVRA=0`: `OK`, and recognition is off. The AG sends no `+BVRA: 0` for a
  deactivation the headset started, as section 5 requires.
- `AT+BVRA=2`: `ERROR`, because enhanced voice-recognition status is not
  advertised.
- An application-started activation sends the unsolicited `+BVRA: 1`. An
  application-ended segment sends `+BVRA: 0` if recognition is still on.

As for every command, the new state commits only after the UART accepts the
complete response. The interface adds an initializer with a `voice` flag, of
which the three current initializers become wrappers, plus
`bt_hfp_ag_voice_admit` and `bt_hfp_ag_voice`. The committed recognition
state is a read-only public field.

**One request, one segment.** In the repeated capture modes (2 for CVSD and 3
for mSBC), the headset application no longer captures continuously. A segment
consumer requests one segment through `bluetooth_headset_listen` with one of
two values. *Accept the button* admits the next `AT+BVRA=1`. *Start now*
makes the AG start an activation itself; with a headset that lacks bit 3 the
board captures one segment without `+BVRA` codes, as an audio connection
without a call (HFP section 4.11). Each activation consumes the request.

- In the Bluetooth image, the recognizer requests a segment each time it
  waits for input. Its first request is *start now*, so that the unattended
  60-second supervised session still records one segment and exercises
  `+BVRA: 1` and `+BVRA: 0`. Later requests accept the button.
- In the integrated OS image, a recognizer generation request becomes
  *accept the button* in workload 0. In workload 4 the keyboard Enter key has
  already started the generation, so its request is *start now*.

A press while recognition is still running finds no request and receives
`ERROR`. A segment ends when it holds the profile's sample count, at most
24,000 samples (3 s of CVSD or 1.5 s of mSBC), or when the headset sends
`AT+BVRA=0`. A segment that ends early is published if it holds at least one
sample. Loss, the capture deadline, the supervised capture window,
cancellation and link failure keep their current stop and close behavior.

## Data flow and ownership

The Bluetooth owner task alone advances UART, H4, HCI, RFCOMM, the AG and the
synchronous audio link, as it does now. The AG engine parses `AT+BVRA` inside
its existing step. The application reads the committed recognition state in
the same owner poll and decides whether a segment starts or ends.

The first activation opens the audio link (`bt_sco_open`). Its deadline is the
current time plus the profile's response timeout, capped by the capture
window. Codec selection stays at service-level setup. The link then stays
open: HFP requires an AG to open an audio connection only if none exists,
and `bt_sco` admits one audio link per peer in each controller lifetime.
Between segments the owner releases each received packet without copying it
and keeps sending silence at the existing cadence.

During a segment, received PCM follows the existing single-copy path from the
SCO packet into a copied slot. The slot's SHA-256 digest is computed once at
publication, and the consumer copies the slot after it is published. Each
segment's `first_sample` becomes the sample position in the received stream
since the link opened, so that gaps between activations are visible. The mSBC
frame parser restarts at each activation, because skipped frames are not a
loss.

## Interrupt and polling

The owner stays tick-paced. Its 1 ms poll follows the receive-latency contract
of the UART and synchronous-audio binding, and voice recognition adds no poll
loop to it. The two consumers change from one wake-up per tick to task
notifications. After each publication, stop or disconnect, the owner calls
`xTaskNotifyGive` on the bound consumer. The consumer waits in
`ulTaskNotifyTake` with a timeout and then re-reads the shared counters, so
a lost or spurious notification changes only latency. The OS capture client
uses 250 ms, inside its 1 s progress window. The Bluetooth recognizer uses
100 ms to keep its deadline and revocation checks.
`os_headset_capture` also notifies the client when a new generation is
requested.

## Memory placement and cache policy

No buffer is added. The two copied slots, the packet records and the
consumers' private copies keep their static placement in Normal write-back,
inner-shareable memory. The Bluetooth UART path is programmed I/O, so no DMA
ownership or cache maintenance changes. The new state is a few words in the
AG object and one atomic request word in the headset state. Neither is
shared with a device.

## Synchronization across cores

The owner and the consumer may run on different cores. The existing
release/acquire counters for published and consumed segments still guard slot
reuse. The consumer writes the request word with a release store. The owner
consumes it with a compare-and-exchange from the observed request to zero, so
a request that arrives during that poll is never lost. Only the owner task
reads or writes AG state. The scheduler's notification provides the wake-up;
the atomics alone order the data.

## Reuse and removal

The design reuses the AG response and commit rules, the `bt_sco` link and
silence cadence, the two-slot segment queue with its tickets and digests, the
stop flags, the close sequence and the 8-to-16 kHz conversion. The SDP server
needs no code change: it already derives SupportedFeatures and Network from
the AG feature bits and accepts bit 2.

Because every published segment now answers a request, the OS capture client
loses its discard path and the ticket arithmetic that found the first segment
after a request. The owner stops copying, decoding and hashing audio that no
consumer requested, and default builds stop linking call control.

## Performance budgets

The baseline sizes are AArch64 `-O2` text and read-only data from `size`,
built from the base commit with `SUNSTONE_BLUETOOTH_APPLICATION=ON` and with
`SUNSTONE_A733_OS_HEADSET=ON`. The baseline instruction counts are taken with
the same Unicorn harness as
[bluetooth-playback-native.py](../../checks/bluetooth-playback-native.py) on
the base commit before coding.

| Metric | Baseline | Target | Measurement |
| --- | --- | --- | --- |
| `bt_hfp_ag.c` code, call control OFF | 15,066 B (call control always compiled) | at most 13,000 B | object size |
| `bt_hfp_ag.c` code, call control ON | 15,066 B | at most 15,800 B | object size |
| `apps/bluetooth/headset.c` code; static data | 11,879 B; 440,320 B | at most 13,400 B; at most 64 B more | object size |
| `apps/os/headset.c` code; static data | 2,069 B; 548,000 B | at most 2,300 B; at most 32 B more | object size |
| `bt_sdp_server.c` code | 10,333 B | unchanged | object size |
| Consumer wake-ups while waiting | 1,000 per second (about 3,000 per 3 s CVSD segment) | at most 4 per second in the OS client and 10 per second in the recognizer, plus 2 per segment | host models count wait returns per published segment |
| Owner audio work between segments | CVSD: 16,000 B/s copied and one 48,000 B SHA-256 every 3 s; mSBC also decodes 133 frames/s | no copy, decode or digest | host-model counters over an idle interval |
| Instructions per owner poll for one packet between segments | base-commit poll that copies one captured packet | at most that baseline | Unicorn |
| Instructions per existing AT command line | base commit, per command | at most 2% more | Unicorn |
| `AT+BVRA=1` to first PCM byte in a slot, link open | none | at most 3 owner polls | host model |
| Segment publication to `+BVRA: 0` queued | none | same owner poll | host model |

Physical activation latency, including the first audio-link setup, depends on
the radio and remains unmeasured until a board run.

The host models cover headset-started and AG-started activation, every
refusal path, `AT+BVRA` while the audio link is open, a link or service-level
connection lost during a segment, and both call-control settings. The
selected builds are the Bluetooth image with recognition, with and without
mSBC, and the OS headset image.

## Left out

- Enhanced voice-recognition status, voice-recognition text, voice tags,
  echo cancellation and in-band ringing have no board use.
- Closing the audio link between segments needs `bt_sco` to reopen a link to
  the same peer after confirmed closure. That change belongs to the owner of
  [bt_sco.c](../../drivers/bluetooth/bt_sco.c). Until it exists, a headset
  that releases the audio link ends capture through the existing failure
  path.
- Interrupt-driven wake-up of the owner from UART receive is a separate
  change in the UART driver
  ([performance findings](../audit/performance.md), UART and SPI).
- The OS recognizer's own capture wait in
  [recognition.patch](../../apps/os/recognition.patch) keeps its per-tick
  poll.
- Call control ON in the OS image needs a definition in
  [a733-os.cmake](../../cmake/a733-os.cmake); no board use requires it.
- The supervised host check `tools/check-a733-bluetooth-supervised.py`
  must accept segments shorter than the profile length and gaps in
  `first_sample`; [observe.py](../../apps/bluetooth/observe.py) changes the
  same way.
