# Headset voice-recognition activation and optional call control

Status: design for review; no code is implemented yet.

The board is the Hands-Free Profile (HFP) Audio Gateway (AG) so that it can
record a headset microphone for mimus. Pressing the headset's voice button
starts one recognition segment, using the HFP 1.9 voice-recognition activation
procedure (section 4.25, `AT+BVRA`). Call control, which exists only for HFP
conformance, becomes a build option that is off by default.

## Behavior

**Call control.** Only [a733-bluetooth.cmake](../../cmake/a733-bluetooth.cmake)
defines `SUNSTONE_BT_HFP_CALL_CONTROL`, from a CMake option that defaults to
OFF, with `target_compile_definitions` on `sunstone-a733-bluetooth` after the
loop that copies the board target's `COMPILE_DEFINITIONS`. The OS image never
defines it. [bt_hfp_ag.h](../../include/bt_hfp_ag.h) tests it with `#ifdef`
and puts the call fields last in `struct bt_hfp_ag`, so the fields that
`bt_sco.c` reads keep their offsets. The header references a guard symbol
named after the setting, which only `bt_hfp_ag.c` defines, so objects compiled
with different settings fail to link.

ON keeps today's behavior: `bt_hfp_ag_init_calls`, `bt_hfp_ag_call`, RING,
call indicators, AG features 352 and SDP Network 1. OFF compiles out
answering, dialing, hang-up, hold, DTMF, RING, call-state indicators and
`bt_hfp_ag_call`, advertises no call feature and sets Network to 0. The
queries headsets send after setup keep today's answers in both settings,
because none depends on a call: `AT+CLCC` (`OK`), `AT+COPS=3,0`, `AT+COPS?`
(`+COPS: 0`), `AT+CNUM`, `AT+CLIP`, `AT+CCWA` and `AT+CMEE` (one stored bit).
`AT+BCC`, a codec-connection request, moves out of the call handler. One board
run must still show that the target headset stays connected to an AG without
call features. `bt-hfp-ag.py` and `bluetooth-headset.py` build both settings;
`bt-sco.py`, `bt-config.py` and `bluetooth-playback-native.py` build OFF.

**One initializer.** `bt_hfp_ag_init(q, rfcomm, indicators, setup_deadline,
timeout_us, codecs, voice)` replaces `bt_hfp_ag_init` (called only by
`checks/bt-hfp-ag.c`) and `bt_hfp_ag_init_codecs` (no caller).

**Voice recognition in the AG.** With `voice` set, the AG advertises bit 2
(value 4) in `+BRSF` and the application sets the same SDP SupportedFeatures
bit, which the SDP server already cross-checks. The AG handles:

- `AT+BVRA=1`: `OK` only when the application admits an activation
  (`bt_hfp_ag_voice_admit`), the headset set bit 3 (value 8) in `AT+BRSF`,
  the service-level connection is ready, committed recognition is off and no
  `+BVRA` response is queued or in flight; otherwise `ERROR`. The last two
  conditions refuse a press between a segment's publication and acceptance of
  its `+BVRA: 0`, which would otherwise end the new activation at the headset.
- `AT+BVRA=0`: `OK`; recognition is off. No `+BVRA: 0` follows (HFP
  section 5).
- `AT+BVRA=2`: `ERROR`; enhanced status is not advertised.
- `bt_hfp_ag_voice(q, 1)` and `(q, 0)` queue `+BVRA: 1` and `+BVRA: 0`, and
  return 0 while a response owns the serial stream.

State commits only after the UART accepts the complete response.

**One request, one segment.** The repeated modes 2 (CVSD) and 3 (mSBC)
capture only on request. Mode 5, mSBC with caller playback, keeps continuous
capture and does not advertise voice recognition, because its playback pacing
and cancellation depend on an open capture.

The bound segment consumer calls `bluetooth_headset_listen(app, kind)`, which
stores the kind in a request word with a compare-and-exchange from 0 and is
refused when the word is not 0. Kind 1, *accept the button*, admits the next
`AT+BVRA=1`. Kind 2, *start now*, makes the owner start an activation, with
`+BVRA: 1` when the headset set bit 3, and otherwise as an audio connection
without a call (HFP section 4.11). The owner starts nothing while recognition
is committed on or a `+BVRA` response is pending.

