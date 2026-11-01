# GPU

**Status: implemented.**

Native firmware loading, power/interrupt ownership,
compute, rendering and application bindings have software evidence. Finite
queue execution, sharing, restart, injected-timeout recovery and one recorded
recognition input have board evidence. General graphics and thermal recovery
remain unqualified.

**Board-qualified scope:** Standalone firmware start, health, stop and restart,
eight compute jobs and two production exact-sum shapes pass at 1008 MHz/960 mV.
The shapes use K=512 and K=516, each with 33 rows and 17 channels. All 1,122
sums, tags and guards match; context cleanup, firmware OFF, isolation, provider
parking and normal ROM return pass for those frozen inputs.
The external recognizer also completes 154 batched submissions and compares
1,737,312 sums with zero mismatches on port 3-1. Its reference CRCs are
`ac657a3b` and `a7ade80e`, and its transcript is `There was silence.`
The qualification entry is `1793558596353360282-28c39811` under
`~/.cache/sunstoneos/board-queue/entries/`.

Evidence paths are relative to `~/.cache/sunstoneos/evidence/`:

- `board-session-20261025-20/port-3-1/` and
  `board-session-20261025-22/port-3-1/` record firmware stop/restart.
- `board-session-20261025-29/port-3-1/` records eight jobs.
- `sol-resume-20261028/port-3-1/gpu-exact-sum/` binds the exact-sum inputs
  and healthy retirement.
- `sol-resume-20261028/port-3-1/gpu-cache-permission/` tests the CPU-only
  read-only-output cache helper; it does not establish general GPU visibility.

The generic compute queue is implemented with one retained program and memory
map, up to four buffer leases and two trusted application slots. One job executes
at a time. Completion uses interrupts with a 4 ms backup poll. Shared mutable
mappings bypass the GPU system cache; data pages are at most 256 KiB.
Startup uses executed-command health, vendor secure writes, initial timer
correlation and MMU-cache completion before READY.

Healthy close powers off while retaining memory for reopen. Timeout/reset
recovery requires idle checks, ordered reset write/read completion, power-off
and firmware-memory reclaim before queue resumption. Failed isolation retains buffers and owed
records. Final completion quiesces both callers before CPU return or resident
suspension. The primary application owns lifecycle; the second has independent
manifest admission, submission sequence and completion queue.

Host checks cover 204 exact-sum jobs, ten shared open/close cycles, queued-peer
preservation during reset recovery, native reset access policy and final
quiescence. The mimus shader/readout checks preserve exhaustive sum comparison.
These modeled results supplement the finite board cases below. The OS uses the
[application kit](../designs/app-abi.md) and does not depend on mimus.

The texture frame fixture measures native render-service work for three scenes,
with two frames each. Its timed probes compile the actual service scheduling
code with modeled time and task readiness. Burst, CPU-run and submission
exemption mutations fail. Work counters exclude the scheduler itself. Extended
texture and batch fixtures prepare their pools once; native translation checks
reject incorrect final write-back and uncached blocks before GPU construction.
Host evidence is under
`~/.cache/sunstoneos/build/gpu-integrate-1031/texture-prepare-1101/` and
`~/.cache/sunstoneos/build/gpu-integrate-1031/texture-scheduling-1101/`.

GPU capture supports image-bound GPU/thermal receipts and bounded binary
expectations in the shared runner. The following port 3-3 cases return normally.
Each entry is under `~/.cache/sunstoneos/board-queue/entries/`; its
`run/result.json` binds the ELF hash, frozen inputs, native return and receipts.

| Workload | Entry | Observed result |
| --- | --- | --- |
| Firmware without jobs | `1793560443073044909-2795e0f8` | Zero GPU jobs; reference transcript; complete version-2 thermal capture |
| One vector job | `1793558556213820434-b15a959e` | 65 exact elements; checksum 8,840; one completed GPU job |
| Eight vector jobs | `1793558639545392070-81ba2729` | Eight checked outputs; eight completed GPU jobs; 144–178 µs each |
| Ten shared open/close cycles | `1793558732769026225-99c2d8da` | Ten owner jobs, one peer job, ten opens and ten closes |
| Injected timeout recovery | `1793560094069288734-8923c447` | One HANG fault and recovery; queued peer and next owner complete |
| Qualified recorded recognition | `1793560622000131271-53954059` | 186 completed GPU jobs; exact reference CRCs and transcript; complete version-2 thermal capture |

