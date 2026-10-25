# Returning DCDC2 observation

`npu-supply.c` captures one DCDC2 snapshot through the existing PMIC provider
and returns after reaping the request and parking the service. The host then
classifies the value. A disabled output, unexpected voltage or undecodable
selector does not cause a failure exit when the observation itself succeeded.

The consumer uses the returning PMIC bundle's platform, service, rail ledger,
pin provider and native PMIC adapter. Its initializer omits the domain row.
The service therefore skips its automatic DCDC2/DCDC4 verification, and this
consumer performs no DCDC4 transaction or PPU access. It requests no voltage or
clock change and runs no access-cost windows.

After provider readiness, the client waits at least 5,160 microseconds, the
existing `A733_POWER_SUPPLY_SETTLE_US`. It takes a shared DCDC2 token, submits
one snapshot, waits for COMPLETE, reaps it, and calls `bb_power_park()`.
That function requires an idle service, no held PMIC transfer, no retained or
failed provider, no active request and neither provider mutex held. Only then
does the client print the snapshot and the completion line and return status 0.

All waits share the existing 1.5-second session deadline inside the returning
monitor's 2-second bound. Transport, provider, timeout and quiescence failures
remain failure exits. The host must retain the failed session under its existing
recovery rules. The parent rail token and provider ownership last for this
returning image's lifetime; no voltage setting is restored or changed.

## Required physical admission

The captured PL0/PL1 configuration must be function 2, pull 1, drive 1, matching
the initializer. The platform must separately authenticate exclusive authority
over these pins, S_TWI0 and its APBS0/APBS1 route. It must exclude legacy,
secure, remote, interrupt, DMA and autonomous PMU users for the probe lifetime.
Pin configuration alone does not establish that authority. The existing pin
acquire sequence temporarily disables and restores the mux; it is covered by
the qualified provider path and requires this ownership.

Before permitting TWI MMIO, establish the following through the protected
configuration-read transport in the same powered lifetime:

| Address | Predicate |
| --- | --- |
| `0x0701019c` | S_TWI0 gate/reset: `(word & 0x10001) == 0x10001` |
| `0x0701000c` | APBS0: under mask `0x0700001f`, source bits26:24 is 0 or 4; retain the divider bits4:0 |
| `0x07010010` | APBS1: under mask `0x0700001f`, exactly 0 or `0x04000000` |
| `0x07025210` | PL interrupt enable: bits1:0 clear |

These are R_CCU and always-on pin-controller reads. No gate or route write is
proposed. The provider validates the route, inherited divisor, idle controller,
interrupt state and disabled autonomous engines itself. The separate gate
observation matters because its `route()` reads the TWI divisor before the later
gate/reset check in `baseline()`. An unknown or closed gate is insufficient
authority for that access.

Use the established returning monitor's current DDR, cache-off, platform-survey
and warm-return admission. The probe does not establish those facts. It has no
dependency on NPU PLL eligibility and does not read the NPU engine.

## Host classification

Require successful return of the frozen image, status 0 and
`COMPLETE DCDC2 snapshot: reaped and provider parked`. Preserve both raw lines
and the decoded line. An NPU-compatible programmed supply has:

- `page_before == 0`, `chip == 3`, `revision == 0`, `page_after == 0`;
- enable bit1 set and `(selector & 0x7f) == 0x1e`;
- `enabled == 1`, `decoded == 1`, `programmed_uv == 800000`.

The enable byte need not equal `0xff`; selector bit7 may be either value.
An unexpected value remains a completed observation and blocks NPU supply
admission. It authorizes no corrective voltage write. This is programmed-supply
evidence; analogue voltage and settling remain unmeasured. Preserve exclusion
of supply writers between the snapshot and any later NPU admission.

The native snapshot sequence and decoder are in
[`a733_pmic.c`](../../drivers/power/a733_pmic.c). The supply predicate and settle
budget are in [`a733_power_domains.c`](../../drivers/power/a733_power_domains.c)
and its [header](../../include/a733_power_domains.h). The observer and service
retain the [PMIC access](../../docs/designs/pmic-access.md) implementation and
its evidence limits.
