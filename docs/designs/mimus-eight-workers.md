# Eight CPU workers for independent recognition rows

Status: implemented and checked in software. The two-worker configuration
remains the default; native eight-worker execution has not run on hardware. This design changes neither the recognition
model nor its weights, quantization, accumulation order or formatting.

The local mimus port at `9c57286b7e1e50cd2181438a2d9ab717102d288e` limits
`kMaxWorkers` to two. Its `ForRowRanges` helper also creates only two ranges.
Stateful row producers allocate two private contexts, and the packed matrix
path repairs one shared cache-line boundary after joining both workers.
Changing the worker limit alone would neither distribute these stages across
eight cores nor preserve their scratch ownership.

## Independent row stages use up to eight workers

An optional eight-worker build extends the existing row-range interface.
The splitter chooses at most eight nonempty contiguous ranges, each with at
least eight rows. Every boundary must align every declared output to a 64-byte
cache line. A range processes each output in its original arithmetic order.
The existing two-worker splitter remains the path for two-worker builds.

Aligned packed AArch64 matrix products use the extended splitter. Each callback
uses its own stack-local accumulator of at most 256 floats. The unaligned
matrix path retains two ranges and its existing single-row boundary repair.
Frontend FFT/log-mel processing retains its two private frame scratch sets.
Produced quantization retains two contexts and an explicit two-range limit.
Its scratch arrays and producer rings keep their existing two-context bounds.
These exclusions make the change useful without redesigning producer pipelines.

The engine's asynchronous interface remains available in two-worker mode.
Eight-worker mode declines asynchronous work, allowing the engine to complete
that item locally before distributing later matrix products. The current
engine otherwise reports one available worker throughout an asynchronous item.

## Each worker owns one bounded job until acknowledgement

The adapter uses seven static worker slots. Each slot has a 64 KiB stack,
private queue storage and a completion queue. Slot boundaries occupy separate
64-byte cache lines. The recognition task submits at most one job per slot,
computes its own range and waits for every matching completion sequence.
Callback contexts, input buffers and output buffers remain owned until every
submitted callback completes. Calls cannot nest, and only the bound recognition
task submits work. No job allocation or work-stealing scheduler is introduced.

Each worker keeps the existing supervised checkpoints around its callback.
A fault, stale acknowledgement or session revocation cannot manufacture a join
or release a borrowed context. The existing terminal path retains unfinished
work. Native progress is reported only after the entire range operation joins.

In the admitted native configuration, worker index 0 is the recognition task
on Cortex-A76 core 6. Worker index 1 uses A76 core 7; indices 2 through 7 use
A55 cores 0 through 5. Emulator indices map directly to cores 0 through 7.
Ranges are divided by item count; no unmeasured heterogeneous speed ratio is
encoded. Two-worker calls keep their existing owner and worker arguments.

The optional eight-worker configuration extends the normal A76-only inference
placement to A55 cores. Existing service affinities stay in place. Inference
workers run at priority 3; native PMIC, USB and watchdog tasks run at priority 4.
The existing recovery task runs at priority 5. A focused emulator case must
show higher-priority service progress on core 0 while its inference worker has
an outstanding callback. Physical service latency and heterogeneous throughput
remain unmeasured; this configuration does not establish physical admission.

## Static memory and synchronization have fixed bounds

Compared with the existing worker, six added stacks reserve 384 KiB.
Additional queue and task state must remain below 16 KiB. QEMU reserves one
32 KiB exception stack per selected core, adding 192 KiB in eight-core builds. A range operation
uses at most seven submissions and seven acknowledgements. Per-callback matrix
residual scratch remains at most 1 KiB. Producer arena reservations do not grow.
The existing 16 MiB inference arena and the model input addresses remain fixed.

The worker count is selected at build time as two or eight. The existing
`mimus_parallel_init(owner, worker)` interface remains valid; the adapter obtains
the selected count from `SUNSTONE_MIMUS_WORKERS`. All linked engine and adapter
translation units must agree on the capacity. Eight-worker native operation
requires admitted A76 placement. No OS thermal or session format changes belong
to this implementation.

## Focused checks establish software behavior

The implementation uses the existing matrix fixture for arithmetic parity,
output guards, unaligned boundaries and scratch limits. Additional bounded
adapter cases check all worker indices, short ranges, completion identity,
borrowed-context lifetime and higher-priority service progress. Host row-split
checks cover aligned and unaligned outputs under sanitizers.

