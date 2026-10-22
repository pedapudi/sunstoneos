# Headset voice-recognition activation and optional call control

Status: design for review; no code is implemented yet.

The board is the Hands-Free Profile (HFP) Audio Gateway (AG) so that it can
record a headset microphone for mimus. Two changes follow. Pressing the
headset's voice button starts one recognition segment, using the HFP 1.9
voice-recognition activation procedure (section 4.25, `AT+BVRA`). Call control,
which exists only for HFP conformance and has no board use, becomes a build
option that is off by default.

## Behavior

**Call control.** [bt_hfp_ag.h](../../include/bt_hfp_ag.h) tests
`SUNSTONE_BT_HFP_CALL_CONTROL` with `#ifdef`, so an undefined macro means off
and every translation unit that includes the header sees the same
declarations and the same AG structure. Only
[a733-bluetooth.cmake](../../cmake/a733-bluetooth.cmake) defines it, from a
CMake option of the same name that defaults to OFF. The OS image never
defines it, so call control is always off there. With the macro, the AG keeps
today's call-control commands and RING path, `bt_hfp_ag_init_calls` (which
adds the RING interval) and `bt_hfp_ag_call`, and the headset application
advertises AG features 352 with the SDP Network attribute 1. Without it, those
functions, the call fields of `struct bt_hfp_ag` and the command handler are
not compiled, so any call-control use in an OFF build fails to compile. No
call feature is advertised and Network is 0. The headset's post-connection
call queries (`AT+CLCC`, `AT+COPS?`, `AT+CNUM`) then receive `ERROR`; a
headset that drops the connection after such an `ERROR` needs the ON build.

Host checks: `bt-hfp-ag.py` and `bluetooth-headset.py` build OFF and ON,
because the AG and `headset.c` each have a branch per setting. `bt-sco.py`,
`bt-config.py` and `bluetooth-playback-native.py` build OFF only. `bt-sco.py`
embeds [bt-hfp-ag.c](../../checks/bt-hfp-ag.c) as a fixture; that file's
call-control cases sit under the same `#ifdef`, so `bt-sco` needs no change.

**One initializer.** `bt_hfp_ag_init(q, rfcomm, indicators, setup_deadline,
timeout_us, codecs, voice)` replaces the current `bt_hfp_ag_init` and
`bt_hfp_ag_init_codecs`. The latter has no caller, and the former is called
only by `checks/bt-hfp-ag.c`, which is updated. The headset application calls
`bt_hfp_ag_init`, or `bt_hfp_ag_init_calls` in an ON build.

**Voice recognition in the AG.** With `voice` set, the AG advertises bit 2
(value 4) in `+BRSF`, and the application sets the same bit in the SDP
SupportedFeatures attribute; the SDP server already refuses a SupportedFeatures
value that disagrees with the BRSF bits. The AG handles:

- `AT+BVRA=1`: `OK` only when the application has admitted an activation
  (`bt_hfp_ag_voice_admit`), the headset set bit 3 (value 8) in its
  `AT+BRSF` and the service-level connection is ready; otherwise `ERROR`.
  The application withdraws admission when an activation starts, so a press
  while recognition is on receives `ERROR` under the same rule.
- `AT+BVRA=0`: `OK`, and recognition is off in either prior state. No
  `+BVRA: 0` follows, as HFP section 5 requires.
- `AT+BVRA=2`: `ERROR`, because enhanced voice-recognition status is not
  advertised.
- `bt_hfp_ag_voice(q, 1)` and `bt_hfp_ag_voice(q, 0)` queue the unsolicited
  `+BVRA: 1` and `+BVRA: 0`. Like `bt_hfp_ag_call`, they return 0 while a
  response owns the serial stream.

As for every command, the new state commits only after the UART accepts the
complete response. The committed recognition state is a read-only field.

**One request, one segment.** The repeated capture modes 2 (CVSD) and 3
(mSBC) capture only on request. Mode 5, repeated mSBC with caller playback,
keeps continuous capture, does not advertise voice recognition, and its
recognizer makes no request, because playback pacing and cancellation depend
on an open capture.

The bound segment consumer requests one segment with
`bluetooth_headset_listen(app, sequence, kind)`. The call stores
`sequence << 2 | kind` in one atomic request word, and refuses while a request
is outstanding or when the sequence does not increase. *Accept the button*
admits the next `AT+BVRA=1`. *Start now* makes the AG start an activation
itself, with `+BVRA: 1` when the headset set bit 3; otherwise the board
captures one segment as an audio connection without a call (HFP section
4.11). The owner consumes the request when an activation starts.

