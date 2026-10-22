# Design: placing mimus linear layers on the CPU or GPU in the OS image

Status: **implemented** and host-modeled on the OS image side; physical
behavior is pending until the board evidence at the end exists. The
regression bound for the linear stage is **not met** as first reviewed: the
time model gives 0.812 s on the A55 pair and 0.414 s on the A76 pair against
0.79 s and 0.39 s. "Budgets and measurement" restates the bound as 0.82 s and
0.42 s from the corrected derivation; the restated bound holds only once the
performance design panel accepts it.
[recognition-dispatch.md](../recognition-dispatch.md) describes what was
built and [the measurements](recognition-dispatch-measurements.md) compare it
with the budgets below. It follows [performance-architecture.md](../performance-architecture.md).

The integrated OS image ([os-image.md](../os-image.md)) runs mimus at EL0 on
the CPU. On the bench recording (8.87 s of audio, one Cortex-A55 at the
configured 494 MHz, NEON kernels) the linear layers take 14.894 s of 19.476 s
and perform 4.17 billion multiply-accumulates (MACs) ([mimus.md](../mimus.md)).
The GPU integer matrix product for mimus (`docs/designs/gpu-mimus-compute.md`
at commit f60d41c on branch `audit/gpu-mimus-compute`, called the kernel
design below) returns exact integer sums, and mimus's CPU code converts and
scales them, so output stays bit-identical to the host reference.

The kernel design owns the kernel, the hook into mimus, the submit message and
the worker's job checks, the output region and its cache maintenance,
cross-core ordering, reply semantics, and RESET and LOST handling inside the
hook. The hook keeps one job in flight: a call is one job, or consecutive jobs
when it has more rows than the tensor's job bound, and the next job is
submitted only after the read-out of the previous one. This design owns the placement table, the `gpu-linear` start entry, the
mapping check in `mimus_board_pins`, the build gate, and the INTID 95 table
entry. It also owns admission after the model hash, the recognizer's policy
after RESET or LOST, supervision of the recognizer's waits, and this
package's budgets.

## Data flow

The loader places the base and overlay models before the image starts, and
the recognizer hashes them once per boot (`apps/mimus-board/recognition.cc`).
The OS image has no model upload path. mimus's arena, which holds the codes,
is the first 12 MiB of the recognition arena at `0x46000000`. The GPU writes
into one 2 MiB output region at `0x46C00000`: a 64 KiB tag area, one word per
lane, followed by the sums. The 2 MiB above it, up to `0x47000000`, is not
leased to any owner. No weight, code or sum byte is copied.

**Subsampling.** The subsampling convolution is 1,584 → 176 over 4,440 rows.
The port produces its rows in blocks of 224 into one 709,632-byte code buffer,
which both the CPU kernel and the GPU job read. This replaces the current
160-row calls, so the port keeps one subsampling path. Each row is a 3 × 3
window over 176 channels, quantized with its own scale. A 224-row block is
the largest multiple of the kernel's 32-row tile within the 64 Mi MAC job cap
at `n_gpu = 176`, so on every admitted core set it is one job with no padded
row. The recording takes 20 calls and 20 jobs, the last of 184 rows padded to
192. It writes 7,032,960 codes, as the CPU path does today, and the GPU path
adds a clean of 219,780 lines. The GPU idles while the CPU produces the next
block.

## The placement table

A host tool in `apps/os/` reads the model files whose SHA-256 the build embeds
in `mimus_asset_sha256`. Its inputs are the admitted core set, the CPU clock,
the GPU clock and a MAC rate for each. Until the board measures the rates,
they are the kernel design's seeds: 0.28 G MAC/s per A55 at 494 MHz and
7.1 G MAC/s for the GPU at 400 MHz. The tool writes one row per tensor,
indexed by the tensor's ordinal, into a `const` table in the image:

- **n_gpu:** the channels the GPU computes, from the kernel design's time
  model evaluated for this tensor, including the recognizer core's read-out.
  Zero marks a tensor the kernel cannot run: an int4 rescale, or K and N
  outside the kernel's shape rules.
- **job_rows:** the GPU worker's own bound for the tensor, rounded down to a
  multiple of 32. The worker computes the bound at OPEN as
  `min(floor(2^26 / (K × n_gpu)), floor((2 MiB − 64 KiB) / (n_gpu × width)),
  32 × floor(512 / ceil(n_gpu / 16)))`: the 64 Mi MAC cap, the sum area, and
  the 512 workgroups of 32 rows by 16 channels whose tags fill the tag area.
  `width` is 4 bytes for K ≤ 512 and 8 otherwise. The table's value is
  therefore never above the worker's, and the boundaries between the jobs of
  one call fall on multiples of 32. If the bound is below 32, the tool sets
  `n_gpu` to zero as well, so no tensor emits a zero-row job.

