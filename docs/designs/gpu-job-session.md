# Several timed GPU jobs in one firmware session

Status: qualified on the board (port 3-1) for eight 32-lane jobs on one context in one firmware session, timed from the board counter; host-tested for the failed-shutdown rules.

The step follows the [first GPU job](gpu-first-job.md). It runs repeated jobs on a live firmware, makes the client CCB wrap and times each job.

## What the session demonstrates

- **Repeat on one context.** The timeline advances 1, 2, … 8 on the same firmware context, with no cleanup between jobs. `pvr_compute_job_repeat` (`drivers/gpu/pvr_compute_job.c`) starts the next job.
- **Client CCB wrap.** A job writes two commands, the CDM command (40-byte header and 96 bytes) and the timeline UPDATE (40 and 8), 184 bytes (`0xb8`) of the 1,024-byte ring. After five jobs the write offset is 920. The sixth CDM command needs 136 bytes plus room for a 40-byte padding header, 176 bytes, and 104 remain, so the host writes a padding command (type 221, size 64) at 920 and restarts at 0, as upstream does (`pvr_cccb.c:163–188`).
- **Time.** Each job is timed from kick to the visible timeline update.

## Operating point

The GPU stays at the vendor's top operating point, 1,008 MHz from PLL_GPU0 with DCDC4 at 960 mV: the highest node of the vendor device tree's `gpu_opp_table` (`sun60iw2p1.dtsi:4736–4761` at kernel `2ac08e8c7`; the others are 800, 600 and 400 MHz). `include/a733_gpu_opp.h` holds it. The image changes no clock or voltage.

## The image

The first-job image (`apps/gpu-power-id/fw-start.c` built with `FWS_STOP` and `FWS_JOB`) takes `FWS_JOBS=8` (default 1) and changes only the job phase of `fw-job.inc`.

1. Workload pages and tables, `MMUCACHE` waited for by the executed count, the client CCB format, then job 1, as in the first job.
2. When job *n* completes (timeline *n*, 32 words and both guards checked) and *n* < 8, `pvr_compute_job_repeat` starts job *n* + 1 on the same context with the session deadline (`bb_power.deadline`). The publish step refills the output with the complement of each expected word and cleans it, and the CDM command is written with the UPDATE value *n* + 1. A job that does not run leaves the fill, so each job's words are its own. A repeat starts only while at least 10 ms of the 150 ms wait remain; otherwise the sequence ends with outcome 6 (budget).
3. The first job that does not complete ends the sequence. Its outcome is reported as 2 (no timeline), 3 (output differs), 4 (transport) or 5 (firmware event), with its number.
4. After the sequence the cleanup is sent when the firmware has finished with the context. That is so when the timeline equals the current job's sequence, or the firmware reset the context, or the current job was never kicked (its client CCB serial is still the previous job's and the timeline is at the previous sequence). A job kicked whose timeline never arrived keeps the context live and no cleanup runs. Then the stop and OFF follow, with or without the firmware's acknowledgements, as in the first job.

### Timing

*Done* is the moment the firmware's timeline UPDATE becomes visible to the CPU, not the end of the compute work. How promptly the firmware's write leaves the GPU's system-level cache is part of the measurement. Both ends come from the 24 MHz generic counter (one tick 41.7 ns):

- **kick**: `fws_access` reads the counter immediately before the `MTS_SCHEDULE` write, after its progress record and log line and a `DSB SY` that completes them.
- **done**: after the KICK's write and read-back return, the image spins on the timeline word alone, with one counter read and one Normal-NC load per pass and no register access, clock lock, log line or yield. The counter at the first read that equals *n* is *done*. The control is serviced only afterwards: the KICK is settled, events are read and the output is checked.

Each job logs the ticks from kick to the spin's first read, the pass count, and the scheduler ticks crossed between its KICK pass and *done*. With one pass the job's time is an upper bound; a nonzero tick count marks a time that a tick or a service step may have lengthened. `first` includes the `MTS_SCHEDULE` read-back and its two log lines, the floor of every time. The `MMUCACHE` and cleanup waits do not yield. The spin ends at the 150 ms wait.

### Log lines

All times are in counter ticks, each line under 127 characters:

- per job: `GPU FWJ run n=6 ticks=… first=… passes=… os=… write=0xb8`, where `write` is the client CCB write offset after the job's commands (`0xb8` … `0x398` for jobs 1 to 5, then `0xb8`, `0x170`, `0x228`);
- `GPU FWJ phases prepare=… publish=… mmu=… jobs=… cleanup=…`;
- `GPU FWS park ticks=…`, from after the OFF's gate report and proof;
- the job line carries `jobs=` (completed jobs).

The log is about 13.6 KB of 16,128 bytes; the host check asserts the worst case stays under the limit.

