# CPU, interrupts and scheduling

**Status: implemented.**

Eight-core FreeRTOS startup, GIC-600 handling,
cache publication, timer and inter-core interrupts, task context restoration,
CPU clock ownership and bounded normal return have software evidence.
Qualification is limited to the frozen workloads below.

**Board-qualified scope:** The returning workload runs 32 floating-point rounds
per core across six Cortex-A55 and two Cortex-A76 cores. It demonstrates
background scheduling, inter-core interrupts, per-core timer delivery, seven
independently observed secondary OFF states and restoration of saved RAM and
control registers. One eight-worker CPU-only mimus input matches its 90-frame
reference at the bin-4 top operating points.

Evidence paths are relative to `~/.cache/sunstoneos/evidence/`:

- `processor-completion-20261024/port-3-1/current-cpu-root-review/runtime/session.json`
  records the current fault-capture workload and 18 MiB restoration.
- `processor-completion-20261024/port-3-1/cpu-local-timer/runtime/session.json`
  adds one timer interrupt per secondary and 32 CPU0 ticks. CPU0 owns the
  global FreeRTOS tick.
- `secondary-coherence-probe-20261018/physical-113/` records 224 ordered
  payload transfers and atomic increments across the seven secondaries.
- `sol-resume-20261028/` and [application evidence](../mimus-evidence.md)
  identify the exact CPU recognition input and restored return.
- `dev-runs-1031/3-1-cpu-return-211702/run/session.json` records the corrected
  hello-compute return with seven secondaries off.

The reliability workload is blocked at CPU-clock retention. Its first launch
records RETAINED at `return-cpu.c:159` in `cpu-reliability-1031/start-1/`.
The first hello input finishes while the owner is REQUEST; the return check
requires FOLLOW and refuses at
`dev-runs-1031/3-1-cpu-return-204119/run/watchdog-report.json`.
The corrected hello result establishes only that control's scope.

Remaining work includes repeated cold startup, sustained scheduling, broader
coherence and per-core IRQ workloads, induced-fault recovery, idle/wake and
thermal transitions. The longer-recognition nonreturn has no recovered fault
location. Do not repeat it without a concrete correction or narrower hypothesis.
Eight-core cached images keep noncacheable table walks until the documented
core-6 walk measurement qualifies cacheable walks. WFI idle behavior has only
instruction-model evidence; retention and cluster power-down are incomplete.

The [startup](../cpu-start.md), [coherence](../cpu-coherence.md),
[SMP runtime](../smp-runtime.md), [CPU clocks](../cpu-clock.md) and
[reliability application](../../apps/cpu-runtime-qualification/RELIABILITY.md)
records define source and test scope. PMU empty/fixed regions, table-walk counts
and WFI counters have a single-core physical result in
`board-session-20261023/port-3-3/set5-bb4-run/`; broader event qualification remains.
