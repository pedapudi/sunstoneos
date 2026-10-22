# Measurements: exact mimus integer matrix products on the GPU

These measurements check the [GPU matrix-product component](../gpu-mimus.md)
against the budgets in its [design](gpu-mimus-compute.md). They compare the
integration base (the merge base with `audit/integration`, commit `6cd12f0`)
with the change at commit `3311a81`. All values come from host builds, host checks and
the time model; no board or GPU was used, and no USC instruction was executed.

## Method

- **Builds.** The base commit and the change were each exported with `git archive`, configured with the shared configuration
  script and `-DSUNSTONE_GPU_COMPUTE_APPLICATION=ON`, and built in separate
  directories with `-j 6` under
  `~/.cache/sunstoneos/build/gpu-mimus-compute-measure/r3/`. The seven targets
  that compile `drivers/gpu/pvr_compute_job.c` were built at both commits:
  the GPU, compute, free-list, IRQ, memory and power service models and the
  GPU render application. `llvm-size -A` gave section sizes.
- **Unchanged job bytes.** At both commits, each tree's
  `apps/gpu-program/compile.py` compiled the affine and vector-addition
  programs for profile two and the transpose and neighbor programs for
  profile three, and `checks/pvr-compute-package.py` compared the native PDS
  and CDM bytes of the resulting packages with the pinned Mesa generators.
  `checks/pvr-compute-service.py` ran its first three emulator cases at both
  commits. `checks/pvr-compute-job.py` compared the fixed job with Mesa's
  reference at both commits. At the change its reference build receives the
  project's include directory; at the base it stops at the missing header
  `pvr_ccb.h`.
