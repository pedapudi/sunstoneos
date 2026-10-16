# Mimus recognition on two FreeRTOS cores

Mimus runs with the [SMP C/C++ runtime](smp-runtime.md) on two emulated
Cortex-A55 cores. A FreeRTOS worker executes the engine's existing parallel
ranges. Both whole-recording transcription and paced utterance replay match
their host references. This is **QEMU functional evidence**; physical multicore
startup, cache coherence, recovery, and performance remain unqualified.

## Recognition owns work until the worker completes

The recognition task runs on core 0 at priority 3. A worker runs on core 1 at
the same priority, with a statically allocated 64 KiB stack. Paced capture runs
on core 1 at priority 4, so it can preempt inference. The application deadline
task runs at priority 5 and can run on either core.

The [parallel adapter](../apps/mimus/parallel_runtime.cc) installs the existing
mimus `ParallelFor` interface. For at least two items, it sends the upper half
to the worker and computes the lower half on the recognition task. Smaller
ranges execute locally. Worker indices remain 0 and 1, matching the engine's
per-worker scratch layout.

One queue publishes each callback, context, bounds, and sequence number.
Another acknowledges completion. Recognition retains the context and output
buffers until the matching acknowledgement arrives. Queue synchronization
publishes CPU stores through the shared cacheable mapping. This mechanism
requires coherent CPU memory; it does not synchronize noncoherent DMA.

Only the bound recognition task may submit work, and calls cannot nest.
Assertions verify the caller on core 0 and the worker on core 1. A final
`WORKERS` record reports completed jobs; the acceptance tool requires a
nonzero count. This adapter has no asynchronous job interface. Execution on all
eight physical cores remains outstanding.

## Packed matrix products share rows between workers

The AArch64 engine also splits eligible packed int16 matrix products across
both workers. Expanded weights and quantized inputs remain read-only until the
join. Integer sums are local to each worker. Residual accumulation uses a
private float buffer of at most 256 outputs per worker.

The split requires at least 16 activation rows, complete packed groups, an
output stride of at least 64 bytes, and no shared staging or active input
handoff. Blocks with split-K accumulation, narrow activations, or larger residual
scratch requirements retain their existing path. Each output keeps its serial
arithmetic order.

If the row boundary can share a cache line, one row separates the workers'
writes. The caller computes that row after the join. The focused matrix check
covers 12 shapes in both plain and residual modes, comparing all output bytes
with serial execution. It checks the 16-row admission threshold, unaligned
bases and strides, padding, partial packed groups, and the private-scratch bound.
It also covers multiple weight blocks, grouped weights, and narrow activations.
Input checksums and output guards pass. One bounded session completed 45 worker
jobs. The portable host kernel preprocesses to the same tokens as the previous
revision, so its arithmetic evidence remains applicable.

```sh
cache="$HOME/.cache/sunstoneos"
cmake --build "$cache/build/qemu-virt" --target smp-mimus-linear-model -j 8
python3 tools/check-smp.py "$cache/build/qemu-virt/smp-mimus-linear-model.elf" \
  --case linear --qemu "$cache/host/usr/bin/qemu-system-aarch64" \
  --evidence "$cache/evidence/mimus-smp-linear.json"
```

## The application uses the rebuilt C library

The build recompiles the pinned recognition sources against dynamic-reentrancy
newlib headers. It retains the AArch64 NEON backend and disables floating-point
contraction. The model, punctuation overlay, and inference arithmetic are
unchanged. The source pin remains [apps/mimus/source.json](../apps/mimus/source.json).

A project termination handler replaces the prebuilt C++ handler that reads
newlib's global task pointer. It stops through the platform panic path without
allocation or stdio. Linking still rejects every `_impure_ptr` reference.
Normal recognition exercises the resulting linked library; the termination
failure path has not received a separate execution check.

