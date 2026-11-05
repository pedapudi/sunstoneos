# Eight CPU workers for independent recognition rows

Status: **host-tested**. The eight-worker engine and FreeRTOS adapter are implemented in the mimus checkout (`sunstoneos/apps/mimus/parallel_runtime.cc` and `parallel_runtime.h`) and pass the software checks below; native eight-worker execution is **open** (it has not run on hardware). The two-worker configuration is the default. The design changes neither the recognition model nor its weights, quantization, accumulation order or formatting.

The engine's worker limit `kMaxWorkers` and its `ForRowRanges` helper default to two ranges, stateful row producers allocate two private contexts, and the packed matrix path repairs one shared cache-line boundary after joining both workers. The eight-worker build extends these rather than only raising the limit, so that scratch ownership holds.

## Independent row stages use up to eight workers

The eight-worker build extends the row-range interface. The splitter chooses at most eight nonempty contiguous ranges of at least eight rows, and every boundary aligns every declared output to a 64-byte cache line. A range processes each output in its original arithmetic order. Two-worker builds keep the two-worker splitter.

- Aligned packed AArch64 matrix products use the extended splitter. Each callback uses a stack-local accumulator of at most 256 floats, and the residual scratch per callback stays at most 1 KiB.
- The unaligned matrix path keeps two ranges and its single-row boundary repair. Frontend FFT and log-mel processing keep two private frame scratch sets, and produced quantization keeps two contexts with an explicit two-range limit, with scratch arrays and producer rings at their two-context bounds.
- The engine's asynchronous interface is available in two-worker mode. Eight-worker mode declines asynchronous work, and the engine completes that item locally before it distributes later matrix products.

## Each worker owns one bounded job until acknowledgement

The adapter uses seven static worker slots. Each has a 64 KiB stack, private queue storage and a completion queue, and slot boundaries occupy separate 64-byte cache lines. The recognition task submits at most one job per slot, computes its own range and waits for every matching completion sequence. Callback contexts, input buffers and output buffers stay owned until every submitted callback completes. Calls cannot nest, only the bound recognition task submits work, and there is no job allocation or work-stealing scheduler. A range operation uses at most seven submissions and seven acknowledgements.

Each worker keeps the supervised checkpoints around its callback. A fault, stale acknowledgement or session revocation cannot manufacture a join or release a borrowed context, and the terminal path retains unfinished work. Native progress is reported only after the whole range operation joins.

In the admitted native configuration worker 0 is the recognition task on Cortex-A76 core 6, worker 1 uses A76 core 7, and workers 2 through 7 use A55 cores 0 through 5. Emulator indices map directly to cores 0 through 7. Ranges are divided by item count with no heterogeneous speed ratio. Inference workers run at priority 3 in the OS profile described in [mimus-os-eight-workers.md](mimus-os-eight-workers.md), where the A55 workers run at priority 2 below the OS services.

## Build selection

`-DSUNSTONE_MIMUS_WORKERS=8` selects eight workers in the SMP engine and its linked callers, and omitting it selects two. The worker count is a build-time choice, `mimus_parallel_init(owner, worker)` is unchanged, and all linked engine and adapter translation units must agree on the capacity. These definitions reach the integrated OS through its board composition. Native eight-worker builds run on the A76 cluster (the default for every OS image), with the owner on core 6 and the first worker on core 7. No OS thermal or session format changes belong to this implementation, and the build selection qualifies no physical session and changes no clock, rail or failure-recovery rule. The supported OS composition is in [mimus-os-eight-workers.md](mimus-os-eight-workers.md).

## Memory and synchronization bounds

Six added stacks reserve 384 KiB and the additional queue and task state stays below 16 KiB. QEMU reserves one 32 KiB exception stack per selected core, 192 KiB more in eight-core builds. Producer arena reservations do not grow, and the 16 MiB inference arena and the model input addresses stay fixed. Linked worker stacks occupy 458,752 bytes in eight-worker images against 65,536 bytes in the two-worker fixture, and queue and task slots occupy 6,720 and 960 bytes.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Matrix fixture, shapes with plain and residual output | 16 shapes; 141 worker jobs (two workers, 20 asynchronous) and 598 (eight workers) | `mimus-eight-workers-software` |
| Eight-core QEMU recognition against the host reference | features, log probabilities, all 222 frames, words, punctuation, capitalization and text match; 5,067 jobs; core mask `ff` | `mimus-eight-workers-software` |
| Worker stacks and slots, eight workers | 458,752 B and 6,720 B | `mimus-eight-workers-software` |

Records: [mimus-eight-workers.jsonl](../evidence/mimus-eight-workers.jsonl). Row-split fixtures pass AddressSanitizer and UndefinedBehaviorSanitizer with fatal errors, and the FreeRTOS adapter fixture checks complete joins before stack-context reuse, short and uneven item counts, private outputs and higher-priority service progress on core 0. Emulator timing is not a physical performance result.

## Open work

1. Measure native eight-worker recognition speed, service latency and recovery on a board.
2. Decide an unequal range split from measured A55 and A76 speeds.
