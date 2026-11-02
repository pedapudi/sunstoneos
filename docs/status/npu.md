# NPU

**Status: implemented.**

Native model decoding, mappings, shared-provider startup, IRQ97 service, runtime clients and thermal policy have software evidence. Finite scheduled startup and inference have scoped physical results. Repeated-transition reliability remains unqualified. Opt-in internal clock gating of the idle NPU, its power owner and a benchmark with a baseline, a control and a gating image are implemented with host evidence only; no gating image has run on a board. A quiesced finite image that fails now returns to the boot ROM with a failure record instead of ending at the watchdog. Hardware values these paths assert (GIC priority views, interrupt routes, CPU identity) are in [the A733 hardware facts](../hardware/a733-facts.md).

**Software evidence:** `checks/npu-idle.py`, `checks/npu-queue-handoff.py` (production owner, queue and idle code with modeled devices and nine regression mutations), `checks/npu-idle-producer.py`, `checks/npu-idle-report.py`, `checks/npu-failure-return.py`, `checks/npu-latency.py`.

**Board-qualified scope:** A finite interrupt diagnostic produces matching
ShuffleNetV2 outputs at 852 MHz/800 mV. Startup takes 4.0 ms and inference takes
2.91 ms doorbell-to-interrupt, about 3.3 ms end to end. Provider-free
entry, disabled-IRQ installation/restoration and real provider/task construction
before scheduler start return normally with saved RAM restored. These
checkpoints perform no inference. The watchdog-admitting finite startup,
one-inference and two-job/two-model runtime images also qualify on 3-3 at
852 MHz/800 mV, with matching copied outputs, CLOSE, local stop and normal
ROM return. The resident command queue runs 1,000 matched jobs at 10.25 µs
p50 and 26,925 jobs/s, and 1,000 fence-chained two-stage jobs, on 3-3. The combined CPU and NPU image runs four NPU jobs through the OS NPU service while eight CPU workers run (overlap mask 15, matching outputs and CPU digests), then CLOSE, local stop and normal return, on 3-3 at 852 MHz/800 mV. The baseline idle benchmark image (no power owner) returns normally with 61,231 matching jobs: five windows of 12,000 jobs at about 26,650 jobs/s, p99 11 µs, maximum 17 µs, and cold jobs after 9, 11 and 100 ms gaps at 10.6 µs p50. Fifteen single-operation test graphs of this repository run through the OS NPU service on 3-1 and every output equals the
SDK simulator's byte for byte, in two runs (eight and seven jobs, the session limit): `add`, `sub` (stream-processor
operations with a second input from a graph input), `mul` (neural-network operation, second input from a graph input),
`relu`, `sig`, `tanh`, `swish` (activation operations), `sm32`, `sm64` (five stream-processor operations each),
`chainA`, `chainB`, `chainC` (a neural-network and a stream-processor operation; `chainB` and `chainC` take their second
input from VIP SRAM), `deep` (three neural-network operations with second inputs and an activation, outputs in VIP
SRAM), `mm` (batched matrix product, four operations, second input from a graph input at slice offsets) and `glu` (two
transposes and a stream-processor gate with its second input in VIP SRAM). The layer-normalization graphs `ln176` and
`ln64` (dynamic-input section, placed read-only after the coefficients) also match byte for byte, 17 of 17. The OS runtime admits a graph by structure and addresses; the digest and program catalogues are gone from the OS path (the finite session driver keeps its table). Per-graph time with the LOAD is about 1 ms. An NPU access outside the page tables
faults and the service recovers (3-1, 852 MHz/800 mV, normal return): five graphs, each with one descriptor pointer aimed
at an address the tables do not map (an input or an output; a DRAM canary of the image for four, page 0 at `0x180` for
one), failed one by one with an NPU MMU exception at the aimed address, MMU status 1 (slave not present), the canary
CRC unchanged after each and the session usable; a relu job after the five equals the simulator. Each recovery takes about
32 ms, of which 20 ms is the vendor's cancel poll, whose completion bit never appeared; the reset and re-initialization
that follow restore the NPU. See [the reference](../reference-backsolve-npu.md#npu-mmu-exceptions). Shader operations (kind 1) are admitted by the shader specification: exp, gather, resize and pad match the simulator byte for byte on the board, and a tensor descriptor pointing at an unmapped address and a program address pointing at one each fault at that address with the canary unchanged and the NPU recovered, so the shader and program-fetch paths go through the page tables. Tensor-processor and software operations (kinds 3 and 4) stay refused: the board's NPU reports zero tensor-processor engines and the compiler emits neither kind for this target. The finite idle benchmark qualifies internal clock gating on 3-3 at 852 MHz/800 mV: five rounds of baseline, control and gating pass every limit of the design (throughput, p99, maximum latency, CPU time), every resume of the 11 ms and 100 ms gaps is within 100 µs (largest 25 µs) and the 9 ms gaps never gate.

## What the NPU runs now

The operating system runs these graph kinds through the NPU, each verified on the board against the SDK simulator byte for
byte: neural-network operations (kind 2), stream-processor operations (6), activation operations (7), transpose and
related operations (8), and shader operations (1: exp, gather, resize, pad, and sin and floor as fault targets). Tensor
processor and software operations (kinds 3 and 4) are refused, because the board's NPU reports zero tensor-processor
engines and the compiler emits neither kind for this target. A graph reaches the NPU in one of two ways: linked into the
OS image, or handed over by the application at run time through `ss_npu_register_graph`, inside the graph size and tensor
size that the application declared in its manifest (board run `1793616402162300846-13133d4b`: relu and three shader
graphs registered after the image started, all five outputs equal to the simulator).

The EL0 queue (`ss_npu_queue_*`) runs on the routed Cortex-A55 core. With one reviewed graph resident in 31 slots under 31
names, ring depth 64 and 1,000 jobs for each in-flight limit k, every output equals the simulator, k up to 64 refuses
nothing, and k = 128 (twice the ring) receives 38,255 to 38,258 refusals in 1,000 jobs. These refusals are the designed
backpressure at depth 64: the submit call answers 0 when the ring is full. They are not a defect, and no job is lost or
duplicated. An application can also choose tensor links at run time, after the scheduler has started
(`ss_npu_queue_open_linked`, declared in the manifest as `npu.links`): the second graph's output equalled the reference
although its own input was never written, so the link reached the hardware.

### Per-job times and the clocks they depend on

A per-job time includes the EL0 submit path, so it depends on the CPU clock of the submitting core as well as on the NPU.
The NPU ran at its qualified 852 MHz at 800 mV, which the start sequence sets; no run counts the NPU clock. The relu run
`1793616938769293285-42647824` read the OS clock record before and after the sweep: the submitting core was core 1, a
Cortex-A55, at 1794 MHz and 1000 mV (the approved top point of that cluster), the Cortex-A76 cluster at 2002 MHz and 1050 mV, the
DSU at 1274 MHz, with no change during the sweep. The relu figures are therefore measured at the approved top points:

| Graph | Per job at k = 2 to 128 | Jobs per second | CPU clock of the submitting core | Entry |
| --- | --- | --- | --- | --- |
| relu | 5.45 µs (k = 1: 5.9 µs) | about 183,400 | 1794 MHz, measured | `1793616938769293285-42647824` (also `1793616163904565650-da3f264f`) |
| add | 10.4 µs at every k | about 96,300 | granted 1794 MHz; the applied point was not recorded by that image | `1793615676480712051-ab430778` |

The time does not depend on k from 2 up, because one engine is the limit: the device is busy for each job and the submit
path keeps it fed already at k = 2. A link pair of two jobs takes 11.4 µs. The add figure carries the same grant but no
measured clock, so it is a figure at the granted top point to be confirmed, and a run that needs it as a result should
repeat the add image with the clock record, as the relu image now does. Figures for other CPU clocks are per cycle
only: a different clock moves the submit-path part of the time.

## Admission and containment as qualified

- **Admission is by structure and addresses.** The loader decodes the model container, verifies its checksums, checks every
  operation and every address the graph patches, and confines the addresses to the job's own buffers
  (`docs/designs/npu-loader-admission.md`). No digest table or program catalogue is consulted on the OS path; the runtime
  hashes each model when it is prepared and LOAD compares the staged copy with that hash.
- **A declaration is required.** A session record without slots, depth and graphs is refused
  (`NPU_DECLARATION_MISSING`); there is no default. The declaration is checked against the image and against the memory
  that the reservation holds.
- **Containment is by the NPU page tables.** They map only the registered buffers, and an access outside them faults.
  Board runs aimed one descriptor pointer at an unmapped address (a tensor descriptor, and a program address of a shader):
  each failed alone with an NPU MMU exception at that address (status 1, slave not present), the DRAM canary of the
  image stayed unchanged (CRC `0x4696ca8e`), the service recovered in about 32 ms and the next job was correct.
- **Command windows are sized by the graph.** Each slot's command allocation is the graph's stream plus one END pair,
  rounded to a page, and the doorbell word is `0x10000 | (allocation >> 3)`. The vendor always writes `0x1ffff`; its sources
  do not define the field beyond a count of 8-byte units (see the reference, "Command allocation sized by the graph").
  The board confirmed the smaller word for a 4 KiB window (word `0x10200`): thousands of correct jobs, no MMU exception.
  Larger windows have no board run yet. If a run ever faults because of the word, the response is the vendor `0x1ffff`
  with the remainder of the 512 KiB window mapped to one shared read-only page of END words.

## Measured limits

| Limit | Value | Basis |
| --- | --- | --- |
| Slots | 31 | The command mask has 32 bits and command 0 is the bootstrap. 31 slots run on the board. |
| Pool use for 31 slots of add or relu | 6,291,456 bytes of 8 MiB, 32,768 bytes per slot | The runtime's own accounting (host run of the same catalogue); three groups of 2 MiB. |
| Ring depth | 64 in the default build, from the manifest at build time | `SUNSTONE_OS_NPU_QUEUE_DEPTH`. |
| Registered buffers | 125 of 128 | `A733_NPU_MEMORY_MAX_BUFFERS`. |
| Tensors per graph | 8 | The loader's own choice (`NBG_ASSEMBLY_MAX_TENSORS`), set at build. |
| Fault recovery | about 32 ms | Cancel poll 20 ms plus reset and re-initialization. |
| Per-job time | 5.45 µs (relu), 10.4 µs (add) | Clock conditions above. |

## Backlog

Items the coordinator or the work added after the finish line was fixed; they are not part of it.

1. Repeat the add queue image with the clock record, so that its figure is measured and not only granted.
2. Run larger graphs (the shufflenet size) under the queue with several slots, and several tensor links at once; the
   31-slot pool use shown above holds for graphs of 32 KiB per slot only.
3. A reservation that includes graphs loaded at run time uses the hardware bound of 512 KiB for each command allocation,
   because the stream length of a later graph is not known; the manifest could declare a command size to shrink it.
4. Larger command windows with the smaller doorbell word, and the repeated start and stop, are not run.
5. The `queue-2` image (entry `1793615803497721940-6794c04f`) ended in a host error of the rail helper after a normal
   return; it is not an NPU result and was not submitted again.
6. The mimus encoder on the NPU, on the mimus branch, after this plan.

## Larger catalogues and a sized pool

Eighteen compiled graphs (15.3 MB) run resident through the EL0 queue on the board (port 3-3, 852 MHz and 800 mV, Cortex-A55
at 1794 MHz): the pool of 22 MiB, placed below the monitor interval, holds one slot for each graph, sized from the graph; the
models come from a runner input region and the runtime checks their digests when it loads them. One pass over the 18 jobs takes
22.75 ms, every output equals the simulator's byte for byte. The loader admits unpatched pointer words below 1 MiB (rule 5 of
the loader specification). See [the resources design](../designs/npu-resources.md), "The pool" and "Models from outside the image".

