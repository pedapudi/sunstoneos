# GPU power control runs in the vendor's clock, supply and secure state

The A733 GPU power controller powers the GPU top-level domain (PCK600 domain
5) through a row of the [power-domain owner](power-domains.md) and the GPU
core domain (domain 6, CORE6) itself, with the GPU core clocked from PLL_GPU0
at 1008 MHz and DCDC4 at 960 mV. It follows the vendor order of
[the default GPU power design](designs/gpu-vendor-default.md), which states
each decision with its reference. The ON sequence is qualified on the board: an
image that applied the same EL3 security writes, supply and clock read the GPU
core identity `36.56.104.183` on port 3-1 (record `gpu-power-vendor-state` in
[the GPU power evidence](evidence/gpu-power.jsonl)). Every image that read the
identity without them stalled. The power-only diagnostic ran an OFF and a second
ON in one lifetime on port 3-1 (records `gpu-power-first-off`,
`gpu-power-second-run` and `gpu-power-third-run`). GPU operation remains
unqualified.

The implementation is [a733_gpu_power.c](../drivers/gpu/a733_gpu_power.c), with
its callback contract in [a733_gpu_power.h](../include/a733_gpu_power.h).
A privileged owner polls the controller. One poll runs the resource and
register steps back to back, with monotonic clock reads. The domain step
requests the row once and returns to the owner until the service task
answers; each later poll reads the answer with one acquire load. The two
PLL_GPU0 lock waits (at most 100 reads, 3 µs apart) and the two GPU clock
update-bit waits (at most 100 reads) run inside one poll; the two 20 µs waits
after each lock wait on the clock. A wait whose clock shows no advance for
1,024 consecutive samples (`A733_GPU_SPIN_STALLED_READS`) returns to the
owner without advancing the step, and the next poll restarts it. Every wait
ends at one absolute deadline. A refused row fails with
`A733_GPU_POWER_STATE`, a failed domain with `A733_GPU_POWER_DOMAIN`, a
PLL_GPU0 write that does not read back with `A733_GPU_POWER_PLL`, a lock
wait that ends unlocked with `A733_GPU_POWER_LOCK` and an update bit that
stays set with `A733_GPU_POWER_UPDATE`. An expired deadline, clock
regression, callback failure, or unexpected register value stops further
I/O. The controller performs no automatic rollback after a failure.

## Shared resources require an explicit owner

The resource manager must acquire the following before register preparation:

- Exclusive GPU LOCAL authority over the local clock, external reset,
  PLL_GPU0 (which no other device uses) and CORE6 policy/PWCR, with TOP5
  controlled only through the shared owner's row.
- DCDC4 at 960 mV. The GPU core clock runs from PLL_GPU0 alone, so the GPU
  takes no PERI0 lease.
- Shared leases for the oscillator, power-controller clock, and GPU AHB and
  memory-bus gates.