The owner keeps the word non-zero for the whole activation and clears it when
it publishes. At activation start it replaces *start now* with *accept the
button*, so a cancel from the headset does not restart capture, and withdraws
AG admission until the activation ends. `listen` is therefore refused until
publication, each published segment answers the one outstanding request, and
the existing ticket check (ticket equals tail + 1) orders segments. An
activation that ends with fewer than 2 bytes publishes nothing and leaves the
word at *accept the button*.

- Bluetooth image: the recognizer requests the next segment as soon as it
  acknowledges a slot. Its first request is *start now*, so the unattended
  60-second supervised session still records one segment and exercises both
  `+BVRA` responses; that run passes `--segments 1` to the host tool.
- OS image: the capture client requests when a new generation arrives and no
  request is outstanding, *accept the button* in workload 0 and *start now*
  in workload 4, where the Enter key started the generation. The next
  published segment becomes the newest generation's WAV.

A segment ends at the profile's sample count (at most 24,000 samples: 3 s of
CVSD or 1.5 s of mSBC) or at `AT+BVRA=0`. An early end publishes the largest
even byte count, because a CVSD packet may split a sample. `first_sample`
counts samples in earlier published segments; gaps show in `first_acquired`
and `last_acquired`. [observe.py](../../apps/bluetooth/observe.py) accepts
`first_sample` up to (ticket − 1) × samples.

## Deadlines and terminal states

- Setup, through codec selection, keeps its stage deadlines and today's
  `fail()` with transport retention on a stop or the window, because ACL,
  authentication, SDP, RFCOMM or codec commands may be in flight.
- With the AG READY, the codec selected and no codec offer outstanding, the
  owner enters a new idle stage and clears the stage deadline; AG line and
  response timeouts still bound each exchange. A stop, or the supervised
  window 38 s after the session origin, ends idle at the first poll with no AG
  response or codec offer in flight, bounded by `bluetooth_close_deadline`.
  The owner reaches DONE and sets `segment_disconnected` without retention.
  Resident idle has no bound.
- An activation is admitted only while now plus the response timeout is
  within the window. The first link open and the mSBC-to-CVSD retry have the
  deadline now plus the response timeout. A stop during an open waits for its
  result: a confirmed OPEN closes through the existing close sequence, a
  confirmed setup failure ends cleanly without the retry, and
  `BT_SCO_RETAINED` fails as today.
- `capture_deadline` is armed at activation start (capture timeout, capped by
  the window) and disarmed at publication or an early end, so no DEADLINE flag
  is raised outside a segment. Silence packets get now plus the response
  timeout, capped by the window, in place of `capture_deadline`.
- With the link open, a stop or the window closes it through the existing
  sequence and discards a partial segment. A confirmed peer release ends
  cleanly; `BT_SCO_RETAINED` and other uncertain states keep `fail()`. After a
  clean end the Bluetooth recognizer finishes, and the OS client keeps
  reporting progress while `os_headset_capture` returns pending.
- DONE with no link skips the stream lookup and steps only the RFCOMM, AG,
  SDP server and security objects, all initialized before idle.

Whether a headset keeps the link after `+BVRA: 0` is unverified until a board
run with the target headset.

## Data flow and ownership

The Bluetooth owner task alone advances UART, H4, HCI, RFCOMM, the AG and the
audio link. In the poll in which the AG parses `AT+BVRA`, the application
updates admission, starts or ends the activation, and clears the request word
at publication.

During a segment, received PCM follows the existing single-copy path from the
SCO packet into a slot, and mSBC frames decode into the slot. The owner hashes
the slot with `storage_digest_add` as it writes, over the even-length prefix;
an odd trailing CVSD byte waits in the slot until the next packet or the end.
Publication and an early end call `storage_digest_end` on the published
length, and the digest state restarts at each activation and on discard.
Between segments the owner releases each packet without copying, decoding or
hashing it.

A consumer takes a segment with one call,
`bluetooth_headset_segment_take(app, dst, capacity, &snapshot)`, which
returns 0 when nothing is published. Otherwise it checks once the calling
task, the destination and snapshot ranges, that `capacity` holds
`capture.bytes`, and that head, tail and ticket agree; then it copies
`capture.bytes` with one `memcpy` and returns the snapshot. The only guard
against slot reuse is the producer's: it writes slot *head* mod 2 only while
*head − tail* < 2, and *tail* advances at the consumer's acknowledgement after
its copy. The reported PCM digest is the owner's.