## Long attention layers (2026-11-02)

The loader admits compiled conformer layers of 184 to 512 frames, which it refused before: the descriptor of the relative-shift
gather (S7: width carried into the height field, height cut at 32 bits), a matrix product whose second input lies in the pool,
the operation count (no fixed limit) and the shader stream length (no fixed limit); a command stream longer than one fetch is cut
into segments joined by LINK commands (`docs/hardware/npu-command-formats.md`, "Long tensors" and "Command segments"). The
layers are the proof graphs of the mimus encoder study (176 channels, one layer, own quantization), run through the OS NPU
service in queue mode on port 3-3 at 852 MHz and 800 mV, one slot, 100 jobs at queue depth 1, 2 and 4.

| Frames | Operations | Stream (bytes) | Layer (MB) | Pool of the slot (MB) | Compiler estimate (ms) | Board job (ms) | Output against the simulator |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 184 | 450 | 33,856 | 1.09 | 1.07 | 6.5 | 5.81 | 10,484 of 32,384 elements differ, at most 132 steps, rows 0 to 105 only |
| 276 | 643 | 42,464 | 1.34 | 2.22 | 11.5 | 35.6 | 15,786 of 48,576 elements differ by more than 8 steps, at most 305 |
| 368 | 1,167 | 78,184 | 1.81 | 3.77 | 23 | 63.3 | 10,814 of 64,768 elements differ by more than 8 steps, at most 438 |

