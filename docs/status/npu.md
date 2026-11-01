# NPU

**Status: implemented.**

Native model decoding, mappings, shared-provider startup,
IRQ97 service, runtime clients and thermal policy have software evidence.
Finite scheduled startup and inference have scoped physical results.
Repeated lifecycle operation remains unqualified.


**Board-qualified scope:** A finite interrupt diagnostic produces matching
ShuffleNetV2 outputs at 852 MHz/800 mV. Startup takes 4.0 ms and inference takes
2.91 ms doorbell-to-interrupt, about 3.3 ms end to end. Provider-free
entry, disabled-IRQ installation/restoration and real provider/task construction
before scheduler start return normally with saved RAM restored. These
checkpoints perform no inference. The watchdog-admitting finite startup,
one-inference and two-job/two-model runtime images also qualify on 3-3 at
852 MHz/800 mV, with matching copied outputs, CLOSE, local stop and normal
ROM return. The resident command queue runs 1,000 matched jobs at 10.25 µs
p50 and 26,925 jobs/s, and 1,000 fence-chained two-stage jobs, on 3-3.

## Continuation priorities and saved work

The continuation branch is `work/npu-fastpath-1101`. Its internal-clock owner,
portable transition checks and finite shared-timer wiring are committed through
`11e50073`. These additions are incomplete and have no physical gating result.
The qualified resident and dependent-buffer paths remain the scope described
below; their board evidence does not qualify the new idle policy.

1. **Complete internal-clock gating validation.** Link `npu_idle.c` into every
   NPU queue build and finish the finite shared-timer composition. Reconcile
   `docs/designs/npu-fast-path.md` with the approved policy and suspended-register
   rules below. Finish the native owner cases
   for cancellation, stale timers, pending GIC interrupts, partial failure,
   suspended close and published EL0 successors. The portable transition fixture
   passes eight groups with delayed MMIO and restricted suspended reads;
   evidence is `gate-correctness-check-03/`, with fault-origin follow-ups in
   `gate-correctness-check-04/` and `gate-correctness-check-05/`. Obtain the same
   three independent reviewers' code approval for simplicity, performance and
   correctness; their design approvals alone do not authorize the board step.
   Complete the finite benchmark decoder and shared-timer build composition,
   then run the always-on control before the gating comparison on port 3-3.
   Require throughput at least 98% of control, p99 within 105%, maximum within
   110%, CPU time within 105%, resume within 100 µs, and no gating in 9 ms gaps.
   Charge the owner task's CPU time and report cold-job latency separately.
2. **Qualify EL0 and prepared joined streams.** Review and finish the saved
   generic sequence-registration API and returning OS diagnostic. Build and
   verify its RAM backup, drain and interrupt restoration before physical use.
   Run the returning OS control first, then the actual EL0 queue and joined
   stream through the board queue. Physical EL0 execution remains unqualified.
3. **Add exact per-job deadline interrupts through the scheduler facility.**
   The landed `sunstone_deadline_wait_until()` facility supports task waits.
   It does not provide asynchronous device-alarm arm/cancel operations from
   submission and completion. Coordinate that extension with
   `work/scheduler-rt-1101`, then connect queue deadlines and test stale alarms
   and completion races. Do not create an independent device timer.

Incomplete implementation is preserved in separate commits, without integration
into the continuation branch:

| Work | Branch and commit | Missing work |
| --- | --- | --- |
| EL0 joined registration and diagnostic | `npu-el0-next-1101`, `088d3144` | Review, complete native builds, power bridge validation and physical control/EL0 runs |
| Idle comparison workload | `npu-idle-bench`, `a9931f03` | Host decoder, timer composition, build, review and board comparison |
| Native owner fixture support | `work/npu-idle-1101`, `267b8e94` | Actual owner race cases and their execution |

Their worktrees are `el0-next/`, `idle-bench/` and `idle-impl/` under
`~/.cache/sunstoneos/build/npu-fastpath-1101/`. The commits require selective
integration: their prerequisite idle commits already exist on the continuation
branch with different hashes. Preserved EL0 host artifacts are in
`el0-next-evidence/` and `el0-ring-evidence/` under the same build directory.
The full baseline builds and integration gate must be refreshed after these
unfinished changes are integrated; the earlier gate evidence below covers the
resident submission implementation.

**Built images awaiting physical execution:** no new internal-gating or physical
EL0 diagnostic image is built or submitted. Build-only OS artifacts remain
unrun. Their README paths, relative to the task build directory, are:

- `final-gates/gate-os-sup-npu-fastpath-1101-d2e4321fdb/os-image/README.md`
- `final-gates/gate-os-res-npu-fastpath-1101-d2e4321fdb/os-image/README.md`
- `final-gates/gate-os-resaud-npu-fastpath-1101-d2e4321fdb/os-image/README.md`
- `final-gates/gate-os-npu-npu-fastpath-1101-d2e4321fdb/os-image/README.md`
- `final-gates/gate-os-npu-queue-npu-fastpath-1101-d2e4321fdb/os-image/README.md`