The emulator clock adapter reads the architectural counter and its reported
frequency. The application retains its 180-second recognition deadline. QEMU
also has a separate 180-second host timeout. The emulator has no A733 secure
monitor or independent hardware-watchdog recovery.

The image uses the SMP runtime's code, data, stacks, and 4 MiB heap layout.
Inputs load at `0x43000000`, and inference uses the existing 16 MiB arena at
`0x46000000`. QEMU UART output replaces the physical bounded result log for
these targets. Physical USB and result-buffer behavior use their existing
implementation.

## Build and compare recorded inputs

Configure the [SMP runtime dependencies](smp-runtime.md#build-and-focused-evidence)
and the pinned `MIMUS_DIR` in the existing Clang build. Prepare inputs with
[the application tools](mimus.md) and retain a host reference for that bundle.

```sh
cache="$HOME/.cache/sunstoneos"
cmake --build "$cache/build/qemu-virt" \
  --target smp-mimus-model smp-mimus-replay-model -j 8
python3 tools/check-mimus-smp.py \
  --image "$cache/build/qemu-virt/smp-mimus-model.elf" \
  --bundle "$cache/inputs/mimus-bench/inputs.bin" \
  --reference "$cache/evidence/mimus-host.json" \
  --evidence "$cache/evidence/mimus-smp-batch" \
  --qemu "$cache/host/usr/bin/qemu-system-aarch64"
```

For paced replay, select `smp-mimus-replay-model.elf`, add `--paced`, and supply
the matching [paced bundle and reference](mimus-replay.md). Use a separate
evidence directory. The runner verifies the input hash and reference mode
before execution. It records the image, emulator, loaded host libraries,
command, runner, and comparison identities. A matching successful record is
reused. Each new case runs once and stops on failure.

## Functional evidence and resource use

On 2026-10-16, one whole-recording session passed the 141,920-sample bench
fixture with 1,115 completed worker jobs. Feature and log-probability CRCs,
frames, words, punctuation, capitalization, and formatted text match the host.
The inference arena peak was 1,761,840 bytes; the allocator reservation peak
was 106,944 bytes.

One paced session passed a 347,957-sample fixture with four utterances and
3,427 completed worker jobs. Segment boundaries, both CRCs, frame counts, words,
marks, capitals, formatted fragments, and the final punctuation flush match.
Capture completed without queue overflow. The inference arena peak was
1,594,864 bytes; the allocator reservation peak was 106,112 bytes.

The initial runtime integration passed these cases with sequential inference
and then with a worker. The matrix-row extension passed each case once on its
final candidate. Emulator timings are retained in the raw output and do not
establish board throughput or latency.

| Image | Code/read-only bytes | Initialized data bytes | Reserved RAM bytes | ELF SHA-256 |
| --- | ---: | ---: | ---: | --- |
| Whole-recording recognition | 350,004 | 3,072 | 4,591,136 | `1792f4483b2554a4b716034be0d8bcc3504bfdeb1dbe6452234c07e0c0488b29` |
| Paced recognition | 358,196 | 3,072 | 4,624,720 | `bf7559104a6150a840008b6b487db3862f1fd68d085964b3e9da579712472ffd` |

Reserved RAM includes the heap and static stacks; inputs and the inference
arena occupy separate address ranges. Raw evidence is under
`~/.cache/sunstoneos/evidence/mimus-smp-matrix-20261016/`. The `batch` and `paced`
directories contain final images, `model.json`, and `parity.json`.
`linear.json` records the matrix check; `portable-source.json` records the host
preprocessing comparison. `manifest.json` records compiled source, header,
library, and tool identities. The initial runtime and worker integration remains
under `~/.cache/sunstoneos/evidence/mimus-smp-20261016/`.

The existing runtime-check image remains byte-identical, so its evidence is
reused. The affected physical USB application also compiles; it has not been
executed on the quarantined board. Broader recording coverage, sustained
concurrent workloads, cancellation during inference, stack-use measurement,
physical worker execution, and the performance target remain outstanding.