The hook reads the table in place. An active row count, set to the table's
length at OPEN and to zero at close, holds the session state, so a call costs
two loads (the count and the row) and one compare (the ordinal with the
count).

The tool chooses `n_gpu` among N and N minus each multiple of 16, NEON's
channel width, by the shortest modeled call. The model follows the kernel
design's hook on two CPU timelines, the recognizer core and the second
inference worker, and one GPU timeline:

- the recognizer core cleans the job's codes and its 128-byte job words and
  submits the job;
- in a channel split, both cores compute half of the CPU channels each while
  the first job runs, and join;
- after each completion the recognizer core invalidates the job's tags and
  sums. It reads them out alone in a channel split; when the GPU has every
  channel, both cores read out half of the rows each.

The recognizer core is therefore one of the two CPU workers, and a channel
split moves read-out that two cores would share onto one. On the A55 pair
this makes every tensor GPU-only: the feed-forward first linear
(176 → 704) at 222 rows takes 6.25 ms with `n_gpu = 704` and 7.95 ms with
16 CPU channels, and the output head (176 → 1,025) 9.12 ms against 11.69 ms.
On the A76 pair the CPU channels are cheap enough to split every tensor,
and the model puts 62.9% of the MACs on the GPU. The emitted tables are:

| Layer, K → N | A55 pair: n_gpu, job_rows | A76 pair: n_gpu, job_rows |
| --- | --- | --- |
| Subsampling convolution, 1,584 → 176 | 176, 224 | 112, 352 |
| Subsampling output, 3,520 → 176 | 176, 96 | 112, 160 |
| Feed-forward first linear, 176 → 704 | 704, 352 | 432, 576 |
| Feed-forward second linear, 704 → 176 | 176, 512 | 112, 832 |
| Attention projections and pointwise convolution 2, 176 → 176 | 176, 1,472 | 112, 2,336 |
| Pointwise convolution 1, 176 → 352 | 352, 736 | 208, 1,248 |
| Output head, 176 → 1,025 | 1,025, 224 | 625, 384 |

Every bench call fits one job. A host case compares the emitted tables with
these values and checks every row's `job_rows` against the worker's
formula.

## Build gate, mapping check and start

`SUNSTONE_A733_OS_GPU_LINEAR` is off by default. When it is on,
`cmake/a733-os.cmake` stops with `FATAL_ERROR` unless the build has
`SUNSTONE_A733_OS_RESIDENT` and two build constants: one for the primary
agent's mapping of `0x46C00000`–`0x47000000` read-only at EL0
(`A733_GPU_OUTPUT_EL0_READ_ONLY`), and one for the kernel design's
clean-and-invalidate (`DC CIVAC`) completion on the output region
(`A733_GPU_OUTPUT_CLEAN_INVALIDATE`).

The completion change must land no later than the mapping. AArch64 stage-1 permissions cannot give EL1 write
access with EL0 read-only access, so the output region becomes read-only at
EL1 too. The architecture checks `DC IVAC` as a write, so an invalidate on a
read-only page faults at EL1; `DC CIVAC` needs read permission only. With no
CPU writer, the clean finds no dirty line. Without the read-only mapping, a
stray EL0 store into the region leaves a dirty line whose eviction can
overwrite GPU sums or tags after the completion invalidate.

The read-only span covers the unleased 2 MiB as well, so the 12 MiB arena
ends at a boundary where any CPU store faults. mimus's arena must end there
too: `apps/mimus/recorded.h` declares 16 MiB today, and the recognizer patch
asserts at compile time that `kMimusArenaBase` equals `0x46000000` and
`kMimusArenaBytes` is at most 12 MiB. The GPU build therefore stops until the
mimus port shrinks its arena, and a later arena change that overlaps the
output region cannot compile.

The image checks the mapping once, in the existing walk in
`mimus_board_pins`, which today requires 16 MiB writable from `0x46000000`.
With the mapping in the tree, it requires the first 12 MiB writable and the
last 4 MiB EL0-readable and not EL0-writable, and a mismatch fails as it does
today. The 4 MiB check uses a shared-helper rule under which each page's EL0 read
translation must succeed and its EL0 write translation must fault, because
negating a write rule would pass a range in which one page is read-only
and the rest are writable. The walk
then makes 5,120 translations at application start.

