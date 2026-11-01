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

Normal stop/restart preserves registered providers and service objects in
software checks. Repeated start/stop, same-instance restart, domain power-off,
reset during an active job, confinement and thermal protection remain
unqualified. The first physical restart nonreturn remains a failed observation.
The queued runner prepares native finite sessions, validates interrupt receipts
and restores saved RAM after retirement. Its adapter has host evidence; the
shared daemon must include that adapter before these submissions.

The runtime models up to eight catalogue clients and model switching. The board
image has one slot and five catalogue models; direct FIFO USB and shader-graph
loading remain incomplete. Configured eight NN cores and zero TP/TP-lite cores
are target records, not a physical identity measurement.

The next host-verified lifecycle images and frozen compatible decoder are under
`~/.cache/sunstoneos/build/npu-wdt-1031/images/` and `next-host-593ce329/`.
`images/next-images.json` binds their artifacts and host archive. The proposed
queue order requires matching normal return before each subsequent run:

1. Local restart: two matching jobs across local start/stop lifetimes and four
   interrupt completions.
2. Model switching: conv12, conv8 and conv12, with three jobs and four interrupts.
3. Public SDK access: two models, three interrupts and owner-authorized quiescence.
4. Domain OFF/restart: OFF policy/status 0/0 then ON 8/8 between matching jobs,
   with four interrupts. Run this last.

The SDK diagnostic uses finite-task privilege; it does not qualify OS EL0
mappings. SDK owner/interleaving, transient-provider, report-capacity, failure
retention and power-transition fault checks have evidence in `next-checks/`,
`next-sdk-api/`, `next-abi/`, `next-frozen-preparation/`, `power/root/` and
`next-baselines/` under that build root. Correctness, hardware-safety and recovery
panels approve the frozen branch through `593ce329` against `a0b092b5`.

Remaining work includes repeated startup, broader graph/operator and numerical
coverage, model switching under load, SDK access, mimus integration, concurrent
CPU/GPU operation and safe fault recovery. See [startup](../npu-startup.md),
[service](../npu-service.md), [SRAM routing](../designs/npu-sram-route.md),
[runtime breadth](../designs/npu-breadth.md), [board image](../designs/npu-board.md)
and [conformance](../npu-conformance.md).