Successful GPU receipts show GPU OFF and a parked worker; CPU return receipts
show all seven secondary CPUs OFF. The recognition ELF compiles
`MIMUS_GPU_QUALIFY_SUMS=1`, comparing every offloaded sum with its CPU reference.
The input produces feature CRC `ac657a3b`, log-probability CRC `a7ade80e`,
90 frames and 57,440 samples. Its text is “there was silence” and its formatted
output is “There was silence.” These results qualify the named finite inputs;
they do not establish sustained load, thermal protection or arbitrary fault recovery.

Additional passing controls are firmware entry `1793558145641638619-38d15f35`
and qualified recognition entry `1793560272415337471-7f3b4f28`. The current
controls include the absolute thermal schedule from `bringup` revision
`f9375cb7`. The thermal merge changes sampling and capture. GPU driver code
and queue job inputs match the earlier passing cases.

Retained failure records distinguish the corrected conditions:

- Entry `1793556885965480682-30338083` reports no accessible new FEL instance
  and no native receipt. The coordinator identified missing udev access;
  the rebuilt firmware control returns after physical recovery.
- Entry `1793558247336076757-d91d1dbf` records application detail `0x412`:
  job 1 receives PARKED before admission. Diagnostics yield and retry for at
  most 500 scheduler ticks while the session remains live.
- Entry `1793558878520344931-0ec4f97b` records a recovery shutdown refusal.
  Entry `1793559451655627386-914e388f` locates its origin at the reset equality
  poll: step 15, timeout, last low word `0xe8000020`. The
  [reset completion contract](../designs/gpu-recovery-reset-readback.md)
  preserves the vendor write and discarded fence read, with mandatory idle
  checks and OFF/reclamation before returning buffer ownership.

Host evidence under `~/.cache/sunstoneos/build/gpu-integrate-1031/` includes
54 vector-add cases and 50 lifecycle cases in `parked-1101/`; 124 shutdown
fault cases, 44 native breadcrumb cases, native backend and vendor comparisons,
five focused QEMU recovery cases and the three baseline builds in
`recovery-1101/`. The merged thermal checks cover eight native cadence cases,
nine decoder cases and the linked return writer. The selected integration gate
and check index pass. These models establish software behavior within their
stated hardware boundaries.

Remaining work includes sustained concurrent workloads, arbitrary stuck-device
recovery, thermal throttling and critical-temperature protection, and graphics.
CPU performance evidence does not establish GPU speedup. Frozen earlier failed
inputs and fault origins remain in [the hardware worklog](../hardware-worklog-20261031.md).
See [backend](../gpu-backend.md), [power](../gpu-power.md),
[rendering](../gpu-render.md) and [compiled programs](../gpu-compiled-programs.md).

## Device execution dominates reference recognition latency

The comparison shader in external mimus revision `99c700db0` takes
444.437 ms without timing diagnostics, against 135.474 ms for CPU-only
recognition of the same 3.59 s recording.
Both paths use CPU-L 1794 MHz, CPU-B 2002 MHz and DSU 1274 MHz.
The GPU uses 1008 MHz at 960 mV. Exhaustive CPU sum comparison is disabled
in these timing runs; the qualification entry above covers that comparison.

| Port 3-1 entry | Diagnostic timing | Compute | Aggregate `kick_us` | Outcome |
| --- | --- | ---: | ---: | --- |
| `1793560651607761780-3cb221f5` | OFF, control | 444.437 ms | 339.188 ms | 154 jobs, reference CRCs/text, healthy shutdown and ROM return |
| `1793561022387122671-06be5388` | ON, firmware timestamps | 448.492 ms | 342.515 ms | Same result, all 154 timing rows present |

Raw output and result records are in each entry's `run/` directory under
`~/.cache/sunstoneos/board-queue/entries/`.
The images' SHA-256 values are respectively
`1d169eb93ab45c52c06b032809bcb214a3b7aeea3b2d14e31a42ad685a6efea4` and
`935573354591c78c9c4bef570557e47faae777f9673ef7bd76c4851db0a29297`.
Parsed records, `analyze.py`, `phase-summary.json`, parity checks and build
evidence are under `~/.cache/sunstoneos/build/gpu-job-timing-1101/`.
The diagnostic pair differs by 4.055 ms, or 0.91% of recognition time.
One pair cannot separate instrumentation cost from run variation.

