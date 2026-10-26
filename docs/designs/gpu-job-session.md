# Several timed GPU jobs in one firmware session

Status: **qualified on port 3-1** in set `board-bundles-20261025-30`
(`board-session-20261025-29/port-3-1`, record `s29-gjs-run`). Eight
32-lane jobs ran on one context in one firmware session; all 32 output
words matched, with no firmware event or reset; the cleanup and the stop
were acknowledged and the GPU powered off. The step follows the first GPU
job ([first GPU job](gpu-first-job.md)): it runs eight jobs on one context
in one firmware session, times each from the board counter, and makes the
client CCB wrap.

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
and `FWS_JOB`) gains a build option `FWS_JOBS=8` (default 1, the
one-job build of the session code). Only the job phase of `fw-job.inc`
changes; the one-job build differs from the image qualified in set 29 in
the same ways: the timed spin, no tick wait in the job, `MMUCACHE` and
cleanup loops, the new lines, a job without its timeline reported in its
KICK phase, and the never-kicked branch of the cleanup rule.

1. As in set 29: the workload pages and tables, `MMUCACHE` waited for by
   the executed count, the client CCB format, then job 1.
2. When job *n* completes (timeline *n*, 32 words and both guards checked)
   and *n* < 8, `pvr_compute_job_repeat` starts job *n* + 1 on the same
   context with the session deadline (`bb_power.deadline`, as the first
   job's init uses). The job's publish step refills the output with the
   complement of each expected word and cleans it, and the same CDM command
   is written with the UPDATE value *n* + 1. A job that does not run leaves
   the fill, so each job's words are its own. A repeat starts only while at
   least 10 ms of the 150 ms wait remain; otherwise the sequence ends with
   outcome 6 (budget).
3. The first job that does not complete ends the sequence. Its outcome is
   reported as in set 29 (2 no timeline, 3 output differs, 4 transport,
   5 firmware event), with its number.
4. After the sequence, the cleanup is sent when the firmware has finished
   with the context: the timeline equals the current job's sequence (that
   job completed or failed after its UPDATE), or the firmware reset the
   context, or the current job was never kicked (its client CCB serial
   still the previous job's, the timeline at the previous sequence). A job
   kicked whose timeline never arrived keeps the context live: no cleanup.
   Then the stop and OFF, with or without the firmware's acknowledgements,
   exactly as set 29.

### Timing

*Done* is the moment the firmware's timeline UPDATE becomes visible to the
CPU, not the end of the compute work; the UPDATE runs after the CDM
command, and how promptly the firmware's write leaves the GPU's
system-level cache is part of the measurement. Both ends come from the
generic counter (24 MHz, one tick 41.7 ns), the counter the progress
record already stamps:

- **kick**: `fws_access` reads the counter immediately before the
  `MTS_SCHEDULE` write itself, after its progress record and log line and a
  `DSB SY` that completes them, and keeps it for the job loop;
- **done**: right after the KICK's write and read-back return, the image
  spins on the timeline word alone: one counter read and one Normal-NC load
  per pass, no register access, no clock lock, no log line and no yield.
  The counter at the first read that equals *n* is *done*. The control is
  serviced only after that, as before: the KICK is settled, events are
  read, and the output is checked.

Each job also logs the ticks from kick to the spin's first read and the
pass count: with one pass, the job's time is an upper bound, not a
latency. `first` includes the `MTS_SCHEDULE` read-back and its two log
lines, the floor of every time. The counter is read before the timeline
load, so a time is right to within one pass. The spin ends early at any value other than *n* − 1.
The image keeps FreeRTOS preemption, and the power service runs above it,
so each job also logs the scheduler ticks crossed between its KICK pass
and *done*: a nonzero count marks a time that a tick or a service step may
have lengthened. The `MMUCACHE` and cleanup waits no longer yield either,
so their times are the firmware's, not the next tick. The spin ends at the 150 ms wait like every other wait. The
output refill and clean happen in the job's publish step, before the kick;
the output check comes after *done*.

Phase times inside set 29's 25.75 ms remainder, also from the counter: the
job preparation before power-on, the page cleans and table publication,
`MMUCACHE`, the eight jobs (first kick to last done, so the gaps between
jobs show), the cleanup, and the park after the OFF. Jobs 1 and 6 are the
ones expected to differ: job 1 follows `MMUCACHE` and first fetches the
programs and input through the system-level cache, and job 6 includes the
firmware's read of the padding command.

### New log lines

All times are in counter ticks. Each line stays under the 127-character
limit:

- one per job: `GPU FWJ run n=6 ticks=… first=… passes=… os=… write=0xb8`,
  where `write` is the client CCB write offset after the job's commands
  (`0xb8` … `0x398` for jobs 1 to 5, then `0xb8`, `0x170`, `0x228`);
- `GPU FWJ phases prepare=… publish=… mmu=… jobs=… cleanup=…`;
- `GPU FWS park ticks=…`, from after the OFF's gate report and proof;
- the job line gains `jobs=` (completed jobs).

Each extra KICK logs its `MTS_SCHEDULE` write and read-back and an
interrupt clear, about 235 bytes, so the log grows by about 2.3 KB in all,
to about 13.6 KB of 16,128; the host check asserts the worst case (eight
jobs, a reset, the unacknowledged stop) stays under the limit.

## Budgets and costs

Section 3 of [the performance architecture](../performance-architecture.md)
budgets the GPU service, not this diagnostic image. Its rules that apply:

| Path, rate | Cost per operation | Budget |
| --- | --- | --- |
| Job submit, 8 per session | Output refill and clean (one page), two CCCB command writes, one KCCB command, one `MTS_SCHEDULE` write and read-back | No tick wait or status poll between the submit and the kick |
| Completion spin, per pass | One counter read and one Normal-NC timeline read | No register access, clock lock, log line or yield; passes logged |
| Control service, per pass after *done* | Two interrupt-status reads, the kernel CCB control reads with their barriers, the counter-clock reads | As set 29 |
| Output check, 8 per session | One page invalidate, 64 word reads | As set 29 |

**Heavier forms used.** Completion is polled, not taken from the interrupt
(performance architecture §1.3): this image has no qualified GPU interrupt
handler, the interrupt path is qualified separately in the GPU service
(`checks/pvr-gpu-irq.c`), and the wait is far shorter than a tick. The
timed spin touches no register; every GPU register access outside it is
logged and recorded in the progress record (`fws_access`), as in every
step since the firmware start, and stays outside the timed window.

**Failure classes** (performance architecture section 1.8). A job that
does not complete is a controlled failure: the sequence ends, the cleanup
runs when the firmware has finished with the context, and the stop powers
the GPU off, with the firmware's acknowledgements or by upstream's hard
reset when the idle checks pass. A GPU that never reports idle stays
powered until a power removal, as in set 29. A successful session ends at OFF; a failed drain retains all resources.

## Checks before a board run

- `checks/pvr-cccb.c`: eight two-command jobs through the 1,024-byte ring,
  compared with upstream's bytes after every job: the padding command at
  920 (type 221, size 64) before job 6, job 6 at 0, each KICK's write
  offset and wrap mask; and a firmware read offset still at job 5's start
  (736) when job 6 is written, which leaves 839 bytes free for the 288
  needed. Because each job completes before the next is written, the ring
  never waits for space in this session; the existing pressure cases cover
  the wait.
- `apps/gpu-power-id/check.py --fw-session` (fifteen cases) on the
  eight-job image, with a
  firmware model that runs every KICK and follows the padding command:
  - eight complete jobs: timeline 8, every job's words, `kccb_done=0xb`
    after the cleanup, the per-job, phase and park lines;
  - a job that never completes, at job 1, 5, 6 and 8: the earlier jobs
    complete, no cleanup is sent, and the stop powers the GPU off;
  - a context reset at job 1, 5, 6 and 8: the cleanup is sent and the stop
    powers the GPU off;
  - a reset whose job writes no timeline (job 6);
  - a firmware that reads job 6's padding command, moves to 0 and stops;
  - the clock passing the wait's end after job 5, during job 6's publish or
    while its commands are written: job 6 is never kicked, the two forms of
    the never-kicked branch of the cleanup rule;
  - less than 10 ms of the wait left after job 4 (outcome 6);
  - the worst-case log under 16,128 bytes: eight jobs, a reset at job 8
    and no power acknowledgement. The model's timeline arrives on the
    spin's third counter read, so the multi-pass spin runs.
- The set-29 image (`--fw-job`, one job) still passes its thirteen cases.

## Predictions

- `timeline=0x8` and `jobs=8`; every job's 32 words as in set 29
  (`w0=0x2e5ca990`, `w31=0x4775f521`), guards `0x6d35a7c1`;
- job 6 at write offset `0xb8` and the ring line `cccb_read=0x228
  cccb_write=0x228`: the firmware followed the padding command, without a
  context reset (`events … n=0`);
- `kccb_done=0xb` after the cleanup (the health check, `MMUCACHE`, eight
  KICKs, the cleanup) and `0xd` after the two power requests;
- per-job latency: no measurement exists. A 32-lane job at 1,008 MHz is
  expected well under 1 ms; a first job much slower than the others would
  point at cold MMU and system-level caches.

## The board step

Set `board-bundles-20261025-30` runs first in a new lifetime on port 3-1:
the cold DDR start, the provider-readiness checkpoint, then bundle 29, the
session image (`850d833f…`, loadable bytes `80fa9d1a…`, from `5c4420d2`).
Its runbook gives the predictions above as the log lines they produce and
what each departure means.

## Set 30 on port 3-1

Kick to timeline, on the 24 MHz counter:

| Job | Time |
| --- | --- |
| 1, after `MMUCACHE` | 113 µs |
| 2 to 5, 7 and 8 | 80 to 83 µs each |
| 6, the first after the ring wrapped | 87 µs |

Preparation took 1.45 ms and the eight jobs 2.39 ms in all, kick of job 1
to timeline of job 8. A job's time includes the firmware's scheduling, the
CDM work, the timeline UPDATE and its write reaching the CPU; the first
read's floor (`first`) includes the `MTS_SCHEDULE` read-back and its log
lines.

## Failed shutdown retains power and ownership

The diagnostic powers off only after a complete stop. The stop includes the
firmware handshakes, register idle checks and platform isolation. A failed
control can instead use the existing unacknowledged stop when its transport
has latched failure and no OFF acknowledgement, register step or isolation
has occurred. That stop runs the complete idle plan, asserts the reset mask
selected from the validated firmware features, and then requests OFF.

The idle plan freshly reads the BIF MMU and outstanding-read registers
(`0x1358`, `0x1350`, `0x1320`, `0x1338`), SLC outstanding transactions
(`0x3870`), SLC idle (`0x3898`) and Sidekick idle (`0x3c8`). Any failed poll
prevents reset and OFF. A firmware OFF acknowledgement followed by a failed
command collection or retirement also retains power. The acknowledgement
alone does not establish that transactions have drained.

A failed firmware start or refused control leaves power and ownership
retained. The application does not manufacture a failed control state to
admit recovery. Before any firmware access, a successful ON followed by
failed GPU mapping, refused identity or insufficient startup time permits
plain OFF. Uncertain ON completion, invalid provider mappings and clock
failure retain resources without further GPU register access. A partial OFF
retains resources and does not retry isolation.

The stop has a 300 ms total budget within the diagnostic's 2 s monitor:
200 ms for acknowledged shutdown and 100 ms reserved for an admitted
unacknowledged stop. This is shorter than the vendor's 500 ms per poll and
upstream's 1 s per poll. Expiry limits diagnostic availability; it never
permits reset or resource release. A retained failure ends GPU use for that
powered lifetime and requires coordinator-directed physical recovery.

These rules follow the pinned upstream `pvr_fw_startstop.c:220–320`, which
returns at each failed idle poll before its reset, and vendor
`rgxpower.c:392–427`, which returns when the power-off handshake fails.
The source revisions and licenses are recorded in
[the firmware-stop design](gpu-firmware-stop.md). CORE6's vendor ON disables
the device-interface handshake, so its OFF cannot replace GPU drain proof.

The native ELF checks model the firmware and platform power callbacks.
They check retained power after failed idle polls, failed startup, refused
control and collection failure after the OFF acknowledgement. They also
check plain OFF before firmware startup and reject every GPU access after
OFF begins. These checks establish software ordering; physical failure
recovery and application operation remain unqualified for this revision.

The corrected firmware-stop, first-job and eight-job native images built
successfully. Fourteen selected stop cases, four job cases and two session
cases passed. Build records, source hashes, ELF files and model results are
preserved under `~/.cache/sunstoneos/evidence/gpu-recovery-20261025/`.
The unchanged job arithmetic and transport evidence remains applicable;
these checks cover the changed shutdown decisions and their callers.

## Next steps

For the next agent on the GPU:

- **The larger Mesa-compiled dispatch.** The compiled-package path
  (`drivers/gpu/pvr_compute_package.c`, profile 2) runs a Mesa-compiled
  kernel over up to 992 elements in 31 dispatches of 32 lanes, each with
  its own PDS slot (first index and count), and a final fence. It shows the
  multi-dispatch CDM stream, the first-index and count order fixed after
  set 27 (stored low word first; never run on a board), and a real
  workload's throughput. It needs the package's input and output regions
  in the image payload, a session variant that loads a package instead of
  the fixed program, and host cases per profile, then the usual panel,
  review and gate. Every set now continues a long-lived lifetime
  (`board-session-20261025-36/port-3-1`): no cold DDR start, no power
  removal, a warm-return chain from the lifetime's records.
- **Clock and DVFS.** The GPU runs only at the vendor's top operating
  point, 1,008 MHz from PLL_GPU0 with DCDC4 at 960 mV (the highest node of
  `gpu_opp_table`, `include/a733_gpu_opp.h`). No DVFS: the 800, 600 and
  400 MHz nodes are unused, and the power service admits DCDC4 only at the
  node's voltages.
- **Startup cost.** The CORE6 power-on to status 8 took 7.11 ms (set 27)
  and 11.09 ms (set 29) on port 3-1. It dominates a session's startup
  beside the firmware start (0.67 ms) and the health check (1.92 ms);
  a service that powers the GPU per job pays it each time.
- **License.** The PowerVR firmware binary is under `LICENSE.powervr`,
  which forbids disassembly. Decode it only through the host driver
  sources and the firmware-interface headers, and the board's own records.
- **Open post-mortem field.** In set 27's capture the MMU status of the
  recovery record names catalogue base 1, while the job's VM record shows
  the catalogue-register field 0 after the firmware wrote it
  ([first GPU job](gpu-first-job.md), "Set 27 post-mortem"). It did not
  affect the fault's cause and is unexplained.

## Design panel

Three reviewers read the first version of this note: over-engineering,
hardware performance and correctness.

- **Changed.** The kick stamp moved to just before the `MTS_SCHEDULE`
  write, after its log line; *done* comes from a spin on the timeline word
  alone, so neither the logging nor a tick wait sits in the timed window.
  The per-job line gives the first-read time and the pass count, so a
  one-pass job reads as an upper bound. The cleanup rule is stated per job,
  with the never-kicked branch. A repeat needs 10 ms of the wait left. The
  `wraps=` counter, the timeline field of the per-job line and the summary
  line were dropped: the write offsets show the wrap, and the summary
  follows from the per-job lines. All times are in counter ticks. The log
  estimate was recounted, and the worst case is checked.
- **Kept, against one reviewer.** The phase and park times stay, and the
  never-completes and reset cases stay at jobs 1, 5, 6 and 8: the step's
  request asks for the job's own time inside set 29's remainder and for
  those cases.
