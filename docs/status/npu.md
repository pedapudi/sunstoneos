# NPU

**Status: implemented.**

Native model decoding, mappings, shared-provider startup,
IRQ97 service, runtime clients and thermal policy have software evidence.
Finite scheduled startup and inference have scoped physical results.
Repeated-transition reliability remains unqualified.


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
portable transition checks, native owner cases, shared-timer wiring and idle
benchmark are integrated and host-tested. They have no physical gating result.
The qualified resident and dependent-buffer paths remain the scope described
below; their board evidence does not qualify the new idle policy.

1. **Gating image: board comparison.** The three independent code reviews
   approved with required changes, which are made. The A733 gating protocol is a
   part descriptor (`include/a733_npu_idle.h`) that the state machine reads. The
   owner treats a busy interrupt controller as "not now" and rearms, charges no
   wake to completions, and measures the quiet state again after a resume.
   `npu_runtime_queue_submit()` admits and submits in one lock span. The native
   owner cases (`checks/npu-queue-handoff.py`) cover start refusals, gating and
   resume on admission, the control policy, stale alarms, a busy and an invalid
   interrupt controller, partial failure between suspend writes, suspended close
   with and without a resume failure, cancellation by STOP, an interrupt while
   suspended and after resume, admit-and-submit in one lock span and over-budget
   resumes, with five relink-based regression mutations. Three benchmark images
   (`build.py --latency queue --idle none|off|gate`: baseline without an owner,
   control, gating) each run five windows of 12,000 jobs and 100 cold jobs after
   each of 9, 11 and 100 ms gaps. Their report decoder and the three-way limit
   comparison (`apps/npu-finite/idle-compare.py`) are checked by
   `checks/npu-idle-report.py` with synthetic reports; the producer itself runs
   only on the board. Run the baseline, the control and then the gating image on
   port 3-3 after the boards are replugged. Require, on medians of the five
   windows, that the control stays within 98% throughput, 105% p99, 110% maximum
   and 105% charged CPU time of the baseline, and the gating image within the
   same limits of the control, with every resume within 100 µs and no gating in 9
   ms gaps. Cold-job latency is reported separately. The PMIC provider exposes no
   DCDC2 current or power readback, so there is no energy evidence unless an
   external meter is added.
2. **Qualify EL0 and prepared joined streams.** Review and finish the saved
   generic sequence-registration API and returning OS diagnostic, and the public
   ring predicate that tells the power owner a published successor exists (add
   it with a kick when the ring drains or a bounded alarm while it is false). Build
   and verify the RAM backup, drain and interrupt restoration before physical
   use. Run the returning OS control first, then the actual EL0 queue and joined
   stream through the board queue. Physical EL0 execution remains unqualified.
3. **Add exact per-job deadline interrupts through the scheduler facility.**
   The landed `sunstone_deadline_wait_until()` facility supports task waits.
   It does not provide asynchronous device-alarm arm/cancel operations from
   submission and completion. Coordinate that extension with
   `work/scheduler-rt-1101`, then connect queue deadlines and test stale alarms
   and completion races. Do not create an independent device timer. This item
   is open.

The EL0 joined registration and diagnostic remain a separate incomplete
implementation, preserved without integration:

| Work | Branch and commit | Missing work |
| --- | --- | --- |
| EL0 joined registration and diagnostic | `npu-el0-next-1101`, `088d3144` | Review, complete native builds, power bridge validation and physical control/EL0 runs |

Its worktree is `el0-next/` under `~/.cache/sunstoneos/build/npu-fastpath-1101/`.
The commit requires selective integration: its prerequisite idle commits already
exist on the continuation branch with different hashes. Preserved EL0 host
artifacts are in `el0-next-evidence/` and `el0-ring-evidence/` under the same
build directory. The full baseline builds and integration gate must be refreshed
after the EL0 changes are integrated.

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
The idle benchmark composes the committed finite `deadline.c`, `monitor.c` and
`runtime-irq.c` wiring with `queue-client.c`, `idle-client.h` and `startup.h`.

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
Failure reporting retains the job index and observed operand at the original
predicate before generic retention. Fifteen native fault cases and decoder
checks cover the measurement formats, explicit high-word records and legacy
stages in `qualification-1101/sustained-fault-validation.json`. These host cases
do not qualify physical fault recovery.

The eight-core concurrency diagnostic is implemented and has correctness,
hardware-safety and recovery review approval. Its host checks cover checked
float32 matrix products, four jobs with hardware-busy overlap receipts,
worker parking, local stop, all-core arrival and retirement, the eight MiB NPU
pool clean, RAM restoration and the approved post-ROM rail helper. Modeled
register, interrupt and scheduler effects establish no physical concurrency.
The three baseline builds and selected integration gate pass.

The approved CPU-only control precedes the combined physical run. Both await
deployment of the reviewed runner support. That runner verifies both CPU rails
at 800 mV before launch. For NPU images it reads the controller clock and fabric
gates before checking that PCK4 policy and status are already ON. It refuses
OFF or uncertain state without enabling a gate or domain. Normal return and
restored RAM precede the DCDC5-only post-ROM helper; faults prohibit it.
NPU stays at 852 MHz/800 mV, PCK4 stays ON and DCDC2 is untouched.

The approved proposal is under `qualification-1101/` in the build root.
`concurrency-1101/review/`, `final-checks/`, `final-native/`, `templates-final/`
and `queue-inputs/` contain the review, checks, native artifacts, session
provenance and image-bound submission inputs. The merged executable source is
`03137448`; its combined ELF SHA-256 is
`686f9c355e0997c2d78e89bf051128f7c974631fd282079de5d4cc7634ae00e9`.