The following intervals partition submit-to-client-return time for each job.
The means cover all 154 jobs; independent minima need not sum to a job's minimum.

| Interval | Mean per job | Observed range |
| --- | ---: | ---: |
| Client submit to worker dispatch | 17.585 µs | 15.542–27.958 µs |
| CPU preparation excluding cache publication | 1.890 µs | 1.625–3.625 µs |
| Input/argument cache publication, including barriers | 8.011 µs | 2.958–55.375 µs |
| Client command construction/write through firmware command preparation | 20.442 µs | 19.208–23.875 µs |
| Firmware kick register write and barrier | 0.499 µs | 0.458–0.875 µs |
| Kick to firmware PRE timestamp | 39.593 µs | 34.780–47.771 µs |
| Firmware PRE to POST: device scheduling and execution | 2160.780 µs | 644.064–11176.889 µs |
| Firmware POST to interrupt-handler entry | 8.455 µs | 7.341–9.262 µs |
| Interrupt-handler entry to blocked GPU worker resumption | 2.320 µs | 1.792–3.292 µs |
| Worker resumption to output acquisition | 15.615 µs | 13.167–17.500 µs |
| Output cache acquisition, including barriers | 1.838 µs | 0.292–4.583 µs |
| Completion record preparation | 0.635 µs | 0.542–1.292 µs |
| Completion marker to waiting client's return | 7.303 µs | 6.875–13.042 µs |
| **Total submit to client return** | **2284.966 µs** | **758.000–11349.125 µs** |

For example, job 97 executes a 3 × 3 × 1 grid. Its device interval is
644.064 µs, `kick_us` is 705 µs and submit-to-client-return time is 758 µs.
Every job has one completion interrupt that resumes a blocked GPU worker.
The 171 total worker blocks include 17 backup-poll expirations during longer
jobs. No job waits for a backup poll after its firmware POST timestamp.

CPU timing uses the 24 MHz architectural counter.
The GPU timer advances at 3.93747 ticks/µs, matching 1008 MHz divided by 256.
Firmware PRE/POST intervals match rounded integer nanoseconds within 0.953 ns
of a GPU timer quantum. This establishes their nanosecond interpretation by
independent clock correlation; the unit remains inferred from measurements.
The PRE/POST interval includes firmware/device scheduling and command setup.
It does not isolate arithmetic cycles or GPU memory stalls.

The PRE/POST span totals 332.760 ms, or 94.6% of aggregate job elapsed time.
`kick_us` encloses this span; its name does not identify firmware overhead.
Removing every measured interval outside PRE/POST would save only 19.125 ms
across these jobs, before accounting for CPU/GPU overlap or trace overhead.
Interrupt polling cannot recover the roughly 309 ms penalty over CPU-only.

## Optional diagnostics preserve the disabled path

`SUNSTONE_GPU_TIMING=ON` enables
[the bounded timing recorder](../../apps/os/gpu-timing.c).
It uses link wrappers, the existing GPU breadcrumbs and the FreeRTOS queue-block
trace hook. [The backend](../../platform/a733/gpu_backend.c) records the actual
kick write and brackets GPU timer reads with CPU timestamps.
[The GPU service](../../apps/os/gpu.c) records interrupt-handler entry.
No file under `drivers/gpu/` changes for these measurements.

The recorder inserts firmware PRE/POST timestamp commands around the existing
compute command, before its completion update. The pinned native DDK declares
PRE/POST command IDs 209 and 217 with eight-byte firmware-address payloads.
Its `rgxccb.c` brackets the device task with these commands, and
`rgxfwutils.c` allocates 64-bit CPU-readable timestamp destinations.
The native payload declarations are in `rogue/rgx_fwif_km.h`.
These cached references belong to the existing `gpu-first-job-ddk-2ac08e8c`
source selection documented in [the compute queue design](../designs/gpu-compute-queue.md).

