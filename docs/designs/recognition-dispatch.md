# Design: placing mimus linear layers on the CPU or GPU in the OS image

Status: **host-tested**. The placement table, dispatcher and supervision are implemented in the mimus checkout (`sunstoneos/apps/os/placement.py`, `dispatch.c`, `dispatch.h`, `recognition-dispatch.patch`) on the OS image's generic GPU queue, and pass `checks/os-image-dispatch.py` and the time model. Physical behavior is **open** until the board items below run. The linear-stage regression bound of 0.82 s on the A55 pair and 0.42 s on the A76 pair holds in the model (0.812 s and 0.414 s) but awaits the performance design panel's acceptance. The linear-stage goal of 0.15 s is not reached by any modeled lever.

[recognition-dispatch.md](../recognition-dispatch.md) describes what was built, and [gpu-recognition.md](gpu-recognition.md) describes the GPU runtime that serves the placed calls. The design follows [performance-architecture.md](../performance-architecture.md).

## Purpose and ownership

The integrated OS image ([os-image.md](../os-image.md)) runs mimus at EL0 on the CPU. On the bench recording (8.87 s of audio, one Cortex-A55 at 494 MHz, NEON kernels) the linear layers take 14.894 s of 19.476 s and perform 4.17 billion multiply-accumulates (MACs) ([mimus.md](../mimus.md)). The GPU integer matrix product for mimus ([gpu-mimus-compute.md](gpu-mimus-compute.md)) returns exact integer sums, and mimus's CPU code converts and scales them, so output stays bit-identical to the host reference.

The kernel design owns the kernel, the hook into mimus, the submit message and the worker's job checks, the output region and its cache maintenance, cross-core ordering, reply semantics, and RESET and LOST handling inside the hook. The hook keeps one job in flight: a call is one job, or consecutive jobs when it has more rows than the tensor's job bound, and the next job is submitted only after the read-out of the previous one. This design owns the placement table, the `gpu-linear` start entry, the mapping check in `mimus_board_pins`, the build gate and the INTID 95 table entry. It also owns admission after the model hash, the recognizer's policy after RESET or LOST, supervision of the recognizer's waits and the budgets.

## Data flow

The loader places the base and overlay models before the image starts, and the recognizer hashes them once per boot (`apps/mimus-board/recognition.cc`). The OS image has no model upload path. mimus's arena, which holds the codes, is the first 12 MiB of the recognition arena at `0x46000000`. The GPU writes into one 2 MiB output region at `0x46C00000`: a 64 KiB tag area with one word per lane, followed by the sums. The 2 MiB above it, up to `0x47000000`, is not leased to any owner. No weight, code or sum byte is copied.

**Subsampling.** The subsampling convolution is 1,584 → 176 over 4,440 rows. The mimus port produces its rows in blocks of 224 into one 709,632-byte code buffer, which both the CPU kernel and the GPU job read. Each row is a 3 × 3 window over 176 channels, quantized with its own scale. A 224-row block is the largest multiple of the kernel's 32-row tile within the 64 Mi MAC job cap at `n_gpu = 176`, so on every admitted core set it is one job with no padded row. The recording takes 20 calls and 20 jobs, the last block of 184 rows padded to 192. It writes 7,032,960 codes, and the GPU path adds a clean of 219,780 lines (8.9 ms at 494 MHz). The GPU idles while the CPU produces the next block.

## The placement table

`apps/os/placement.py` reads the model files whose SHA-256 the build embeds in `mimus_asset_sha256`. Its inputs are the admitted core set, the CPU clock, the GPU clock and a MAC rate for each. The rates are the kernel design's seeds (0.28 G MAC/s per A55 at 494 MHz and 7.1 G MAC/s for the GPU at 400 MHz) until a board measures them. It writes one row per tensor, indexed by the tensor's ordinal, into a `const` table:

- **n_gpu:** the channels the GPU computes, from the kernel design's time model for this tensor, including the recognizer core's read-out. Zero marks a tensor the kernel cannot run: an int4 rescale, or K and N outside the kernel's shape rules.
- **job_rows:** the GPU worker's own bound for the tensor, rounded down to a multiple of 32. The worker computes the bound at OPEN as `min(floor(2^26 / (K × n_gpu)), floor((2 MiB − 64 KiB) / (n_gpu × width)), 32 × floor(512 / ceil(n_gpu / 16)))`: the 64 Mi MAC cap, the sum area, and the 512 workgroups of 32 rows by 16 channels whose tags fill the tag area. `width` is 4 bytes for K ≤ 512 and 8 otherwise. The table's value is never above the worker's, and job boundaries fall on multiples of 32. If the bound is below 32, the tool sets `n_gpu` to zero, so no tensor emits a zero-row job.

The hook reads the table in place. An active row count, set to the table's length at OPEN and to zero at close, holds the session state, so a call costs two loads (the count and the row) and one compare (the ordinal with the count).

The tool chooses `n_gpu` among N and N minus each multiple of 16, NEON's channel width, by the shortest modeled call. The model has two CPU timelines (the recognizer core and the second inference worker) and one GPU timeline:

