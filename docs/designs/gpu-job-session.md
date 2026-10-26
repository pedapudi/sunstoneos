# Several timed GPU jobs in one firmware session

Status: **designed; not implemented.** It follows the first GPU job, which
passed on port 3-1 in set 29 ([first GPU job](gpu-first-job.md)): one
32-lane compute job between the firmware's health check and its stop. This
step runs eight jobs on one context in one firmware session, times each
one from the board counter, and makes the client CCB wrap. The step after
it runs a larger dispatch (last section).

## Why this step

Goal-level compute needs the GPU to run job after job on a live firmware,
and needs to know how long each takes. Set 29 showed one KICK completing,
but not:

- **repeat on one context.** The timeline advances 1, 2, … 8 on the same
  firmware context, with no cleanup between jobs. `pvr_compute_job_repeat`
  (`drivers/gpu/pvr_compute_job.c:313`) is host-checked but has never run
  on a board.
- **the client CCB wrap.** A job writes two commands: the CDM command (40-
  byte header and 96 bytes) and the timeline UPDATE (40 and 8), 184 bytes
  (`0xb8`) of the 1,024-byte ring. After five jobs the write offset is 920.
  The sixth CDM command needs its 136 bytes plus room for a 40-byte padding
  header, 176 bytes, and 104 remain; so the host writes a padding command
  (type 221, size 64) at 920 and restarts at 0, as upstream does
  (`pvr_cccb.c:163–188`). The firmware has never read a padding command
  from us.
- **time.** Set 29's log has no time for the job; its 62.42 ms image time
  leaves a 25.75 ms remainder covering preparation, the job, the cleanup,
  the park and the report together.

## Operating point

The GPU stays at the vendor's top operating point, 1,008 MHz from PLL_GPU0
with DCDC4 at 960 mV: the highest node of the vendor device tree's
`gpu_opp_table` (`sun60iw2p1.dtsi:4736–4761` at kernel `2ac08e8c7`; the
others are 800, 600 and 400 MHz, and 300 MHz is commented out). Set 29 ran
there; `include/a733_gpu_opp.h` holds it. No clock or voltage changes.

## The image

The first-job image (`apps/gpu-power-id/fw-start.c` built with `FWS_STOP`
and `FWS_JOB`) gains a build option `FWS_JOBS=8` (default 1, the set-29
image). Only the job phase of `fw-job.inc` changes:

1. As in set 29: the workload pages and tables, `MMUCACHE` waited for by
   the executed count, the client CCB format, then job 1.
2. When job *n* completes (timeline *n*, 32 words and both guards checked)
   and *n* < 8, `pvr_compute_job_repeat` starts job *n* + 1 on the same
   context: the job's publish step refills the output with the complement
   of each expected word and cleans it, and the same CDM command is
   written with the UPDATE value *n* + 1. A job that does not run leaves
   the fill, so each job's words are its own.
3. The first job that does not complete ends the sequence. Its outcome is
   reported as in set 29 (2 no timeline, 3 output differs, 4 transport,
   5 firmware event), with the job's number.
4. After the sequence: the cleanup, when the firmware has finished with
   the context (the last timeline reached, or a context reset); then the
   stop and OFF, with or without the firmware's acknowledgements, exactly
   as set 29.

### Timing

Both ends come from the generic counter (24 MHz, one tick 41.7 ns), the
counter the progress record already stamps:

- **kick**: the counter read in the kernel CCB's kick callback
  (`fws_kick`) just before the `MTS_SCHEDULE` write of the job's KICK;
- **done**: the counter read just after the timeline word is first read at
  *n*.

While a job is in flight the loop does not yield: each pass services the
control (two interrupt-status reads and the kernel CCB scan) and reads the
timeline word, and no `vTaskDelay` runs until the job completes or the
budget ends. The pass count is logged, so the run reports its own timing
resolution: the longest pass bounds the error of each *done*.

Phase times inside set 29's remainder, also from the counter: the job
preparation before power-on, the page cleans and table publication,
`MMUCACHE`, the eight jobs (first kick to last done, so the gaps between
jobs show), the cleanup, and the park after the OFF.

