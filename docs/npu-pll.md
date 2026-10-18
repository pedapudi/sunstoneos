# NPU PLL preparation

The native PLL controller programs a fixed 492 MHz output from the 24 MHz
reference. It implements the documented disabled-PLL enable sequence and a
separate enabled-PLL retune sequence. An optional [atomic handoff](npu-clock-handoff.md)
transfers its reservations to NPU startup. The supervised native application
provides an explicit live-DRAM enable mode. Physical PLL qualification remains
required for every mode.

The initial NPU operating point is 492 MHz at 800 mV. That voltage belongs to the
NPU domain's DCDC2 supply. The PLL's internal LDO is a separate control. PLL
preparation does not program DCDC2 or power the NPU domain. The existing
[NPU startup binding](npu-startup.md) must still establish stable, read-back
verified DCDC2 and PCK ownership before local clock/reset/domain startup.

## Source basis

The pinned A733 User Manual V1.00 defines the following behavior. Its central
source-lock identity is `a733-user-manual-1.00`.

| Manual section | Implemented requirement |
| --- | --- |
| 4.1.4.1, pages 25–27 | Disabled enable method 2; enabled retune; warning that enabling or disabling a PLL can disturb other PLLs |
| 4.1.4.2 | Three consecutive lock observations separated by at least 3 microseconds |
| 4.1.4.6, 4.1.6.255 and 4.1.6.264 | Dedicated software gate enabled before selecting No-Auto; effective gate status checked |
| 4.1.6.56–60 | PLL enable, internal LDO, lock control, output gate, factors, modulation and bias fields |
| 4.1.6.76, 114–116 and 120–124 | MBUS, video encode/decode, NPU and DRAM0 consumer selectors and gates |

