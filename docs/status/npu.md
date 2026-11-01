# NPU

**Status: implemented.**

Native model decoding, mappings, shared-provider startup,
IRQ97 service, runtime clients and thermal policy have software evidence.
Finite scheduled startup and inference have scoped physical results.
Repeated lifecycle operation remains unqualified.


## Resident command submission

**Implemented; physically unqualified.** The generic command queue submits in
its caller's context, uses caller-owned job records and ordered timeline fences,
and directly notifies its waiting task from IRQ97. Several records may be
pending. Prepared sequences retain device synchronization between command
streams and emit one final event. The queue handles commands and fences without
interpreting a model or application partition.

The native adapter obtains an acknowledged worker handoff before accessing the
engine. Returning ownership retires the queue's admission; stale handles cannot
trigger hardware. Commands and buffers remain resident, with CPU cache work on
the caller's dirty tensor ranges. The legacy application mailbox also wakes its
owner on publication and reuses a READY model. These software changes establish
no physical latency improvement.

Host evidence under `~/.cache/sunstoneos/build/npu-fastpath-1101/` covers ordered
queues of 10,000 records, failure retention, sequence encoding, native handoff
interleavings and regression mutations, SDK notifications, and measurement
producer/parser/return-receipt agreement. The OS, hello and NPU baseline builds
pass. The integration gate's selected builds, check index and affected pure-host
checks pass; its broader inferred coverage remains incomplete because indexed
checks require additional images, captures or physical inputs.

The finite diagnostic selects `--latency legacy` or `--latency queue`, checks
one warmup and 1,000 measured jobs, and records every latency sample plus p50,
p99, maximum, throughput and scheduled client/worker task time. An optional
`--sequence 32` measures a resident repeated-command sequence. It uses the same
input and output windows repeatedly; it does not establish dataflow between
different network binaries. Task-time counters exclude idle-task time and IRQ
execution charged to the idle task; IRQ driver elapsed time is reported separately.

The legacy control is built and admitted offline. Its ELF is
`~/.cache/sunstoneos/build/npu-fastpath-1101/images/legacy/image.elf`, SHA-256
`743b3bd492b4c69a4859cac2a2d6a3d2b7c580b375a1c4e19a16369617f9908b`.
Port 3-3 submissions require the coordinator to integrate runner commit
`1cdb6e2485421979f0aa4d8e7ff59053157449b6`, which admits the typed finite latency
profile while retaining the existing watchdog, image, session and receipt checks.
No new physical measurement or queue entry exists for this path.

Approved measurements retain 852 MHz and 800 mV. Automatic idle clock gating is
a proposal, with transition budgets in [the design](../designs/npu-fast-path.md);
its physical run needs coordinator approval and an identified power meter.
The direct adapter currently supports a trusted EL1 owner on the IRQ route core.
Exact per-job deadline interrupts, EL0 SDK access to this queue, physical
multiple-in-flight evidence and buffer-dependent sequences remain open.


**Board-qualified scope:** A finite interrupt diagnostic produces matching
ShuffleNetV2 outputs at 852 MHz/800 mV. Startup takes 4.0 ms and inference takes
2.91 ms doorbell-to-interrupt, about 3.3 ms end to end. Provider-free
entry, disabled-IRQ installation/restoration and real provider/task construction
before scheduler start return normally with saved RAM restored. These
checkpoints perform no inference. The watchdog-admitting finite startup,
one-inference and two-job/two-model runtime images also qualify on 3-3 at
852 MHz/800 mV, with matching copied outputs, CLOSE, local stop and normal
ROM return.

Evidence paths are relative to `~/.cache/sunstoneos/evidence/`:

- `board-session-20261025-15/port-3-3/` records finite interrupt inference.
- `sol-resume-20261028/port-3-3/npu-entry-mbus-checkpoint/` and
  `npu-entry-irq-checkpoint/` under that same port directory record provider-free
  entry and disabled-IRQ restoration.
- `sol-resume-20261031/port-3-3/npu-provider-construction/` records real queues,
  tasks and IRQ binding before scheduling.
- `sol-npu-cold-gates-1029/` and `sol-npu-restart-host-1028/` contain software
  checks for access gates and ordered close/reopen capture.

SRAM routing is required: startup clears SYSCTRL `0x03000008` bit 1 while
NPU reset is held and restores it at local stop. The failed numerical image in
`processor-completion-20261024/port-3-1/npu-numerical/runtime/session.json`
omits that routing. The corrected image produces both matching jobs in
`board-session-20261025-3/port-3-3/`. Two SDK input patterns for the published ShuffleNetV2 graph both produce
`[0,255]`; those results do not establish input-sensitive classification or
class meaning.

Current finite-operation evidence is under
`~/.cache/sunstoneos/build/npu-wdt-1031/runs/`:

- `01-startup-only-28c8e15a2b62-834973ec/` records READY_CLOSED_STOPPED with no job.
- `02-single-inference-0dd327616ce6-834973ec/` records one matching inference.
- `03-runtime-8f51a95699b7-834973ec/` records two matching jobs/models using ISR
  notification for completion.

The images include post-bootstrap settling and native startup-register
breadcrumbs. [The inference comparison](../../apps/npu-finite/INFERENCE-SEQUENCE.md)
states the vendor sequence and [startup failure decoding](../../apps/npu-finite/STARTUP-FAILURE.md)
retains the original setting check and inputs. Three independent review roles
cover correctness, hardware safety and recovery; review/build evidence is in
`land-review-*/`, `land-checks/` and `land-baselines/` under the same build root.
These finite images use the established 852 MHz/800 mV point and do not write
PMIC rails.