The operating point is the vendor device tree's `opp@1008000000` at its
`opp-microvolt-vfdefault`, 960 mV ([a733_gpu_opp.h](../include/a733_gpu_opp.h),
which also holds the node's 21 speed-bin voltages, 860–960 mV). Before the
GPU's first ON the [GPU platform](../drivers/gpu/a733_gpu_platform.c) takes an
exclusive DCDC4 lease, reads the selector and, at the power-on 800 mV, writes
960 mV through the PMIC provider with a readback, releases the lease and waits 1.2 ms. That is the regulator's 1 ms ramp for 160 mV at 250 µV/µs plus the vendor's
200 µs. The provider admits a DCDC4 write only to a voltage of the speed-bin
row. The supply consumer then samples DCDC4 through the provider and holds it
shared; the resource manager requires 960 mV. DCDC4 stays at 960 mV after OFF,
as in the vendor driver, and the board marks it always-on.

The controller checks that the shared bus gates and power-controller clock are
enabled. It refuses an inherited GPU whose core clock, bus clock or external
reset indicates possible execution, or whose PLL_GPU0 is enabled. It does not
attempt to take over a running GPU. Resource acquisition can have effects
before that refusal; the failed controller retains its diagnostics and does
not release resources automatically.

Register operations use these address spaces:

| Register bank | Physical base | Fields used |
| --- | --- | --- |
| Main clock controller | `0x02002000` | PLL_GPU0 `0x0e0`, AHB gate `0x5c0[7]`, memory-bus gate `0x5e0[16]`, GPU clock `0xb20`, bus gate/reset `0xb24[0,16]` |
| Always-on clock controller | `0x07010000` | Power-controller gate `0x1ac[0]` |
| Shared TOP power controller | `0x07060000` | Domain 5 policy `0x5000`, status `0x5008`, sequencing delays, through its row |
| GPU-owned CORE power controller | `0x07066000` | Policy `0`, status `8`, PWCR `0x20`; MISR `0x14` and IDR0 `0xfb0` read only |

Shared gates are read-only in this controller. The platform's register-update
callback must preserve unrelated fields atomically under the shared register
lock. GPU clock writes include the update bit at bit 27, which clears when the
configuration becomes valid; ON waits for it to clear.

## Startup and shutdown follow the vendor order

ON records the GPU AHB and MBUS master gate words (CCU `0x5c0` and `0x5e0`)
and checks the power-controller clock, the GPU clock word at gate closed,
source 0 (PLL_GPU0) and divider 1, the bus gate and reset closed and PLL_GPU0
disabled. It then captures CORE6 policy, status and PWCR once.
Whole policy/status words must match exactly at 0 or 8, or be the power-up
state (policy 8, status 0, PWCR `0x101`). TOP5 is ON from reset, so these
reads reach CORE6's PPU before any domain request. It then:

1. writes PWCR 0;
2. enables PLL_GPU0 as `ccu_pll_gate_helper_enable` does: output gate
   cleared, LDO, enable and lock enable set, the lock wait, 20 µs, output
   gate set;
3. releases the GPU bus reset, opens the bus gate, opens the core clock gate
   with the update bit and writes the update bit again, as
   `sunxi_enable_clks_wrap` does;
4. sets PLL_GPU0 to 1008 MHz (N 84, P0 2) as `ccu_nm_set_rate` does: output
   gate cleared, factors written, lock enable cleared and set, the lock
   wait, 20 µs, output gate set;
5. requests the domain-5 (TOP5) row ON with the GPU clocked, as
   `sunxiPrePowerState` does after `sunxi_enable_clks_wrap`;
6. records both master gate words again and requires the AHB gate (bit 7)
   and the MBUS gate (bit 16) open;
7. writes CORE6 policy 8 and polls CORE6 status until it reads 8, before
   permitting GPU register access.

The master gates are checked only after TOP5 ON because a TOP5 OFF closed
the AHB gate on a board, which then took no write while TOP5 was OFF. The MBUS gate was not read and is treated the same way
([TOP5 OFF closes the master gates](#top5-off-closes-the-gpu-master-gates)).
The vendor never reads or writes them after boot (`CLK_IS_CRITICAL`,
`ccu-sun60iw2.c:587–590, 658–661`).

Every write is read back. At power-up the CORE6 policy already reads 8 with
its transition outstanding; the board showed the transition completes once
the GPU is clocked. The vendor never reads CORE6 status; the poll is this
controller's check.

OFF requires stopped firmware, closed job admission, and masked GPU
interrupts. It runs `sunxiPostPowerState`'s order: CORE6 policy 0 and a poll
for status 0, then the TOP row OFF, both while the GPU is still clocked; the
owner waits for domain status 0. It records both master gate words, then
closes the bus gate and the core clock gate, disables PLL_GPU0 as
`ccu_pll_gate_helper_disable` does (output gate, lock enable, enable, LDO,
each read back), asserts the reset and records the gate words again. The
controller then releases its shared-resource leases through the resource
manager, which also verifies CORE6 policy and status zero and PLL_GPU0
disabled. The owner admits a domain-5 transition whatever the GPU clock
state, as the vendor makes both with the GPU clocked, and admits a domain-5
ON from OFF with the master gates closed.

A failure stops at its step and leaves the state that step left, which the
controller reports as its error and step:

- an ON failure before step 2 leaves the GPU gated, in reset and PLL_GPU0
  disabled, with at most PWCR 0 written;
- an ON failure in steps 2 to 4 leaves the clocks and PLL_GPU0 as far as
  they got, with TOP5 untouched (ON from reset);
- an ON failure at step 5, 6 or 7 leaves the GPU clocked at 1008 MHz, with
  the TOP5 row as the owner answered it; a gate that stays closed after TOP5
  ON fails at step 6 with error STATE and the word it read;
- an OFF failure at CORE6 or TOP5 leaves the GPU clocked, so the handshakes
  that need the clock can still complete in a later attempt;
- an OFF failure in the clock steps leaves CORE6 and TOP5 OFF and the
  clocks as far as they got.

Every failure retains the leases and the TOP5 row and makes no further
access.

## TOP5 OFF closes the GPU master gates

A domain-5 OFF closes the GPU AHB master gate (CCU `0x5c0`, bit 7) and the
MBUS master gate (CCU `0x5e0`, bit 16): the AHB word reads `0x1101037f` and the
MBUS word `0xf1045803`. The gates do not take a write while TOP5 is OFF. A
domain-5 ON from OFF reopens them (`0x110103ff` and `0xf1055803`). The vendor
never touches them (`CLK_IS_CRITICAL`, `ccu-sun60iw2.c:587–590, 658–661`;
`ccu_gate_init` skips critical gates, `ccu_gate.c:129`), and its power-on after
an OFF is a bare policy write with the gates closed. The driver therefore:

- leaves the gates alone in the resource manager while TOP5 is OFF;
- admits a domain-5 ON from OFF with the gates closed in the power-domain
  owner;
- requires both gates open after TOP5 ON, and records both words at ON entry,
  after TOP5 ON, after TOP5 OFF and at the end of OFF (the `GPU gates` lines).

The owner rewrites the domain's delay row before each ON from OFF and reads each
word back. After a domain OFF the `+0xc10` word reads `0x00ff0008` in TOP5's and
in CORE6's rows, and a write of 8 keeps bits 23:16, so the owner compares only
the low 16 bits. The vendor writes the row once at registration
(`sunxi_pd_init`, `pck600_domains.c:126–142`) and never reads it back. The
GPU's TOP5 node has no `ppu-always-on` (`sun60iw2p1.dtsi:766–768`), so
`sunxiPostPowerState` (`sunxi_platform.c:361,379`) powers the domain off once
the NSI's GPU master device, which shares the domain, has autosuspended
(`sunxi-nsi.c:1203–1215`).

The supply consumer keeps its completed DCDC4 sample in a provider request slot,
with its rail borrow, so that every later admission can check it against the
provider (`a733_pmic_consumer.h`). The vendor reads the regulator voltage once,
at probe (`regulator_get_voltage`, `sunxi_platform.c:839`), and holds only the
handle. After an OFF, `a733_gpu_platform_supply_end` ends the completed sample
while keeping the shared DCDC4 token; a later begin samples again. The
power-only and identity clients call it after their last OFF, before the park.
Three board runs of the power-only diagnostic produced these rules (records
`gpu-power-first-off`, `gpu-power-second-run` and `gpu-power-third-run`).

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

No host check runs the controller against a modeled PLL_GPU0 whose lock bit
follows its enable bits. The design leaves PLL_GPU0 at `0xf9105300` after ON and
`0x01105300` after OFF, keeps unrelated fields and shared gates. It follows the vendor order: the reset released before the bus gate, the bus gate before the
core gate, the PLL output gate opened at least 20 µs after each lock. The order continues with PLL_GPU0 disabled only after the core gate closes, the reset asserted after both gates
close, and the GPU clocked at both CORE6 policy writes. Two further sets have
no host check:

- 253 retained faults: every callback failure, ignored PLL_GPU0 writes, a lock
  that never sets or is late or dropped, an update bit that stays set, refusals
  of a GPU clock off its PLL source, of a running PLL and of a PLL input
  divider M1 of 2, stalled transitions, and a stopped clock that returns at the
  first lock wait without advancing.
- The CORE6 entry states: four inherited TOP/CORE pairs with reopening, eight
  refused entries, ten partial samples, three power-up completions, two invalid
  observed statuses and eight bounded acknowledgement cases.

The [supply-raise check](../checks/a733-gpu-supply-raise.c) runs the GPU
platform's admission with the actual PMIC provider, rail ledger, supply
consumer and power-domain owner. DCDC4 at 800 mV is written to 960 mV once, with bit 7 kept, and the consumer samples it only after the 1.2 ms settle. DCDC4 already at 960 mV is not written. A write submit that meets a busy
provider is retried. DCDC4 at 860 mV is refused with no write, its snapshot
reaped and the lease released. After an owner's boot check that refused
DCDC4, the platform fails before it takes a lease.

The no host check (evidence: docs/evidence/power-domains.jsonl power-domains-implementation-notes; docs/gpu-power.md) compiles the original
power-domain initialization and transition functions with a register model. All
emitted TOP writes match the trace of one GPU power-on and power-off through
the real owner, which `checks/power-domains-pck.c` writes. It reads the BXM
`sunxi_platform.c` (identified by its Git blob) and the pinned CCU helpers, and
requires the CORE6 writes, the GPU clock and reset order, the PLL_GPU0 bits
and the 1008 MHz factors of every native CCU write to follow them. It also
verifies register bases, the board's GPU supply assignment, the always-on rail
requirement and the 22-entry voltage row of `a733_gpu_opp.h` against the
device tree's `opp@1008000000`.

References come from the [pinned board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f):

- `bsp/drivers/pm_domain/pck600_domains.c` and
  `bsp/include/dt-bindings/power/sun60iw2-power.h` define domain identifiers,
  sequencing delays, policy requests, and acknowledgement fields.
- `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c`, `ccu_gate.c`, `ccu_nm.c` and
  `ccu_common.c` define clocks, gates, the PLL helpers, the reset, the
  divisor table and the update key.
- `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` and
  `sun60i-a733-orangepi-zero3w.dts` define addresses, supply wiring, and operating
  points.
- The BXM DDK `services/system/rogue/rgx_sunxi/sunxi_platform.c` powers CORE6
  and orders the clocks at first power-on, later power-ons and power-off.

Downloaded files retain their original license notices. The native controller
uses independently written state transitions and source-derived register facts.
The reference checker preserves the original power-domain source notice in its
compiled extraction. Its off transition requires status zero; the original
helper accepts any status other than eight as off. The evidence of the default
path's checks is in `~/.cache/sunstoneos/evidence/gpu-vendor-default-20261025/`.

## Audit of the nonreturning identity probe

The source audit found one shutdown prerequisite. The reference
`pck600_domains.c:126–142,269–283` initializes five delay words at domain
registration, independently of the current power state. Its table and offsets
at lines 547–569 supply the native values. With an inherited ON, adoption skips
the ON command, so the owner initializes those words before the first OFF, with
readbacks and a fresh deadline check. The vendor fields at offsets `0xc00`,
`0xc04` and `0xc10` are not described by the PCK-600 TRM; the code follows the
pinned implementation without inventing their timing semantics. The owner's two
transition paths, 18 failure cases and deadline cases have no host check; the
earlier results are in
`~/.cache/sunstoneos/evidence/gpu-first-off-delays-20261024/snapshot/manifest.json`.
It keeps domain 4 untouched and retains ownership on uncertainty.

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
The [pinned official build recipe](https://github.com/orangepi-xunlong/orangepi-build/tree/a95129bf30ea11886cc35f08ebb3e69695a26656)
does establish its default current-kernel selection for Zero 3W. The board and
family configs select `linux-sun60iw2-current-a733.config`, whose lines 450 and
1124 disable `CONFIG_AW_DRM_POWERVR` and set `CONFIG_AW_GPU_TYPE="bxm"`.
`scripts/compilation.sh:528–530` builds the external `bsp/modules/gpu` target
against the kernel's actual `.config`. At the pinned kernel revision, that
Makefile selects BXM's `sunxi_linux` build and `pvrsrvkm.ko`; its
`PVR_SYSTEM=rgx_sunxi` selects the `sunxi_platform.o` adapter audited above.
The family script explicitly installs the module and runs `depmod` for desktop
Bullseye, Resolute and Trixie profiles (`sun60iw2.conf:137,162–166`). This
installation path does not establish packaging for other profiles.

BXM is therefore the default build reference, while the driver loaded by a
particular released image remains unverified. The recipe permits saved or user
configs and extension hooks, and the module Makefile gives explicit `GPU_TYPE`
precedence. The build also names a moving kernel branch; no image-specific
configuration, package manifest or loaded-module record binds a release to these
two source revisions. Source URLs, hashes and line excerpts are preserved in
`~/.cache/sunstoneos/evidence/gpu-power-id-diagnosis-20261024/reference-audit/driver-selection.json`.
Changing native reset order still requires separate justification.

The domain driver attaches clocks but does not set `GENPD_FLAG_PM_CLK`
(`pck600_domains.c:186–213,282`); generic domain code installs automatic clock
start/stop callbacks only with that flag (`drivers/base/power/domain.c:2084–2086`).

## CORE6 starts with an unfinished ON transition

CORE6 (PPU of the GPU core domain) reads policy 8 with status 0 and PWCR
`0x101` after reset, and stays there until the GPU is clocked in the vendor
state. The records `gpu-power-core6-admission-refusal`,
`gpu-power-core6-corrected-run`, `gpu-power-delay-row-probe` and
`gpu-power-wrapper-reads` hold the board runs that established this.

### The entry words are the PPU's power-up values

- **PWCR `0x101`.** DEN0051E Table 5-10 gives this reset value for a PPU
  with one device Q-Channel: DEVQACTIVE 0 enabled (bit 8) and the
  channel's handshake enabled (DEVREQEN bit 0).
- **Policy 8 with status 0.** A PPU configured with a default ON policy
  starts its transition to ON at reset and updates `PWR_STATUS` only when
  that transition completes (DEN0051E section 4.2.1 and Table 5-4, note a).
- **Other domains finish that transition.** On port 3-3, a read-only bundle
  run in a lifetime with no GPU or NPU power request found domains 1, 4
  and 5 at policy 8 and status 8.
- **No SunstoneOS writer.** The provider-readiness image that ran earlier
  in that lifetime makes no PCK access, and the three words match the
  reset values.

In that reset-time transition, and in any OFF-to-ON transition of a
Q-Channel PPU, the PPU powers the domain and raises the device Q-Channel
request. It releases the domain's reset only after raising that request,
and the transition completes when the device accepts (DEN0051E Figures
4-18 and 4-21). The device on CORE6's channel is the GPU. With its core clock gated
(`0xb20[31]` clear) and its bus reset asserted (`0xb24[16]` clear), the
GPU's Q-Channel logic is not running and cannot accept. A Q-Channel
transition to a higher-power mode cannot be denied (DEN0051E section 4.2.9),
so the PPU waits indefinitely with status 0.

The BXM adapter meets the same state. `sunxi_platform_init`
(`sunxi_platform.c:912–920`) requests TOP5, writes CORE PWCR 0 and policy
8, and only then enables the GPU clocks. With the channel disabled,
"transitions continue without issuing requests or waiting for responses on
the disabled channels"
(DEN0051E section 4.1.3). The SCP fragment described under CORE ownership
below, which writes policy 8 to every row but skips only CORE6's status
poll, is consistent with the same behaviour.

### Native admission accepts the power-up state

The controller admits CORE6 when its policy and status words are both 0 or
both 8, or when policy is 8, status is 0 and PWCR equals
`A733_GPU_CORE_RESET_PWCR` (`0x101`). Any other PWCR with policy 8 and
status 0 means another writer changed CORE6, and the controller refuses it
before any write. In the admitted power-up state, the PWCR write happens
while a transition is outstanding, which DEN0051E section 5.2.8 calls
UNPREDICTABLE. The vendor adapter performs that write before any GPU clock,
so it meets the same pending transition unless earlier boot firmware
completed it. No other documented sequence leaves the pending transition.

After admission the controller writes the vendor CORE6 delay row, then
PWCR 0, policy 8, GPU parent, clocks, 1 µs, reset release, then a bounded
poll for status 8 before any GPU register access. After the delay row and
after each of the two other CORE writes the controller reads CORE status
once, and from the power-up state it re-reads for up to 50 µs until the
status is 8. These observations require no value. They record
the status and the microseconds waited, and the sequence continues either
way. The final poll alone decides acknowledgement.

### What the vendor code does around the CORE6 write

- **BXM adapter** (`sunxi_platform.c:863–945`). It enables the GPU
  regulator and sets its voltage, requests TOP5, writes CORE6 PWCR 0 and
  policy 8, then enables the GPU PLL (`clk_parent`, `CLK_PLL_GPU0`) and the
  PERI0 800, 600, 400, 300 and 200 MHz outputs. It then releases reset and opens
  the bus and core clocks, and sets the update bit. It never reads CORE6.
  SunstoneOS does the same, with the GPU clock from PLL_GPU0 at 1008 MHz.
  Each power-on of the device also calls `sunxi_secure_config` just
  before `RGXStart`; it writes the SMC0 word `0x0a0000b0` and the
  wrapper's `drm_ctrl`
  ([below](#the-vendor-writes-the-wrapper-only-through-the-secure-configuration)).
  The vendor's first GPU core read precedes it
  ([below](#the-vendor-writes-the-wrapper-only-through-the-secure-configuration)).
- **Vendor domain table** (`pck600_domains.c:554`). It pins a CORE6 delay
  row: `0x1f1f1f`, `0x1f1f`, `0x08080808`, `0x0808` and `8` at `0x170`,
  `0x174`, `0xc00`, `0xc04` and `0xc10`, the same row as TOP5's.
  `sunxi_pd_init` writes a row only for a registered domain, and the device
  tree comments out the CORE6 node and its test node, so no vendor path
  writes the CORE6 row. The power-domain owner writes TOP5's row before
  each TOP5 ON from OFF; nothing writes CORE6's row.
- **SCP firmware**, file offsets `0x5e7e–0x5f80`, in a function whose
  messages name standby resume. It clears bits 3 and 8 of R_CCU `0x250`
  (mask `~0x108`) and leaves bit 0 set; `ccu-sun60iw2-r.c` names bits 3 and
  8 the `vdd-sys2usb` and `vdd-usb2cpus` gates and bit 0 the `vdd-ddr`
  gate. It then ORs policy 8 into rows 0 to 10, polls every status except
  CORE6's, and re-enables a table of PLLs. Nothing in it is specific to
  CORE6 apart from the skipped poll. The reference backsolve
  (`docs/reference-backsolve-gpu.md`) confirms this against the SCP blob at
  `0x5e9a`: bit 0 is not cleared.
- **PRCM.** The register list of manual section 4.2.4 has no GPU power
  switch. The PPU bus register `0x1ac` has a gate bit and no reset bit. The
  gate is open: the PPU registers respond and TOP5 works.
- **Interrupt masks.** `PPU_IMR` and `PPU_AIMR` mask interrupt events only
  (DEN0051E section 5.2.12); they do not gate transitions.

### The vendor writes the wrapper only through the secure configuration

The GPU wrapper registers at `0x01880000` (`sunxi_dvfs_ctrl.c:113–150`) are
`+0x100` `pwr_cmp_dly` (16-bit up and down counts) and `+0x104` `pwr_qchan_cfg`
(for each core a software-control enable and the `q_active`, `q_accept` and
`q_deny` signals, and a core-0 clock enable). No active vendor path writes
either: the DVFS block that would write other wrapper words is compiled only
with `SUNXI_DVFS_CTRL_ENABLE`, which no cached build defines, and its only
`pwr_cmp_dly` value (`0x00640064`) is commented out. The only wrapper write of
the active build is `drm_ctrl` 0 at `+0x14` in `sunxi_secure_config`
(`sunxi_platform.c:964–979`), which also writes 2 to the SMC0 word `0x0a0000b0`
(`SMC_DRM_GPU_HW_RST`) and runs immediately before `RGXStart`. No reference
shows software driving CORE6's Q-Channel.

The vendor reads the core identity (`RGX_CR_CORE_ID__PBVNC`, `0x01800020`, as two
32-bit loads `+0x24` then `+0x20`) at probe, after `sunxi_platform_init` has
powered TOP5, written CORE6 PWCR 0 and policy 8, clocked the GPU at 1008 MHz and
set the supply, and before any secure configuration. It reads the core timer
(`0x01800160`) before `RGXDoStart` on every later power-on. The full trace is in
record `gpu-power-vendor-identity-trace`; the reference sources are listed in
`~/.cache/sunstoneos/evidence/gpu-wrapper-reference-20261025/sources.json`.

### The probe-read probe makes the vendor's probe-time read, then the secure configuration

The probe-read variant of `apps/gpu-power-id` runs the default power path, reads
the identity as the vendor's probe does, makes the vendor's secure configuration
(SMC0 `0x0a0000b0` = 2, then wrapper `drm_ctrl` = 0), reads the identity again
and restores both words. Every access is a 32-bit Device-nGnRnE access with
`DSB SY` before and after, preceded by a progress record in the result
area. Its write window masks D, A, I and F at EL1 and points `VBAR_EL1` at vectors that restore both words on any synchronous exception. It starts only if at
least 500 ms remain before the monitor's 2 s recovery, which is a FIQ routed to
EL3 and cannot be masked. The coordinator accepted the two writes on 2026-10-25
because the vendor makes them on every GPU power-on. The first run did not
return, and the later default-path runs read the identity (record
`gpu-power-probe-read`, and `gpu-firmware-start-probe-read` in the
[firmware-start evidence](evidence/gpu-firmware-start.jsonl)).

### The wrapper-write probe was refused

A probe that wrote `pwr_cmp_dly` and `pwr_qchan_cfg` in three stages, the last
holding core 0's `q_accept` HIGH by software, was refused before any board run:
no vendor source writes those values, and no reference shows software driving
CORE6's Q-Channel.

### The vendor-state run reads the identity

An image that applied the vendor's clock, supply and secure state read the
identity `36.56.104.183` (loads `0x00240038` and `0x006800b7`) on port 3-1, and
CORE6 then read status 8 with its MISR showing the device accept (`0x100`).
The state is 28 of BL31's security writes at EL3, DCDC4 at 960 mV with 1.2 ms of
settling, CORE6 PWCR 0 and policy 8, PLL_GPU0 enabled, the GPU reset, bus gate
and core gate opened on PLL_GPU0, and PLL_GPU0 at 1008 MHz. Which of the three
differences from the earlier images is necessary is not separated. The default
path applies this state ([the default design](designs/gpu-vendor-default.md)).
Both PLL_GPU0 lock waits ended after four reads and DCDC4's selector read `0xae`
(960 mV). The record is `gpu-power-vendor-state`.

<a id="an-unpowered-gpu-core-is-the-leading-hypothesis-for-both-failed-gpu-runs"></a>
Two earlier GPU images (the power/identity probe and a finite computation)
never returned on port 3-1 and port 3-3. They never wrote CORE6 policy 8 before
touching the GPU register window, and the vendor-state run explains their stall
(records `gpu-power-unpowered-core-hypothesis` and
`gpu-power-identity-probe-no-return`).

### The CORE6 delay-row writes are removed

The vendor never writes CORE6's delay row: its CORE6 node is commented out, so
`sunxi_pd_init` does not run for it. The board read the vendor values at reset
in both CORE6's and TOP5's rows, and the vendor-state image, which left the row
unwritten, reached CORE6 ON. The controller therefore writes no CORE6 delay
word, and its native adapter neither reads nor writes one.

A failed power-only lifetime leaves TOP5 ON with its row unreleased and CORE6 in
the state it held (record `gpu-power-failed-lifetimes`). The queue's quarantine
closes these lifetimes, and a power removal resets the PCK and CCU.

## CORE ownership follows the selected BXM adapter

The selected `rgx_sunxi` source defines `USE_FPGA` unconditionally at
`sunxi_platform.c:70`; the macro name does not make this an excluded FPGA path.
`SysDevInit` calls this adapter (`sysconfig.c:281`) and installs its later
Pre/Post callbacks (337–338). Initial setup requests TOP runtime power, writes
CORE PWCR zero and policy 8, then enables local clocks (911–922). The later Pre
callback enables clocks before TOP/CORE requests (345–354); Post writes CORE
policy zero before TOP runtime suspend and clock teardown (374–380). The CORE
node is commented out in `sun60iw2p1.dtsi:766–774`, so the generic domain driver's
delay setup does not cover CORE. Native code writes no CORE delay registers.
The native controller performs the omitted CORE operations.

The PCK TRM identifies policy/status/PWCR at offsets 0/8/0x20. Arm
[DEN0051E section 5.2.8](https://documentation-service.arm.com/static/5f873177f86e16515cdb6d99)
requires a settled static power policy before changing device-request enables.
Native admission therefore compares complete policy/status words, rejecting
dynamic, locked, operating-mode or mismatched values before PWCR writes, with
one exception: the state that CORE6 holds after power-up, described in the
next section. PWCR zero disables the device handshake for transitions
(DEN0051E section 4.1.3, "Device Interface Channel Enables"); it does not prove
transaction drain. The existing firmware-stop or admitted never-booted
prerequisite remains necessary for OFF.

The reference helpers do not await CORE acknowledgement. Native code adds a
conservative completion check. Each poll re-reads CORE status for up to
`A733_GPU_SPIN_US` (50 µs) while the clock advances, with a fresh monotonic
deadline check after every read, and then returns AGAIN. A delayed or unavailable CORE status
may fail this criterion on hardware. ON is checked after local clock/reset
progress; OFF is checked before teardown. Timeout, failed access or unexpected
status retains the TOP row, rail pin and GPU leases without rollback.

A host inspection of the pinned
[SCP firmware](https://raw.githubusercontent.com/orangepi-xunlong/orangepi-build/a95129bf30ea11886cc35f08ebb3e69695a26656/external/packages/pack-uboot/sun60iw2/bin/scp.fex)
(SHA-256 `07e6b97628101963e7944e012948c4bdf721f914bad347acd7dfbde89db42749`)
provides supporting context, not a new SunstoneOS runtime dependency. File
offsets `0x5ec6–0x5ee8` write policy OR 8 to every row 0–10, including CORE6,
but skip only CORE6's status poll. The fragment has no identified caller or
proven execution on these boards and provides no OFF/drain proof. It establishes
neither eventual acknowledgement nor actual inherited state. Its existing
provenance and decoded bytes are preserved in
`~/.cache/sunstoneos/evidence/npu-low-power-source-20261024/source-search.json`.

With immediate acknowledgement, CORE control costs two CORE writes and
eleven reads on ON, then one write and two reads on OFF, plus two release
reads. Five of the ON reads are diagnostic: identification (`PPU_IDR0`,
`0xfb0`), the accept and deny inputs (`PPU_MISR`, `0x14`) before any write
and before the final status poll, and the status after the PWCR write and
after the policy write. The native adapter permits the two diagnostic
offsets for reads only. A waiting poll adds at most 50 µs of status reads,
one controller clock sample per read. Native writes use the existing shared
register lock. The power structure holds the cached policy, status and PWCR,
the five diagnostic words, the PLL lock and clock update read counts, the
last PLL_GPU0 word and a seen mask. The mask records completed reads,
including a read followed by deadline rejection, and is not admission
authority. The power/identity diagnostic
prints those cached values and the failure step without accessing hardware
again.

Focused results for the CORE6 observations and the pre-default controller are
in record `gpu-power-core-host-checks`; they are software checks, not physical
qualification.
