# Local mimus transcription on A733

The FreeRTOS application transcribes a recorded 16 kHz mono clip on the physical
Orange Pi boot CPU. One bench recording matches the host’s feature checksum,
log-probability checksum, frame count, words, punctuation, capitalization, and
formatted text. This establishes an initial integration result. Wider recording
parity, paced replay, persistent boot, interactive input, accelerators, and
performance qualification remain outstanding.

## Engine and model

The companion mimus port is pinned in [apps/mimus/source.json](../apps/mimus/source.json).
It adds external monotonic-clock and task-identity providers. The sunstoneos
adapter uses the architectural counter and the current FreeRTOS task handle.
Arithmetic and portable kernels retain their existing implementation, with
floating-point contraction disabled. Profiling counter units on this path are
nanoseconds, not CPU cycles.

The application uses the P4 all-int8 model and its punctuation overlay as the
reference. `tools/prepare-mimus.py` resolves that model through mimus’s model
table, verifies its identity, and writes a separate copy declaring Orange Pi
target bit 8. Both files retain byte-identical bodies. Only target metadata,
content hashes, and the overlay’s base reference change.

The prepared base contains 7,501,968 bytes and has content hash prefix
`6b3dd2749b3db288`; the overlay contains 6,560,592 bytes with prefix
`3d7c25e823ff98a0`. The input manifest records complete hashes for the originals,
prepared files, audio, and combined bundle. `Model::Load` receives the Orange Pi
target and checks tensor CRCs. The application also checks each input’s transfer
CRC before parsing it.

## Runtime and memory

A recognition task runs sequentially on the boot Cortex-A55. A higher-priority
recovery task renews the EL3 timer every 20 ticks and imposes a 180-second
application deadline. If the scheduler stops, the secure timer expires; if
recognition remains unfinished, the recovery task returns to ROM at its deadline.
The current application runs once and returns its result through retained RAM.
It does not yet combine recognition with the native USB runtime.

| Region | Purpose |
| --- | --- |
| `0x41000000` | Monitor, FreeRTOS, application, static storage, stacks, and 4 MiB heap |
| `0x42000000` | 16 KiB retained result and log buffer |
| `0x43000000` | Input bundle, limited to 32 MiB |
| `0x46000000` | 16 MiB recognition arena |

The input bundle starts with a 128-byte header containing magic `SUNMIM01`,
three 64-bit lengths, and three 32-bit CRCs in little-endian order. Base model,
overlay, and WAV bytes follow in that order, each starting on a 64-byte boundary.
The bench bundle contains 14,346,688 bytes. The application uses a separate
97,280-byte internal arena to match the host’s portable execution configuration.

The physical image contains 396,768 bytes. ELF sections contain 393,088 bytes
of text/read-only data, 3,664 bytes of initialized data, and 4,489,664 bytes of
reserved RAM, including the heap. Input, result, and recognition-arena regions
are additional reservations. These are image and reservation sizes; peak
application allocation has not yet been measured on the board.

## Build and execute

Start with the [C++ runtime prerequisites](runtime.md). Use the isolated mimus
worktree at the recorded revision and its existing Python environment for the
model-format dependency. Prepare inputs once:

```sh
set -e
cache="$HOME/.cache/sunstoneos"
mimus="$HOME/git/mimus-worktrees/sunstoneos"
"$HOME/git/mimus/.venv/bin/python" tools/prepare-mimus.py \
  --mimus "$mimus" --output "$cache/inputs/mimus-bench"
cmake -S . -B "$cache/build/qemu-virt" -DMIMUS_DIR="$mimus"
cmake --build "$cache/build/qemu-virt" --target a733-mimus -j 16
llvm-objcopy -O binary "$cache/build/qemu-virt/a733-mimus.elf" \
  "$cache/build/qemu-virt/a733-mimus.bin"
```

Preparation refuses an existing output directory. Use a distinct directory for
changed inputs; retain the manifest with the corresponding results. The FEL
executable must include the bounded-receive patch described below. Once DDR and
automatic recovery access are established, load and check the RAM application:

```sh
python3 tools/check-a733-kernel.py \
  --fel "$cache/build/sunxi-a733/sunxi-fel" \
  --image "$cache/build/qemu-virt/a733-mimus.bin" \
  --ram-input "$cache/inputs/mimus-bench/inputs.bin" \
  --expect 'PASS mimus:' --return-timeout 200
```

`--ram-input-present` verifies a matching bundle already in RAM and avoids
uploading it again. Both paths read back the complete bundle before executing
the image. The loader holds the shared board lock and stops at a transport
failure. The companion `a733-mimus-model` target uses the secure QEMU model;
load the same bundle at `0x43000000` with QEMU’s loader device.

`checks/mimus-parity.py` compares a host reference record and either an emulator
record or a physical session record. It requires matching input-bundle identity,
successful completion, and agreement on all recorded output fields. The exact
emulator and host commands used for the bench are retained in their evidence
records, including the host formatter’s final punctuation flush.

## Physical result and limits

The fixture contains 141,920 samples, or 8.87 seconds of audio. One physical run
on 2026-10-16 produced 222 frames and these checksums:

- Features: `5ebc7d18`.
- Log probabilities: `2798af5c`.

The formatted result was:

> Hand it to me, said the chief, and when it came into his hands he began to turn over the leaves hurriedly.

The physical image SHA-256 is
`01e0e97a19e02fa06c5fbee40e93055ec23e621ae154cec7a3059cf552ed62d2`;
its ELF hash begins `d1deb6dbc3a0d919`. Compute time was 39.086 seconds in this
single run, about 4.41 times the audio duration. There is no repeatability range.
The required compute target is at most 0.25 times audio duration. This initial
portable result does not meet that target. CPU clock configuration and bottleneck
measurements must precede performance claims or optimization.

The board returned to ROM with stage 5, status 0, and automatic USB access.
Raw evidence resides under `~/.cache/sunstoneos/evidence/`:

- `mimus-host.json` and `mimus-model.json`: host and FreeRTOS emulator output.
- `mimus-physical-bounded-read-20261016/`: physical transactions, complete input
  readback, image readback, result memory, log, and source hashes.
- `mimus-emulator-parity.json` and `mimus-physical-parity.json`: comparisons.

The matching CRCs and text establish parity for this fixture only. Broader audio,
segment boundaries, cancellation, long-duration operation, concurrent I/O, and
resource-exhaustion behavior need separate checks. No microphone is present.
I2S and Bluetooth input and full onboard hardware support remain required.

## Bounded FEL reads and recovery

FEL’s previous receive implementation submitted an entire read with a ten-second
USB timeout. A 14 MB readback exceeded that limit at the board’s current USB full
speed. The loader stopped before executing the application. The ROM retained
pending response data; host logs showed no disconnect or controller fault.

`patches/sunxi-fel-bounded-read.patch` limits each receive request to 512 KiB and
preserves short-packet termination, including error status followed by an AWUS
transport response. Apply it after `sunxi-fel-status.patch` when building the
pinned FEL source. The compiled receive-function fixture checks a large read
and a short status without consuming the following response. The physical model
readback then passed with the patched tool, whose SHA-256 is
`4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e`.

The specific interrupted read was recovered without a reset. The bounded
`tools/finish-fel-read.py` operation matched pending data uniquely to the known
bundle at offset 5,758,528, verified its remaining bytes, and completed the
transport and FEL status. Evidence is in
`mimus-physical-20261016/finish-read.json`. This tool is only for a diagnosed
oversized receive with the same device instance and known bundle. It refuses a
second attempt and stops at the first mismatch or transport error; it is not an
automatic retry mechanism.