## Interrupt and polling

The owner keeps its 1 ms poll, which the UART and audio receive-latency
contract requires. Before the first press that poll has no audio work, so its
1,000 wake-ups per second become idle cost until the UART receive-interrupt
change (Left out).

Consumers wait in `ulTaskNotifyTake(pdTRUE, timeout)` on index 0 and re-read
the shared counters after every return, so a lost or spurious notification
changes only latency. The owner gives to the bound consumer after a
publication, stop or disconnection.

- The Bluetooth recognizer's two per-tick waits, for a segment and for USB to
  collect a result, become 100 ms waits; `bluetooth_recognition_segment_ack`
  and the recognizer's failure path also give to it.
- The OS client waits 250 ms, inside its 1 s progress window;
  `os_headset_capture` gives to it. The client does not notify the
  recognizer, whose wait still polls every tick, so
  [apps/os/headset.c](../../apps/os/headset.c) records no recognizer handle.

The owner and consumer handles are set before the scheduler starts. Other
notification users in the tree each notify only their own worker.

## Memory placement and cache policy

No buffer is added; slots, records and the consumers' PCM copies keep their
static placement in Normal write-back, inner-shareable memory. The UART path
is programmed I/O, so DMA ownership and cache maintenance do not change. New
state: the request word, an activation flag, the published-sample total, a
112-byte digest state with its hashed count, and a few AG words; OFF drops the
call fields. Hashing each packet right after its copy reads it from L1, where
today's publication re-reads 48,000 B. The consumers' 1 KiB stack buffers go.

## Synchronization across cores

The owner and a consumer may run on different cores. Release and acquire head
and tail counters still guard slot reuse. The consumer sets the request word
with a compare-and-exchange from 0 in release order; the owner reads it with
acquire. Only the owner writes a non-zero word, so replacing *start now* is a
plain atomic store. The owner clears the word with release before storing
*head* with release, so a consumer that observes a segment can request the
next. Only the owner touches AG state. Notifications only wake tasks.

## Reuse and removal

Reused: the AG response and commit rules, the `bt_sco` link and silence
cadence, the two-slot queue and ticket check, the stop flags, the close
sequence, the incremental `storage_digest` API and the 8-to-16 kHz
conversion; the SDP server is unchanged. Removed: the unused initializer and
wrapper, call control from default builds, the OS client's discard path and
ticket arithmetic, the consumers' 2-byte peek, chunk loops, per-chunk snapshot
comparison, stack copy and second SHA-256, the hash burst at publication,
owner work on unrequested audio, and the consumers' per-tick waits.

## Performance budgets

Sizes are AArch64 `-O2` text and read-only data from `size`, built from the
base commit with `SUNSTONE_BLUETOOTH_APPLICATION=ON` and
`SUNSTONE_A733_OS_HEADSET=ON`. Instruction counts come from Unicorn running
the functions compiled with the Bluetooth image's flags (`-O2`,
`-mgeneral-regs-only`) and linked with the runtime `memcpy`, `bt_sco.c`, the
storage digest and libsbc. Each case starts from one RFCOMM buffer holding one
AT line or one received 60-byte SCO packet. The existing checks cannot give these counts:
`bt-hfp-ag.py` and `bluetooth-headset.py` build x86-64 programs at `-O1` with
AddressSanitizer, and `os-image-headset.py` runs Unicorn on a model of segment
publication. The harness therefore stays in the package build cache as
before-and-after measurement; its after values go into this table and the
implementing commit message. The `bluetooth-headset.py` and
`os-image-headset.py` host models count wake-ups, packets, UART bytes and
digest calls. CVSD sends 266.7 packets per second and mSBC 133.3.