The 184-frame layer's output is not the same in two runs: the unsplit stream (entry `e48806ba`), the same stream in three
segments (entry `a693df6c`) and a single job after loading (entry `d254a3f2`) each differ from the simulator in rows 0 to 105
only (10,484, 10,452 and 5,629 elements; at most 132, 98 and 30 steps of 1/128) and differ from one another in 10,000 to 12,000
elements, while rows 106 to 183 equal the simulator in every run. Against the float result of the same graph the board output
and the simulator output are equally far (RMS 1.116 and 1.116, the quantization of the proof graph; the two differ by 0.026
RMS). The segmented stream ran 300 jobs without a fault and its late rows, computed by commands after both links, are exact,
so the front end follows the links. The 92-frame layers, which have no pool traffic, were byte-exact in every run.

The same layers with the attention computed for blocks of 92 query rows (so that the scores stay in VIP SRAM) differ:

| Frames | Operations | Board job (ms) | Output |
| --- | --- | --- | --- |
| 184 | 320 | 4.83 | byte-exact against the simulator after 300 jobs |
| 276 | 502 | 8.00 | byte-exact after 300 jobs |
| 368 | 598 | 14.77 | three identical jobs differ from one another in 19,000 to 24,000 of 129,536 bytes |
| 460 | 5,482 | 47.8 | three identical jobs differ in 30,000 of 161,920 bytes |
| 512 | 3,719 | 45.3 | three identical jobs differ in 23,000 to 52,000 of 180,224 bytes |

