# Recorded-audio transcription through native USB

The `a733-mimus-usb` image combines FreeRTOS, the A733 USB device driver, and the
mimus recognition engine. The host uploads model and WAV files, starts recognition,
polls its state, and retrieves formatted results while the OS remains running.
The models stay in RAM between recordings. This provides a development interface;
persistent boot, live audio capture, host cancellation, and full OS
qualification remain outstanding.
[Paced utterance replay](mimus-replay.md) is implemented and passes the FreeRTOS
emulator comparison. Its native USB path still needs physical validation.

The [USB driver](usb.md) defines the controller, descriptors, and transport
framing. The [mimus application](mimus.md) defines the model, arithmetic, input
format, and performance target. Linux is not a runtime dependency.

## Task ownership and recovery

The runtime uses a recognition task at priority 3, a USB task at priority 4, and
a recovery task at priority 5. The paced capture task also runs at priority 4
and blocks between replay requests. A static queue transfers one recognition request
from USB to the recognition task. Atomic state publication releases ownership
only after recognition and destruction of its temporary objects finish.

USB refuses asset writes, commits, and another start while recognition owns the
buffers. Each start constructs the engine from the committed model files. Model
loading verifies tensor checksums. The engine releases its temporary heap objects
before accepting the next recording; the arena is reset for each invocation.
Each recording starts a separate formatting sequence and flushes it at the end.
Cross-recording sentence continuity is not implemented.

The recovery paths have separate deadlines:

- The secure EL3 timer returns to ROM if renewal stops for two seconds.
- The recovery task enforces a 180-second recognition deadline, including model
  loading and result construction.
- The recovery task also checks that the USB task completes an iteration within
  five seconds. This prevents a stalled USB task from surviving indefinitely
  while the recovery task continues renewing the secure timer.
- Boot mode 4 returns to ROM after 30 seconds without a completed host request
  reaching dispatch. Valid polling keeps a recognition session active.
- The existing recovery command acknowledges the host, disconnects, and returns
  to ROM. It can end a session without waiting for the host lease.

The five-second USB progress check is implemented; a forced USB-task stall has
not yet been physically injected. Recovery from arbitrary faults remains outside
this integration evidence.

## RAM and input integrity

| Address | Capacity | Purpose |
| --- | --- | --- |
| `0x41000000` | Application layout | Monitor, kernel, tasks, and 4 MiB allocator heap |
| `0x42000000` | 20 KiB | Retained result, log, and USB event trace |
| `0x43000000` | 8 MiB | Base model, asset 0 |
| `0x43800000` | 8 MiB | Punctuation overlay, asset 1 |
| `0x44000000` | 2 MiB | WAV recording, asset 2 |
| `0x46000000` | 16 MiB | Recognition arena |

The native asset layout replaces the one-shot application's combined RAM bundle.
After native uploads, an earlier bundle is no longer present at `0x43000000`.
Load it again before using the one-shot application; its loader verifies existing
RAM and refuses a mismatching `--ram-input-present` request.

Each upload declares its length and CRC-32. Chunks must arrive at the next
expected offset, and the receiver bounds every write by the declared slot length.
Commit requires all bytes and a matching CRC. Recognition requires all three
assets to be committed. A failed or incomplete upload remains unusable until a
successful commit. Begin invalidates only its selected slot, allowing WAV
replacement while retaining both models.

The host verifies prepared model hashes against `inputs.json` before connecting.
It also matches the models and WAV to each saved host reference. Their logical
bundle hash uses the same serialization as `prepare-mimus.py`, even though native
USB uploads the three files separately.

## Application commands

