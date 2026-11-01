# Power, clocks and thermal management

**Status: implemented.**

Shared clock/reset leases, the rail ledger, PMIC
provider, power-domain owner and processor thermal policy have focused software
evidence. Processor lifecycle and thermal protection need physical qualification.

**Board-qualified scope:** Returning PMIC reads establish programmed selectors,
including enabled DCDC1 at 3.3 V, DCDC2 at 800 mV and CLDO3 at 1.8 V.
The domain owner verifies supplies and adopts domain 1 already ON. Finite
thermal acquisition and normal return pass on both boards. Development runs
normalize CPU rails to 800 mV at verified boot clocks before guarded launch and
after validated normal return. Selector readback does not measure analog voltage.

Evidence paths are relative to `~/.cache/sunstoneos/evidence/`:

- `pmic-provider-probe-20261018/physical/runtime/session.json` records a DCDC1
  request, reap, lease release and inherited bus/pin restoration.
- `sys-supply-probe-20261018/runtime/` records the second-board supply reads.
- `thermal-sid-shadow-20261018/runtime/` records calibration-word access.
- `processor-completion-20261024/port-3-1/thermal-before-workload/` records finite
  samples below 40 degrees Celsius before the CPU workload.
- `dev-runs-1031/3-3-rail-helper-201322/` binds boot-clock checks and CPU rail
  normalization. PMIC rails can retain raised values through watchdog reset.

The ledger controls the fifteen used rail outputs; one service task owns PMIC
transactions and PCK-600 domain transitions. NPU startup uses domain 4;
framebuffer, G2D and GPU clients retain their domain rows. Late acknowledgments
retain ownership rather than publish success. The provisional 155% PMIC step
bound needs reconciliation with measured hardware behavior.

DCDC8/DCDC9 selectors 88–127 remain raw because pinned Linux and U-Boot sources
disagree. The six-rail observer has host evidence under
`~/.cache/sunstoneos/build/power-thermal-1031/supply-decode/` and the integrated evidence under
`~/.cache/sunstoneos/build/integrate-streams-1031/`. Its physical run needs the
shared watchdog contract. Fitted-PMIC identity, BLDO5, radio supply routing and
analog voltage/settling remain unobserved.

The thermal policy throttles CPU/GPU at 90 degrees Celsius and DDR at 85;
every channel is critical at 110. Invalid readings throttle after one second
and become critical after five. A CPU qualification session can lower its
critical trip to 70. CPU raises require NORMAL and a cluster reading; THROTTLE
lowers raised clusters to 1014 MHz. Late thermal permission is rechecked before
clock writes. Physical accuracy, IRQ delivery, throttling, critical shutdown,
restore and concurrent CPU/GPU/NPU heat remain unqualified.

Vendor top points are CPU-L 1794 MHz/1000 mV, CPU-B 2002 MHz/1050 mV,
GPU 1008 MHz/960 mV and NPU 852 MHz/800 mV for the reviewed bin. Lower modes
are optional. The [runner policy](../board-runner.md) controls new-point approval.

## Management processor

**Board-qualified scope:** None.

E902 management-core control, mailbox services and sleep/wake are blocked by
firmware and shared-resource ownership. Read-only remote-writer accounting is
implemented; omission of SCP firmware does not prove shutdown or bus drain.
The OR100 mask bit is reserved because that processor is absent on A733.

See [PMIC](../pmic-runtime.md), [power domains](../power-domains.md),
[thermal](../thermal.md), [performance modes](../performance-modes.md),
[PMIC reference gaps](../designs/pmic-reference-reconciliation.md),
[thermal qualification](../designs/thermal-management-qualification.md) and
[remote writers](../remote-writers.md).