These build artifacts are not frozen queue submissions. Build board diagnostics
only when the remaining review and host prerequisites permit immediate queuing.

**Approvals and register policy:** the coordinator approves implementation and
conditional measurement of internal VIPLite clock gating only. PCK4, DCDC2,
PLL, voltage and top-point CCU clocks retain their existing ownership and state.
Any additional CPU/DSU or rail transition required by a returning OS image needs
the coordinator's applicable session grant. While suspended, permit only the
vendor resume-entry read at NPU offset `+0x000`. After ungating, wait the vendor's
1 µs settling interval and verify the gate clears before reading `+0x10c`.
Ordinary idle/event reads are prohibited while suspended. Partial transition
failure retains ownership and its first fault inputs.

**Shared files:** coordinate lifecycle changes with the NPU diagnosis worker in
`drivers/npu/npu_runtime_queue.c`, `drivers/npu/npu_service.c`,
`include/npu_runtime_queue.h` and `platform/a733/npu_irq.c`. Coordinate SDK
consumers through `include/sunstone_npu_queue.h`, `runtime/npu_app_queue.c` and
`apps/os/npu-queue.c`; the API remains generic. The scheduler worker owns the
shared deadline facility in `include/sunstone_deadline.h` and `runtime/deadline*`.
The saved benchmark changes overlap `apps/npu-finite/build.py`,
`queue-client.c` and `startup.h`; integrate them with the committed finite
`deadline.c`, `monitor.c` and `runtime-irq.c` wiring.

## Resident command submission

**Implemented; matched small-model timing has physical evidence.** The generic
queue accepts caller-owned records and ordered fences, submits from the caller,
and notifies its waiting task directly from IRQ97. Commands and buffers remain
resident. Dirty ranges receive cache maintenance outside the device lock.
Acknowledged worker handoff and permanent queue retirement prevent stale triggers.
The legacy mailbox wakes its owner on publication and reuses READY models.

Port 3-3 control `1793563838443635411-92c1fdae` and resident comparison
`1793564135238449408-5dbdd6ad` each check 1,000 measured model-0 outputs, with
identical linked models and inputs at 852 MHz/800 mV. Both return normally.
Helper versus resident p50/p99/maximum latency is 463.88/472.46/478.92 µs versus
10.25/12.83/18.71 µs. Throughput is 2,029.97 versus 26,925.33 jobs/s.
Scheduled client-plus-worker CPU time is 491.16 versus 34.19 µs/job, including
output checks and digest. This excludes IRQ time charged to idle. Mean
API-entry-to-completed-doorbell delay is 403.44 versus 0.943 µs.
[The design](../designs/npu-fast-path.md) records hashes, timing scope and vendor references.

Port 3-3 entry `1793565141195481474-2e135a8c` returns normally after 1,000
two-record FIFO chains and 1,000 joined streams. Two distinct binaries share a
400-byte output/input allocation and every intermediate and final output matches.
FIFO and joined throughput is 45,987.32 and 48,166.66 logical stages/s, including
output checks and digest. Scheduled task CPU cost is 19.87 and 18.70 µs/stage.
Each chain has two stages; its mean interval excluding those checks is 13.94
and 12.02 µs. Both forms perform zero interstage CPU copies. The joined stream
emits one final event and no intermediate event. The complete image retires
4,001 hardware jobs and 4,002 IRQs, with restored DRAM.

The EL0 application-kit ring has resident handles, image-declared tensor edges,
connected buffer leases and an SVC doorbell only on idle-to-active publication.
IRQ refill handles each successor without a service task hop. Eight sanitizer
cases and twelve native SVC/IRQ cases pass; physical EL0 execution remains
unqualified. The selected integration gate passes the supervised, resident,
resident-audio, normal NPU and two-model EL0-ring builds, kernel checks, check
index and shared-helper checks.
Host evidence under `~/.cache/sunstoneos/build/npu-fastpath-1101/` also covers
10,000 ordered records, native handoff faults, tensor relocation, SDK events
and diagnostic producer/parser/receipt agreement. The final selected gate is
`final-gates/gate-npu-final/`, with both affected NPU builds refreshed in
`final-gates/gate-npu-final-layout/`. Native SVC evidence is `svc-final-layout/`
and diagnostic byte-equivalence evidence is `timing-layout-check/` under the
same task build directory. Disabled timing counters occupy no production storage.

Automatic internal-clock gating has an opt-in implementation and portable host
evidence; its native owner integration and board qualification remain open.
Current qualified jobs keep the NPU powered at 852 MHz/800 mV. There is no
physical energy evidence: inspected cached PMIC references expose no documented
DCDC2 current/power channel or conversion, and no host-readable meter is
identified. Voltage-selector readback is not an energy measurement. The source
audit is `pmic-energy/audit.json` under the task build directory. The coordinator
permits the conditional gating comparison without energy evidence. Exact
per-job deadline interrupts remain open; the independent session watchdog
bounds a lost IRQ. Repeated lifecycle and thermal/fault recovery also remain
unqualified.

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