The cause of the run-to-run differences (graphs of 184 frames with whole-length attention, and of 368 frames and more in any
form) is not known. Mapping the regions with 64 KiB pages (build parameter `A733_NPU_MAP_SHIFT`, not landed) was meant to test
whether address translation explains the slow whole-length layers; its image stopped in the OS start with a pool of 20 MiB and
was not examined further.

## Open work

1. Done: declared resources, run-time graph loading, shader integration, backpressure depth and tensor linking after the scheduler starts (see "What the NPU runs now"). What remains of this item is the backlog below.
2. Call the idle power owner from the OS NPU service (`npu_runtime_queue_power_start` is not called by the operating system image), then repeat the idle benchmark there. Measure energy and the break-even idle time: the PMIC provider exposes no DCDC2 current or power readback. Qualify shared SRAM access while the clocks are gated and recovery from a fault while suspended.
3. Qualify the combined image repeatedly and across both models: the active-cancellation image, and a 20-cycle start, inference, CLOSE and local-stop image.
4. Qualify the EL0 queue further (it now runs on the board with 31 slots, depth 64, backpressure and a run-time link, but only with the two small graphs add and relu): larger graphs under the queue, several links at once, the public-ring predicate in the power owner, and the saved generic sequence registration and returning diagnostic (`npu-el0-next-1101`).
5. Add exact per-job deadline interrupts through the scheduler's deadline facility (`work/scheduler-rt-1101`); do not add a device timer.
6. Measure energy: the PMIC provider exposes no DCDC2 current or power readback, so idle savings and break-even time have no evidence without an external meter.
7. Qualify repeated start and stop, shared SRAM access while the clocks are gated, and recovery from a fault while suspended.
8. Run the `restart-*` cases of `checks/npu-irq-route.py` on a restart image; the other cases pass.