- In the Bluetooth image, the recognizer requests the next segment as soon as
  it acknowledges a slot, so a press during transcription is accepted. Its
  first request is *start now*, so the unattended 60-second supervised session
  still records one segment and exercises `+BVRA: 1` and `+BVRA: 0`. Later
  requests accept the button. An unattended supervised run passes
  `--segments 1` to the existing host tool.
- In the OS image, generation *g* requests sequence *g*: *accept the button*
  in workload 0, and *start now* in workload 4, where the keyboard Enter key
  has already started the generation.

A segment ends when it holds the profile's sample count, at most 24,000
samples (3 s of CVSD or 1.5 s of mSBC), or when the headset sends
`AT+BVRA=0`. An early end publishes the largest even byte count, because a
CVSD packet may split a sample; the last record's copied count and the digest
follow the truncated length. An activation that ends with fewer than 2 bytes,
such as a quick double press or a deactivation that crosses `+BVRA: 1`,
publishes nothing. The owner then restores the request word with the same
sequence and the kind *accept the button*, so that a cancel from the headset
does not restart capture at once.

Each segment snapshot carries the request sequence it answers, and both
consumers compare it with the sequence they requested. This one comparison
replaces the OS client's ticket arithmetic and keeps its guard against a
segment that does not answer its request. `first_sample` is the total of
samples in previously published segments, advanced only at publication; gaps
between activations show in `first_acquired` and `last_acquired`.
[observe.py](../../apps/bluetooth/observe.py) accepts `first_sample` up to
(ticket − 1) × samples and input samples from 1 to the profile count; the
supervised host checker uses it unchanged.

## Deadlines and terminal states

Today's deadlines assume continuous capture. The request-gated modes use
these rules:

- Setup until the AG is ready keeps its stage deadlines.
- After the AG is ready and the codec is selected, the owner enters an idle
  stage and clears the stage deadline. The AG's per-line and response
  timeouts still bound every exchange. In the supervised lifetime the capture
  window, 38 s after the session origin, ends the idle stage with the WINDOW
  flag; in the resident lifetime idle has no bound.
- The first audio-link open has the deadline now plus the profile's response
  timeout, capped by the window. The mSBC-to-CVSD retry gets the same bound.
- `capture_deadline` is armed when an activation starts, at now plus the
  capture timeout capped by the window, and disarmed at publication or any
  early end. No DEADLINE flag can be raised while no segment is active.
- Every silence packet, during or between segments, has the deadline now
  plus the response timeout, capped by the window, and its cadence is capped
  by the window. Today it uses `capture_deadline`, which would refuse every
  send once that deadline passes.
- A stop, from the consumer or the window, before the audio link exists ends
  cleanly: the owner reaches DONE and sets `segment_disconnected` without
  retaining transports. Today a stop before audio calls `fail()`.
- A confirmed peer release of the audio link (`BT_SCO_CLOSED` after a matched
  disconnection completion) at any time after it opened also ends cleanly,
  as the existing close does. An unpublished partial segment is discarded.
  `BT_SCO_RETAINED` and other uncertain states keep `fail()` and transport
  retention. The Bluetooth recognizer already finishes cleanly on a
  disconnection with nothing pending. The OS client stops copying and keeps
  reporting progress, and `os_headset_capture` keeps returning pending;
  neither calls `board_fail`.

Whether a headset keeps the audio link after `+BVRA: 0` is unverified until a
board run with the target headset. A headset that releases it ends the
session after one segment until `bt_sco` can reopen a link (see Left out).

## Data flow and ownership

The Bluetooth owner task alone advances UART, H4, HCI, RFCOMM, the AG and the
synchronous audio link. The AG parses `AT+BVRA` inside its existing step. In
the same poll the application updates admission from the request word, starts
and ends segments from the committed AG state and consumes requests.

During a segment, received PCM follows the existing single-copy path from the
SCO packet into a slot; mSBC frames decode into the slot. The owner computes
the slot's SHA-256 once, at publication. Between segments the owner releases
each received packet without copying, decoding or hashing it, and the mSBC
frame parser restarts at each activation.

