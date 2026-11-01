# NPU

**Status: implemented.**

Native model decoding, mappings, shared-provider startup,
IRQ97 service, runtime clients and thermal policy have software evidence.
Current scheduled startup and same-instance restart remain unqualified.

**Board-qualified scope:** A finite interrupt diagnostic produces matching
ShuffleNetV2 outputs at 852 MHz/800 mV. Startup takes 4.0 ms and inference takes
2.91 ms doorbell-to-interrupt, about 3.3 ms end to end. Current provider-free
entry, disabled-IRQ installation/restoration and real provider/task construction
before scheduler start return normally with saved RAM restored. These current
checkpoints perform no inference.

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

Normal stop/restart preserves registered providers and service objects in
software checks. The first physical restart attempt does not return; current
scheduled startup, enabled interrupt delivery, same-instance restart, domain
power-off, confinement and thermal protection remain unqualified. Current
standalone images reject the armed watchdog; CPU-return excludes NPU, so no
admitted NPU return path exists in the current board runner.

The runtime models up to eight catalogue clients and model switching. The board
image has one slot and five catalogue models; direct FIFO USB and shader-graph
loading remain incomplete. Configured eight NN cores and zero TP/TP-lite cores
are target records, not a physical identity measurement.

Remaining work includes current-image inference, broader graph/operator and
numerical coverage, model switching under load, mimus integration, concurrent
CPU/GPU operation and safe fault recovery. See [startup](../npu-startup.md),
[service](../npu-service.md), [SRAM routing](../designs/npu-sram-route.md),
[runtime breadth](../designs/npu-breadth.md), [board image](../designs/npu-board.md)
and [conformance](../npu-conformance.md).