One eight-core QEMU recognition run uses the existing bench input bundle at
`~/.cache/sunstoneos/inputs/mimus-bench/inputs.bin`, SHA-256
`5696b67ce99c40804ed9d876d021bac7ba122ae3b1a80f3210b66ec9b550e0c3`.
Its successful host reference is `~/.cache/sunstoneos/evidence/mimus-host.json`,
SHA-256 `493aa5bd64a5708fbc9a04fd76a69bd6b0b4d19aefe92dfa5a08c2c6b77762ea`.
The existing comparator must match features, log probabilities, frames, words,
punctuation, capitalization and formatted text. Execution must record completed
work on all eight cores. Emulator timing is not a physical performance result.

The engine changes belong to a separate local mimus worktree based on the
accepted port. They are limited to the parallel interface, row dispatch,
explicit producer scratch bounds and focused existing tests. SunstoneOS owns
the FreeRTOS adapter, worker configuration, selected build targets, source pin
and focused runners. The coordinator retains OS thermal and session ownership.

## Build selection and observed results

Configure with `-DSUNSTONE_MIMUS_WORKERS=8` to select eight workers in the SMP
engine and its linked callers. Omitting the option selects two. These public
compile definitions also reach the integrated OS through its board composition.
The integrated OS admits eight workers for supervised recorded recognition
with A76 admission. Its A55 inference workers run at priority 2, below the OS
services, while the A76 pair remains at priority 3. Linked configuration and
host entry checks bind that placement; other OS profiles retain two workers.
The [OS extension](mimus-os-eight-workers.md) records its supported composition,
resource bounds and focused evidence. Standalone native and QEMU eight-worker
builds retain the adapter behavior described here. Native eight-worker
builds also require `-DA733_A76_ADMITTED=ON`; their recognition owner uses core 6,
and their first worker uses core 7. This build selection does not qualify a
physical session or change the existing clock, rail or failure-recovery rules.

The local engine revision is
`967747f10dcf038aadc2961b5990d540f6b76455`. Its two- and eight-worker row-split
fixtures passed AddressSanitizer and UndefinedBehaviorSanitizer with fatal
sanitizer errors. The actual FreeRTOS adapter and AArch64 matrix fixture passed
in both configurations: 16 shapes, each with plain and residual output. The
fixture checks complete joins before reusing stack contexts, short and uneven
item counts, private outputs and higher-priority service progress on core 0.
The two-worker case completed 141 worker jobs, including 20 asynchronous jobs;
the eight-worker case completed 598 jobs with asynchronous work disabled.

One eight-core recognition run used the unchanged bundle and host reference
identified above. Features (`5ebc7d18`), log probabilities (`2798af5c`), all 222
frames, words, punctuation, capitalization and formatted text matched. The
adapter recorded 5,067 completed jobs and core mask `ff`. The native image also
built successfully. No physical speed, service-latency or recovery result is
inferred from these checks.

The linked worker stacks occupy 458,752 bytes in eight-worker images, compared
with 65,536 bytes in the two-worker fixture. Worker queue/task slots occupy
6,720 and 960 bytes respectively. The eight-core recognition emulator reserves
exception stacks at `[0x412ba940, 0x412fa940)`, exactly eight 32 KiB stacks.
Its heap ends at `0x416fa940`; the native image's heap ends at `0x417569c0`.
Both remain below the image RAM limit `0x42000000` and the fixed model and
inference-arena regions.

Evidence is retained in
`~/.cache/sunstoneos/evidence/resume-mimus-eight-20261025/`:
`host-ranges.txt`, `linear-two.json`, `linear-eight.json`,
`recognition/model.json`, `recognition/parity.json`, `images.json` and
`source-closure.json`. The image record contains all four ELF hashes, section
extents and every emulator exception-stack top. The source record includes
compiled dependencies, tool identities and effective build flags. Existing
two-worker recognition evidence remains the baseline; only the changed
adapter/arithmetic cases and the selected eight-worker recognition ran here.

The earlier blanket OS refusal has historical compile evidence in
`~/.cache/sunstoneos/evidence/resume-mimus-os-worker-guard-20261026/`.
That boundary amendment rejected eight workers with or without A76 admission
and accepted the two-worker OS adapter. Standalone native and QEMU adapters
compiled unchanged. The later OS extension above replaces that blanket refusal
with explicit supported-profile admission; the historical artifacts remain
preserved.