Each consumer copies a published segment once, straight into its private PCM
buffer. `bluetooth_headset_segment_copy` checks the destination over the
requested length, so the final chunk at the end of a buffer is accepted, and
the 1 KiB stack buffer is removed. The consumers' second SHA-256 is removed.
The producer writes only slot *head* mod 2 while *head − tail* < 2, a check
that still runs, so a published slot is never written before its
acknowledgement. Consumers still compare the snapshot after every chunk, which
detects a reused slot. The reported PCM digest remains the owner's.

## Interrupt and polling

The owner stays tick-paced: its 1 ms poll follows the receive-latency
contract of the UART and synchronous-audio binding, and voice recognition
adds no poll loop. Consumers wait in `ulTaskNotifyTake(pdTRUE, timeout)` on
notification index 0 and re-read the shared counters after every return, so
a lost or spurious notification changes only latency.

- The owner gives to the bound consumer after a publication, stop or
  disconnection.
- The Bluetooth recognizer converts both of its per-tick waits, for a segment
  and for USB to collect a result, to a 100 ms timeout.
  `bluetooth_recognition_segment_ack` and the recognizer's failure path give
  to it.
- The OS client uses a 250 ms timeout, inside its 1 s progress window.
  `os_headset_capture` gives to the client when a generation is requested.
  [apps/os/headset.c](../../apps/os/headset.c) records the calling recognizer
  task at the first `os_headset_capture` (a different caller is refused) and
  gives to it when it stores a capture.

No other code waits on these tasks' notifications. The tree's other users
are the DMA and NPU services and the UART, SPI and board host interrupt
paths, each notifying its own worker, and the linked mimus components use
none. The consumer handles are fixed for the life of the image.

## Memory placement and cache policy

No buffer is added. The two slots, the packet records and the consumers'
PCM copies keep their static placement in Normal write-back, inner-shareable
memory. The Bluetooth UART path is programmed I/O, so no DMA ownership or
cache maintenance changes. The new state is the request word, the active
request and the published-sample total in the headset state, and a few words
in the AG object. The OFF build drops the AG's call fields.

## Synchronization across cores

The owner and a consumer may run on different cores. The release and acquire
counters for published and consumed segments still guard slot reuse. The
consumer stores a request with a compare-and-exchange from 0 in release
order. The owner consumes it with a compare-and-exchange to 0 and restores
it with one from 0; a failed restore means the consumer broke the one-request
rule, and the owner fails. Only the owner task touches AG state.
Notifications only wake tasks; the atomics order the data.

## Reuse and removal

The design reuses the AG response and commit rules, the `bt_sco` link and
silence cadence, the two-slot queue, the stop flags, the close sequence and
the 8-to-16 kHz conversion. The SDP server needs no change. The design
removes the unused initializer and the wrapper layer, call control from
default builds, the OS client's discard path and ticket arithmetic, the
consumers' stack copy and second digest, owner work on unrequested audio, and
the per-tick waits in the two consumers.

## Performance budgets

Object sizes are AArch64 `-O2` text and read-only data from `size`, built
from the base commit with `SUNSTONE_BLUETOOTH_APPLICATION=ON` and
`SUNSTONE_A733_OS_HEADSET=ON`. Instruction counts come from Unicorn running
the base commit's functions, compiled with the Bluetooth image's flags and
linked with the runtime `memcpy`, `bt_sco.c`, the storage digest and the
pinned libsbc. Each case starts from a fixed state: one RFCOMM buffer holding
one AT line, or one received 60-byte SCO packet. The harness is added as
`checks/bluetooth-voice-cost.py` with the code and rerun after it. Host
models count wake-ups, packets, UART bytes and digest calls. Per-second
figures use 60-byte packets: 266.7 per second for CVSD and 133.3 for mSBC.

