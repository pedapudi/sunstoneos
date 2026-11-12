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

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| Eight-core startup, GIC-600, timers, inter-core interrupts | qualified on the board | 32 rounds on each of 8 cores; 7 secondaries observed OFF | [records](../evidence/cpu.jsonl); [multicore skill](../../.agents/skills/sunstone-a733-multicore/SKILL.md) |
| Bounded normal return and static-OFF normalization | qualified on the board | 3 returning controls on port 3-1, 10.5 to 11.0 s | [records](../evidence/cpu.jsonl) |
| Reliability workload | qualified on the board (one run) | 4096 rounds in 4096 ticks, 69.2 s | `1793513595977482947-679bf1ea` in [records](../evidence/cpu.jsonl) |
| CPU-B, CPU-L and DSU top operating points | qualified on the board (returning workload, port 3-3) | 2002, 1794, 1274 MHz; CPU-bound loop 3.85 times (A55) and 2.06 times (A76) | [top points](../designs/cpu-top-opp.md); [records](../evidence/cpu-top-opp.jsonl) |
| A76 placement, CPU clock grants, in-line SGIs in the OS image | host-tested | Idle tick 554 instructions, 0 SGIs (base 1,345, 5) | [A76 design](../designs/cpu-a76-dvfs.md); [records](../evidence/cpu-a76-dvfs.jsonl) |
| Integrated OS image at the top points (B1, TOP) | open | Predicted 2.10 s at boot rates, 1.20 s at the top points | [top points](../designs/cpu-top-opp.md) |
| Cold secondary ownership through static OFF | host-tested | 23 owner cases, stage2 80 bytes below 32 KiB | [design](../designs/cold-cpu-static-off.md); [records](../evidence/cold-cpu-static-off.jsonl) |
| Thermal capture after returning recognition | host-tested | 33,816 bytes in two ROM reads; longer-input run did not return | [design](../designs/cpu-return-thermal.md); [records](../evidence/cpu-return-thermal.jsonl) |
| Cacheable EL1 table walks | host-tested; core-6 comparison blocked | Application-table maintenance: 832 operations uncached, 0 cached; cached stores complete with `DSB ISH` | [design](../designs/cpu-table-walks.md); [records](../evidence/cpu-table-walks.jsonl) |
| Performance modes (high and low) | host-tested | 3 CPU requests and 2 PMIC writes per full switch | [design](../designs/performance-modes.md); [resident](../designs/resident-performance.md); [records](../evidence/performance-modes.jsonl) |
| Sustained scheduling, WFI idle, retention, cluster power-down, PMU events | open | Instruction-model evidence only | Open work below |

## Open work

1. Repeat the corrected reliability workload from three cold starts; one run passes, so repeatability and full restoration are unqualified.
2. Qualify sustained scheduling, broader coherence and per-core IRQ workloads, induced-fault recovery, and idle, wake and thermal transitions.
3. Locate the fault of the longer-recognition nonreturn. Repeat it only with a concrete correction or a narrower hypothesis.
4. Recover port `3-1` through the [handoff](../handoff.md), then qualify the prepared core-6 OS walk comparison. The uncached control is refused before launch. Eight-core defaults keep uncached walks; the cached return and recognition checks require a measured gain ([design](../designs/cpu-table-walks.md)).
5. Qualify WFI idle behavior (instruction-model evidence only), retention and cluster power-down.
6. Broaden PMU event qualification beyond the single-core result in `board-session-20261023/port-3-3/set5-bb4-run/`.
7. Release the held static-OFF policy through the reviewed path before later multicore consumers use it.
8. Run the integrated OS image at the top points (B1, then TOP), the performance-mode board case and the longer-input thermal capture; see the design docs listed above.

## Evidence

Board results are records in `docs/evidence/cpu.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-cpu-20261101.md). Design records: [A76 and operating points](../evidence/cpu-a76-dvfs.jsonl), [top points](../evidence/cpu-top-opp.jsonl), [static OFF](../evidence/cold-cpu-static-off.jsonl), [thermal capture](../evidence/cpu-return-thermal.jsonl), [performance modes](../evidence/performance-modes.jsonl), [resident performance](../evidence/resident-performance.jsonl).

<!-- evidence-table:begin -->
4 records in `docs/evidence/cpu.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793513595977482947-679bf1ea` | 2026-11-01 | 3-3 | `ad25bc86d871` | returned | passed | run_total 69.236 s |
| `1793563765178868625-47ea9df5` | 2026-11-01 | 3-1 | `3baaace90784` | returned | passed | run_total 10.599 s |
| `1793563856677556459-2fc4fcf6` | 2026-11-01 | 3-1 | `61d3d9582ec9` | returned | passed | run_total 10.995 s |
| `1793563904446986983-31e882d1` | 2026-11-01 | 3-1 | `3baaace90784` | returned | passed | run_total 10.524 s |
<!-- evidence-table:end -->