## Budgets and failure classes

| Path, rate | Cost per operation | Budget |
| --- | --- | --- |
| Job submit, 8 per session | Output refill and clean (one page), two CCCB command writes, one KCCB command, one `MTS_SCHEDULE` write and read-back | No tick wait or status poll between submit and kick |
| Completion spin, per pass | One counter read and one Normal-NC timeline read | No register access, clock lock, log line or yield |
| Control service, per pass after *done* | Two interrupt-status reads, kernel CCB control reads with barriers, counter-clock reads | |
| Output check, 8 per session | One page invalidate, 64 word reads | |

Completion is polled because the image has no qualified GPU interrupt handler; the interrupt path is qualified in the GPU service (`checks/pvr-gpu-irq.c`) and the wait is far shorter than a tick. A job that does not complete is a controlled failure: the sequence ends, the cleanup runs when the firmware has finished with the context. The stop powers the GPU off, with the firmware's acknowledgements or by upstream's hard reset when the idle checks pass. A GPU that never reports idle stays powered until a power removal.

## Failed shutdown retains power and ownership

The diagnostic powers off only after a complete stop: the firmware handshakes, register idle checks and platform isolation. A failed control can instead use the unacknowledged stop when its transport has latched failure and no OFF acknowledgement, register step or isolation has occurred. That stop runs the complete idle plan, asserts the reset mask selected from the validated firmware features, and then requests OFF.

The idle plan freshly reads the BIF MMU and outstanding-read registers (`0x1358`, `0x1350`, `0x1320`, `0x1338`), SLC outstanding transactions (`0x3870`), SLC idle (`0x3898`) and Sidekick idle (`0x3c8`). Any failed poll prevents reset and OFF. A firmware OFF acknowledgement followed by a failed command collection or retirement also retains power; the acknowledgement alone does not establish that transactions have drained.

A failed firmware start or refused control leaves power and ownership retained, and the application does not manufacture a failed control state to admit recovery. Before any firmware access, a successful ON followed by failed GPU mapping, refused identity or insufficient startup time permits plain OFF. Uncertain ON completion, invalid provider mappings and clock failure retain resources without further GPU register access. A partial OFF retains resources and does not retry isolation. A retained failure ends GPU use for that powered lifetime and requires coordinator-directed physical recovery.

The stop has a 300 ms total budget within the diagnostic's 2 s monitor: 200 ms for acknowledged shutdown and 100 ms reserved for an admitted unacknowledged stop. Expiry limits diagnostic availability and never permits reset or resource release.

These rules follow the pinned upstream `pvr_fw_startstop.c:220–320`, which returns at each failed idle poll before its reset, and vendor `rgxpower.c:392–427`, which returns when the power-off handshake fails. Source revisions and licenses are in the [firmware-stop design](gpu-firmware-stop.md). CORE6's vendor ON disables the device-interface handshake, so its OFF cannot replace GPU drain proof.

## Checks

- `apps/gpu-power-id/check.py --fw-session` runs fifteen cases on the eight-job image: complete jobs, a job that never completes or a context reset at jobs 1, 5, 6 and 8, a firmware that reads the padding command, the clock passing the wait's end (job 6 never kicked), less than 10 ms of wait left, and the worst-case log.
- The native ELF checks model the firmware and platform power callbacks and cover the shutdown rules above. They establish software ordering only.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Eight jobs on one context | all 32 words matched each time, no firmware event or reset; cleanup and stop acknowledged, GPU off | [gpu-job-session.jsonl](../evidence/gpu-job-session.jsonl) `gpu-job-session-set30` |
| Kick to timeline | job 1 after `MMUCACHE` 113 µs; jobs 2 to 5, 7, 8: 80 to 83 µs; job 6 (first after the wrap) 87 µs | same record |
| Preparation and the eight jobs | 1.45 ms; 2.39 ms (kick of job 1 to timeline of job 8) | same record |
| Host checks | 15 session cases, 14 stop, 4 job and 2 session cases for shutdown | `gpu-job-session-host-checks`, `gpu-job-session-shutdown-host` |

## Limitations and open work

- Physical failure recovery and application operation remain unqualified for the failed-shutdown rules.
- The GPU runs only at 1,008 MHz with DCDC4 at 960 mV; the 800, 600 and 400 MHz nodes are unused.
- The CORE6 power-on to status 8 (7 to 11 ms) dominates a session's startup; a service that powers the GPU per job pays it each time.
- The PowerVR firmware binary is under `LICENSE.powervr`, which forbids disassembly; it is decoded only through the host driver sources, the interface headers and the board's own records.
- The MMU-status field of the first-job fault record (catalogue base 1) is unexplained; it did not affect the cause ([first GPU job](gpu-first-job.md)).