| Metric | Baseline | Target |
| --- | --- | --- |
| `bt_hfp_ag.c` code, call control OFF | 15,066 B (call control always compiled) | ≤ 13,000 B |
| `bt_hfp_ag.c` code, call control ON | 15,066 B | ≤ 15,800 B |
| `apps/bluetooth/headset.c` code; static data | 11,879 B; 440,320 B | ≤ 13,400 B; ≤ 64 B more |
| `apps/os/headset.c` code; static data | 2,069 B; 548,000 B | ≤ 2,300 B; ≤ 32 B more |
| `bt_sdp_server.c` code | 10,333 B | unchanged |
| Existing AT line, input and response | per command below | ≤ 2% more |
| `AT+BVRA=1` or `AT+BVRA=0` line | none | ≤ 1,400 instructions |
| Owner step for one packet between segments | 171 CVSD, 174 mSBC on today's release-only path; `bt_sco_receive` and its release 109 | ≤ 188 CVSD, ≤ 191 mSBC |
| Owner step for one packet in a segment | 422 CVSD; 36,811 mSBC (one frame decoded) | ≤ 10% more |
| Owner step for the publishing packet | 1,684,087 CVSD, of which SHA-256 of 48,000 B is 1,683,514; 1,715,924 mSBC | ≤ 10% more |
| Owner audio-packet instructions per second between segments | continuous capture: about 674,000 CVSD, 6,052,000 mSBC | ≤ 50,200 CVSD, ≤ 25,500 mSBC |
| UART bytes per second each way between segments | 17,067 CVSD, 8,533 mSBC (as in capture) | unchanged until `bt_sco` reopens links |
| Consumer work per 48,000 B segment | 47 chunks × 1,699 plus SHA-256 1,683,514: about 1,763,000 | ≤ 53,600, no digest |
| Consumer wake-ups while waiting | 1,000 per second in each wait | ≤ 10 per second per recognizer wait, ≤ 4 in the OS client, plus 1 per event |
| `AT+BVRA=1` to first PCM byte in a slot, link open | none | ≤ 3 owner polls |
| Segment publication to `+BVRA: 0` queued | none | same owner poll |

The per-second owner figures exclude the poll's lower-layer work and silence
sends, which this design does not change. The 1.5 Mbaud UART carries 150,000
bytes per second each way, so idle CVSD audio uses 11.4% of it.

Baseline instructions per AT line, with call control on and with it off at
run time, the nearest existing case to an OFF build:

| Command | On | Off |
| --- | --- | --- |
| `AT+BRSF=959`, `AT+BAC=1,2`, `AT+BCS=2` | 1,292; 1,205; 1,104 | same |
| `AT+CIND=?`, `AT+CIND?` | 2,441; 1,427 | same |
| `AT+CMER=3,0,0,1`, `AT+BIA=1,1,1,1,1,1,1` | 1,567; 1,723 | same |
| `AT+VGS=8`, `AT+VGM=8` | 1,190; 1,173 | same |
| `AT+CMEE=1`, `AT+CLCC`, `AT+COPS?` | 1,273; 1,163; 1,246 | 1,769; 1,683; 1,719 (`ERROR`) |
| Unknown command (`AT+NREC=0`) | 1,781 | 1,738 |

Physical activation latency, including the first audio-link setup, depends on
the radio and stays unmeasured until a board run.

The host models cover:

- headset-started and AG-started activation, every refusal path and
  `AT+BVRA` while the audio link is open;
- zero-sample and odd-byte early ends for both kinds of start;
- in the resident lifetime, a first press more than 40 s after start and gaps
  between presses longer than the capture timeout;
- in the supervised lifetime, idle until the capture window;
- peer release and uncertain loss of the audio link, between and during
  segments;
- mode 5 and both call-control settings.

The selected builds are the Bluetooth image with recognition, with and
without mSBC, and the OS headset image.

## Left out

- Enhanced voice-recognition status, voice-recognition text, voice tags,
  echo cancellation and in-band ringing have no board use.
- Closing the audio link between segments needs `bt_sco` to reopen a link to
  the same peer after confirmed closure, a change for the owner of
  [bt_sco.c](../../drivers/bluetooth/bt_sco.c). It would remove the idle UART
  traffic (about 34,100 bytes per second for CVSD and 17,100 for mSBC, both
  directions together), the idle packet work and the radio's eSCO slots, and
  let a headset that releases the link after `+BVRA: 0` record again.
- Interrupt-driven wake-up of the owner from UART receive is a separate
  change in the UART driver
  ([performance findings](../audit/performance.md), UART and SPI).
- The OS recognizer's capture wait in
  [recognition.patch](../../apps/os/recognition.patch) still wakes every tick.
  With button gating in workload 0 it lasts until someone presses, possibly
  minutes. The follow-up replaces its `vTaskDelay(1)` with
  `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250))`; the notification already
  comes from `apps/os/headset.c`.
- Call control in the OS image is not offered.
- Loss of the ACL link keeps its current handling; reconnection is separate
  work.
