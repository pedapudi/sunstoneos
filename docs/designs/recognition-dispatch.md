# Design: placing mimus linear layers on the CPU or GPU in the OS image

Status: **design**; no code exists. Each item ends at implemented and
host-modeled, with physical behavior pending until the board evidence at the
end exists. It follows [performance-architecture.md](../performance-architecture.md).

The integrated OS image ([os-image.md](../os-image.md)) runs mimus at EL0 on
the CPU. On the bench recording (8.87 s of audio, one Cortex-A55 at the
configured 494 MHz, NEON kernels) the linear layers take 14.894 s of 19.476 s
and perform 4.17 billion multiply-accumulates (MACs) ([mimus.md](../mimus.md)).
The GPU integer matrix product for mimus (`docs/designs/gpu-mimus-compute.md`
at commit 05e3c7c on branch `audit/gpu-mimus-compute`, called the kernel
design below) returns exact integer sums, and mimus's CPU code converts and
scales them, so output stays bit-identical to the host reference.

The kernel design owns the kernel, the hook into mimus, the submit message and
the worker's job checks, the two output slots and their cache maintenance,
cross-core ordering, reply semantics, and RESET and LOST handling inside the
hook. This design owns the placement table, the `gpu-linear` start entry with
its mapping check, and the INTID 95 table entry. It also owns admission after
the model hash, the recognizer's policy after RESET or LOST, supervision of
the recognizer's waits, and this package's budgets.

## Data flow

The loader places the base and overlay models before the image starts, and
the recognizer hashes them once per boot (`apps/mimus-board/recognition.cc`).
The OS image has no model upload path. Codes stay in the first 12 MiB of the
recognition arena at `0x46000000`, which the recognizer passes to mimus. Sums
land in two 2 MiB slots at `0x46C00000` and `0x46E00000`. No weight, code or
sum byte is copied.

**Subsampling.** The subsampling convolution is 1,584 → 176 over 4,440 rows.
The port produces its rows in blocks of 240 into one 760,320-byte code buffer,
which both the CPU kernel and the GPU job read. This replaces the current
160-row calls, so the port keeps one subsampling path. Each row is a 3 × 3
window over 176 channels, quantized with its own scale. The recording writes
7,032,960 codes, as the CPU path does today, and the GPU path adds a clean of
219,780 lines. Each block is one call of two jobs; the GPU idles while the
CPU produces the next block.

## The placement table

A host tool in `apps/os/` reads the model files whose SHA-256 the build embeds
in `mimus_asset_sha256`. Its inputs are the admitted core set, the CPU clock,
the GPU clock and a MAC rate for each. Until the board measures the rates,
they are the kernel design's seeds: 0.28 G MAC/s per A55 at 494 MHz and
7.1 G MAC/s for the GPU at 400 MHz. The tool writes one row per tensor,
indexed by the tensor's ordinal:

- **n_gpu:** the channels the GPU computes, from the kernel design's time
  model evaluated for this tensor, including the recognizer core's read-out.
  Zero marks a tensor the kernel cannot run: an int4 rescale, or K and N
  outside the kernel's shape rules.
- **job_rows:** `min(floor(2^26 / (K × n_gpu)), floor(2 MiB / (n_gpu × width)))`
  rounded down to a multiple of 32, which keeps a job within the kernel
  design's 64 Mi MAC cap and one slot; zero when `n_gpu` is zero.

Per call, the hook loads the active-table pointer, which is null while the
session is closed, compares the ordinal with the row count and loads the row.

For the A55 pair at 494 MHz, the model gives `n_gpu = N − 16` for the
feed-forward first linear (176 → 704) and the output head (176 → 1,025),
whose 16 CPU channels fit inside the first job while both workers would
otherwise wait; this saves about 3 ms per recording. Every other tensor gets
`n_gpu = N`. With N = 176, 16 channels are 9.1% of the work, above the 7.3%
that balances 0.56 G MAC/s against 7.1 G MAC/s; for pointwise convolution 1
(176 → 352), 16 CPU channels take 1.1 ms against a 0.97 ms first job. For the
A76 pair at 2,002 MHz the model puts 61% of the MACs on the GPU. A host case
compares the emitted table with these results.

## Start, admission and the EL0 mapping