Under the option the start list gains `gpu-linear`, which runs before the
scheduler and creates the GPU worker. The recognizer sends OPEN only after
every model hash matches. OPEN leases the model regions, the 12 MiB arena
span and the output region, and cleans the model regions and the output
region once.

## Interrupts and cores

The interrupt table gains INTID 95 (SPI 63) at priority `0xd8`. The
recognition core pair is a build constant: cores 6 and 7 in the A76 admission
build, cores 2 and 3 otherwise. The recognizer, the GPU worker and INTID 95
use the first core, mimus's second inference worker the second; core 0 keeps
USB0, thermal and DMA. An interrupt on another core takes the status-4 stop,
so `os-image-link.py` checks that the INTID 95 entry's core equals the
constant in the ELF. Completion is interrupt-driven with no inter-processor
interrupt and no polling (kernel design, "Interrupts and polling"). Until
INTID 95 delivery is qualified, the worker's 4 ms timed wake applies.

## Session lifecycle and failures

| Event | Class (section 1.8) | Action | Retained |
| --- | --- | --- | --- |
| OPEN refused or fails | 1 | CPU for the boot | Nothing |
| Job refused before device access | 1 | The hook computes those rows on the CPU; the session stays open | Nothing |
| Job reports OK with a missing tag | 2 | The worker stops the GPU and acknowledges; the hook records `incomplete`, computes the rest of the call on the CPU, and every later call runs on the CPU. Without the acknowledgement within the stop bound, LOST applies | The output region, never read again in the boot |
| RESET or LOST | 3 | The hook calls `board_fail` before it computes any row on the CPU; the watchdog resets the board | The output region, model regions and 12 MiB arena span, which nothing reuses or exports until the watchdog reset or power removal |
| No reply | Unknown | The mimus service window expires: `board_fail`, watchdog reset | Everything |

RESET takes the LOST path because the models and arena are trustworthy after
a GPU fault only if the GPU MMU enforces read-only mappings, which the board
has not shown. After the watchdog reset the loader places the models again
and the recognizer hashes them before the next OPEN. The recognizer counts
placed calls and refusals in the dispatch record, records a RESET or LOST
before `board_fail`, and is the record's only writer.

**Supervision.** The recognizer's waits for OPEN, close and job replies are
blocking receives that record no service progress. The second worker reports
progress only while it computes CPU channels, which end at the hook's join. A
reply that never comes therefore expires the mimus service's 3 s window in
`apps/os/services.c`, and the watchdog resets the board within 4 s of the last
renewal. That window exists only in the resident lifetime, which is why the
build gate requires it; supervised first runs use the CPU path. A legitimate
wait is far shorter: the worker's own deadlines end every job in OK, RESET or
LOST, and OPEN's 20 MiB clean takes about 13 ms at 494 MHz. The recognizer
has no timer of its own.

## Budgets and measurement

The goal allows 0.4435 s from PCM to text for the bench recording. Attention
and the remaining stages took 4.582 s on one A55 at 494 MHz. Two A76 cores at
2,002 MHz, each doing twice an A55's work per clock and scaling perfectly,
would take 0.283 s. This design allows those stages 0.2935 s and sets the
linear stage's goal at **0.15 s**.

Baselines are the CPU-only image on the bench recording.
`checks/os-image-dispatch.c` (new) runs the dispatcher, the table and a
modeled worker over the bench call list and the longest accepted capture,
with the host GPU backend from the kernel design's `checks/gpu-mimus-layers`.
Per-job barriers, cache lines and overhead are budgeted in the kernel design.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Linear stage, regression bound | 14.894 s | At most 0.82 s on the A55 pair and 0.42 s on the A76 pair, GPU at 400 MHz, INTID 95 delivery; awaits the panel's acceptance | Time model asserted against this bound in `os-image-dispatch` |
| Linear stage, goal | 14.894 s | 0.15 s; no modeled combination of the levers below reaches it | Board item |
| Output | CPU path | Text and CRCs equal to the host reference, nominal and with each failure injected | `gpu-mimus-layers` with the dispatcher |
| Calls on the GPU; jobs | 0; 0 | 204; 204 on each pair | `os-image-dispatch` |
| Table rows above the worker's job bound | None | 0 | `os-image-dispatch` |
| Subsampling jobs; padded rows | None | 20; 8 | `os-image-dispatch` |
| Placement work per call | None | 2 loads, 1 compare | `os-image-dispatch` |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | Copy counter in `gpu-mimus-layers` |
| Subsampling codes written; lines cleaned | 7,032,960; 0 | 7,032,960; 219,780 (8.9 ms at 494 MHz) | Producer and `aarch64_device_cache` counters |
| Recognizer wake-ups; IPIs; tick waits per job | None | 1; 0; 0 with INTID 95 qualified, otherwise up to one 4 ms wait (up to 0.82 s per recording) | Queue and notification counters |
| Arena peak | 1,761,840 bytes | At most 2,784,048 bytes at any utterance length | Host arena high-water mark |
| GPU idle per subsampling gap, including the block's clean | None | At most 1 ms on the A55 pair (clean 0.45 ms); at most 0.25 ms on the A76 pair (clean 0.11 ms) | `os-image-dispatch`, producer under Unicorn |