The host output check `checks/npu-source-output.py` passes in the mandated
virtual environment, including its NumPy calculations and saved-SDK output
handling. SDK execution is modeled; this check adds no physical inference
qualification. Its log is
`~/.cache/sunstoneos/build/check-selection-1031/followup-3/gate-links-full/indexed-npu-source-output.log`.

Bounded local stop/restart, model switching and finite-task SDK access return
normally with matching outputs and restored DRAM on port 3-3. The queue evidence
is under `~/.cache/sunstoneos/board-queue/entries/`:

| Operation | Matching jobs / interrupt completions | Entry |
| --- | --- | --- |
| Current local stop/restart control | 2 / 4 | `1793558381244643310-6a29a71d` |
| conv12, conv8, then conv12 | 3 / 4 | `1793558909256910454-ab3a76b4` |
| Finite application SDK | 2 / 3 | `1793559233825594424-693cc430` |
| Alternating conv12/conv8 batch | 200 / 201 | `1793559627833816772-2003f2e7` |

These bounded workloads do not establish repeated-transition reliability.
Active-job reset recovery, confinement, thermal protection and full OS EL0 SDK
mappings remain unqualified. The queued runner validates native interrupt
receipts and restores saved DRAM after verified retirement.

The current local-restart control uses merged source `32f18fe5` and the runner
at `6ebe687a`. Its ELF is
`~/.cache/sunstoneos/build/npu-wdt-1031/control-1101/images/local-restart/image.elf`,
SHA-256 `dd826f5ff75795b6beb2b96b352e28439574156fc39c222cd00e338d70e8eac3`.
The returned report has status zero, matching outputs, stopped interrupt service,
normal local restart, normal ROM return and verified DRAM restoration. It has
no first failure or quarantine.

The current model-switch image checks conv12, conv8 and conv12 outputs in one
runtime lifetime. Entry `1793558909256910454-ab3a76b4` has status zero, four
interrupt completions, normal return and verified DRAM restoration. Its ELF is
`~/.cache/sunstoneos/build/npu-wdt-1031/qualification-1101/images/model-switch/image.elf`,
SHA-256 `8ede7a8341fa28abe93f717d3d410b499333eca3e32aeafd83fa7bafe0c846d7`.

The finite SDK client runs both models through `ss_npu_run` and closes through
`ss_npu_close`. Entry `1793559233825594424-693cc430` has two matching outputs,
three interrupt completions, status zero, normal return and restored DRAM.
Its ELF is
`~/.cache/sunstoneos/build/npu-wdt-1031/qualification-1101/images/sdk-access/image.elf`,
SHA-256 `a0dd10679c00829a9a2f16357cdae57064ce6a063fdeac714f1273f235311c32`.
This client uses an EL1 application task; OS EL0 SDK mappings remain unqualified.

The sustained diagnostic checks every copied output across 200 consecutive
jobs, 100 per model, in one runtime lifetime. Entry
`1793559627833816772-2003f2e7` reports zero mismatches, 201 interrupt completions,
normal return and restored DRAM. The batch takes 0.138715625 seconds:
1,441.8 jobs/second including model loading, copies, comparisons and hashing.
Maximum `npu_job_run` API latency is 2.100042 ms. This is a bounded batch;
long-duration and thermal reliability remain unqualified.
The ELF is
`~/.cache/sunstoneos/build/npu-wdt-1031/qualification-1101/images/sustained/image.elf`,
SHA-256 `f9cc623ab7debeb388d9e9ea0be05f64424623ea5e8ef03525afe43b3798478f`.
Its frozen host parser verifies 200 jobs, every ordered output digest, timing
windows, the measured maximum and the actual interrupt receipts. Host evidence
covers 37 producer/parser, corruption, count, marker and timeout cases in
`qualification-1101/sustained-host-7/` under the same build root.

The next concurrency diagnostic requires additional approval for the existing
eight-core CPU startup, vendor top CPU/DSU points and verified post-ROM rail
normalization. The NPU-only authorization keeps PCK4 ON and DCDC2 untouched;
it does not authorize those CPU power transitions. The written proposal and
three-role design reviews are under `qualification-1101/` in the build root.
The active-job cancellation design is prepared there; its physical run follows
the concurrency stage.

The runtime models up to eight catalogue clients and model switching. The board
image has one slot and five catalogue models; direct FIFO USB and shader-graph
loading remain incomplete. Configured eight NN cores and zero TP/TP-lite cores
are target records, not a physical identity measurement.

**Physical NPU domain OFF is unsupported.** The selected vendor configuration
keeps runtime domain power ON. DCDC2 is shared with SoC system and DRAM logic;
ROM recovery uses shared SRAM that requires NPU power. The request API and
power owner refuse physical OFF before changing hardware or ownership. The
runner refuses the physical-OFF image even with its old frozen parser.
The [power audit](../npu.md#the-supported-npu-power-boundary-preserves-shared-sram)
records the failed run, register writes, schematic sources and recovery limits.

Correctness, hardware-safety and recovery panels approve the conservative
shared-power refusal; evidence is under
`~/.cache/sunstoneos/build/npu-wdt-1031/power-fault/`. Host SDK ownership,
interleaving, report-capacity and failure-retention evidence remains in
`next-checks/`, `next-sdk-api/`, `next-abi/` and `next-frozen-preparation/`
under the same build root.

Remaining work includes repeated startup, broader graph/operator and numerical
coverage, model switching under load, OS EL0 SDK access, mimus integration, concurrent
CPU/GPU operation and safe fault recovery. See [startup](../npu-startup.md),
[service](../npu-service.md), [SRAM routing](../designs/npu-sram-route.md),
[runtime breadth](../designs/npu-breadth.md), [board image](../designs/npu-board.md)
and [conformance](../npu-conformance.md).
