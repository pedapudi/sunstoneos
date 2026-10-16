# Paced utterance replay

The native mimus service can replay a committed 16 kHz mono WAV on the sample
clock while recognizing completed utterances. The AArch64 FreeRTOS emulator
passes segmentation and recognition parity with the host reference. Physical
board scheduling, USB integration with replay, and live capture remain untested.

Mimus uses full-utterance normalization and attention. Its existing energy-based
speech segmenter determines the utterances; the recognition model is unchanged.
A segmented recording can produce different words and punctuation from recognition
of the entire recording. Parity therefore compares the same segments, including
their boundaries, on the host and target.

## Capture and recognition run independently

The replay task runs at priority 4 and releases one analysis frame after its
320 samples have arrived, every 20 ms. Deadlines derive from absolute sample
indices, so late scheduling does not shift subsequent capture deadlines.
The task records its maximum lateness and processes at most one frame per call.
It yields for at least one tick between frames when recovering from lateness.

The segmenter uses its pinned default configuration, including noise-floor
adaptation, pre-roll, silence detection, trailing audio, and maximum-length
splits. A final partial frame is zero-padded for analysis only. Recognition
receives original samples through the recording's actual end.

A single-producer, single-consumer queue holds four pending utterances. The
recognition task takes one at a time at priority 3. Queue entries contain offsets
into the immutable WAV, which remains owned until capture and recognition finish.
Queue exhaustion fails the session without replacing a pending utterance.
This recorded source does not model a microphone ring overwriting unread samples.

The capture task acknowledges completion before the recording and replay state
can be released. Recognition failure cancels capture and waits for that
acknowledgement. The existing 180-second application deadline and USB host lease
also apply. A result-capacity check prevents a segment from being partially
written into the bounded diagnostic log.

The formatter carries punctuation context between utterances, uses their speech
sample indices for pauses, and runs the existing boundary head when applicable.
It releases the final held mark at end of replay. Formatted output remains text
in the response log; no keyboard events are sent.

## Protocol and measurements

Command `0x16`, with an empty body, starts paced replay of committed assets.
It has the same ownership, generation, status, and result retrieval rules as
whole-recording command `0x14`. [The USB protocol](mimus-usb.md) defines framing.

Each `SEGMENT` record contains its sample interval, speech interval, publication
time, completion time, and completion latency from speech end. Times use the
replay start as their origin; model loading finishes before this origin.
`RESULT`, `TEXT`, `MARKS`, `CAPS`, and `FORMATTED` describe that utterance.
`FLUSH` contains the held punctuation released after the final utterance.

The final `REPLAY` record reports processed samples, published and consumed
utterances, maximum frame lateness, total elapsed time, and terminal state.
States are 0 for running, 1 for complete, 2 for queue overflow, 3 for cancellation,
and 4 for invalid input or a backwards clock. A failed session may contain
earlier completed utterances; it must not be treated as a complete transcript.

## Prepare a reference and run

Build the host reference against the portable host libraries from the pinned
mimus worktree. It uses the same application and segmentation code, advances a
logical capture clock without waiting, and drains each utterance immediately.
It provides arithmetic and formatting expectations. Its timing and heap values
do not represent the target, and it does not test scheduling or overload.

```sh
set -e
cache="$HOME/.cache/sunstoneos"
mimus="$HOME/git/mimus-worktrees/sunstoneos"
clang++ -std=c++23 -O2 -Wall -Wextra -Werror -ffp-contract=off \
  -Iinclude -Iplatform/a733 \
  -I"$mimus/firmware/components/asr/include" \
  -I"$mimus/firmware/components/pipeline/include" \
  tools/mimus-replay-reference.cc apps/mimus/replay.cc apps/mimus/paced.cc \
  "$cache/build/mimus-host/asr/libmimus_asr.a" \
  "$cache/build/mimus-host/pipeline/libmimus_pipeline.a" \
  -o "$cache/build/mimus-replay-reference"
python3 tools/prepare-replay.py --inputs "$cache/inputs/mimus-bench" \
  --wav /path/to/recording.wav --runner "$cache/build/mimus-replay-reference" \
  --output "$cache/evidence/replay-reference.json"
```

After independent board recovery and the required recovery qualification, build
the native image using [the USB build instructions](mimus-usb.md#build-and-run).
Run the recording with its paced reference:

```sh
"$cache/venv/bin/python" tools/usb-mimus.py \
  --fel "$cache/build/sunxi-a733/sunxi-fel" \
  --inputs "$cache/inputs/mimus-bench" --paced \
  --wav /path/to/recording.wav --reference "$cache/evidence/replay-reference.json"
```

The host rejects a whole-recording reference for paced replay. The parity checker
compares every segment's boundaries, feature and log-probability CRCs, frame
count, words, marks, capitals, formatted fragment, final flush, and completion
counts. Timing fields are measured but excluded from deterministic parity.

## Validation evidence

The focused source check passes sample-deadline enforcement, known speech
boundaries, a partial final frame, late capture, queue exhaustion, cancellation,
and backwards-clock refusal. The service fixture passes request selection and
asset ownership for both whole-recording recognition and paced replay.

The `a733-mimus-replay-model` target runs the actual capture and recognition tasks
under the secure AArch64 QEMU platform. Its development fixture contains the
bench recording twice with intervening silence and a partial final frame.
The fixture has 347,957 samples and produces four utterances. All four match
the host reference. Capture publishes an utterance while recognition is still
processing the preceding one. A changed final-segment CRC is rejected by the
parity checker; the checker also accepts existing whole-recording evidence.

Raw evidence, commands, fixture provenance, source hashes, and image hashes are
under `~/.cache/sunstoneos/evidence/mimus-paced-20261016/`. The final emulator
record is `model.json`; `parity.json` records its reference comparison.
The native image compiles but has not run on the board. No physical device
requests were issued for this work because the failed USB instance remains
quarantined. [Watchdog recovery](watchdog.md) remains the hardware prerequisite.

This work does not qualify I2S, Bluetooth audio, DMA, microphone buffering,
physical capture timing, or sustained real-time throughput. Those requirements
remain in the [hardware matrix](hardware.md).