- **New checks.** `checks/gpu-mimus-program.py`, `checks/gpu-mimus-layers.py`
  and `checks/gpu-mimus-readout.py` ran as listed in
  [gpu-mimus.md](../gpu-mimus.md#checks), with mimus at commit `3e05158`.
  `checks/gpu-mimus-time-model.py` ran twice: with the design's assumptions,
  and with the call list, instruction groups per MAC, temps and read-out
  counts from those checks. Each run prints the issue-bound times and the
  times for load and store latencies of 50, 100, 200 and 400 GPU clocks.

The base has no GPU path for mimus, so its rows below are the CPU profile in
[mimus.md](../mimus.md) or the design's stated baseline.

## Results against the design's budgets

| Metric | Baseline | Target | Result | Within budget |
| --- | --- | --- | --- | --- |
| Output bytes differing from mimus's CPU path | No GPU path | 0 | 0 of 12,923,134 bench outputs; 0 in every other case | Yes |
| Copies per weight, code and sum byte | 1 weight expansion per call | 0, 0, 0 | 0, 0, 0: the argument record holds the model and arena addresses, and the read-out reads the output region | Yes |
| Jobs per recording | Not possible | 204 | 204 | Yes |
| USC instruction groups per MAC | None | At most 1.75 | 3.39: 217 groups per 64 MACs | **No** |
| Program code bytes | None | At most 4,000 | 7,624, in two 4 KiB program pages | **No** |
| Temps per instance | None | At most 112, which keeps 8 tasks resident | 106 | Yes |
| GPU compute, all channels | 14.894 s on the A55 | 0.574 s | At least 1.111 s, issue-bound; 1.726 s with 200-clock loads | **No** |
| DRAM bytes read per MAC, per lane | None | 0.75 | 0.75 | Yes |
| Lines maintained per recording, GPU only | None | 597,601 cleaned; 988,060 cleaned and invalidated | 597,397 cleaned; 988,060 cleaned and invalidated | Yes |
| `DSB SY` per job | None | 6 | 6 | Yes |
| Tag words per recording, GPU only | None | 815,008 | 815,008 | Yes |
| Read-out instructions per recording | Inside the CPU kernel | At most 43.3 M | 37.6 M (with bias in every layer) | Yes |
| Compares per job; translations; hashes | Native resource admission and a CPU mapping check | At most 12; none; none | 9; none; none | Yes |
| Ring and job-word bytes per job | 368-byte bound | At most 300 | 276: 184 ring bytes, 92 job-word bytes | Yes |
| Linear stage, best channel split, 494 MHz | 14.894 s | 0.629 s | At least 0.997 s, 80.6% of MACs on the GPU; 1.328 s, 71.5%, with 200-clock loads | **No** |
| Linear stage, best channel split, 2,002 MHz | 14.894 s | 0.396 s | At least 0.535 s, 45.5% of MACs on the GPU; 0.664 s, 33.5%, with 200-clock loads | **No** |
| Linear stage, GPU only, 2,002 MHz | 14.894 s | 0.612 s | At least 1.149 s; 1.764 s with 200-clock loads | **No** |
| GPU leaf entries | 4 KiB only | 17 | 18: the program takes two 4 KiB pages | **No** |

The time rows give the issue-bound figure as a lower bound; "Memory latency
and occupancy" below explains why. The cleaned-line count is 204 lines below
the design's, because the model places every call's codes on a 64-byte
boundary and the design counts one extra line per job for an unaligned
start.

## The instruction-count miss

The compiler's listing of the inner loop holds, per 4 K steps of one lane:

| Operation | Groups |
| --- | --- |
| Multiply-adds (`imadd32`) | 64 |
| Signed extractions (`ibfe`) | 32 |
| Selects that the compiler adds after each extraction for a zero field width | 32 |
| Register moves | 42 |
| 64-bit address adds, loads and load waits (`add64_32`, `ld`, `wdf`) | 36 |
| Loop control | 11 |

The design's 1.75 assumed one group per extraction and no moves. The pinned
compiler translates every signed field extraction, including those written as
two shifts, as `ibfe` followed by a select on the field width, even when the
width is the constant 8 or 16. Four rewrites were tried and rejected:
extraction with `mul_hi` and pointers incremented per load stream both added
moves, a separate per-row product sum added multiply-adds, and a 64-bit
accumulator did not compile within two minutes. Keeping the 64-bit base of
each load stream in a register cut each address computation from two groups
to one; the loop above is the shortest found.

With the measured counts, the time model's best placement keeps more channels
on the CPU: at 2,002 MHz the linear stage takes 0.535 s against 0.396 s, and
it no longer fits the 0.4435 s pipeline goal. Two changes
would reduce the count, and each needs board evidence first. Loads of 16
bytes would take four words per instruction once the board shows their
alignment rule. Shift amounts passed at run time would reduce the extraction
to one shift where the field is the top of its word; a first attempt put the
shift amounts in scratch memory, which the profile forbids.

## Memory latency and occupancy

The issue-bound figures assume that the USC never waits for memory. Two
properties of the compiled program make that a lower bound:

- **One wait per load.** The pinned scheduler places a `wdf` wait directly
  after each load, so in every pass of 4 K steps a lane's 12 loads complete
  one after another. After its last pass, a lane makes 32 sum stores
  (4 rows, 4 channels, 2 words), each followed by `idf` and `wdf`.
- **Few resident tasks.** The program uses 106 temps per instance. Temps
  are allocated in units of 8, so a 32-lane task holds 32 × 112 registers.
  Mesa's formulas for the BXM-4-64 give a unified store of 28,672 temps:
  256 rows × 8 banks per instance, less two reserved 128-register lines,
  for 16 parallel instances. That holds 8 such tasks to hide the waits.

The time model therefore charges each 32-lane task
`max(issue, chain / R)` GPU clocks. Here `issue` is 217 groups per pass,
`chain` is the task's own serial time of 217 + 12 L clocks per pass plus 32 L
for the stores, L is the load and store latency in GPU clocks, and R is the
number of resident tasks, 8, or the job's workgroup count when that is
smaller. With 8 resident tasks a pass becomes latency-bound above about 127
clocks. For the measured program:

| L (GPU clocks) | GPU compute, all channels | Best split, 494 MHz | Best split, 2,002 MHz |
| --- | --- | --- | --- |
| Issue-bound | 1.111 s | 0.997 s, 80.6% on the GPU | 0.535 s, 45.5% |
| 50 | 1.111 s | 0.997 s, 80.6% | 0.535 s, 45.5% |
| 100 | 1.111 s | 0.997 s, 80.6% | 0.540 s, 45.1% |
| 200 | 1.726 s | 1.328 s, 71.5% | 0.664 s, 33.5% |
| 400 | 3.313 s | 1.890 s, 53.5% | 0.814 s, 18.4% |

At 100 clocks only jobs of fewer than 8 workgroups change, because one task's
chain then exceeds the issue time of the whole job. At 2,002 MHz `n_gpu` for
the 176 × 176 shapes falls from 80 at 100 clocks to 64 at 200 and 32 at 400.
A latency of 200 clocks is 500 ns at 400 MHz. No measurement of the A733's
GPU load latency exists yet. The board item that settles it is GPU time per
layer shape against a single-load latency probe; the probe's result replaces
`--load-cycles`. If the hardware keeps fewer than 8 tasks resident, the same
comparison shows it.

## Left-out changes, with measured constants

At 2,002 MHz on the A76 pair, the issue-bound best channel split saves
0.33% with a second job in flight, 0.02% with the GPU share's read-out on
both cores and 0.00% with a larger CPU-channel share on core 7. With
400-clock loads the savings are −0.70%, −0.14% and 0.53%: a second job
halves each job, and small jobs keep fewer tasks resident. Each stays below
the design's 2% rule at every latency in the table above.

## Images

| Target | Base `.text` | Change `.text` | Base `.rodata` | Change `.rodata` |
| --- | --- | --- | --- | --- |
| GPU service model | 148,480 | 148,480 | 135,416 | 135,416 |
| Compute service model | 184,440 | 183,928 | 137,684 | 137,684 |
| Free-list service model | 151,504 | 151,504 | 135,362 | 135,362 |
| IRQ model | 147,712 | 147,712 | 135,304 | 135,304 |
| Memory model | 153,776 | 153,776 | 135,432 | 135,432 |
| Power service model | 153,680 | 153,680 | 136,244 | 136,244 |
| GPU render application | 290,268 | 289,968 | 150,768 | 150,848 |

`.data` and `.bss` are equal at both commits in every image. The compute
service model's `.text` is 512 bytes smaller. The render application's
`.text` is 300 bytes smaller and its `.rodata` 80 bytes larger, 220 bytes
smaller in total.

The profile-three PDS and CDM encoders, the fence program and the firmware
command are external functions that the mimus session shares. In every
image the job builder inlines all four, and the linker drops the external
copies, because no image links the mimus session yet. Those copies make
`pvr_compute_job.c`'s object larger before linking: 21,148 bytes against
20,120 in the service models and 11,672 against 10,820 in the render
application. The two grid encoders are `always_inline`, since the render
application's options otherwise keep them out of line. The word-copy loop
is not unrolled, so the constant PDS tables stay in `.rodata` instead of
becoming immediate stores. `image` keeps `noinline`; without it the
compiler inlines one of its two calls and `pvr_compute_job.o` grows by
2,644 bytes. With `noinline` on the four encoders, the IRQ model's `.text`
grew by 256 bytes (28 bytes of call overhead, rounded up by section
alignment) and the render application's by 324 bytes.

## Evidence

Raw outputs are under
`~/.cache/sunstoneos/build/gpu-mimus-compute-measure/r3/`: `images.txt`
(section sizes of both builds and of `pvr_compute_job.c`'s object), and
under `evidence/`: `program/`, `layers/` (with `calls.txt`), `readout/`,
`package-{base,head}-p{2,3}/` (the compiled programs, packages and check
results of each commit), `service-{base,head}/`, `jobcheck-{base,head}/`,
`time-model-design.txt` and `time-model-measured.txt`.

At both commits every package check and every service case passed. The 12
packages that the package check builds, six per profile, and their four
programs are byte-identical between the commits. In the service cases the
open path made no timed waits at either commit. The yield count of the
first two cases is 6 and 5 at the base and 5 and 6 at the change; it varies
between 5 and 6 from run to run of the emulator at the same commit.

At the change, both 267,368-byte fixed jobs of `checks/pvr-compute-job.py`
equal Mesa's reference. The check reads its instrumented probe source
(`probe.c`, `probe.h`, `probe.spv`) from its output directory, so those
three files were copied from an earlier run of the check at the change.
