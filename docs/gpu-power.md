# GPU power control still needs physical qualification

The A733 GPU power controller prepares the GPU at 400 MHz and requests power
for its top-level domain (PCK600 domain 5) through a row of the
[power-domain owner](power-domains.md). It supports bounded startup, shutdown, and reopening.
The native platform integrates the shared clock and rail owners and the FreeRTOS
GPU service. Host checks and an AArch64 compilation pass. A physical power/identity
attempt on 2026-10-24 did not return; no startup, identity or shutdown phase was
observed. GPU operation remains unqualified.

The implementation is [a733_gpu_power.c](../drivers/gpu/a733_gpu_power.c), with
its callback contract in [a733_gpu_power.h](../include/a733_gpu_power.h).
A privileged owner polls the controller. One poll runs the resource and
register steps back to back, with monotonic clock reads. It waits on the clock
for the one-microsecond clock-settling delay. The board clock reports whole
microseconds rounded down, so the delay ends only when the clock passes the
sample taken after the clock-gate write by more than one microsecond. The
domain step requests the row once and returns to the owner until the service
task answers; each later poll reads the answer with one acquire load. A
delay whose clock shows no advance for 1,024 consecutive samples
(`A733_GPU_SPIN_STALLED_READS`) returns to the owner without advancing the
step, and the next poll restarts the delay. Clock settling and the domain
answer share one absolute deadline. A refused row fails with
`A733_GPU_POWER_STATE`, a failed domain with `A733_GPU_POWER_DOMAIN`. An expired deadline, clock
regression, callback failure, or unexpected register value stops further I/O.
The controller performs no automatic rollback after a failure.

## Shared resources require an explicit owner

The resource manager must acquire the following before register preparation:

- Exclusive ownership of the GPU local clock, external reset, and GPU top-level
  power domain.
- A stable 400 MHz peripheral-clock output and an established 800 mV DCDC4 rail.
- Shared leases for the oscillator, power-controller clock, and GPU AHB and
  memory-bus gates.

The board operating-point table specifies 400 MHz at 800 mV. This configuration
is a bring-up setting; it is not a measured frequency or a performance target.
The manager must complete regulator settling and parent-clock preparation before
its acquisition callback returns. It must arbitrate concurrent users and prevent
changes while this controller holds its leases. The board marks DCDC4 as always
on, so releasing the GPU lease must preserve that rail.

The controller checks that the shared bus gates and power-controller clock are
enabled. It refuses an inherited GPU whose core clock, bus clock, or external
reset indicates possible execution. It does not attempt to take over a running
GPU. Resource acquisition can have effects before that refusal; the failed
controller retains its diagnostics and does not release resources automatically.

Register operations use these address spaces:

| Register bank | Physical base | Fields used |
| --- | --- | --- |
| Main clock controller | `0x02002000` | AHB gate `0x5c0[7]`, memory-bus gate `0x5e0[16]`, GPU clock `0xb20`, bus gate/reset `0xb24[0,16]` |
| Always-on clock controller | `0x07010000` | Power-controller gate `0x1ac[0]` |
| Power controller | `0x07060000` | GPU domain 5, policy `0x5000`, status `0x5008`, sequencing delays |

Shared gates are read-only in this controller. The platform's register-update
callback must preserve unrelated fields atomically under the shared register
lock. GPU clock writes include the update bit at bit 27, which the manual says clears
when the configuration becomes valid. The controller checks the configured
fields, not an explicit update-bit completion poll.
No parent PLL or PMIC register is written by the controller.

## Startup and shutdown preserve explicit limits

Startup checks the MBUS gates, the power-controller clock, a closed GPU clock
gate and an asserted reset, then requests the domain-5 row ON. The owner
adopts a coherent inherited ON domain without a command. From OFF it programs
the source-defined power-controller delays, requests domain state 8 and waits
for that exact status. After the row reads ON the controller selects the 400 MHz peripheral
parent with divisor one, enables the core and bus clocks, waits at least one
microsecond, and releases external reset. Relevant writes receive readback
checks. The selected clock requires only 80 ns for 32 cycles, so the one
microsecond delay covers the reference driver's settling requirement.

Shutdown requires stopped firmware, closed job admission, and masked GPU
interrupts. It asserts external reset, disables the local bus and core clocks,
and requests the row OFF. The owner rereads the gate and reset closed, initializes
the delay row if needed, requests domain state zero and waits for that exact status. It then releases
shared-resource leases through the resource manager. The controller allows no
new operation after a failed transition.