- the recognizer core cleans the job's codes and 128-byte job words and submits;
- in a channel split both cores compute half of the CPU channels each while the first job runs, and join;
- after each completion the recognizer core invalidates the job's tags and sums, then reads them out alone in a channel split, or with both cores (half of the rows each) when the GPU has every channel.

A channel split therefore moves read-out that two cores would share onto one. On the A55 pair every tensor is GPU-only (for the feed-forward first linear, 176 → 704 at 222 rows: 6.25 ms with `n_gpu = 704` against 7.95 ms with 16 CPU channels). On the A76 pair the CPU channels are cheap enough to split every tensor, and the model puts 62.9% of the MACs on the GPU. The emitted tables are:

| Layer, K → N | A55 pair: n_gpu, job_rows | A76 pair: n_gpu, job_rows |
| --- | --- | --- |
| Subsampling convolution, 1,584 → 176 | 176, 224 | 112, 352 |
| Subsampling output, 3,520 → 176 | 176, 96 | 112, 160 |
| Feed-forward first linear, 176 → 704 | 704, 352 | 432, 576 |
| Feed-forward second linear, 704 → 176 | 176, 512 | 112, 832 |
| Attention projections and pointwise convolution 2, 176 → 176 | 176, 1,472 | 112, 2,336 |
| Pointwise convolution 1, 176 → 352 | 352, 736 | 208, 1,248 |
| Output head, 176 → 1,025 | 1,025, 224 | 625, 384 |

Every bench call fits one job. A host case compares the emitted tables with these values and checks every row's `job_rows` against the worker's formula. (The regenerated tables with the measured GPU program rates are in [gpu-recognition.md](gpu-recognition.md).)

## Build gate, mapping check and start

`SUNSTONE_A733_OS_GPU_LINEAR` is off by default. When it is on, `cmake/a733-os.cmake` stops with `FATAL_ERROR` unless the build has `SUNSTONE_A733_OS_RESIDENT` and sets the mapping of `0x46C00000`–`0x47000000` read-only at EL0 (`A733_GPU_OUTPUT_EL0_READ_ONLY`). AArch64 stage-1 permissions cannot give EL1 write access with EL0 read-only access, so the output region is read-only at EL1 too. The architecture checks `DC IVAC` as a write, so an invalidate on a read-only page faults at EL1, while `DC CIVAC` needs read permission only. The completion therefore cleans and invalidates the output region, and with no CPU writer the clean finds no dirty line. Without the read-only mapping, a stray EL0 store into the region leaves a dirty line whose eviction can overwrite GPU sums or tags after the completion invalidate.

The read-only span covers the unleased 2 MiB as well, so the 12 MiB arena ends at a boundary where any CPU store faults. mimus's arena ends there too: the recognizer patch asserts at compile time that `kMimusArenaBase` equals `0x46000000` and `kMimusArenaBytes` is at most 12 MiB, so a later arena change that overlaps the output region cannot compile.

The image checks the mapping once, in the walk in `mimus_board_pins`. It requires the first 12 MiB writable and the last 4 MiB EL0-readable and not EL0-writable, and a mismatch fails the application start. The 4 MiB check uses a shared-helper rule under which each page's EL0 read translation must succeed and its EL0 write translation must fault, because negating a write rule would pass a range in which one page is read-only and the rest are writable. With the option the walk makes 5,120 translations at application start in the per-page form ([gpu-recognition.md](gpu-recognition.md) reduces it to 18 with block stride).

Under the option the start list gains `gpu-linear`, which runs before the scheduler and creates the GPU worker. The recognizer sends OPEN only after every model hash matches, and OPEN leases the model regions, the 12 MiB arena span and the output region.

## Interrupts and cores

The interrupt table has INTID 95 (SPI 63) at priority `0xd8`. The recognition core pair is a build constant: cores 6 and 7 in the A76 admission build, cores 2 and 3 otherwise. The recognizer, the GPU worker and INTID 95 use the first core and mimus's second inference worker the second; core 0 keeps USB0, thermal and DMA. An interrupt on another core takes the status-4 stop, so `os-image-link.py` checks that the INTID 95 entry's core equals the constant in the ELF. Completion is interrupt-driven with no inter-processor interrupt and no polling. Until INTID 95 delivery is qualified, the worker's 4 ms timed wake applies.

## Session lifecycle and failures

| Event | Class (section 1.8) | Action | Retained |
| --- | --- | --- | --- |
| OPEN refused or fails | 1 | CPU for the boot | Nothing |
| Job refused before device access | 1 | The hook computes those rows on the CPU; the session stays open | Nothing |
| Job reports OK with a missing tag | 2 | The worker stops the GPU and acknowledges; the hook records `incomplete`, computes the rest of the call on the CPU, and every later call runs on the CPU. Without the acknowledgement within the stop bound, LOST applies | The output region, never read again in the boot |
| RESET or LOST | 3 | The hook calls `board_fail` before it computes any row on the CPU; the watchdog resets the board | The output region, model regions and 12 MiB arena span, which nothing reuses or exports until the watchdog reset or power removal |
| No reply | Unknown | The mimus service window expires: `board_fail`, watchdog reset | Everything |

