# Power, clocks and thermal management

**Status: implemented.**

Shared clock and reset leases, the rail ledger, PMIC provider, power-domain owner and processor thermal policy have focused software evidence. Processor lifecycle and thermal protection need physical qualification.

**Board-qualified scope:** Returning PMIC reads establish programmed selectors,
including enabled DCDC1 at 3.3 V, DCDC2 at 800 mV and CLDO3 at 1.8 V.
The domain owner verifies supplies and adopts domain 1 already ON. Finite
thermal acquisition and normal return pass on both boards. Development runs
normalize CPU rails to 800 mV at verified boot clocks before guarded launch and
after validated normal return. Selector readback does not measure analog voltage.

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| PMIC provider and rail ledger | qualified on the board (selector reads) | DCDC1 3.3 V, DCDC2 800 mV, CLDO3 1.8 V | [PMIC](../pmic-runtime.md) |
| Power-domain owner | qualified on the board | Domain 1 adopted ON | [power domains](../power-domains.md) |
| Finite thermal acquisition and normal return | qualified on the board | Both boards | [thermal](../thermal.md) |
| Thermal protection, throttling and critical shutdown | host-tested | 70 °C CPU stop, 90 °C throttle, 110 °C critical | [thermal qualification plan](../designs/thermal-management-qualification.md); [capture](../designs/cpu-return-thermal.md) |
| CPU rails at the top points | qualified on the board | DCDC3 1050 mV, DCDC5 1000 mV, lowered to 800 mV by the rail helper | [top points](../designs/cpu-top-opp.md); [records](../evidence/cpu-top-opp.jsonl) |
| Performance modes | host-tested | 3 CPU requests and 2 PMIC writes per switch | [design](../designs/performance-modes.md); [records](../evidence/performance-modes.jsonl) |
| Management processor (E902), suspend and resume | open | Blocked by firmware and shared-resource ownership | [qualification plan](../designs/thermal-management-qualification.md) |

## Open work

1. Qualify thermal accuracy, IRQ delivery, throttling, critical shutdown, restore, and concurrent CPU, GPU and NPU heat.
2. Observe the fitted PMIC identity, BLDO5, radio supply routing, and analog voltage and settling.
3. Resolve DCDC8 and DCDC9 selectors 88-127, which stay raw because the pinned Linux and U-Boot sources disagree.

## Management processor

**Board-qualified scope:** None.

E902 management-core control, mailbox services and sleep and wake are blocked by firmware and shared-resource ownership. Read-only remote-writer accounting is implemented; omitting SCP firmware does not prove shutdown or bus drain. The OR100 mask bit is reserved because that processor is absent on A733. See [PMIC](../pmic-runtime.md), [power domains](../power-domains.md), [thermal](../thermal.md), [performance modes](../performance-modes.md) and [remote writers](../remote-writers.md).

## Evidence

Board results are records in `docs/evidence/power-thermal.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-power-thermal-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/power-thermal.jsonl`.
<!-- evidence-table:end -->