Timestamp destinations use unused words in the retained uncached timeline page.
Publication and acquisition barriers preserve ordering with the device.
The original timeline update remains the completion condition.
Diagnostic GPU timer reads require a live, admitted backend and an actual kick;
refusals and failed recovery paths do not gain register access.

The external application's `GPUTIME` records print after timed recognition.
`GPUTIME_FIELDS` names the columns; stamp values are 24 MHz tick deltas from
`base`, and -1 denotes an absent stamp. Cache fields are accumulated CPU ticks.
Firmware fields and GPU counter samples remain raw in the output.
`GPUTIME_END rows=154 total=154` establishes complete capture for this input.
The public diagnostic record API is declared in
[gpu_queue.h](../../include/gpu_queue.h) and
[gpu_queue_timing.h](../../include/gpu_queue_timing.h).

With the option OFF, no timing buffer, wrappers, timestamp commands, trace calls
or diagnostic output is linked. Compared with the qualified control, executable
instructions are unchanged; source-line fault constants move where lines shift.
The sanitized [producer check](../../checks/gpu-job-timing.py), native ON build,
all three baseline builds, selected integration gate and check index pass.

## Proposed driver changes target measured costs

The current workload's observed overhead outside device PRE/POST is
112.266 µs at best and 124.186 µs on average. These are observed overheads
with diagnostics; a universal firmware minimum is not established.
Within `kick_us`, the residual outside PRE/POST is 58.111–71.063 µs,
averaging 63.343 µs. A timestamped minimal kernel is needed to separate fixed
device setup inside PRE/POST from shader execution and establish a firmware floor.
The smallest production job already spends 644 µs inside that interval.

The estimates below are inferred ceilings or targets from measured intervals.
They are not demonstrated speedups, and overlapping savings cannot be added.
They use the comparison shader above. Driver implementation belongs to the
GPU queue owner.

1. **Characterize and reduce device execution first.** Compare minimal, small
   and large kernels using the same timestamp commands and retained context.
   Then vary tile size, load reuse and occupancy while preserving exact sums.
   Halving the measured device span saves up to 166.4 ms per utterance before
   overlap. The factor of two is a hypothetical target.
2. **Batch ready descriptors behind fewer kicks.** Extend `pvr_compute_queue`
   and its service to retain multiple independently ready descriptors, emit
   their commands in order and retire each sequence from timeline updates.
   Assume 60–90 µs of amortizable transport per job, using the measured
   63.343 µs `kick_us` residual as a reference and including some CPU dispatch.
   A four-job burst would reduce that cost to 15–23 µs per logical job.
   Across 154 jobs the ceiling under this assumption is 6.9–10.4 ms,
   before dependencies and CPU overlap. Admission, lease lifetimes, cancellation
   and per-sequence failures require explicit ownership throughout the burst.
3. **Keep command streams resident where they repeat.** The program, context,
   model mappings and firmware already persist. Cache validated command
   templates and stage the next descriptor while the device runs.
   Assume a target reduction of 20–40 µs per job, informed by the measured
   20.442 µs command preparation and 39.593 µs launch delay.
   This target would save up to 3.1–6.2 ms per utterance.
   Removing firmware launch latency requires measured support for streaming
   commands; a CPU-side template alone cannot remove it.
4. **Publish only CPU-dirtied ranges.** Track ownership and dirty generations
   for arguments, activations and outputs instead of maintaining every declared
   range. Cache calls total 1.517 ms for the utterance, or 9.849 µs per job.
   That total is the maximum direct saving; immutable model mappings already
   persist. Skipping required publication without ownership tracking is invalid.
5. **Keep interrupts; consider only bounded completion polling.** Eliminating
   IRQ-to-worker resumption saves at most 2.320 µs per job, or 0.357 ms total.
   Even eliminating POST-to-acquisition saves at most 26.390 µs per job,
   or 4.064 ms. Polling consumes a CPU and can reduce useful CPU/GPU overlap.

An application-visible job below 100 µs needs reduced transport overhead and
a device interval below the remaining budget. At a hypothetical 40 µs total
host/firmware overhead, the device interval must fall below 60 µs: more than
10 times faster for the smallest job and 36 times faster at the observed mean.
Batching amortizes overhead per logical projection; it increases the work in
each physical kick and does not make a larger physical job finish in 100 µs.