Under `SUNSTONE_A733_OS_GPU_LINEAR`, off by default, the start list gains
`gpu-linear`. It runs before the scheduler, creates the GPU worker, and checks
that each of the 1,024 pages of `0x46C00000`–`0x47000000` passes an EL0 read
translation and faults an EL0 write translation. The check calls
`a733_mapping_check_pages` with an EL0 form of `aarch64_device_translate`
(`AT S1E0R`, `AT S1E0W`) kept in `apps/os/`. If it fails, the worker refuses
every OPEN and recognition stays on the CPU for the boot. The GPU path is
therefore inert until the primary agent maps that range read-only at EL0.
Without that mapping, a stray EL0 store into a slot leaves a dirty line whose
eviction can overwrite GPU sums after the completion invalidate. When the
mapping lands, `application.patch` checks 12 MiB writable and 4 MiB readable
in place of 16 MiB writable.

AArch64 stage-1 permissions cannot give EL1 write access with EL0 read-only
access, so the slots become read-only at EL1 too. The architecture checks
`DC IVAC` as a write, so the completion maintenance on the slots uses
clean-and-invalidate (`DC CIVAC`), which needs read permission only. With no
CPU writer, the clean finds no dirty line.

The recognizer sends OPEN only after every model hash matches. OPEN leases the
regions, cleans the model regions and slots once, and copies the table.

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
| Mapping check fails; OPEN refused or fails; GPU off after its fourth reset | 1 | CPU for the boot | Nothing |
| Job refused before device access | 1 | The hook computes those rows on the CPU; the session stays open | Nothing |
| RESET | 2 | The hook finishes the call. The recognizer closes the session, discards the generation's result, re-hashes the models and recomputes the held input on the CPU with a cold engine load. The next generation reopens the session | Nothing |
| LOST | 3 | The recognizer's existing failure path: `board_fail`, then a watchdog reset in the resident lifetime | Slots stay device-owned |
| No reply | Unknown | The mimus service window expires: `board_fail`, watchdog reset | Everything |

RESET discards the generation because the models and arena are trustworthy
after a GPU fault only if the GPU MMU enforces read-only mappings, which the
board has not shown. The recognizer counts placed calls, refusals, RESET and
LOST in the dispatch record and is its only writer.

**Supervision.** The recognizer's waits for OPEN, close and job replies are
blocking receives that record no service progress. The second worker reports
progress only while it computes CPU channels, which end at the hook's join. A
reply that never comes therefore expires the mimus service's 3 s window in
`apps/os/services.c`, and the watchdog resets the board within 4 s of the last
renewal. A legitimate wait is far shorter: the worker's own deadlines end
every job in OK, RESET or LOST, and OPEN's 20 MiB clean takes about 13 ms at
494 MHz. The recognizer has no timer of its own.

## Budgets and measurement

The goal allows 0.4435 s from PCM to text for the bench recording. Attention
and the remaining stages took 4.582 s on one A55 at 494 MHz. Two A76 cores at
2,002 MHz, each doing twice an A55's work per clock and scaling perfectly,
would take 0.283 s. This design allows those stages 0.2935 s and sets the
linear stage's share at **0.15 s**.

Baselines are the CPU-only image on the bench recording.
`checks/os-image-dispatch.c` (new) runs the dispatcher, the table and a
modeled worker over the bench call list and the longest accepted capture,
with the host GPU backend from the kernel design's `checks/gpu-mimus-layers`.
Per-job barriers, cache lines and overhead are budgeted in the kernel design.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Linear stage | 14.894 s | 0.15 s; modeled 0.79 s on the A55 pair, 0.39 s on the A76 pair | Time model in `os-image-dispatch`; board item |
| Output | CPU path | Text and CRCs equal to the host reference, nominal and with each failure injected | `gpu-mimus-layers` with the dispatcher |
| Calls on the GPU; jobs | 0; 0 | 203; 399 | `os-image-dispatch` |
| Placement work per call | None | 2 loads, 1 compare | `os-image-dispatch` |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | Copy counter in `gpu-mimus-layers` |
| Subsampling codes written; lines cleaned | 7,032,960; 0 | 7,032,960; 219,780 (8.9 ms at 494 MHz) | Producer and `aarch64_device_cache` counters |
| Recognizer wake-ups; IPIs; tick waits per job | None | 1; 0; 0 | Queue and notification counters |
| Arena peak | 1,761,840 bytes | At most 2,834,736 bytes at any utterance length | Host arena high-water mark |
| GPU idle per subsampling gap | None | At most 1 ms on the A55 pair | `os-image-dispatch`, producer under Unicorn |
| Mapping check | None | 2,048 translations once per boot | `os-image-dispatch` |