Active-job cancellation is implemented and independently reviewed. A finite
WAIT stream supplies an active command; cancellation requires its acknowledgment
and full idle before controller close, interrupt retirement and rearm, bootstrap
and a matching real inference. Recovery retains the allocations and changes
neither PCK4, local CCU words nor DCDC2. Host cases cover the real client,
worker, interrupt callbacks, cleanup and failure records in `cancel-1101/`.
Its physical run follows both concurrency controls and remains unqualified.

The SDK discovers model names, tensor counts and byte sizes from its image's
immutable catalogue. A caller resolves a name to a local index; the SDK assumes
neither a specific model list nor a fixed number of models. The data-driven
generator supports a selected model or every entry in a supplied list. Host
cases cover one, two and three reordered models, check metadata against decoded
model tensors and refuse invalid indices or tensor sizes before device access.
The runtime admits up to sixteen models and eight catalogue clients; the
default OS profile has one client slot and links its selected model. Direct
FIFO USB and shader-graph loading remain incomplete. Configured eight NN cores and zero TP/TP-lite cores
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


## Land the merged runner before the CPU and NPU controls

The branch is `work/npu-wdt-1031`. Merge `03137448` includes `bringup`
`35ee1836`. It preserves resident NPU queues, the EL0 submission ring, GPU fast
paths, USB admission and the latency profile alongside concurrency and active
cancellation. No concurrency or cancellation board entry is submitted.
The passing queue entries above remain the physical evidence.

Open work, in priority order:

1. **Land and deploy.** The coordinator lands this branch and restarts the
   queue daemon with its runner support. Keep current USB source-off and probe
   admission, plus NPU ON-only admission, RAM preservation, image-bound decoding
   and the approved rail-helper ordering. The old standalone
   `concurrency-1101/runner-support.patch` is superseded by the merged branch.
2. **Run the CPU-only control, then concurrency on 3-3.** Use the exact commands
   in the image READMEs below. Before each run, confirm DCDC3 and DCDC5 at
   800 mV through the runner. Stop at the first failure. The combined run checks
   four NPU outputs and proves overlap with matrix multiplication on all eight
   CPU cores. Record each queue entry and result before continuing.
3. **Qualify active-job cancellation.** After both controls pass, build the
   cancellation board package from the integrated source and rerun its focused
   cases. Test bounded WAIT-stream cancellation followed by matching inference.
   `cancel-1101/producer-host-4/image.elf` remains a host fixture, not a board
   package. Cancellation and domain OFF have no physical qualification.

Both rebuilt, unrun images are under
`~/.cache/sunstoneos/build/npu-wdt-1031/land-35ee1836/`:

| Image | ELF SHA-256 | README relative to that directory |
| --- | --- | --- |
| CPU-only control | `7f18c978f34cd3105bfdeb85aa08ac83fd907d996a1827ae60233b16216060a8` | `images/control/README.md` |
| CPU and NPU concurrency | `686f9c355e0997c2d78e89bf051128f7c974631fd282079de5d4cc7634ae00e9` | `images/combined/README.md` |

Each package has its ELF, complete receipt expectation, image-bound decoder,
session template and exact submission command. `images/commands.json` lists
both commands in order. The frozen host source is `host-03137448/`; use it
with these images. The older `concurrency-1101/queue-inputs/commands.json`
and its image hashes are superseded.

Approved points are CPU-L 1794 MHz/1000 mV on DCDC5, CPU-B 2002 MHz/1050 mV
on DCDC3 and DSU 1274 MHz. CPU-B lowers to 1014 MHz before ROM. NPU stays
at 852 MHz/800 mV, PCK4 stays ON and DCDC2 stays untouched. The post-ROM
DCDC5-only helper runs only after verified normal return and RAM restoration;
faults or uncertain return prohibit it.

The coordinator approves the control, concurrency and subsequent cancellation
runs within those bounds. The three earlier independent reviews cover source
`9dff0667`; no new reviews are requested for this bounded merge. Landing and
runner deployment remain with the coordinator. Any NPU domain OFF, clock
gating or new voltage/clock point needs a separate proposal and approval.

Merged-tree checks are under `land-35ee1836/` in the build root.
`final-board-runner.log`, `final-usb-disconnect.log` and
`final-usb-discover.log` pass. `final-concurrency.log`, `final-owners.log`,
`final-geometry.log`, `final-terminal.log` and `final-chain.log` pass.
The linked chain includes client completion, worker and interrupt retirement,
all-core arrival, the 8 MiB pool clean, RAM restoration and the permitted
post-ROM helper. SDK event, failure-record and copy-guard checks also pass.
Full gate evidence is `gate-ready.log`, label `npu-land-35ee1836-ready`.

Shared files require coordination with CPU, NPU fast-path and runner workers:
`tools/board.py`, `tools/board_images.py`, `tools/a733-crumbs.py`,
`include/sunstone_crumb.h`, `include/sunstone_app.h`, `runtime/npu_app.c`,
`CMakeLists.txt`, `cmake/sunstone-os-core.cmake`, `apps/os/npu*`,
`apps/os/cpu-return*`, `apps/os/return-fail-harness.h`, `apps/os/main.c`,
`apps/os/task.c`, `platform/a733-smp-supervised/launch.c` and `drivers/npu/*`.
Preserve other workers' interfaces when resolving these files.

Auxiliary scratch branches remain separate: `npu-cancel-1101` at `38841b70`
and `work/npu-control-merge-1101` at `4a06665a`. Those WIP commits are
excluded from this branch and must not be landed.