## In-job readbacks confirm the applied GPU top point

Port 3-1 entry `1793562523846617254-42844119` returns with reference CRCs and
text, 27 checked minimal jobs, 154 production jobs and complete timing capture.
The readbacks use the existing power provider and validated firmware mappings.
No queue-driver source changes for these diagnostics.

| Readback | Observed value | Interpretation |
| --- | --- | --- |
| GPU PLL control | `0xf9105300` | PLL enabled, output enabled, lock asserted; vendor factors select 1008 MHz |
| GPU clock mux/divider | `0x80000000` | GPU PLL selected, divisor one, gate enabled and no update pending |
| GPU bus/reset | `0x00010001` | Bus gate enabled and reset released |
| CORE6 power policy and status | `8`, `8` | Domain ON before, during and after jobs |
| PMIC DCDC4 selector/enable | `0xae`, `0xff` | Selector low seven bits `0x2e`, rail enable bit set: programmed 960 mV |
| Firmware core-clock metadata | `1008000000` | Configured timestamp frequency; independent PLL/divider readbacks establish the applied setting |
| Firmware DVFS enable | zero | Autonomous PDVFS disabled in the sampled firmware configuration |
| Firmware active-power latency/persistence | `0 ms`, `1` | Retained native runtime configuration |

The PLL, divider, domain and retained rail signatures are unchanged in every
retained state sample: 181 before kicks, 181 after completion and first/latest
middle samples from nine long production jobs.
Nineteen middle observations occur while PRE is published and POST remains
unpublished across the CPU sampling interval. Exported first/latest observations
retain their CPU timestamps and raw firmware timestamp words.
Their firmware state is ON; every pre-kick firmware state is IDLE.
No retained sample shows domain OFF or forced idle.

The voltage record is an actual startup PMIC provider readback retained under
its original rail/request leases and checked against their live identities.
It establishes the programmed selector during this owned lifetime.
It is not a fresh I²C transaction during each job or an analog voltage reading.
The thermal sampler records temperature independently.

Firmware IDLE is a command-scheduling state while CORE6 remains powered.
The samples contain no domain-OFF state or evidence of per-kick full-domain
power-up. They do not exclude brief transitions between samples.
The idle-gap experiment below bounds their observable latency effect.

## A checked minimal dispatch takes about 153 microseconds after its kick

The external application's optional `MIMUS_GPU_MINIMAL_PROBE=ON` runs before
timed recognition. It retains the production context and shader, submits one
workgroup with K=4, one row and one output channel, and checks its zero sum
and every padded lane's completion tag. Borrowed activation bytes are restored
before recognition. Twenty-seven jobs cover three repetitions at each idle gap.
`SUNSTONE_GPU_TIMING=ON` supplies the same PRE/POST commands as production jobs.

| Requested idle gap | Median device PRE–POST | Median kick-to-completion |
| --- | ---: | ---: |
| 0 ms, including the first dispatch | 96.000 µs | 157 µs |
| 1 ms | 94.730 µs | 155 µs |
| 2 ms | 97.016 µs | 156 µs |
| 4 ms | 96.762 µs | 156 µs |
| 8 ms | 96.000 µs | 155 µs |
| 16 ms | 96.000 µs | 156 µs |
| 32 ms | 96.507 µs | 155 µs |
| 64 ms | 97.270 µs | 157 µs |
| 256 ms | 97.016 µs | 157 µs |

Excluding the first dispatch, medians are 96.254 µs inside PRE/POST,
156 µs kick-to-completion and 213.479 µs submit-to-client-return.
The respective best observations are 93.460, 152 and 209.708 µs.
The first dispatch takes 121.396 µs inside PRE/POST, about 25 µs above the
warm median. This is a one-time cold-dispatch effect of unresolved origin.
There is no idle-gap latency step through 256 ms; warm gap medians span
94.730–97.270 µs. No measured hysteresis explains the production millisecond
kernel costs.

These figures measure the observed cost of the near-empty retained production
program, including its setup and padded work. They do not isolate a zero-instruction
firmware cost. No tested dispatch finishes in 100 µs; reaching that target
requires a change to the measured path. Amortizing dispatch across logical
operations is a separate goal.
Queue batching belongs to the fastpath implementation stream.