A successful power-off result does not establish that outstanding memory
transactions have drained. This controller cannot supply the isolation callback
required by [GPU shutdown](gpu-stop.md). Device backing must remain reserved
until a separate platform mechanism proves memory isolation. Shared-resource
release also does not authorize freeing device buffers.

The `lease_attempted` and `lease_held` diagnostics describe callback progress.
A failed acquisition or release can leave partial ownership. A successful
release followed by an expired deadline records failure with `lease_held` clear.
Platform recovery must reconcile that state before reuse.

## Focused checks and source identities

The changed behavior is clock, reset, and power-domain sequencing. The relevant
risks are accessing a live device, changing shared fields, releasing resources
prematurely, accepting a transient domain state, or continuing after failure.
One host model and one reference comparison cover those risks without accessing
the board or running performance measurements.

The [native check](../checks/a733-gpu-power.c) runs under AddressSanitizer and
UndefinedBehaviorSanitizer. Successful startup, shutdown, and reopening preserve
unrelated fields and shared gates. Reset release follows the clock gate by at
least the settling delay. A second clock model counts one 24 MHz tick per
clock read and callback and reports whole microseconds rounded down; under it,
reset release followed the clock gate by 44 counter ticks, at least the 24 that
one microsecond requires. A stand-in for the owner answers the row from the
modeled power-controller words. The first poll checks the preconditions and
requests the row without waiting; the poll after the answer runs every
remaining step, including 10 µs register updates. An unanswered row keeps the
controller at the domain step with no register write, and with a stopped
clock the settling delay returns without advancing and completes once the
clock runs. The check covers 69 retained faults, including every callback
failure on a successful startup/shutdown path, stalled transitions, invalid
prerequisites, a refused and a failed row, a partially applied write, and a
late resource release. The owner itself is covered by
`checks/power-domains-pck.c`.

The [reference check](../checks/a733-gpu-power-reference.py) compiles the original
power-domain initialization and transition functions with a register model. All
emitted power-controller writes match the trace of one GPU power-on and
power-off through the real owner, which `checks/power-domains-pck.c` writes. It derives GPU mux,
divisor, update key, gate, and reset expectations from the clock definitions.
It also verifies register bases, the selected operating point, the board's GPU
supply assignment, and the always-on rail requirement.

References come from the [pinned board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f):

- `bsp/drivers/pm_domain/pck600_domains.c` and
  `bsp/include/dt-bindings/power/sun60iw2-power.h` define domain identifiers,
  sequencing delays, policy requests, and acknowledgement fields.
- `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c` and `ccu-sun60iw2-r.c` define clocks,
  gates, reset, the divisor table, and the update key.
- `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` and
  `sun60i-a733-orangepi-zero3w.dts` define addresses, supply wiring, and operating
  points.
- `bsp/drivers/gpu/imagination/pvr_power.c` enables parent, core, and bus clocks,
  waits one microsecond, and deasserts reset. Its shutdown reverses reset and
  clock enablement.

Downloaded files retain their original license notices. The native controller
uses independently written state transitions and source-derived register facts.
The reference checker preserves the original power-domain source notice in its
compiled extraction. Its off transition requires status zero; the original
helper accepts any status other than eight as off.

Raw evidence is `~/.cache/sunstoneos/evidence/gpu-power-20261016/`. It contains
native and reference register traces, compiled artifacts, source hashes,
commands, and results. Source files and their provenance manifest are cached in
`~/.cache/sunstoneos/sources/gpu-power/`. An initial reference-shim compilation
failed because its disabled logging macro left a local variable unused. The
corrected shim passed; native controller evidence did not require repeating.
The later native check adds partial-write and late-release coverage. Its
successful register trace is unchanged, so the reference result remains valid.

## Audit of the nonreturning identity probe

The frozen [power/identity probe](../apps/gpu-power-id/README.md), ELF
`fd3d5839f700b5e0f569b0ce722e186c33beba65512815b136d9c969a7341afe`, was
uploaded byte for byte to port 3-1. All six FEL transactions succeeded, including
launch. The 16 KiB zero file was the prelaunch result initialization; there was
no post-execution result read. No ROM node or native result was observed within
30 seconds. This leaves entry, provider readiness, power, identity and return
unlocalized. The board requires recovery before any further execution.