| Metric | Baseline | Target |
| --- | --- | --- |
| `bt_hfp_ag.c` code, OFF | 15,066 B; 11,938 B with all call handling removed | ≤ 13,000 B |
| `bt_hfp_ag.c` code, ON | 15,066 B | ≤ 15,800 B |
| `apps/bluetooth/headset.c` code; static data | 11,879 B; 440,320 B | ≤ 13,400 B; ≤ 160 B more |
| `apps/os/headset.c` code; static data | 2,069 B; 548,000 B | ≤ 2,100 B; not larger |
| `bt_sdp_server.c` code | 10,333 B | unchanged |
| Existing AT line | 1,104 to 2,441 instructions per command in the harness results | ≤ 2% over each command's baseline |
| `AT+BVRA` line | none | ≤ 1,400 instructions |
| Owner step, packet between segments | 171 CVSD, 174 mSBC (release-only path) | ≤ 188 CVSD, ≤ 191 mSBC |
| Owner step, packet in a segment | 422 CVSD, 36,811 mSBC, unhashed; SHA-256 1,683,514 per 48,000 B, about 2,245 per 64-byte block | ≤ 3,000 CVSD, ≤ 46,500 mSBC, hashed |
| Owner step, publishing packet | 1,684,087 CVSD; 1,715,924 mSBC | ≤ 8,000 CVSD, ≤ 52,000 mSBC |
| Owner audio instructions per second between segments | continuous capture: about 674,000 CVSD, 6,052,000 mSBC | ≤ 50,200 CVSD, ≤ 25,500 mSBC |
| Owner wake-ups per second, idle before the first press | 1,000 | 1,000; after the UART receive-interrupt change ≤ 10 plus 1 per HCI packet |
| UART bytes per second each way between segments | 17,067 CVSD, 8,533 mSBC | unchanged until `bt_sco` reopens links |
| Consumer work per 48,000 B segment | 47 chunks × 1,699 plus SHA-256: about 1,763,000 | runtime `memcpy` of 48,000 B plus ≤ 600 |
| Consumer wake-ups while waiting | 1,000 per second | ≤ 10 per second per recognizer wait, ≤ 4 in the OS client, plus 1 per event |
| `AT+BVRA=1` to first PCM byte in a slot, link open | none | ≤ 3 owner polls |
| Publication to `+BVRA: 0` queued | none | first owner poll with the serial stream free |

The in-segment targets allow one 64-byte block beyond the bytes added (one
block for 60 CVSD bytes, four for a 240-byte mSBC frame); publication adds the
two-block finalization. The 1.5 Mbaud UART carries 150,000 bytes per second
each way, so idle CVSD audio uses 11.4% of it. Physical activation latency
stays unmeasured until a board run.

New host cases: a window or stop during CONNECT, the AG stage, a link open and
idle with an AG response in flight; a request between publication and
acceptance of `+BVRA: 0`; zero-sample and odd-byte early ends against the
digest of the even prefix; and a link of mixed call-control objects, which
must fail. The builds are the Bluetooth image with recognition, with and
without mSBC, and the OS headset image.

## Left out

- Enhanced voice-recognition status, recognition text, voice tags, echo
  cancellation and in-band ringing have no board use.
- Closing the audio link between segments needs `bt_sco` to reopen a link to
  the same peer, a change for the owner of
  [bt_sco.c](../../drivers/bluetooth/bt_sco.c). In the resident image the link
  stays open from the first press for the rest of the lifetime. Its idle cost
  is therefore the steady state: about 34,100 UART bytes per second for CVSD, both
  directions, 266.7 packet releases and silence sends per second, and the
  radio's eSCO slots. That follow-up removes the largest remaining hardware
  cost of this feature and lets a headset that releases the link after
  `+BVRA: 0` record again.
- Interrupt-driven owner wake-up from UART receive is a UART driver change
  ([performance findings](../audit/performance.md), UART and SPI).
- The OS recognizer's capture wait in
  [recognition.patch](../../apps/os/recognition.patch) still wakes every tick,
  for minutes with button gating. Replacing its `vTaskDelay(1)` with a 250 ms
  notification wait needs `apps/mimus-board` to expose the recognizer's task
  handle.
- `storage_digest.c` is scalar under `-mgeneral-regs-only`; ARMv8 SHA2
  instructions or the crypto engine's hash queue are a follow-up for its
  owner.
- The finite capture modes 0, 1 and 4 keep `bluetooth_headset_copy`, call
  control is not offered in the OS image, and ACL loss keeps its handling.
