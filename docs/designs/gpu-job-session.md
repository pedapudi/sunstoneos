# Several timed GPU jobs in one firmware session

Status: **proposed; not implemented.** It follows the first GPU job, which
passed on port 3-1 in set 29 ([first GPU job](gpu-first-job.md)): one
32-lane compute job between the firmware's health check and its stop. This
step runs several jobs on one context in one firmware session, times each
one, and makes the client CCB wrap. The step after it runs a larger
dispatch (below).

## Why this step

Goal-level compute needs the GPU to run job after job on a live firmware,
and needs to know how long each takes. The first job showed one KICK
completing, but not:

- **repeat on one context.** The timeline advances 1, 2, 3 … on the same
  firmware context, with no cleanup between jobs. `pvr_compute_job_repeat`
  exists and is host-checked, but has never run on a board;
- **the client CCB wrap.** Each job takes 184 bytes (`0xb8`) of the
  1,024-byte ring, so the sixth job's commands no longer fit at the end. The
  host writes a padding command (type 221) and restarts at offset 0. The
  firmware has never read a padding command from us;
- **time.** The first job's log has no time for the job itself; set 29
  bounds KICK-to-timeline only by a 25.75 ms remainder that also covers
  preparation, cleanup and park.

## Operating point

The GPU runs at the vendor's top operating point, the one it already runs
at: 1,008 MHz from PLL_GPU0 with DCDC4 at 960 mV. That is the highest node
of the vendor device tree's `gpu_opp_table` (`sun60iw2p1.dtsi:4736–4761` at
kernel `2ac08e8c7`; the other nodes are 800, 600 and 400 MHz, and 300 MHz is
commented out). `include/a733_gpu_opp.h` holds it, and the power service
admits DCDC4 only at that node's voltages. No clock or voltage changes.

## The image

The first-job image (`apps/gpu-power-id/fw-start.c` with `FWS_JOB`), built
with a new option `FWS_JOB_REPEAT=8`, changes only the job phase:

1. As now: the workload pages and tables, `MMUCACHE` waited for by the
   executed count, the client CCB format, the first job.
2. After job *n* completes and its 32 words and guards check, job *n + 1*
   is `pvr_compute_job_repeat`: the image refills the output with the
   complement of each expected word, cleans it, and kicks the same CDM
   command with the timeline UPDATE to *n + 1*. The fill makes each job's
   words its own: a job that does not run leaves the fill.
3. Each job is timed with the generic counter (24 MHz, 41.7 ns): from just
   before its KICK's `MTS_SCHEDULE` write to the first poll that reads the
   timeline at *n*. The image polls the timeline without yielding while a
   job is in flight, bounded by the job budget.
4. After the last job: one cleanup, then the stop and OFF, as now.

New lines, each under the 127-character limit:

- `GPU FWJ time n=8 first_us= min_us= max_us= total_us=`;
- `GPU FWJ repeat done= timeline= wraps=` (`wraps` counts the padding
  commands written).

The job budget stays 200 ms, of which the cleanup keeps 50 ms; eight jobs
fit if each takes under 18 ms. A job that has not finished by the end of
the budget ends the sequence as outcome 2, and the stop runs as now.

## Predictions

- `timeline=0x8`, every job's 32 words as in set 29 (`w0=0x2e5ca990`,
  `w31=0x4775f521`), guards `0x6d35a7c1`;
- one wrap after the fifth job, which the firmware reads without a
  context reset (`events … n=0`);
- `kccb_done=0xb` after the cleanup: the health check, `MMUCACHE`, eight
  KICKs and the cleanup; `0xd` after the two power requests;
- each job well under 1 ms at 1,008 MHz, the first the slowest. This is a
  guess: no measurement exists. A first job much slower than the others
  would point at cold MMU and system-level caches.

## Checks before a board run

- `checks/pvr-cccb.c`: a host case that wraps the ring at the sixth job,
  with the padding command's bytes checked against upstream's
  `pvr_cccb_write_command_with_header` padding.
- `apps/gpu-power-id/check.py --fw-job`: the firmware model runs eight
  KICKs, follows the padding command, and checks the time line's fields;
  cases for a job that never completes at job 1, 5 and 8, and for a reset
  event during job 6.
- The usual review and gate before freezing.

## Design gate

**Cost.** Seven more KICKs and seven output refills and cleans (one page
each), well inside the 200 ms job budget; no new memory. The log grows by
two lines.

**Risk.** As set 29. The ring wrap is new to the firmware from us; if the
firmware rejects the padding command, the job fails with a context reset
or a stuck timeline, and the stop, with or without acknowledgements, still
powers the GPU off.

## The step after: a larger dispatch

The compiled-package path (`pvr_compute_package.c`, profile 2) runs a
Mesa-compiled kernel over up to 992 elements in 31 dispatches of 32 lanes,
each dispatch with its own PDS slot (first index and count) and a final
fence. On the board it would show the multi-dispatch CDM stream, the tile
pair's low-word-first order (fixed after set 27 and never run), and the
throughput of a real workload. It needs the 4 KB package output and input
regions in the image's payload and a host case per profile, and is
proposed once this step passes.