The arena peak adds the subsampling buffer and at most 312,576 bytes of codes
moved from a ring into the arena (222 rows × 1,408 bytes).

**Time estimate.** Every figure assumes the GPU at 400 MHz
(`A733_GPU_INITIAL_HZ`), its only verified clock. On the A55 pair the linear
stage takes 0.79 s. GPU compute takes 0.585 s and the read-out of each call's
last job on one A55 0.155 s. First-job cleans, last-job invalidates and job
overhead add 51 ms, and the split saves 3 ms. The other stages alone take 2.29 s
on that pair, so the A55 configuration cannot meet the goal. The A76 pair at
2,002 MHz takes 0.39 s. The kernel design's time model on the A76 pair gives
these figures for the remaining levers:

| Lever | Owner | Linear stage |
| --- | --- | --- |
| GPU at 600 or 800 MHz | GPU power and thermal owner | 0.31 s or 0.26 s |
| NEON int32-lane accumulation at 2× or 3× the CPU rate | mimus port | 0.28 s or 0.22 s |
| No exposed tail: more jobs in flight, read-out on both A76 cores | Kernel design | 0.38 s |
| 800 MHz, 3× and no tail | All three | 0.17 s |
| 800 MHz, 4× and no tail | All three | 0.14 s |

Only the last row meets 0.15 s. No GPU operating point above 400 MHz is
recorded, and the CPU factors are unmeasured.

Host cases in `checks/os-image-dispatch.c`:

- no OPEN before the hash, and a failed mapping check;
- refusal, RESET with recompute, and LOST;
- a worker that never answers OPEN, close or a job, which expires the window;
- two consecutive jobs near the worker's deadline that end OK;
- no zero-row job, and the longest capture with three or more jobs per call;
- the emitted table for each core set and pair of clocks.

## Left out

- **NPU placement.** The pinned NPU compiler refuses the INT32 output that
  exact sums need, and its input quantization uses one scale where mimus uses
  one per row ([npu-mimus-linear.md](../npu-mimus-linear.md)). It returns when
  exact sums build with the pinned compiler.
- **Keeping GPU rows after RESET and the CPU path after LOST.** This returns
  when the board shows that a GPU write through a read-only mapping faults and
  leaves memory unchanged.
- **Read-out on both inference workers.** It runs only when `n_gpu = N`, on
  the A55 pair, where it saves about 78 ms and the goal stays out of reach. If
  it returns, a slot is resubmitted only after the join that ends its
  read-out.
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
  - this design's supervision in place of its "No answer by the deadline"
    class;
  - `rows = 0` refused, with the extent computed in 64 bits after the
    `rows ≤ job_rows` check;
  - clean-and-invalidate on the slots;
  - one maintenance pass at submit over the activation extent and the
    argument record, or the record and CDM words in the uncached pool, so a
    submit carries one `DSB SY` pair or none.

  Its model-upload class does not arise in the OS image.
- **mimus port:** the tensor ordinal in `TensorView`; the 240-row subsampling
  producer; the second worker's affinity; attention and the remaining stages
  within 0.2935 s. It also needs NEON accumulation in int32 lanes, flushed to
  int64 every 512 products; the table then takes the measured CPU rate.
- **GPU power and thermal owner:** an operating point above 400 MHz, with the
  modeled times above.
- **GPU runtime:** the worker on the recognition core.
- **Memory architecture:** the 12 MiB span and the slots as fixed spans lent
  to the GPU; an EL0 read mapping of the slots while they are device-owned;
  the leases in the capacity replay.
- **Primary agent:** A76 admission for the image; `0x46C00000`–`0x47000000`
  read-only at EL0.
- **Bluetooth voice recognition package:** it also edits `apps/os/os.h` and
  `docs/os-image.md`; this package only adds declarations and sections there.

## Board evidence that would confirm the design

- The GPU build recognizes the bench recording with text and CRCs equal to the
  host, and the dispatch record shows every placed call on the GPU.
- An EL0 store into the slots faults, and the start entry's check passes.
- INTID 95 arrives on the recognition core for every job.
- An injected hang ends in RESET, the recomputed result equals the host, and a
  one-hour resident run never stops watchdog renewal.
- CPU and GPU rates per shape at each admitted clock replace the table's
  seeds.