### New log lines

Each line stays under the 127-character limit:

- one per job: `GPU FWJ run n=3 ticks=12345 passes=17 timeline=0x3`;
- `GPU FWJ time n=8 min=… max=… total=…` in ticks;
- `GPU FWJ phases prepare_us=… publish_us=… mmu_us=… jobs_us=…
  cleanup_us=…`;
- `GPU FWS park us=…`;
- the job line gains `jobs=` (completed jobs) and the ring line `wraps=`.

The log grows by about 700 bytes, to about 12 KB of 16,128.

## Budgets and costs

Section 3 of [the performance architecture](../performance-architecture.md)
budgets the GPU service, not this diagnostic image. Its rules that apply:

| Path, rate | Cost per operation | Budget |
| --- | --- | --- |
| Job submit, 8 per session | Output refill and clean (one page), two CCCB command writes, one KCCB command, one `MTS_SCHEDULE` write and read-back | No tick wait or status poll between the submit and the kick |
| Completion wait, per pass | Two interrupt-status reads, one kernel CCB control read, one timeline read (Normal-NC) | No yield while a job is in flight; passes logged |
| Output check, 8 per session | One page invalidate, 64 word reads | As set 29 |

**Heavier forms used.** Completion is polled, not taken from the interrupt:
this image runs without a GPU interrupt handler, and the interrupt path is
qualified separately in the GPU service (`checks/pvr-gpu-irq.c`). Polling
without yield gives the timing its resolution. Every GPU register access
is logged and recorded in the progress record (`fws_access`), as in every
step since the firmware start; repeated reads of one register stay quiet.
The per-access record costs time on every pass and lengthens each
measured latency; the pass count measures it.

**Failure classes** (performance architecture section 1.8). A job that
does not complete is a controlled failure: the sequence ends, the cleanup
runs when the firmware has finished with the context, and the stop powers
the GPU off, with the firmware's acknowledgements or by upstream's hard
reset when the idle checks pass. A GPU that never reports idle stays
powered until a power removal, as in set 29. Nothing is retained between
sessions: the image ends at the OFF.

## Checks before a board run

- `checks/pvr-cccb.c`: a host case that runs eight two-command jobs
  through the 1,024-byte ring and checks the padding command at 920
  (type 221, size 64) and the sixth job's commands at 0.
- `apps/gpu-power-id/check.py --fw-job` on the eight-job image, with a
  firmware model that runs every KICK and follows the padding command:
  - eight complete jobs: timeline 8, every job's words, `kccb_done=0xb`
    after the cleanup, the per-job and phase lines;
  - a job that never completes, at job 1, 5, 6 and 8;
  - a context reset at job 1, 5, 6 and 8;
  - in each failure case: the earlier jobs complete, the sequence stops
    at that job, the cleanup when allowed, and the GPU powered off.
- The set-29 image (`FWS_JOBS=1`) still passes its thirteen cases.
- `checks/pvr-compute-job.c`: the repeat path, unchanged code.

## Predictions

- `timeline=0x8` and `jobs=8`; every job's 32 words as in set 29
  (`w0=0x2e5ca990`, `w31=0x4775f521`), guards `0x6d35a7c1`;
- `wraps=1`, before job 6, read by the firmware without a context reset
  (`events … n=0`);
- `kccb_done=0xb` after the cleanup (the health check, `MMUCACHE`, eight
  KICKs, the cleanup) and `0xd` after the two power requests;
- per-job latency: no measurement exists. A 32-lane job at 1,008 MHz is
  expected well under 1 ms; a first job much slower than the others would
  point at cold MMU and system-level caches.

## The step after: a larger dispatch

The compiled-package path (`pvr_compute_package.c`, profile 2) runs a
Mesa-compiled kernel over up to 992 elements in 31 dispatches of 32 lanes,
each dispatch with its own PDS slot (first index and count) and a final
fence. On the board it would show the multi-dispatch CDM stream, the
first-index and count order fixed after set 27 and never run, and a real
workload's throughput. It needs the package's input and output regions in
the image's payload and a host case per profile, and is proposed once this
step passes.
