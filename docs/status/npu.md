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
provenance and image-bound submission inputs. The current checked source is
`9dff0667`; its combined ELF SHA-256 is
`2d8551bc4baa599f031e46a3f1a76e97c84cda03e3716c0474df4b2dcc86e680`.

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


## Continuation requires runner integration before board testing

The branch is `work/npu-wdt-1031`. Reviewed executable source is `9dff0667`;
subsequent commits update this status only. No concurrency or cancellation
board entry is submitted. The four passing queue entries above remain the
physical evidence; host checks do not extend their scope.

Open work, in priority order:

1. **Integrate the runner support.** The coordinator must reconcile this branch
   with current `bringup`, preserving the separately landed NPU fast path and
   other workers' runner changes. Deploy RAM preservation, image-bound decoding,
   fresh ON-only admission and the reviewed rail-helper ordering before any run.
   The scoped review material is
   `concurrency-1101/runner-support.patch`; it is not guaranteed to apply cleanly
   to current `bringup`. The alternative is to merge current `bringup` into this
   branch, then repeat affected validation and reviews for changed native paths.
2. **Run the CPU control, then concurrency.** The coordinator-approved port is
   3-3. Exact ordered commands, templates, expectations and decoders are in
   `concurrency-1101/queue-inputs/commands.json`. First verify both CPU rails at
   800 mV. Stop on the first failure. After each verified return, record the entry
   and result before continuing. The combined run checks four NPU outputs and
   proves overlap with checked matrix multiplication on all eight CPU cores.
3. **Qualify active-job cancellation.** After both controls pass, build the
   cancellation board package from the integrated source and rerun its focused
   cases. Submit the bounded WAIT-stream cancellation and matching-inference
   recovery test. The existing `cancel-1101/producer-host-4/image.elf` is a host
   fixture, not a queued board package.

Both unrun native images are under
`~/.cache/sunstoneos/build/npu-wdt-1031/concurrency-1101/`:

| Image | ELF SHA-256 | README relative to that directory |
| --- | --- | --- |
| CPU-only control | `d80812cf85fc703394818dfef84f4330e35fa57003fa289dd1d213ed9221959d` | `final-native/control/README.md` |
| CPU and NPU concurrency | `2d8551bc4baa599f031e46a3f1a76e97c84cda03e3716c0474df4b2dcc86e680` | `final-native/combined/README.md` |

The coordinator approves the existing CPU lifecycle and top points, the CPU
control followed by concurrency, and then active-job cancellation. Three
independent reviewers approve the frozen concurrency source. Deployment of the
runner support remains pending with the coordinator; no further review is
requested. Any NPU domain OFF, clock gating or new voltage/clock point requires
a separate proposal and coordinator approval. PCK4 stays ON and DCDC2 stays
untouched. The post-ROM helper is permitted only after verified normal return
and RAM restoration.

Host evidence is in `concurrency-1101/final-checks/`, `review/` and
`final-gate.log`. The three baseline builds, selected integration gate and
complete linked return-chain check pass. The return-chain check includes
client completion, worker and interrupt retirement, all-core arrival, the
8 MiB pool clean, RAM restoration and the permitted post-ROM helper.

Shared files require coordination with CPU, NPU fast-path and runner workers:
`tools/board.py`, `tools/board_images.py`, `tools/a733-crumbs.py`,
`include/sunstone_crumb.h`, `include/sunstone_app.h`, `runtime/npu_app.c`,
`CMakeLists.txt`, `apps/os/npu*`, `apps/os/cpu-return*`,
`apps/os/return-fail-harness.h`, `apps/os/main.c`, `apps/os/task.c`,
`platform/a733-smp-supervised/launch.c` and `drivers/npu/*`.
Check the actual branch diff before resolving these files; preserve unrelated
workers' interfaces and changes.


Auxiliary scratch branches are preserved and clean. `npu-cancel-1101` at
`38841b70` and `work/npu-control-merge-1101` at `4a06665a` contain WIP commits
from old development fixtures. Their reviewed equivalents already exist on
`work/npu-wdt-1031`; neither scratch commit is a landing candidate.