RESET takes the LOST path because the models and arena are trustworthy after a GPU fault only if the GPU MMU enforces read-only mappings, which the board has not shown. After the watchdog reset the loader places the models again and the recognizer hashes them before the next OPEN. The recognizer counts placed calls and refusals in the dispatch record, records a RESET or LOST before `board_fail`, and is the record's only writer.

**Supervision.** The recognizer's waits for OPEN, close and job replies are blocking receives that record no service progress. The second worker reports progress only while it computes CPU channels, which end at the hook's join. A reply that never comes therefore expires the mimus service's 3 s window in `apps/os/services.c`, and the watchdog resets the board within 4 s of the last renewal. That window exists only in the resident lifetime, which is why the build gate requires it; supervised runs bound the waits as [gpu-recognition-wait.md](gpu-recognition-wait.md) describes. A legitimate wait is far shorter: the worker's own deadlines end every job in OK, RESET or LOST, and OPEN's 20 MiB clean takes about 13 ms at 494 MHz. The recognizer has no timer of its own.

## Budgets

The goal allows 0.4435 s from PCM to text for the bench recording. Attention and the remaining stages took 4.582 s on one A55 at 494 MHz, and two A76 cores at 2,002 MHz, each doing twice an A55's work per clock and scaling perfectly, would take 0.283 s. The design allows those stages 0.2935 s and sets the linear stage's goal at **0.15 s**. The target set, with its baselines and the measuring tool of each, is in the record `recognition-dispatch-host-budgets`; the modeled results are:

| Figure | A55 pair | A76 pair |
| --- | --- | --- |
| Linear stage, time model (regression bound) | 0.812 s (0.82 s) | 0.414 s (0.42 s) |
| Share of MACs on the GPU | every placed tensor GPU-only | 62.9% |
| Calls and jobs on the GPU | 204 and 204 | 204 and 204 |
| Output mismatches against the CPU-only exact sums | 0 of 12,923,134 | 0 of 12,923,134 |
| Subsampling jobs; padded rows | 20; 8 | 20; 8 |

The regression bound is the modeled result rounded up to 10 ms. The A55 breakdown is GPU compute 0.579 s, read-out of 12,923,134 sums at 12 instructions each shared by both cores 0.157 s, cache maintenance (596,989 cleaned and 988,060 invalidated lines at 20 cycles each) 0.064 s, and submit, wake and hand-off at 60 µs per job 0.012 s. Nothing overlaps a GPU job except the CPU channels of a split call, and one job stays in flight, so no placement recovers the earlier bound of 0.79 s and 0.39 s that assumed overlapped two-job calls. The A76 linear stage is dominated by 0.366 s of GPU compute. The records are in [recognition-dispatch.jsonl](../evidence/recognition-dispatch.jsonl).

## Limitations

- No lever combination reaches the 0.15 s linear goal. The best modeled value is 0.163 s on the A76 pair (GPU at 800 MHz, NEON int32-lane accumulation at four times the CPU rate and read-out on both cores); the table is in `recognition-dispatch-time-model`. No GPU operating point above 400 MHz was recorded then, and the CPU factors are unmeasured. The A55 configuration cannot meet the goal because its other stages alone take 2.29 s.
- Until INTID 95 delivery is qualified, the timed wake can add up to 204 × 4 ms = 0.82 s per recording.
- The pinned NPU compiler refuses the INT32 output that exact sums need, and its input quantization uses one scale where mimus uses one per row ([npu-mimus-linear.md](../npu-mimus-linear.md)), so linear layers cannot be placed on the NPU.
- There is no recovery from RESET or LOST without a reboot, since GPU rows would need the board to show that a GPU write through a read-only mapping faults and leaves memory unchanged.
- Every placed tensor has K × N of at least 30,976, so a 32,768-MAC break-even needs at most two rows and a per-tensor minimum row count is not needed. Each subsampling window has its own quantization scale, so windows cannot share codes. THROTTLE is ignored by this placement and CRITICAL ends the session. Other left-out options (overlapping subsampling with GPU work, one job for the query, key and value projections) are in `recognition-dispatch-left-out`.

## Open work

1. Recognize the bench recording on a board with the GPU build, with text and CRCs equal to the host and the dispatch record showing every placed call on the GPU.
2. Show that an EL0 store into `0x46C00000`–`0x47000000` faults and that the check in `mimus_board_pins` passes, and that INTID 95 arrives on the recognition core for every job.
3. Inject a hang (RESET, `board_fail` and a watchdog reset) and a LOST, check the loader's model hashes after each, and run one hour resident without a stop in watchdog renewal.
4. Replace the table's seeds with the measured CPU and GPU rates per shape at each admitted clock.
5. Obtain the performance design panel's acceptance of the restated regression bound.