The source audit found one concrete shutdown gap. The reference
`pck600_domains.c:126–142,269–283` initializes five delay words at domain
registration, independently of the current power state. Its table and offsets
at lines 547–569 supply the native values. The failed image instead initialized
only on an ON command. With inherited ON, adoption skipped the command and its
first OFF used inherited delay settings. The owner now initializes those words
before that first OFF, with readbacks and a fresh deadline check. This closes a
missing transition prerequisite; it does not establish that the failed probe
reached shutdown, or that the inherited settings were wrong. The vendor fields
at offsets `0xc00`, `0xc04` and `0xc10` are not described by the PCK-600 TRM;
the correction follows the pinned implementation without inventing their timing
semantics. Existing ON writes and later OFF writes are unchanged.

The remaining sequence comparison is:

| Source and location at pinned kernel revision | Finding |
| --- | --- |
| A733 manual 4.1.6.121–122; `ccu-sun60iw2.c:1021–1049` | Source 3 selects PERI0 400 MHz, divisor 0 masks no cycles, bit 31 gates the core, and `0xb24` bits 0/16 control bus gate/reset. The native fields agree. |
| `ccu_div.c:110–140`, `ccu_mux.c:182–208`, `ccu_gate.c:144–148,213–243` | Rate, parent and gate writes include the update key. Only `CCU_FEATURE_CLEAR_MOD` selects a clear-bit wait; GPU has `KEY_FIELD_MOD` alone. There is no demonstrated missing reference wait, so the native clock sequence is unchanged. |
| Open PVR `pvr_power.c:256–307` | Parent/core/bus enable, at least 1 µs delay, then reset release; shutdown asserts reset before disabling bus/core/parent. Native order follows this path. |
| BXM `sunxi_platform.c:167–275,911–922` | Initial probe requests runtime power before clocks; its helper releases reset before bus/core enable and explicitly writes bit 27. This differs from open PVR and agrees with the manual's generic reset-before-bus advice in 4.1.4.4. |
| BXM `sunxi_platform.c:345–353,374–380` | Later power callbacks enable clocks before runtime ON and request runtime OFF before disabling clocks. These are different lifecycle paths, not authority to reorder the native never-booted probe. |
| Open PVR `pvr_device.c:511–541,595–603,680–739`; `pvr_power.c:353–376` | Runtime power precedes the 64-bit identity read and legacy 32-bit fallback; identity precedes firmware loading. A never-booted unwind skips firmware shutdown. The native ID widths, offsets and no-firmware scope agree. |

The `img,gpu` compatible selects the manual callbacks *within* open PVR
(`pvr_drv.c:1495–1510`); it does not prove which driver a Linux image loads.
`bsp/drivers/gpu/Kconfig:35–51` and the imagination Makefile select open PVR with
`CONFIG_AW_DRM_POWERVR`. The separate `bsp/modules/gpu/Makefile` selects BXM when
`GPU_TYPE` or `CONFIG_AW_GPU_TYPE` is `bxm`. Neither cached sun60iw2 minimum config
establishes that selection. No built board `.config` or loaded-module record is
bound to this audit, so the effective board-driver choice remains unresolved.
The differing reset orders are recorded rather than used to justify a change.
The domain driver attaches clocks but does not set `GENPD_FLAG_PM_CLK`
(`pck600_domains.c:186–213,282`); generic domain code installs automatic clock
start/stop callbacks only with that flag (`drivers/base/power/domain.c:2084–2086`).

The narrow [provider-readiness checkpoint](../apps/gpu-provider-ready/README.md)
remains frozen at ELF `3adfa4ad6ba386ce8d39f76646f540107c2a5d02ce42db6c1c35c90de68a75ed`.
It returns after the actual constructors, PMIC readiness and both boot supply
verifications, before any GPU power request or PCK access. It is untested on
hardware. The delay fix does not alter that artifact or its evidence. Its newer
monitor includes the accepted restoration readbacks; the failed image did not.
Neither that cleanup change nor matching CPU objects locates the failed stage.

Audit sources, hashes, the exact-image constructor result and the independent
CPU-object comparison are in
`~/.cache/sunstoneos/evidence/gpu-power-id-diagnosis-20261024/`. The corrected
owner's two transition paths, 18 failure cases, deadline cases and AArch64
compilation are preserved in
`~/.cache/sunstoneos/evidence/gpu-first-off-delays-20261024/snapshot/manifest.json`.
The model keeps domain 4 untouched and verifies retained ownership on uncertainty;
it does not simulate physical PCK handshakes or prove GPU execution. No new native
image, hardware access or retry was made for this correction.