The arena peak adds the subsampling buffer and at most 312,576 bytes of codes
moved from a ring into the arena (222 rows × 1,408 bytes). The clean figures
assume 20 cycles per 64-byte line over the block's 11,088 lines.

**Time estimate.** `apps/os/placement.py` computes every figure below with
the seeds of "The placement table" and the GPU at 400 MHz
(`A733_GPU_INITIAL_HZ`), its only verified clock. On the A55 pair the linear
stage takes 0.812 s:

- GPU compute for 204 jobs, including firmware overhead, 0.579 s;
- read-out of 12,923,134 sums at 12 instructions each, shared by both cores,
  0.157 s;
- 596,989 cleaned lines and 988,060 invalidated lines, tags included, at
  20 cycles each, 0.064 s;
- submit, wake and hand-off, 60 µs per job, 0.012 s.

Nothing overlaps the GPU job except the CPU channels of a split call, so the
read-out and cache maintenance are exposed in full. The regression bound of
0.82 s and 0.42 s is this result rounded up to 10 ms. It replaces 0.79 s and
0.39 s, which assumed that a call split into two jobs whose read-out
overlapped and that the recognizer core read out without computing CPU
channels. With 32-row boundaries, a 222-row call splits into 96 and 126 rows,
and the recognizer core is one of the two CPU workers; the kernel design also
keeps one job in flight. No placement recovers the earlier bound: under the
two-job model, the best split points gave 0.813 s and 0.407 s.

Until INTID 95 is qualified, the timed wake can add up to 204 × 4 ms. The
other stages alone take 2.29 s on the A55 pair, so the A55 configuration
cannot meet the goal. The A76 pair at 2,002 MHz takes 0.414 s, with 0.366 s
of GPU compute for 62.9% of the MACs. The same model on the A76 pair gives
these figures for the remaining levers:

| Lever | Owner | Linear stage |
| --- | --- | --- |
| GPU at 600 or 800 MHz | GPU power and thermal owner | 0.332 s or 0.288 s |
| NEON int32-lane accumulation at 2×, 3× or 4× the CPU rate | mimus port | 0.302 s, 0.241 s or 0.195 s |
| Read-out of the GPU share on both cores in channel splits | Kernel design | 0.404 s |
| 800 MHz and 3× | Two owners | 0.197 s |
| 800 MHz and 4× | Two owners | 0.169 s |
| 800 MHz, 4× and read-out on both cores | All three | 0.163 s |

No row meets 0.15 s. At 800 MHz and 4× the GPU computes for 0.128 s of the
0.169 s, so the call is GPU-bound; the CPU channels run in parallel with the
job, and cache maintenance, read-out and per-job overhead take the remaining
0.041 s. The kernel design leaves out more jobs in flight, which would hide
part of that remainder, until the board measures job overhead. No GPU operating point above 400 MHz is recorded, and the CPU
factors are unmeasured.

Host cases in `checks/os-image-dispatch.c`:

- no OPEN before the hash;
- refusal, and RESET and LOST each ending in `board_fail` with no CPU row
  computed after the failed job;
- a worker that never answers OPEN, close or a job, which expires the window;
- two consecutive jobs near the worker's deadline that end OK;
- a job that reports OK with a missing tag, which moves the rest of the call
  and every later call to the CPU without ending the boot;
- no zero-row job, a bound below 32 rows giving `n_gpu = 0`, and the
  longest capture with three or more jobs per call;
- the emitted table for each core set and pair of clocks, every row's
  `job_rows` at most the worker's formula, and the modeled linear stage
  within the regression bound;
- the recognizer patch's arena assertion, which accepts a 12 MiB arena at
  `0x46000000` and refuses a 16 MiB arena or another base;
- configuration with the option on and the resident lifetime or either build
  constant missing, which fails.

## Left out

- **NPU placement.** The pinned NPU compiler refuses the INT32 output that
  exact sums need, and its input quantization uses one scale where mimus uses
  one per row ([npu-mimus-linear.md](../npu-mimus-linear.md)). It returns when
  exact sums build with the pinned compiler.
