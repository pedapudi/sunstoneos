# CPU, interrupts and scheduling

**Status: implemented.**

Eight-core FreeRTOS startup, GIC-600 handling, cache publication, timer and inter-core interrupts, task context restoration, CPU clock ownership and bounded normal return have software evidence. Qualification is limited to the frozen workloads in the scope.

**Software evidence:** host, emulator, source.

**Board-qualified scope:** The returning workload runs 32 floating-point rounds
per core across six Cortex-A55 and two Cortex-A76 cores. It demonstrates
background scheduling, inter-core interrupts, per-core timer delivery, seven
independently observed secondary OFF states and restoration of saved RAM and
control registers. One eight-worker CPU-only mimus input matches its 90-frame
reference at the bin-4 top operating points. A corrected reliability workload
completes 4096 rounds in 4096 ticks on all eight pinned CPUs with matching
payload digests, seven secondaries off and WDT0 disarmed. Three returning
controls on port 3-1 normalize all secondary policies to static OFF and return
normally without a replug. The records list the queue entries.

## Open work

1. Repeat the corrected reliability workload from three cold starts; one run passes, so repeatability and full restoration are unqualified.
2. Qualify sustained scheduling, broader coherence and per-core IRQ workloads, induced-fault recovery, and idle, wake and thermal transitions.
3. Locate the fault of the longer-recognition nonreturn. Repeat it only with a concrete correction or a narrower hypothesis.
4. Qualify cacheable table walks with the core-6 walk measurement; eight-core cached images keep noncacheable walks until then.
5. Qualify WFI idle behavior (instruction-model evidence only), retention and cluster power-down.
6. Broaden PMU event qualification beyond the single-core result in `board-session-20261023/port-3-3/set5-bb4-run/`.
7. Release the held static-OFF policy through the reviewed path before later multicore consumers use it.

## Evidence

Board results are records in `docs/evidence/cpu.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-cpu-20261101.md).

<!-- evidence-table:begin -->
4 records in `docs/evidence/cpu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793513595977482947-679bf1ea` | 2026-11-01 | 3-3 | `ad25bc86d871` | returned | passed | run_total 69.236 s |
| `1793563765178868625-47ea9df5` | 2026-11-01 | 3-1 | `3baaace90784` | returned | passed | run_total 10.599 s |
| `1793563856677556459-2fc4fcf6` | 2026-11-01 | 3-1 | `61d3d9582ec9` | returned | passed | run_total 10.995 s |
| `1793563904446986983-31e882d1` | 2026-11-01 | 3-1 | `3baaace90784` | returned | passed | run_total 10.524 s |
<!-- evidence-table:end -->
