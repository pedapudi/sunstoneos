# Local mimus transcription on A733

The FreeRTOS application transcribes a recorded 16 kHz mono clip on the physical
Orange Pi boot CPU. The native USB application matches the host on 32 development recordings for
feature and log-probability checksums, frame count, words, punctuation,
capitalization, and formatted text. A separate [multicore emulator target](mimus-smp.md)
also passes whole-recording and paced-reference comparisons with two inference
workers. Physical paced replay, persistent boot, live input,
accelerators, and performance qualification remain outstanding.

The [native USB application](mimus-usb.md) adds model and recording uploads,
repeated recognition requests, and result retrieval within one running OS session.
That document records the native asset layout and transport evidence.

## Engine and model

The companion mimus port is pinned in [apps/mimus/source.json](../apps/mimus/source.json).
It adds external monotonic-clock and task-identity providers. The sunstoneos
adapter uses the architectural counter and the current FreeRTOS task handle.
The AArch64 backend replaces integer dot and packed matrix products with NEON
implementations. Output arithmetic retains the reference ordering, with
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
CRC before parsing it. The board and Bluetooth recognition tasks authenticate
the exact model bytes with SHA-256 first, so their loads skip the tensor CRC pass.

The engine libraries `mimus_asr` and `mimus_asr_smp` add compute options after
the shared kernel options: `-march=armv8.2-a+nolse -mtune=cortex-a55
-mno-strict-align` and, for C++, `-fbuiltin`. Every A733 core is a Cortex-A55 or
Cortex-A76, both ARMv8.2-A implementations. The library runs only after the MMU
maps its code, heap, models and arena as Normal memory with alignment checking
off, so it needs no strict alignment. Builtins let the compiler inline
fixed-size copies; the bench recognition library no longer calls `memcpy` for
any constant size of 16 bytes or less. Floating-point contraction stays
disabled, and LSE atomics stay off as in the rest of the system. The dot-product
extension stays off until the cores' `ID_AA64ISAR0_EL1` values are recorded and
an int8 kernel exists to use it. Boot, monitor, driver and application sources
keep the shared options. Recognition output matches the host reference in the
emulator with these options; their speed effect on the board is unmeasured.

## Runtime and memory