- **Recovery from RESET or LOST without a reboot.** It would keep GPU rows
  after RESET, or close the session, clean and invalidate the model regions
  and arena span, re-hash, reload the engine cold and recompute on the CPU.
  It returns when the board shows that a GPU write through a read-only
  mapping faults and leaves memory unchanged.
- **Read-out of the GPU share on both cores in channel splits.** The kernel
  design leaves it out. This model gives a saving of 10 ms on the A76 pair at
  2,002 MHz, where the kernel design's model gives 1.3 ms; the board's
  read-out rate settles which one holds.
- **A minimum row count per tensor.** Every placed tensor has K × N of at
  least 30,976, so a 32,768-MAC break-even needs at most two rows, and the
  smallest bench call has 30. A board-measured rate that makes it bind brings
  it back as a table column.
- **Gathering subsampling windows from unexpanded codes.** Each window has its
  own quantization scale, so windows cannot share codes without changing the
  output.
- **Overlapping subsampling production with GPU work.** It needs a second code
  buffer and an asynchronous hook, and returns if the board shows the 19 gaps
  above 20 ms in total.
- **One job for the query, key and value projections**, until board-measured
  job overhead shows that the engine change pays.
- **Thermal load shedding.** The image ignores THROTTLE; CRITICAL ends the
  session.

## Changes outside this package

- **Kernel design:**
  - this design's supervision in place of its recognizer timeout of
    `T_answer + 10 ms`;
  - LOST ending in `board_fail` and the watchdog reset, as RESET does, in
    place of a parked recognizer that keeps reporting;
  - a failure callback that the hook calls on RESET or LOST before it
    computes any row on the CPU, and `os_dispatch_incomplete` after the stop
    acknowledgement for a missing tag;
  - clean-and-invalidate on the output region, with the build constant
    `A733_GPU_OUTPUT_CLEAN_INVALIDATE`;
  - the tag area cleared at OPEN without a CPU store, because the region is
    read-only at EL1 too; a GPU fill job at OPEN is one way;
  - subsampling calls of 224 rows (last 184) in place of 240, which pads
    each block to 256 rows;
  - as a lever, each producer core cleans the code lines it wrote to the
    point of coherency and issues a `DSB` before the join's release, which
    halves the serial clean at submit.

  Its model-upload class does not arise in the OS image.
- **mimus port:** under the option, the recognition arena of 12 MiB:
  `kMimusArenaBytes` equal to `OS_DISPATCH_CODE_BYTES`, which the recognizer
  patch asserts; the tensor ordinal in `TensorView`; the 224-row subsampling
  producer; the second worker's affinity; attention and the remaining stages
  within 0.2935 s. It also needs NEON accumulation in int32 lanes, flushed to
  int64 every 512 products; the table then takes the measured CPU rate.
- **GPU power and thermal owner:** an operating point above 400 MHz, with the
  modeled times above.
- **GPU runtime:** the worker on the recognition core.
- **Memory architecture:** the 12 MiB span and the output region as fixed
  spans lent to the GPU, and `0x46E00000`–`0x47000000` unleased; the leases
  in the capacity replay.
- **Primary agent:** A76 admission for the image; `0x46C00000`–`0x47000000`
  mapped read-only at EL0 as two 2 MiB blocks, with the build constant
  `A733_GPU_OUTPUT_EL0_READ_ONLY`; an
  EL0 form of `aarch64_device_translate` (`AT S1E0R`, `AT S1E0W`); removal of
  the leading `DSB SY` in `aarch64_device_cache` in
  `arch/aarch64/device_memory.c`.
- **Shared helpers:** an `a733_mapping_check.h` rule that requires an EL0 read
  translation to succeed and an EL0 write translation to fault on every page,
  with a host case in which one writable page among 1,024 fails.
- **Bluetooth voice recognition package:** it also edits `apps/os/os.h` and
  `docs/os-image.md`; this package only adds declarations and sections there.

## Board evidence that would confirm the design

- The GPU build recognizes the bench recording with text and CRCs equal to the
  host, and the dispatch record shows every placed call on the GPU.
- An EL0 store into `0x46C00000`–`0x47000000` faults, and the check in
  `mimus_board_pins` passes.
- INTID 95 arrives on the recognition core for every job.
- An injected hang ends in RESET, `board_fail` and a watchdog reset; after it,
  and after an injected LOST, the loader's model hashes match. A one-hour
  resident run never stops watchdog renewal.
- CPU and GPU rates per shape at each admitted clock replace the table's
  seeds.