Requests and responses use the [USB message header](usb.md#application-protocol).
Every application response starts with a little-endian uint32 status:

| Status | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Invalid length, slot, or bounds |
| 2 | Recognition owns the assets |
| 3 | Unexpected chunk or result offset |
| 4 | CRC mismatch |
| 5 | Incomplete assets or unavailable result |

The payload columns below exclude that response status word. All numeric fields
are little-endian uint32 values. Only one USB request may be outstanding.

| Command | Value | Request body | Successful response body |
| --- | --- | --- | --- |
| Application status | `0x10` | Empty | State, generation, result length, committed-slot bit mask |
| Begin asset | `0x11` | Slot, byte length, CRC-32 | Empty |
| Write asset | `0x12` | Slot, offset, up to 2,040 data bytes | Empty |
| Commit asset | `0x13` | Slot | Empty |
| Start recognition | `0x14` | Empty | Empty |
| Read result | `0x15` | Log offset | Up to 2,044 log bytes |
| Start paced replay | `0x16` | Empty | Empty |

States are 0 for idle, 1 for busy, 2 for successful completion, and 3 for failed
recognition. A successful start increments the generation. Status reports a
result length only after completion. Retrieve that result before beginning the
next upload; each start clears the retained log. An input parsing or model error
produces a failed result that the host can read. Internal assertions still return
to ROM through the recovery monitor.

## Build and run

Use the [runtime build prerequisites](runtime.md), the pinned mimus worktree,
and the pinned TinyUSB source. With the configured cache build:

```sh
set -e
cache="$HOME/.cache/sunstoneos"
cmake --build "$cache/build/qemu-virt" --target a733-mimus-usb -j 16
/usr/lib/llvm-21/bin/llvm-objcopy -O binary \
  "$cache/build/qemu-virt/a733-mimus-usb.elf" \
  "$cache/build/qemu-virt/a733-mimus-usb.bin"
"$cache/venv/bin/python" tools/usb-mimus.py \
  --fel "$cache/build/sunxi-a733/sunxi-fel" \
  --inputs "$cache/inputs/mimus-bench" \
  --reference "$cache/evidence/mimus-host.json"
```

The default recording comes from the prepared input manifest. To run several
recordings in one boot, repeat `--wav PATH --reference HOST_JSON` in matching
order. The tool requires successful host references with matching input hashes.
The [parity checker](../checks/mimus-parity.py) compares feature and log-probability
CRCs, frame counts, text, punctuation, capitalization, and formatted output.

The loader identifies ROM, verifies the image upload, requires the session access
rule, and holds the shared board lock. It then identifies the native runtime by
its descriptor before sending application commands. Any transport or unexpected
protocol failure ends communication without a reset retry. The runtime's host
lease provides the independent return path.

`--finish recover` sends the recovery command after all recordings.
`--finish lease` stops requests and verifies a return caused by the 30-second lease. That
case requires the recorded lease-expiry message and its expected nonzero return
status. The tool journals every native exchange and saves each recording's log,
input identities, and reference comparison.

## Validation scope

The host fixture `checks/mimus-service.cc` executes the actual asset service with
queue stubs and fixed RAM mappings. It checks length and offset rejection, CRC
failure, ownership during recognition, two generations, and readable success and
error results. The physical session covers actual USB transfers, scheduler
preemption, recognition, and reconnect access. Arithmetic-kernel evidence from
the NEON port remains applicable; this integration does not change those kernels.

Host-reported request latency starts after the WAV upload and ends after result
retrieval. It includes model loading, recognition, polling, and USB result
transfer. It excludes WAV transfer, capture, and host reference comparison, so it
does not measure live speech-to-text latency.

## Physical integration evidence

On 2026-10-16, one boot processed 32 distinct development recordings through
native USB. Every recording matched its host reference on feature CRC,
log-probability CRC, frames, words, punctuation, capitals, and formatted text.
The batch included the bench recording, a separate integration recording, and
30 duration quantiles from the remaining 598 `replay_devother` recordings.
It used no held-out test-clean or test-other recordings.

The recordings total 260.504 seconds and range from 2.675 to 29.84 seconds each.
Compute times range from 5.497 to 84.048 seconds across these different inputs;
this is not a timing repeatability range. The bench took 19.588 seconds of
compute and 20.259 seconds from start request through result retrieval. The
required compute target remains unmet.

Peak arena use reached 4,152,832 bytes within its 16 MiB reservation. The
allocator reservation peak remained 106,880 bytes across every recording.
That peak includes allocator overhead and free blocks; it is not live object
size or a proof against leaks over arbitrary workloads.

The session completed 13,690 framed native exchanges without a transport error.
Expected negative responses rejected an incomplete start, an incorrect offset,
a bad upload CRC, and asset replacement or another start during recognition.
Status remained available while recognition ran. Both models were uploaded once;
each recording then replaced only the WAV slot.

After the final result, the host closed its interface and issued no recovery
command. The board returned to ROM with automatic access after 31.733 seconds,
including re-enumeration. Retained state reported stage 5, status 1, and
`USB host lease expired`, identifying the intended recovery path. A preceding
two-recording integration session tested the explicit recovery command.

The final raw image contains 421,344 bytes and has SHA-256
`a9608fb8e46583f6e61703453161a9fb38e5d2f5d331b90c2b7764cef33593f8`.
Its ELF SHA-256 is
`cf5d2c7822db97ffaae0922d0ca6eb8b292762362330dc885ec33c4655110043`.
ELF sections contain 417,520 bytes of text/read-only data, 3,808 bytes of
initialized data, and 4,521,600 bytes of reserved RAM, including the heap.
The fixed input, arena, and diagnostic regions are additional reservations.

Evidence resides under `~/.cache/sunstoneos/evidence/`:

- `mimus-usb-parity32-20261016/`: final session, native-exchange journal,
  per-recording results and comparisons, input identities, recovery log, and
  aggregate measurements.
- `mimus-usb-20261016/`: initial two-recording integration and explicit ROM return.
- `mimus-host-native-qualification/`: the 30 additional host references.

The selected inputs and complete host command are retained under
`~/.cache/sunstoneos/inputs/mimus-native-qualification*.json`.
A review moved one-shot input verification inside its application deadline;
the native image and ELF remained byte-identical. `review-artifacts.json` records
that comparison, so another physical run was unnecessary.

This establishes recorded-audio parity for the physical image identified above.
The current image also includes the unqualified native paced-replay path.
That evidence covers the single-core runtime, native transfers, and recovery.
Other models, multicore
execution, physical paced/live input, accelerated backends, power transitions, forced
USB-task stalls, and long-duration or arbitrary-fault qualification remain open.