The optimized shader repeats this experiment in entry
`1793563985616621662-f052bac6`, with 27 minimal and 148 production jobs.
Its warm medians are 93.461 µs inside PRE/POST, 153 µs kick-to-completion and
210.062 µs submit-to-client-return. Best observations are respectively
91.174, 150 and 207.042 µs. Its first device interval is 120.635 µs,
about 27 µs above the warm median. Warm device medians across idle gaps
span 92.952–94.730 µs, with no onset through 256 ms.
All retained clock, domain and PMIC signatures remain at the applied top point.
The unchanged interface contributes about 60 µs outside the minimal device
interval after the kick, before the application's other preparation and return.

Evidence is under `~/.cache/sunstoneos/build/gpu-device-exec-1101/`:
`before-summary.json`, `before-probes.json`, `before-state.json`,
`large-probe-summary.json`, the QEMU
probe-to-recognition parity checks and the three baseline builds in `validation/`.
The disabled diagnostic control, entry `1793562362886636744-4b4fa9c2`, returns
154 production jobs in 444.063 ms after the lifecycle recovery merge.

## The optimized reference program remains slower than CPU-only

The external application's smaller register tile and wider loads reduce its
timed device interval total from 335.998 to 221.633 ms with the same 154 jobs.
Larger producer calls then reduce the workload to 148 jobs and device time to
216.716 ms. These are single diagnostic traces with CPU sum comparison disabled.
The shader, placement and producer changes belong to mimus; OS queue-driver
code matches `bringup` `2651238d`.

Three runs with diagnostics and CPU comparison disabled take 327.617,
327.476 and 327.169 ms. Their median is 327.476 ms, with a
327.169–327.617 ms range. This is 26.3% below the merged GPU control's
444.063 ms and 2.42 times CPU-only 135.474 ms. It remains above the
179.5 ms recognition target for the 3.59 s recording.
All three use ELF SHA-256
`62bd94efdb4fd356d886f8ae9fddf5ecb5b97fa61bbdaacca5674ce179d02fbb`.

Both the 154-job shader and the 148-job larger producer pass exhaustive
comparison of all 1,737,312 GPU sums, with reference CRCs and text.
The larger producer's arena peak is 5,295,696 bytes against 2,330,576 bytes
for 224-row calls, within the existing 12 MiB application grant.
All successful entries retain native proof of completed GPU work, GPU OFF,
worker parking, retired PMIC sampling, seven secondary CPUs OFF and ROM return.
This qualifies the reference recording and healthy shutdown, with the broader
GPU qualification limits stated above.

| Port 3-1 entry | Case | Outcome | Compute |
| --- | --- | --- | ---: |
| `1793562362886636744-4b4fa9c2` | Merged GPU control, diagnostics OFF | Returned | 444.063 ms |
| `1793562523846617254-42844119` | Comparison shader, readbacks and 27 minimal probes | Returned | 453.758 ms |
| `1793562798861818897-0827fa55` | Smaller tile, exhaustive sums ON | Returned | 708.073 ms |
| `1793563069525024856-fae93123` | Smaller tile, timed 154 jobs | Returned | 339.535 ms |
| `1793563416684239128-a996b815` | Larger producer, exhaustive sums ON | Returned | 702.618 ms |
| `1793563721079634350-e908cac3` | Larger producer, timed 148 jobs | Returned | 333.483 ms |
| `1793563985616621662-f052bac6` | Optimized shader and 27 minimal probes | Returned | 333.315 ms |
| `1793564152591260158-7b442d20` | Final timing, diagnostics OFF, trial 1 | Returned | 327.617 ms |
| `1793564244209939257-9848a75a` | Final timing, diagnostics OFF, trial 2 | Returned | 327.476 ms |
| `1793564362816985826-5f5fd2b5` | Final timing, diagnostics OFF, trial 3 | Returned | 327.169 ms |

The task build directory retains `board-outcomes.json`, complete `PROFILE.txt`,
`final-timings.json`, raw parsed jobs, per-image README commands and hashes,
and `roofline/comparison.md`. The latter distinguishes source-level integer
operations and byte requests from conditional FP32 and DRAM references.
Actual GPU DRAM transactions are not measured by these diagnostics.