## Evidence

Board results with a queue entry are records in `docs/evidence/npu.jsonl`, added with `tools/evidence.py add`. Results cited by path, and earlier prose evidence, are in [the historical log](../history/status-npu-20261101.md).

<!-- evidence-table:begin -->
47 records in `docs/evidence/npu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793558381244643310-6a29a71d` | 2026-11-01 | 3-3 | `dd826f5ff757` | returned | passed | run_total 110.813 s, matching_jobs 2, matching_completions 4 |
| `1793558909256910454-ab3a76b4` | 2026-11-01 | 3-3 | `8ede7a8341fa` | returned | passed | run_total 110.785 s, matching_jobs 3, matching_completions 4 |
| `1793559233825594424-693cc430` | 2026-11-01 | 3-3 | `a0dd10679c00` | returned | passed | run_total 110.751 s, matching_jobs 2, matching_completions 3 |
| `1793559627833816772-2003f2e7` | 2026-11-01 | 3-3 | `f9cc623ab7de` | returned | passed | run_total 110.927 s, matching_jobs 200, matching_completions 201 |
| `1793563838443635411-92c1fdae` | 2026-11-01 | 3-3 | `862114046425` | returned | passed | run_total 111.246 s |
| `1793564135238449408-5dbdd6ad` | 2026-11-01 | 3-3 | `2a6411ba3fcc` | returned | passed | run_total 112.541 s |
| `1793565141195481474-2e135a8c` | 2026-11-01 | 3-3 | `ae8dc27e6c16` | returned | passed | run_total 111.135 s |
| `1793572859635159128-187e5901` | 2026-11-01 | 3-3 | `7f18c978f34c` | watchdog-reset | failed | run_total 51.235 s |
| `1793573593531771376-a5c808a4` | 2026-11-01 | 3-3 | `a2e66e80dd5c` | refused-before-launch | inconclusive | run_total 0.594 s |
| `1793576493129223154-47e61f61` | 2026-11-01 | 3-3 | `c8b67e0dd9e4` | returned | passed | run_total 133.524 s, workload 5.001 s, cores_verified 8 cores, npu_jobs 0 jobs |
| `1793576635217632637-5879361e` | 2026-11-01 | 3-3 | `e574d622a946` | watchdog-reset | failed | run_total 71.723 s |
| `1793581334418340064-a4b4d83e` | 2026-11-02 | 3-3 | `ea38709362f5` | result-mismatch | failed | run_total 77.417 s |
| `1793587317962103239-5bbfc48d` | 2026-11-02 | 3-3 | `5669db271593` | returned | passed | run_total 126.154 s, window_jobs_per_s 26653.6 jobs/s, window_latency_p50 10.125 µs, window_latency_p99 11.04 µs, window_latency_max 17.04 µs, charged_cpu_per_job 34.5 µs, cold_job_p50_9ms_gap 10.58 µs, cold_job_p50_100ms_gap 10.58 µs, matching_jobs 61231 jobs, deadline_margin_at_end 12.69 s |
| `1793592163786755757-7c8b2818` | 2026-11-02 | 3-3 | `c0fb7bbadc21` | watchdog-reset | failed | run_total 26.192 s, first_failure_line 257, open_reply_state 1 |
| `1793592717914134756-b95efca0` | 2026-11-02 | 3-3 | `24f8f0f6a0b5` | returned | passed | run_total 31.329 s, npu_jobs 4, npu_output_mismatches 0, overlap_mask 15, cpu_cores_acknowledged 8, npu_trigger_to_irq_p50 2904 us, npu_trigger_to_irq_max 2911.96 us |
| `1793595222816645677-be2320f0` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 24.292 s, jobs 61231, window_jobs_per_s 26964.4 jobs/s, window_latency_p99_ticks 251, window_latency_max_ticks 429, charged_cpu_per_job_ticks 816.693, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595292307271613-0417379e` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 23.857 s, jobs 61231, window_jobs_per_s 26964.9 jobs/s, window_latency_p99_ticks 259, window_latency_max_ticks 426, charged_cpu_per_job_ticks 816.792, gates 130, resumes 130, register_writes 780, resume_max_ticks 183, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595393793090144-178725b8` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 24.043 s, jobs 61231, window_jobs_per_s 27005.8 jobs/s, window_latency_p99_ticks 258, window_latency_max_ticks 403, charged_cpu_per_job_ticks 816.552, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595461923184040-005689c1` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 24.102 s, jobs 61231, window_jobs_per_s 26962.4 jobs/s, window_latency_p99_ticks 256, window_latency_max_ticks 428, charged_cpu_per_job_ticks 816.586, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595557085395502-ba052514` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 24.02 s, jobs 61231, window_jobs_per_s 26965.4 jobs/s, window_latency_p99_ticks 252, window_latency_max_ticks 430, charged_cpu_per_job_ticks 816.488, gates 130, resumes 130, register_writes 780, resume_max_ticks 170, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595585257669811-c80ab9ea` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 24.095 s, jobs 61231, window_jobs_per_s 27011.6 jobs/s, window_latency_p99_ticks 251, window_latency_max_ticks 405, charged_cpu_per_job_ticks 816.401, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595613409995497-5db5d0ff` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 25.411 s, jobs 61231, window_jobs_per_s 26963.9 jobs/s, window_latency_p99_ticks 255, window_latency_max_ticks 443, charged_cpu_per_job_ticks 816.599, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595642848063892-cd5db0e8` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 24.007 s, jobs 61231, window_jobs_per_s 26965.3 jobs/s, window_latency_p99_ticks 250, window_latency_max_ticks 428, charged_cpu_per_job_ticks 816.608, gates 130, resumes 130, register_writes 780, resume_max_ticks 175, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595670424088724-d888a853` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 24.022 s, jobs 61231, window_jobs_per_s 27003.2 jobs/s, window_latency_p99_ticks 309, window_latency_max_ticks 403, charged_cpu_per_job_ticks 816.336, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595698599182458-0c1066a4` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 23.986 s, jobs 61231, window_jobs_per_s 26970.6 jobs/s, window_latency_p99_ticks 255, window_latency_max_ticks 428, charged_cpu_per_job_ticks 816.598, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595726557173335-e009f1c3` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 23.974 s, jobs 61231, window_jobs_per_s 26963.8 jobs/s, window_latency_p99_ticks 250, window_latency_max_ticks 428, charged_cpu_per_job_ticks 816.34, gates 130, resumes 130, register_writes 780, resume_max_ticks 165, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595754162008571-71c3a964` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 24.022 s, jobs 61231, window_jobs_per_s 27003.5 jobs/s, window_latency_p99_ticks 308, window_latency_max_ticks 407, charged_cpu_per_job_ticks 816.505, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595781813556905-b8d31af9` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 24.072 s, jobs 61231, window_jobs_per_s 26961 jobs/s, window_latency_p99_ticks 259, window_latency_max_ticks 425, charged_cpu_per_job_ticks 817.111, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595809517564687-116973a0` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 24.111 s, jobs 61231, window_jobs_per_s 26966 jobs/s, window_latency_p99_ticks 255, window_latency_max_ticks 426, charged_cpu_per_job_ticks 816.489, gates 130, resumes 130, register_writes 780, resume_max_ticks 169, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595837683091653-202bf23e` | 2026-11-02 | 3-3 | `4fd7815df3e0` | returned | passed | run_total 23.97 s, jobs 61231, window_jobs_per_s 27004.3 jobs/s, window_latency_p99_ticks 323, window_latency_max_ticks 407, charged_cpu_per_job_ticks 816.311, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595865267017264-80a35307` | 2026-11-02 | 3-3 | `1417c8183b75` | returned | passed | run_total 24.23 s, jobs 61231, window_jobs_per_s 26964.4 jobs/s, window_latency_p99_ticks 257, window_latency_max_ticks 432, charged_cpu_per_job_ticks 816.772, gates 0, resumes 0, register_writes 0, resume_max_ticks 0, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793595893101543391-d845400a` | 2026-11-02 | 3-3 | `b3e4795f525c` | returned | passed | run_total 24.01 s, jobs 61231, window_jobs_per_s 26967 jobs/s, window_latency_p99_ticks 294, window_latency_max_ticks 429, charged_cpu_per_job_ticks 816.553, gates 130, resumes 130, register_writes 780, resume_max_ticks 169, resume_limit_ticks 2400, value_deviation_cycles 0, deviation_mask 0 |
| `1793599212547775355-868652d5` | 2026-11-02 | 3-1 | `835daa9f532d` | result-mismatch | failed | run_total 23.118 s, graphs_listed 15, graphs_run 1, first_difference_byte 2, observed_byte 60, expected_byte 59, first_graph_us 998.8 |
| `1793599501583289076-4e5e8892` | 2026-11-02 | 3-1 | `83ef6b2a55fb` | result-mismatch | failed | run_total 22.181 s, graphs 8, graphs_matching 3, max_error_16bit 17 |
| `1793599785143842348-e58c3f38` | 2026-11-02 | 3-1 | `653e167425fb` | result-mismatch | passed | run_total 21.663 s, graphs 8, graphs_matching 8, differing_bytes 0, graph_us_min 984.3 us, graph_us_max 999.5 us |
| `1793599845892192862-30dc9323` | 2026-11-02 | 3-1 | `7a00716d1fd9` | returned | passed | run_total 21.941 s, graphs 7, graphs_matching 7, differing_bytes 0, graph_us_min 984.9 us, graph_us_max 999.2 us |
| `1793600480786110831-4d4877ee` | 2026-11-02 | 3-1 | `bd9603642b7c` | watchdog-reset | failed | run_total 18.745 s |
| `1793601039576662539-3cfb241d` | 2026-11-02 | 3-1 | `dbc06474fcbc` | watchdog-reset | inconclusive | run_total 18.69 s |
| `1793601982806302701-49b951b4` | 2026-11-02 | 3-1 | `69a7aa80419f` | returned | passed | run_total 21.817 s |
| `1793602347321241109-498efad0` | 2026-11-02 | 3-1 | `cfb505b770a8` | returned | passed | run_total 21.199 s |
| `1793605718512399089-e43229a0` | 2026-11-02 | 3-1 | `808f31915bb4` | returned | passed | run_total 65.724 s |
| `1793610943103960873-cbaaa9b6` | 2026-11-02 | 3-1 | `f6d56b1c7141` | host-error | failed | run_total 46.685 s |
| `1793615676480712051-ab430778` | 2026-11-02 | 3-1 | `f7dde3731608` | returned | passed | run_total 45.4 s, jobs_per_k 1000, us_per_job_k1_to_64 10.4 us, jobs_per_s 96300, refusals_k_le_64 0, pool_bytes_31_slots 6.29146e+06, bytes_per_slot 32768 |
| `1793615803497721940-6794c04f` | 2026-11-02 | 3-1 | `a26fd9e4dfc9` | host-error | inconclusive | run_total 112.832 s |
| `1793616163904565650-da3f264f` | 2026-11-02 | 3-3 | `9117aaf6a95f` | returned | passed | run_total 88.57 s, jobs_per_k 1000, us_per_job_k2_to_128 5.45 us, jobs_per_s 183400, refusals_k_le_64 0, refusals_k128 38255, linked_pair 11.4 us |
| `1793616402162300846-13133d4b` | 2026-11-02 | 3-1 | `2c64a0a714ea` | returned | passed | run_total 21.996 s |
| `1793616938769293285-42647824` | 2026-11-02 | 3-1 | `7d5acbff80f3` | returned | passed | run_total 24.225 s, jobs_per_k 1000, us_per_job_k2_to_128 5.45 us, jobs_per_s 183400, refusals_k_le_64 0, refusals_k128 38258 |
<!-- evidence-table:end -->