A recognition task runs sequentially on the boot Cortex-A55. A higher-priority
recovery task renews the EL3 timer every 20 ticks and imposes a 180-second
application deadline. If the scheduler stops, the secure timer expires; if
recognition remains unfinished, the recovery task returns to ROM at its deadline.
The one-shot image runs once and returns its result through retained RAM.
The native USB image keeps running between recordings and uses a separate
[asset layout and ownership protocol](mimus-usb.md#ram-and-input-integrity).
The following RAM table describes the one-shot image.

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

The physical NEON image contains 395,952 bytes. ELF sections contain 392,272 bytes
of text/read-only data, 3,664 bytes of initialized data, and 4,489,728 bytes of
reserved RAM, including the heap. Input, result, and recognition-arena regions
are additional reservations. The bench recording uses a peak of 1,761,840 bytes
in the recognition arena, matching the host. The allocator obtains at most
105,984 bytes from the runtime heap through `_sbrk`. That figure includes free
allocator blocks and overhead; it does not measure live object sizes.

## Retained engine and timing records

The shared recorded engine in `apps/mimus/recorded.cc` loads the model and
creates the recognizer for a request. A one-shot caller destroys both before
the request returns. A repeated caller passes `kMimusWarm`: the recognition task
keeps both in static storage and reuses them while the model buffers stay the
same. The keyboard-triggered board image, repeated Bluetooth segments and the
[native USB service](mimus-usb.md#task-ownership-and-recovery) use this mode.
Each loads the model once, and again only after its model buffers change. A failed request marks the
retained objects stale; the next request reloads them, and a task that parks
after the failure retains them. The USB service also marks them stale before
model buffers change. One-shot images and the I2S transcription task keep the
per-request load.

A request that loads the model logs `LOAD load_us=<time> verify_crc=<0|1>`.
Whole-recording results add `TIMING total_us=<time> compute_us=<time>`.
`total_us` runs from WAV parsing through the final formatting flush. It is the
PCM-to-formatted-text time without model loading. `compute_us` covers
recognition alone, as in the `RESULT` record. Paced replay reports formatting
completion per utterance in its [`SEGMENT` records](mimus-replay.md#protocol-and-measurements).

The engine calls its yield hook once per matrix row. The hook runs the
supervised liveness checkpoint on every call where the image defines one. It
yields the processor at most once per millisecond, because preemption and time
slicing already serve equal-priority tasks. The whole-recording `MEMORY` record
reports `yields`, the number of actual yields. The emulator bench recording
yields 2,256 times in 2.95 seconds of emulated compute.

`checks/mimus-warm.py --output <directory>` runs the actual recorded engine on
the bench bundle with the host portable library. Two retained-engine requests
load once, skip the tensor CRC pass and both match the host reference. Marking
the engine stale, other model buffers, a failed request and a cold request each
cause a new load. The check models the clock and requires at most one yield
per millisecond of modeled time.

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

The OS build defaults `MIMUS_AARCH64_NEON` to `ON`. Configure it explicitly when
reusing a build directory that previously disabled the option. `OFF` selects the
portable kernels for a comparison image.

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

The initial portable image SHA-256 is
`01e0e97a19e02fa06c5fbee40e93055ec23e621ae154cec7a3059cf552ed62d2`;
its ELF hash begins `d1deb6dbc3a0d919`. Compute time was 39.086 seconds in this
single run, about 4.41 times the audio duration. There is no repeatability range.
The accepted warm recognition target is at most 0.05 times audio duration. This initial
portable result does not meet that target.

The board returned to ROM with stage 5, status 0, and automatic USB access.
Raw evidence resides under `~/.cache/sunstoneos/evidence/`:

- `mimus-host.json` and `mimus-model.json`: host and FreeRTOS emulator output.
- `mimus-physical-bounded-read-20261016/`: physical transactions, complete input
  readback, image readback, result memory, log, and source hashes.
- `mimus-emulator-parity.json` and `mimus-physical-parity.json`: comparisons.

The one-shot checks establish parity for this fixture. The native USB session
extends that evidence to 32 recordings with status polling during recognition.
Segment boundaries, cancellation, long-duration operation, other concurrent I/O,
and resource-exhaustion behavior need separate checks. No microphone is present.
I2S and Bluetooth input and full onboard hardware support remain required.

## Recognition bottlenecks and configured CPU clock

The application reports existing engine stage counters after recognition,
without adding timers inside arithmetic loops. Stages nest, so their times and
multiply-accumulate counts must not be summed as exclusive work.

One physical profiling run on the same bench recording took 39.086250 seconds.
Its checksums, words, punctuation, capitals, and formatted output matched the
existing host reference. The board returned to ROM with automatic USB access.
This check covers the added diagnostics and memory accounting; unchanged USB
and kernel checks retain their earlier evidence.

| Stage | Time | Interpretation |
| --- | --- | --- |
| Linear matrix products and output scaling | 32.430 s | 83.0% of recognition time; 4,167,910,560 useful multiply-accumulates |
| Attention matrix products | 3.709 s | 9.5% of recognition time |
| Linear input quantization | 1.603 s | Includes work nested in recognition stages |
| Linear weight expansion | 0.111 s | Small relative to matrix products |
| Audio frontend | 0.168 s | Small relative to matrix products |

These measurements identify linear kernels as the first optimization target.
The following section records the implemented Arm kernels and their screen.
The [performance plan](#performance-plan-for-the-005-target) orders the remaining
CPU work and records why GPU and NPU evaluation is deferred.

The runtime reads the oscillator selection, little-core PLL control, and CPU
selection registers without modifying them. Values were `0x983fb0f7`,
`0xf8802600`, and `0x03010000`, respectively. The
[pinned kernel clock driver](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/clk/sunxi-ng/ccu-sun60iw2-cpupll.c)
and its RTC driver decode a 26 MHz reference, multiplier 38, and divisor 2:
`26 MHz × 38 ÷ 2 = 494 MHz`. This is the configured rate inferred from registers;
an independent frequency measurement remains outstanding. The diagnostic emits
zero for a selected PLL it cannot decode or whose required enable/lock bits are
absent.

The architectural timer uses the existing 24 MHz declaration from the board
device tree. The oscillator selection describes the PLL reference; it does not
establish the architectural timer frequency. Voltage,
frequency transitions, and thermal control remain unimplemented.

The profiling image SHA-256 is
`4bf68ba1c04cf26d7fd0fe1be3044d63b9de37bb44949e9e8989ea7255804bc8`;
its ELF SHA-256 is
`d9c2e7aae44a913f2c1b3843d27e91466117f685943ef2bcf7d3fada5096c55c`.
Raw transactions, output, source/reference hashes, and readbacks reside in
`~/.cache/sunstoneos/evidence/mimus-profile-20261016/`. The comparison is
`~/.cache/sunstoneos/evidence/mimus-profile-parity.json`. The two physical runs
establish integration and profiling results, without a timing repeatability study.

## NEON matrix products halve the bench compute time

The AArch64 backend uses baseline Armv8-A NEON operations for int16 dot products
and packed matrix products. Each product is widened before accumulation into
int64 lanes. The packed path accumulates eight output channels together and
retains the existing conversion, scaling, bias, residual addition, and row-level
yield calls. It uses the existing operand layout and no additional scratch.

On the same physical recording and configured 494 MHz CPU clock, one NEON run
took 19.475858 seconds, compared with 39.086250 seconds for the portable profiling
image. This is a 2.01-times speedup in a provisional screen. There is no repeated
trial range. The feature and log-probability CRCs, frames, text, punctuation,
capitals, and formatted output all matched the host reference.

| Measurement | Portable | NEON |
| --- | --- | --- |
| Recognition compute | 39.086 s | 19.476 s |
| Linear matrix products and output scaling | 32.430 s | 14.894 s |
| Attention matrix products | 3.709 s | 1.634 s |
| Recognition arena peak | 1,761,840 bytes | 1,761,840 bytes |
| Allocator reservation peak | 105,984 bytes | 105,984 bytes |

The compute-to-audio ratio is about 2.20, so the required maximum of 0.05 remains
unmet. The [native USB session](mimus-usb.md#physical-integration-evidence) adds broader recorded-audio parity. Physical kernel
boundary checks, repeatability, and other concurrent workloads remain unqualified.

The contiguous-dot implementation alone produced no meaningful bench improvement:
39.085063 seconds with unchanged CRCs. This model primarily uses the separate
packed matrix path. That screen is retained under
`~/.cache/sunstoneos/evidence/mimus-neon-20261016/`; it does not establish a speed
benefit for contiguous-dot workloads.

Compiled-code checks executed 796 dot-family calls and 536 packed-kernel calls
under Unicorn against independent integer sums and float output bytes. Cases
cover accumulator extremes, row tails, padded strides, scaling, signed zero,
residual addition, and raw accumulator packing. Operand bounds, output guards,
input preservation, and callee-saved registers passed. The affected portable host
dot and GEMM fixtures also passed. These checks support the arithmetic change;
the physical recognition run establishes the screen’s actual board behavior.

The companion mimus tool `tools/aarch64_kernel_check.py` accepts `--elf`,
`--output`, and `--only dot|gemm|all`. Link the backend’s compiled
`dot_aarch64.cc`, `dot_portable.cc`, `gemm_portable.cc`, and
`gemm_combine_portable.cc` objects into an ELF, retaining all functions. Supply a
no-op `mimus::asr::MaybeYield()` provider for this isolated arithmetic check.
Run it with the cache Python environment containing Unicorn and pyelftools.
Raw results are `aarch64-dot.json` and `aarch64-gemm.json` in the evidence cache.

The physical NEON image SHA-256 is
`29d3f24683c92719c77094770a8622356b633b6088d107006e14def32cd97e5c`;
its ELF SHA-256 is
`76c9a8bfb6d222d365067040794fa8f9f583f1e8424f46ec687ba36cf198eae7`.
Transactions, readbacks, logs, and source identities reside under
`~/.cache/sunstoneos/evidence/mimus-neon-packed-20261016/`. The host comparison is
`~/.cache/sunstoneos/evidence/mimus-neon-packed-parity.json`. The board returned
to ROM with stage 5, status 0, and automatic USB access.

## Performance plan for the 0.05 target

The bench recording is 8.87 seconds long, so 0.05 times its duration allows
0.4435 seconds from PCM to formatted text. The single-core NEON measurement took
19.476 seconds of recognition compute, which requires a total speedup of about
44. Model loading is reported separately and is not part of that budget.

| Stage on the NEON image | Time | Share |
| --- | --- | --- |
| Linear matrix products and output scaling | 14.894 s | 76% |
| Attention matrix products | 1.634 s | 8% |
| Remaining recognition time, including quantization and the frontend | 2.948 s | 15% |

The linear stage performs 4,167,910,560 multiply-accumulates. At the target,
linear work alone must sustain about 9.4 billion per second; the NEON image
reaches about 0.28 billion per second on one core at the configured 494 MHz.

The CPU factors multiply. Each estimate below is unmeasured:

- Clock: the configured clock is 494 MHz. The datasheet lists cores up to
  2.0 GHz, a factor of up to about 4. Frequency transitions are unimplemented,
  and the rate each core type reaches is unrecorded.
- Workers: recognition uses two inference workers. Eight cores could add a
  factor of up to about 4 on the parallel stages. The serial stages limit the
  overall gain.
- Core placement: the two Cortex-A76 cores have higher per-clock throughput than
  the Cortex-A55 cores; placing the serial stages on them is unmeasured.
- Kernel: the linear kernel widens int8 weights and multiplies them by int16
  activations into int64 sums. A kernel that splits each int16 activation into
  two int8 parts for the dot-product instruction keeps the sums exact. It needs
  the dot-product ID evidence and is estimated at about 2 on the linear stage.
- Serial overhead: the retained engine already removes per-request loading.
  Quantization, attention and formatting still run once per recording.

Clock, workers and kernel together give about 32 at best, short of 44, so
attention and the remaining serial stages also need reduction.

GPU and NPU evaluation is deferred, and it remains a goal requirement. The
linear layers need exact integer sums followed by a fixed-order float scale,
which keeps results identical to the host reference. The
[NPU path](npu-two-stage.md) runs only UINT8 graphs with requantized outputs and
does not support this model's mixed precision. The
[GPU compute service](gpu-compute-service.md) runs only fixed trusted programs and
excludes mimus. Either would need a new exact-integer program to keep parity.
The NPU becomes worth revisiting if it exposes raw 32-bit or 48-bit accumulators
for exact subgraphs.

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