The board clock source at revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` identifies the same PLL register,
24 MHz reference parent and N/divider fields. Its common lock helper waits for
three lock observations, spaces reads by 3 microseconds, limits observations to
100 and waits another 20 microseconds. The host source check records exact input
hashes. Existing platform evidence remains applicable to the separate rail,
PCK, local reset and service components.

The profile uses N=82, M1=1 and P0=4: a 1,968 MHz oscillator divided to 492 MHz.
The encoded factor fields are `0x00305100`. The multiplier is within the board
source's supported 53–105 range and the documented oscillator range. This is an
initial functional profile, not a performance target.

## Admission and lifetime ownership

The caller supplies a private immutable configuration and bounded operations.
Initialization copies both before changing controller storage. The acquisition
callback must authenticate the entire configuration against independent startup
authority. Its numeric handoff identity does not establish ownership by itself.
All state, callbacks and reservations remain live for platform lifetime.

Admission reserves exclusive PLL_NPU and local NPU authority in the common clock
registry. Stable reference, oscillator and fabric reservations remain held.
A selected PERI0 fabric route adds stable PERI0 ownership. The provider must
validate the actual selected parent rate, modulation and effective gates;
reserving a scope alone does not verify hardware. AHB, NSI and MBUS selectors
outside the supported SYS24/PERI0 routes are refused.

A separate whole-platform reservation must exclude every remaining consumer and
writer: video encode/decode, DRAM0 and its PHY, remote MCU, secure firmware and
autonomous clock management. All register writers must honor the same register
mutex and lifetime reservations. Software leases cannot establish that remote
firmware is stopped. The cold boot provider must supply that evidence.

The controller compares complete snapshots of AHB, NSI, MBUS, video module and
bus clocks, NPU module and bus clocks, DRAM0 clock and bus controls, and video/NPU
autonomous clock modes before its first write. Active video/NPU module gates,
deasserted video/NPU core bus resets, pending fabric updates and autonomous
video/NPU modes refuse admission. DRAM0 selectors are limited to DDR/PERI1
sources. DE- or NPU-selected DRAM0 is refused even when its output gate is off.
The controller never migrates consumers or writes their registers.

The three entry modes have different additional requirements:

- **Disabled PLL enable:** execution, stack, data and every callback must reside
  in owned non-NPU SRAM before DRAM initialization. This profile excludes the
  shared NPU SRAM alias. DRAM0
  must remain reset with its module clock off. Fabric must use SYS24. CPU clocks
  and other active PLL users must be safe against the manual's PLL-enable
  disturbance. A normal FreeRTOS task whose storage resides in DRAM cannot meet
  these requirements. A separate early boot provider is required.
- **Enabled PLL retune:** PLL, internal LDO and lock detection must already be
  enabled and lock must be observed. Every PLL_NPU consumer and writer must be
  excluded. Running RAM may remain active only with separately authenticated,
  independent DRAM and PHY sources. The manual identifies a PHY clock path in
  normal DRAM operation; the DRAM0 selector alone does not prove that path.
  The controller leaves unrelated running RAM and its clocks untouched.
- **Live-DRAM disabled PLL enable:** the PLL, output and lock detector must be
  off, with LOCK clear. The full snapshot must show DRAM0 enabled, its reset
  released, its controller sourced by DDRPLL and its external memory clock
  selected from the separate PHY PLL. No update may be pending. Independent
  PHY operation, stable fabric parents and exclusion of every PLL_NPU consumer
  and writer remain authenticated prerequisites. This supervised mode retains
  every reservation through success or failure and never disables or retries
  the PLL. It uses the existing method-2 sequence and timeout handling.

The live-DRAM mode follows a concrete Linux runtime precedent at the pinned
revision above. `vip_drv_device_platform.c` calls `clk_set_rate(pclk)` and later
`clk_prepare_enable(mclk)`; the device tree binds that parent to PLL_NPU.
Its CCU descriptor uses `CLK_SET_RATE_UNGATE`. The pinned
[clock core](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/clk/clk.c#L2278)
temporarily enables such a clock before changing its rate. Enabling the module
also recursively enables its parent. The CCU gate helper explicitly handles
a disabled PLL: output off, LDO on, PLL on, lock detector on, wait for lock,
then output on. These calls run in the normal DRAM-resident driver path.

This source establishes a runtime implementation, not the board's current state
or physical safety. The manual warns that enabling a PLL can disturb other PLLs;
independent selectors do not remove that analogue risk. The live mode therefore
requires a supervised single attempt and full power removal after the session.
Software deadlines depend on a responsive CPU and interconnect. Unlike the
Linux helper's warning-only lock failure, SunstoneOS retains failure without
opening the output gate. Its pre-DRAM mode remains available under its original
conservative contract; the manual does not mandate that restriction.

Reference checks require enabled, stable DCXO and exact 24 MHz PLL_REF using
the [shared decoder](clock-leases.md#detected-oscillator-and-normalized-reference).
The detected input may be 19.2, 24 or 26 MHz. The fixed PLL_REF parent from the
pinned kernel keeps the programmed NPU output unchanged. Baseline checks run
under held REF/OSC ownership; final control, lock and output checks remain in
force, without claiming continuous reference monitoring. PLL
modulation, fractional interpolation, smooth updates and dithering must be
disabled. The phase-interpolator bias and charge-pump bias must have their
specified reset settings. Unlock interrupts must be disabled. Pending unlock
status is preserved; no event is acknowledged.

## Programming and failures

The controller first sets the dedicated software gate bit, then disables
automatic gating by setting No-Auto. Disabled enable follows method 2: enable the
internal LDO, write factors, enable the PLL, then enable lock detection. Enabled
retune closes the output gate, writes factors and toggles lock detection off
then on. It never disables an enabled PLL or LDO.

The first lock observation occurs at least 3 microseconds after lock detection
is enabled. Three consecutive successful observations, each separated by at
least 3 microseconds, precede a 20 microsecond settling interval. At most 100
observations are allowed. The output gate opens only after settling and another
lock/gate-status check. Final readback verifies the output and effective gate.
Method 2 can produce an output-enable glitch according to the manual; excluded
consumers must remain excluded through output enable and eventual publication.

Only PLL control at CCU offset `0x2a0` and the dedicated gate at `0x1920` are
written. Read-modify-write preserves unrelated fields. The read-only lock bit
is excluded from writes. Native access rejects other write addresses, changes
to unrelated fields, PLL/LDO disable and factor changes while output is open.
This allowlist is an additional guard, not a replacement for startup authority.

Each poll does bounded work. Absolute deadlines and the shared monotonic
microsecond clock are checked around every operation, including after the final
callback. BUSY before effects permits another poll. Pending acquisition retains
all admitted tokens. Token replacement preserves both original and observed
identities for fault handling.

A failed read, write, readback, clock observation or deadline ends hardware
access immediately. Before unlock, the controller publishes failure and calls
the metadata-only retention callback while the mutex remains held. A failure
first observed after successful unlock retains tokens and terminal state but
cannot claim to retain the released mutex. An unsuccessful unlock leaves mutex
ownership uncertain. No terminal failure permits another device callback,
release, reset, rollback or retry.

## Required integration

Successful preparation retains exclusive PLL_NPU and NPU reservations. The
optional startup binding atomically replaces exclusive PLL ownership with stable
ownership and adopts the local NPU token. Parent, fabric and whole-platform
reservations remain live throughout. The source controller stays reserved for
the successor's lifetime. Releasing and reacquiring the PLL is not permitted.

The concrete provider must authenticate cold boot/remote handoff, register
security access, physical reference and parent state, independent recovery and
memory residency. It must share a monotonic clock with registry and mutex
operations. The existing scheduler-based register mutex cannot be assumed
available before DRAM and scheduler initialization. Reviewed entry conditions
and the selected recovery plan must precede a physical qualification attempt.

## Focused validation

The controller fixture uses the real common clock registry and modeled registers.
It checks both sequences, timed lock admission, retained exclusive ownership,
unchanged consumers, unsafe admission refusal, partial acquisition, token
replacement, time regression, write/readback failures and deadline failures on
both sides of unlock. It also checks bounded failure when the PLL never locks.
The native files are compiled for AArch64. These checks establish controller
behavior; they do not establish analog PLL stability, remote exclusion, safe
RAM clocks or a production cold boot provider.

The native live-DRAM composition is recorded under
`~/.cache/sunstoneos/evidence/npu-live-20261017/batch1/`. One affected native link
and the three changed cases passed on their first attempt. The successful case
executes the real PMIC provider, PLL enable, clock transfer, ON-domain startup
and service activation with modeled hardware responses. An OFF domain causes
retained local-startup refusal after PLL preparation; lock timeout retains
ownership without opening the output. No PCK or DRAM clock writes occur, and
SRAM reset remains deasserted. Existing controller and numerical suites were
not rerun. These results do not establish physical operation or inference.

The focused detected-reference checks are preserved under
`~/.cache/sunstoneos/evidence/accelerator-clock-reference-20261018/`.
Actual provider code accepts exact 26 MHz N96/M104 and 19.2 MHz N100/M80
profiles and retains wrong ratios, lost reference lock, and disabled or unstable
DCXO before writes. GPU/NPU PERI0 cases preserve reserved bit24; the NPU PLL
case also retains final control corruption without opening the output.
These modeled-register checks do not establish physical operation. No native
image was rebuilt; accelerator launch still requires a final image built from
the accepted startup and providers, plus actual resource and recovery admission.
