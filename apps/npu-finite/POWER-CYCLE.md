# Physical NPU power-off is unsupported

The board keeps NPU power domain 4 and its DCDC2 supply ON. The startup
sequencer refuses physical OFF and its paired restart with RETAINED. It
preserves the counted row, rail pin, clock lease and PCK ownership.
The terminal failure prevents subsequent restart of that provider.

The V1.2 schematic connects VDD-NPU, VDD-SYS and VDD-DRAM to DCDC2. The board
device tree marks DCDC2 always-on. SoC and DRAM consumers exist outside the
software rail ledger, including when the NPU holds its sole software token.
The A733 manual also prohibits NPU power-off while using shared SRAM.
The selected vendor kernel configuration disables CONFIG_AW_PM_DOMAINS;
its VIP2 runtime domain OFF/ON branch therefore does not execute.
Source identities and the complete write inventory are in
[the NPU record](../../docs/npu.md).

## Refuse before hardware or ownership changes

`a733_npu_startup_power_off()` records the board policy and retained session
observations before latching failure. It reads no PMIC or domain register,
acquires no transport or register lock, and changes no request generation,
counted row, pin or lease. `off_refusal` identifies shared supply and shared
SRAM restrictions. Its supply-verified bit describes prior supply validation;
absence means the observation is unavailable. `off_authority`,
`off_rail_shared`, `off_rail_exclusive`, `off_request`, `off_counted` and
`off_sram_routed` preserve the known inputs. The counted-row observation is
unknown (`UINT32_MAX`) while its request is pending. None establishes exclusive
physical ownership. The retained first-failure record stores the reason
before a later lifecycle failure can replace it.

The public domain request rejects domain-4 OFF before generation changes.
The service independently refuses a directly injected OFF request before
MMIO or pin changes. Domain-5 transitions retain their existing behavior.
The PMIC selector interface rejects every DCDC2 target, including for a sole
exclusive software holder. It exposes no regulator-enable operation.

The refusal budget is synchronous and bounded: zero PCK, CCU, SYSCTRL or
PMIC writes and zero ownership changes. Diagnostic RTC breadcrumbs are the
only hardware-write exception. No refusal attempts recovery or cleanup.

## Local clock stop and restart remain supported

After controller END, output copy, reap, idle checks and CLOSE, call
`a733_npu_startup_stop()`. It closes software register admission, disables
the module and bus gates, asserts AHB, AXI and core resets, and routes shared
SRAM to the CPU. Domain 4, DCDC2 and the provider tokens remain owned.

`a733_npu_startup_restart()` reuses that stopped provider and its original
deadline. It qualifies the adopted PLL, routes SRAM to the NPU, releases
local resets and opens local gates. The caller must then reopen and configure
the controller, republish translation tables, bootstrap and require a fresh
interrupt before another job. Local restart after a physical-OFF refusal is
forbidden because failure is terminal.

Local stop/open table writes, PLL opening, SRAM routing, the power precheck,
refusal and supported domain-4 ON delay/policy/status operations have named
16-bit breadcrumbs. Each marker precedes its operation and grants no access.
The local restart retains its 20 us PLL qualification, 2 ms local settle,
1 s bootstrap/job deadlines and 20 s total session budget.

## Host checks and remaining physical gap

`checks/npu-startup-host.c --power-cycle` verifies synchronous physical-OFF
refusal with unavailable domain access, invalid status, ledger contention
and unavailable supply validation. It checks zero MMIO/PMIC accesses and
unchanged generations, counts, pins and tokens. Fabricating a legacy OFF
stage cannot revive a refused startup. The separate `--restart` mode checks
successful local restart and retained failure behavior.

`checks/power-domains-pck.c` checks both OFF refusal boundaries and retains
the delayed-status model for supported NPU ON and GPU transitions. NPU ON
coverage includes partial delay writes, readback errors and exact deadline
boundaries. `checks/a733-pmic.c` rejects all DCDC2 target bytes before
transport with shared and exclusive tokens. `crumb-check.py` checks marker
identities and decoding.

The failed physical run lost USB without a recovered native fault. It does
not establish rail shutdown, watchdog expiry failure or an executed failure
point. Physical NPU OFF remains unsupported. After coordinator recovery,
an instrumented local stop/restart control can test two matching jobs and
normal return with domain 4 ON and DCDC2 at 800 mV. That proposal requires
its own admission and frozen-image review; host checks do not qualify it.
