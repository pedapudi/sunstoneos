# GPU power control still needs physical qualification

The A733 GPU power controller prepares the GPU at 400 MHz and requests power
for its top-level domain (PCK600 domain 5) through a row of the
[power-domain owner](power-domains.md). The exclusive GPU owner also controls
CORE (domain 6). It supports bounded startup, shutdown, and reopening.
The native platform integrates the shared clock and rail owners and the FreeRTOS
GPU service. Host checks and an AArch64 compilation pass. A physical power/identity
attempt on 2026-10-24 did not return; no startup, identity or shutdown phase was
observed. A later power-only run returned after refusing the power-up state
of CORE6, which the controller now admits
([CORE6 starts with an unfinished ON transition](#core6-starts-with-an-unfinished-on-transition)).
GPU operation remains unqualified.

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

- Exclusive GPU LOCAL authority over the local clock, external reset and CORE6
  policy/PWCR, with TOP5 controlled only through the shared owner’s row.
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
| Shared TOP power controller | `0x07060000` | Domain 5 policy `0x5000`, status `0x5008`, sequencing delays, through its row |
| GPU-owned CORE power controller | `0x07066000` | Policy `0`, status `8`, PWCR `0x20`; ordered 32-bit accesses |

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
for that exact status. After the row reads ON, the controller captures CORE
policy, status and PWCR once. Whole policy/status words must match exactly at
0 or 8 before it writes PWCR zero and policy 8, with exact readbacks. It then
selects the 400 MHz peripheral parent with divisor one, enables core and bus
clocks, waits at least one microsecond and releases external reset. Only then
does it check CORE status 8, before permitting GPU register access. Relevant writes receive readback
checks. The selected clock requires only 80 ns for 32 cycles, so the one
microsecond delay covers the reference driver's settling requirement.

Shutdown requires stopped firmware, closed job admission, and masked GPU
interrupts. It writes CORE policy zero and checks CORE status zero while TOP
and local clocks remain available. It then asserts external reset, disables
the local bus and core clocks, and requests the TOP row OFF. The owner rereads the gate and reset closed, initializes
the delay row if needed, requests domain state zero and waits for that exact status. It then releases
shared-resource leases through the resource manager, which also verifies whole
CORE policy/status words are zero. The controller allows no
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
reset release followed the clock gate by 50 counter ticks, at least the 24 that
one microsecond requires. A stand-in for the owner answers the row from the
modeled power-controller words. The first poll checks the preconditions and
requests the row without waiting; the poll after the answer runs every
remaining step, including 10 µs register updates. An unanswered row keeps the
controller at the domain step with no register write, and with a stopped
clock the settling delay returns without advancing and completes once the
clock runs. The check covers 101 retained faults, including every callback
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


## CORE6 starts with an unfinished ON transition

### The power-only run stopped at admission

The power-only image (ELF `81d4a728…`) ran on port 3-1 on 2026-10-24 after
a passing provider-readiness run in the same powered lifetime. It returned
to the ROM through the monitor with status 1 about 2 s after launch. Its log
was:

```
GPU power phase=5 error=5 step=8 last=0x101
GPU CORE entry seen=7 policy=0x8 status=0x0 pwcr=0x101
FAIL GPU ON poll
```

Against that image's step table, phase 5 is FAILED and error 5 is STATE.
Step 8 is the admission step, which reads PWCR (`0x101`, the `last` value)
and then requires the CORE policy and status words to be equal. Seen mask 7
records that the policy, status and PWCR reads all completed. TOP5 was ON,
because the domain step (step 5) had finished. The image refused CORE6
because its policy was already 8 while its status was 0. It wrote neither
PWCR nor policy, opened no GPU clock and released no reset. "FAIL GPU ON
poll" is the client's message for any failed ON poll; the final status poll
never ran.

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

### The corrected run never saw CORE6 ON

The corrected power-only image (ELF `4ade8fdf…`) ran on port 3-1 after a
cold DDR initialization and a passing provider-readiness run in the same
lifetime. The monitor returned status 1 about 1.5 s after launch, and the
runner exited at once with that result. The log was:

```
GPU power phase=5 error=6 step=27 last=0x0
GPU resources result=0 error=0 stage=13 last=0xf1055803
GPU CORE entry cycle=1 seen=255 policy=0x8 status=0x0 pwcr=0x101
GPU CORE inputs cycle=1 idr0=0x10130101 misr=0x0 misr_late=0x0
GPU CORE after cycle=1 pwcr_status=0x0 pwcr_us=51 policy_status=0x0 policy_us=51
FAIL GPU ON poll
```

- **Where it stopped.** Step 27 of that image is the final CORE6 status
  poll, and 0 was its last read. Error 6 is CANCELLED: the platform's clock
  check found the client's 1.5 s deadline expired during ON and cancelled
  the controller (`a733_gpu_platform.c`, `fail`). The poll therefore read
  status 0 for about 1.4 s.
- **What preceded it.** The entry was the power-up state, every
  observation completed (seen 255), and the resource acquisition succeeded.
  The PWCR 0 and policy 8 writes read back, so did the GPU clock selection
  of PERI0 400 MHz with the gate open, and the bus gate and reset release.
  The A733 manual (section 4.1.4.5) says that the clock register reads back
  a new selection only after the update has taken effect.
- **Observations.** Status stayed 0 for the full 50 µs after the PWCR
  write and after the policy write. MISR read 0 before any write and after
  reset release: no power-switch accept, no GPU Q-Channel accept, no deny.

This rules out the three explanations that predicted status 8 after the
PWCR write, after the policy write or after clock and reset progress.

With PWCR 0 the PPU drives the device request DEVQREQn permanently HIGH
(DEN0051E section 4.1.3), which asks a Q-Channel device to leave
quiescence. A running GPU Q-Channel interface would answer with
DEVQACCEPTn HIGH, which MISR bit 8 shows. That bit read 0 after reset
release, with the GPU clocked and out of reset. The GPU therefore never
accepted: its Q-Channel logic is unpowered, which is the case if CORE6's
domain never powered up, or it is not wired to this PPU.

`PPU_IDR0` reads `0x10130101`. Decoded against DEN0051E Table 5-26:

- **DEVCHAN 1** (bits 3:0): a Q-Channel PPU with one device channel,
  which matches the PWCR reset value `0x101`.
- **NUM_OPMODE 0** (bits 7:4): one operating mode, so no operating-mode
  control.
- **Static modes** (bits 8, 16, 17): OFF, ON and WARM_RST. No OFF_EMU,
  MEM_RET, MEM_RET_EMU, LOGIC_RET, MEM_OFF, FULL_RET, FUNC_RET or
  DBG_RECOV (bits 9 to 15 and 18 are 0).
- **Dynamic modes** (bits 20 and 28): dynamic OFF and dynamic ON, and no
  other dynamic mode. The policy word 8 has `PWR_DYN_EN` clear, so the PPU
  runs static transitions.

### What the vendor code does around the CORE6 write

- **BXM adapter** (`sunxi_platform.c:863–945`). It enables the GPU
  regulator and sets its voltage, requests TOP5, writes CORE6 PWCR 0 and
  policy 8, then enables the GPU PLL (`clk_parent`, `CLK_PLL_GPU0`) and the
  PERI0 800, 600, 400, 300 and 200 MHz outputs, releases reset and opens
  the bus and core clocks, and sets the update bit. It never reads CORE6.
  SunstoneOS does the same apart from the GPU PLL: its GPU clock runs from
  PERI0 400 MHz, and the readback above shows that selection in effect.
  Each power-on of the device also calls `sunxi_secure_config` just
  before `RGXStart`; it writes the SMC0 word `0x0a0000b0` and the
  wrapper's `drm_ctrl`
  ([below](#the-vendor-code-never-writes-the-q-channel-or-delay-registers)).
  The vendor's first GPU core read precedes it
  ([below](#the-vendor-reads-the-core-identity-before-any-secure-configuration)).
- **Vendor domain table** (`pck600_domains.c:554`). It pins a CORE6 delay
  row: `0x1f1f1f`, `0x1f1f`, `0x08080808`, `0x0808` and `8` at `0x170`,
  `0x174`, `0xc00`, `0xc04` and `0xc10`, the same row as TOP5's.
  `sunxi_pd_init` writes a row only for a registered domain, and the device
  tree comments out the CORE6 node and its test node, so no vendor path
  writes the CORE6 row. The power-domain owner writes TOP5's row before
  each TOP5 ON from OFF; nothing wrote CORE6's row in either run.
- **SCP firmware**, file offsets `0x5e7e–0x5f80`, in a function whose
  messages name standby resume. It clears bits 0, 3 and 8 of R_CCU
  `0x250`, which `ccu-sun60iw2-r.c` names the `vdd-ddr`, `vdd-sys2usb` and
  `vdd-usb2cpus` gates. It then ORs policy 8 into rows 0 to 10, polls every
  status except CORE6's, and re-enables a table of PLLs. Nothing in it is
  specific to CORE6 apart from the skipped poll.
- **PRCM.** The register list of manual section 4.2.4 has no GPU power
  switch. The PPU bus register `0x1ac` has a gate bit and no reset bit. The
  gate is open: the PPU registers respond and TOP5 works.
- **Interrupt masks.** `PPU_IMR` and `PPU_AIMR` mask interrupt events only
  (DEN0051E section 5.2.12); they do not gate transitions.

### The delay-row probe ruled out the delay row

The power-only image `ade71834…` ran on port 3-1 in a new lifetime, after a
power removal, a cold DDR start and a passing provider-readiness run. Each
ON wrote the vendor CORE6 delay row and read it back, and the client took
a 35-word snapshot before the first ON and after it. The ON failed as
before: step 38, the final status poll, read 0 until the client deadline
(error 6, CANCELLED), and status stayed 0 for 51 µs after the delay row,
after PWCR 0 and after policy 8.

Snapshot 0, taken before any write, already held the vendor row in CORE6
(`0x1f1f1f`, `0x1f1f`, `0x08080808`, `0x0808`, `8`) and the same row in
TOP5. The row therefore holds the vendor values from reset, and writing it
changed nothing. The delay-row explanation is ruled out.

The CORE6 words, decoded with DEN0051E chapter 5:

| Offset | Register | Before ON | After ON | Meaning |
| --- | --- | --- | --- | --- |
| `0x000` | `PPU_PWPR` | `0x8` | `0x8` | Static policy ON; dynamic transitions, lock and operating policy off |
| `0x004` | `PPU_PMER` | `0x0` | `0x0` | Emulation off |
| `0x008` | `PPU_PWSR` | `0x0` | `0x0` | Status OFF, no dynamic or lock status |
| `0x010` | `PPU_DISR` | `0x1` | `0x1` | `DEVQACTIVE` of Q-Channel 0 is HIGH |
| `0x014` | `PPU_MISR` | `0x0` | `0x0` | No `PCSMPACCEPT`, no `DEVQACCEPTn`, no `DEVQDENY` |
| `0x018` | `PPU_STSR` | `0x0` | `0x0` | Reserved for a PPU with one Q-Channel |
| `0x020` | `PPU_PWCR` | `0x101` | `0x0` | Reset value; the controller wrote 0 |
| `0x024` | `PPU_PTCR` | `0x0` | `0x0` | No device handshake for WARM_RST |
| `0x030` | `PPU_IMR` | `0x2` | `0x2` | Reset value: the static-accept event masked; static policy completion and static denial events enabled; lock and emulation bits reserved |
| `0x034` | `PPU_AIMR` | `0x6` | `0x6` | Reset value: dynamic accept and deny masked, unsupported-policy event enabled, policy-completion events reserved |
| `0x038` | `PPU_ISR` | `0x0` | `0x0` | No event: no static policy completion, no denial |
| `0x03c` | `PPU_AISR` | `0x0` | `0x0` | No event: policy 8 is supported |
| `0x160`, `0x164` | `PPU_EDTR0/1` | `0x0` | `0x0` | Reserved: no power-mode entry delays |
| `0x170`, `0x174` | `PPU_DCDR0/1` | `0x1f1f1f`, `0x1f1f` | same | Device control delays (clock-to-reset, isolation-to-clock, reset-to-status and the two OFF delays) of 32 PPU cycles each |
| `0xc00`, `0xc04`, `0xc10` | vendor | `0x08080808`, `0x0808`, `8` | same | Vendor row, outside DEN0051E |
| `0xfb4` | `PPU_IDR1` | `0x2` | `0x2` | Only software device-delay control: no entry delays, no lock, no retention RAM registers, no policy-completion events in `PPU_AISR` |

In TOP5's row, policy and status are 8, `PPU_DISR` is 0, `PPU_MISR` is
`0x100` (its device's `DEVQACCEPTn` HIGH, so its Q-Channel is running) and
PWCR is `0x101`. R_CCU `0x1ac` is 1 (PPU gate open) and `0x250` is 1. The
GPU PLL word `0x0e0` reads `0x41104500`: PLL disabled. The GPU clock word
`0xb20` changed from 0 to `0x83000000` (gate open, PERI0 400 MHz, divisor 0,
update bit cleared) and `0xb24` from 0 to `0x10001` (bus gate open, reset
released).

CORE6's `PPU_DISR` of 1 means that the device side of its Q-Channel drives
`DEVQACTIVE` HIGH: it signals that it needs the domain. The signal does not
gate a transition to a higher-power mode (DEN0051E section 3.4.2), and
dynamic transitions are off, so it does not explain the stall. It is
already HIGH before any write, with the GPU clock gated and its reset
asserted, while `DEVQACCEPTn` stays LOW and nothing denies. That pattern
fits a Q-Channel interface whose outputs come from logic that does not
run: an isolation clamp of an unpowered domain, or a wrapper input held in
a fixed state. It also means that the channel is connected to something,
because TOP5's channel, which is running and idle, reads `DEVQACTIVE` LOW.
`PPU_ISR` stays 0 with the static-completion and denial events enabled, so
no static transition completed and none was denied after reset.

The vendor GPU source names that device side. `sunxi_dvfs_ctrl.c:113–150`
defines the GPU wrapper registers at `0x01880000`, inside the GPU register
window: at `+0x100` a power-complete delay with 16-bit up and down counts,
and at `+0x104` a Q-Channel configuration with, for core 0 and core 1, a
software-control enable and the `q_active`, `q_accept` and `q_deny`
signals, and a core-0 clock enable. No vendor code writes either register;
the optional DVFS controller writes other words of the block, and none of
the cached build files defines its build option `SUNXI_DVFS_CTRL_ENABLE`.
These registers
are the closest documented candidate for the logic that answers CORE6's
Q-Channel and power-switch handshakes.

### The remaining explanations, ranked

1. **The GPU wrapper answers CORE6's handshakes and has not been brought
   up.** The wrapper's Q-Channel configuration and power-complete delay
   suggest that it produces CORE6's accept signals. Its handshake may have
   been held in reset when the PPU raised its request at power-up (the GPU
   bus reset was asserted), or it may need the core-0 clock enable or a
   power-complete delay that nothing has set. The evidence is the register
   layout, `DEVQACTIVE` HIGH with no accept, and the earlier vendor-side
   signs: the device tree comments out both CORE6 nodes, the SCP skips only
   CORE6's status poll, and BXM never reads CORE6.
2. **This PPU does not sequence CORE6's power switches at all.** They are
   switched with TOP5 or by the GPU itself, and CORE6 status never shows
   whether the GPU core is powered. This differs from the first explanation
   only in whether software can complete the handshake.
3. **The reset-time transition is wedged for good.** It may have started
   before TOP5 reached ON. A PPU in transition does not abort on a new
   policy, and no software reset of the PPU is documented.
4. **The PWCR write had no effect during the outstanding transition.**
   DEN0051E calls that write UNPREDICTABLE. The PPU would then still wait
   for the accept that `PPU_MISR` shows never came, which leaves the same
   question as the first explanation.
5. **GPU PLL, SCP mailbox, PRCM power switch, interrupt mask or PPU
   clock.** None is supported. The GPU clock ran from PERI0, the SCP block
   has no CORE6 step, the PRCM has no GPU switch, masks gate no
   transition, and the PPU gate is open. The delay row is ruled out above.

### The wrapper reads returned with every Q-Channel control unset

The coordinator accepted an exception to the power-only rule for four
reads of the GPU wrapper, with TOP5 ON and no CORE6 acknowledgement. The
wrapper-read variant of the diagnostic
([README](../apps/gpu-power-id/README.md#the-wrapper-read-variant-reads-four-gpu-wrapper-words))
runs the power-only ON sequence, waits at most 200 ms for CORE6 status 8,
and then makes the four reads, each a 32-bit Device-nGnRnE load named in a
progress record before it is issued. It writes nothing in the window.

It ran on port 3-1 on 2026-10-25 (session
`board-session-20261024-14/port-3-1`, after a power removal, a cold DDR
start and a passing provider-readiness run). CORE6 again stayed at status
0, the controller's deadline cancelled the final poll (step 38, error 6),
and all four reads completed:

| Address | Vendor name | Value | Meaning |
| --- | --- | --- | --- |
| `0x01880104` | `pwr_qchan_cfg` | `0x0` | Core 0's Q-Channel is not under software control; the `q_active`, `q_accept`, `q_deny` and core-0 clock-enable bits are all 0 |
| `0x01880100` | `pwr_cmp_dly` | `0x0` | Power-complete up and down delays of 0 |
| `0x01880018` | `mult_core_cnt` | `0x2` | Two GPU cores |
| `0x0188001c` | `mult_core_id` | `0x10` | Primary core 0, secondary core 1 |

The wrapper therefore answers 32-bit reads with TOP5 ON, the GPU clock
open, its reset released and CORE6 unacknowledged. Both earlier GPU images
that did not return read `0x01800020`, the core's identity register, in
the same CORE6 state; the wrapper is a different block from the core
registers, and these reads show that it is reachable.

The `q_active` bit reads 0 while CORE6's `PPU_DISR` reports `DEVQACTIVE`
HIGH. With software control off, the register bits are therefore software
override values, or the wrapper is not the source of CORE6's channel; the
reads alone cannot tell which.

### The vendor code never writes the Q-Channel or delay registers

The pinned BXM sources at `2ac08e8c7`
(`~/.cache/sunstoneos/evidence/gpu-wrapper-reference-20261025/sources.json`
records each file's URL, revision and SHA-256) define the wrapper registers
and write three of them, none of which is `pwr_qchan_cfg` or `pwr_cmp_dly`.

Register layout, `sunxi_dvfs_ctrl.c:113–150`, at `0x01800000 +
SUNXI_DVFS_CTRL_OFFSET` (`0x80000`, `sunxi_dvfs_ctrl.h:33`):

| Offset | Name | Fields (bit) |
| --- | --- | --- |
| `+0x100` | `pwr_cmp_dly` | `pwr_dly_up` (15:0), `pwr_dly_down` (31:16) |
| `+0x104` | `pwr_qchan_cfg` | core 0: `sw_cfg_en` (0), `q_active` (1), `q_accept` (2), `q_deny` (3); core 1: the same at 4–7; `core0_clk_en` (8) |

The vendor writes, in the order of one GPU power-on:

1. **Probe** (`sunxi_platform_init`, `sunxi_platform.c:863–945`). The
   DVFS block at 883–898 is compiled only with `SUNXI_DVFS_CTRL_ENABLE`
   (`Kbuild.mk:52–54`), which no cached build defines. If enabled, it
   would write the wrapper before the regulator and before TOP5:
   `vf_level[i]` (`+0x20` onward, `sunxi_dvfs_ctrl.c:170`), `timeout_dbg`
   `0x1ff` (303), `dvfs_dbg` 1 (305), `irq_ctrl` 0 (307) and `dvfs_ctrl`
   `0x1003` (318). Line 309, `pwr_cmp_dly = 0x00640064` (100 up, 100
   down), is commented out; it is the only `pwr_cmp_dly` value in any
   vendor source. The DVFS interrupt handler, compiled under the same
   option, writes `irq_ctrl` 0 (203) and only prints the two registers
   (218–219). The active probe then enables the regulator (899),
   requests TOP5 runtime power (912), writes CORE6 PWCR 0 and policy 8
   (915–916, functions at 72–91) and enables the GPU clocks: PERI0
   outputs, reset release (203), bus clock (208), core clock (213) and the
   update bit `0x02002b20` bit 27 (220).
2. **System pre-power** (`sunxiPrePowerState`, 316–359), when the runtime
   state is OFF: clocks and reset (349), TOP5 (350), CORE6 policy 8 (352).
   The DDK calls it after the device pre-power callback and before the
   post-power callbacks (`power.c:743–873`).
3. **Device post-power** (`RGXPostPowerState`, `rgxpower.c:1010–1037`)
   calls `RGXDoStart` (865–889). Without `SUPPORT_TRUSTED_DEVICE` it calls
   `pfnTDSystemSecureConfig`, which `sysconfig.c:371` sets to
   `sunxi_secure_config` (`sunxi_platform.c:964–979`), and then `RGXStart`,
   the register accesses of the GPU start sequence; the core identity and
   timer reads come earlier (next section). `sunxi_secure_config` writes 2 to
   `0x0a0000b0` (`SMC_DRM_GPU_HW_RST`, in the SMC0 DRAM firewall) and 0 to
   wrapper `+0x14` (`drm_ctrl`: `drm_bypass` bit 0, `tb_bypass` bit 8).

The only wrapper write of the active vendor build is therefore `drm_ctrl`
0, after TOP5, CORE6 policy 8, the clocks and the reset release, and
immediately before `RGXStart`. It sets DRAM security
bypasses, which `sunxi_dvfs_ctrl.c` declares with no power meaning.

The BSP 1.4.6 revision `142ce05b` of the same files writes no CORE6 word
(`USE_FPGA` is undefined), has no `sunxi_secure_config`, and enables the
GPU clocks before TOP5. Its DVFS source has the same register layout and
the same commented-out delay. A web search found no other source that names
`pwr_qchan_cfg` or its fields.

So no reference supplies a value for `pwr_qchan_cfg`, and none shows
software driving CORE6's Q-Channel.

### The wrapper-write probe was refused

A probe that wrote `pwr_cmp_dly` and `pwr_qchan_cfg` in three stages, the
last holding core 0's `q_accept` HIGH by software, was built and
host-checked at commit `ffb3db0d`. The coordinator refused it before any
board run: no vendor source writes those values, and no reference shows
software driving CORE6's Q-Channel. Its source, checks and bundle row
were then removed.

### The vendor reads the core identity before any secure configuration

The vendor's first GPU core accesses, traced in the same pinned sources:

1. **Probe.** `PVRSRVCommonDeviceCreate` calls `SysDevInit`
   (`pvrsrv.c:2258`), which runs `sunxi_platform_init` (`sysconfig.c:281`).
   That function sets the GPU supply to the highest operating point's
   voltage (`sunxi_platform.c:901`), requests TOP5 (912), writes CORE6
   PWCR 0 and policy 8 (915–916), enables the clocks and releases the
   reset (920), and sets the GPU clock to the highest operating point,
   1008 MHz, from the GPU PLL (922, through `sunxiSetFrequency`, 405–416).
   `PVRSRVCommonDeviceCreate` then calls `RGXRegisterDevice`
   (`pvrsrv.c:2348`), whose `RGXBvncInitialiseConfiguration`
   (`rgxinit.c:5267`) reads the identity from the hardware when no load
   parameter names it (`rgxbvnc.c:694`). Its request for system power ON
   (`rgxbvnc.c:687–690`) changes nothing, because `sunxi_platform_init`
   has already marked the GPU ON (`sunxi_platform.c:918`). The read is
   `OSReadHWReg64` of `RGX_CR_CORE_ID__PBVNC` (`0x01800020`,
   `rgxbvnc.c:119`), which the kernel build defines as two 32-bit `readl`
   calls, `+0x24` and `+0x20`, combined with `|` (`osfunc.h:1086–1095`).
   C leaves the order of the two operands to the compiler; the source
   names `+0x24` first. No CORE6 status poll and no `sunxi_secure_config`
   precede that read. It is the first GPU core access of a boot.
2. **Every later power-on.** `RGXPostPowerState` calls `RGXTimeCorrBegin`
   (`rgxpower.c:1031`), which reads `RGX_CR_TIMER` (`0x01800160`,
   `rgxtimecorr.c:300`, `rgxfwutils.h:349`), and only then `RGXDoStart`
   (1036) with `sunxi_secure_config` followed by `RGXStart`.
3. **After the secure configuration.** BXM `36.V.104.183` has
   `SYS_BUS_SECURE_RESET` (`rgxconfig_km_36.V.104.183.h:94`), so
   `RGXStart` first writes 0 to `RGX_CR_SYS_BUS_SECURE` (`0x0180a100`) and
   reads it back as a fence (`rgxstartstop.c:859–860`), before its soft
   reset sequence (876).

The vendor therefore reads the core identity before any secure
configuration, and reads the core timer before making it on each
power-on. The missing secure configuration does not by itself explain the
two stalled core reads. Before its first read the vendor differs from the
power/identity probe `fd3d5839…` in:

- the access width: two 32-bit loads, where `fd3d5839…` made one 64-bit
  load of `0x01800020`;
- CORE6 PWCR 0 and policy 8, which `fd3d5839…` never wrote and the current
  power-only sequence writes;
- the operating point: 1008 MHz from the GPU PLL at the highest operating
  point's voltage, where SunstoneOS runs 400 MHz from PERI0 at 800 mV;
- no wait for CORE6 status 8.

### The probe-read probe makes the vendor's probe-time read, then the secure configuration

The probe-read variant (`apps/gpu-power-id/probe-read.c`, build and check
option `--probe-read`, bundle row 17) runs the power-only ON sequence
through the reset release and reads the core identity as the vendor's
probe does. It then makes the vendor's secure configuration and reads the
identity again. One run thus separates the access width from the secure
configuration while the clock and voltage stay at 400 MHz and 800 mV. It
is implemented and host-checked; it has not run on a board.

Sequence:

1. Snapshot 0, then ON with the client deadline as the controller's bound.
   The CORE6 status poll for 8 runs at most 200 ms as an observation;
   `GPU PROBE core6` prints whether status 8 came, the last status and the
   wait.
2. Snapshot 1 and the controller and CORE6 lines. The accesses are refused
   unless the controller is ON or has read MISR after the reset release,
   the step before its final status poll: the GPU clock is then open and
   its reset released, as before the vendor's probe-time read.
3. Identity Device-nGnRnE mapping checks of the wrapper and SMC0 pages for
   read and write, and of the GPU core page for read.
4. The rows of `probe_steps`, in order. Before each access the client
   writes the row number and a counter stamp into the progress record at
   result offset 128 (magic `GPUPRD01`) and prints the access; after it,
   it stores the value in the record and prints it. The client clears the
   value slots at entry. Every access is a 32-bit Device-nGnRnE access
   with `DSB SY` before and after.

| # | Access | Purpose |
| --- | --- | --- |
| 1 | read `0x01880014` | `drm_ctrl` entry value |
| 2 | read `0x01880010` | `irq_ctrl` entry value |
| 3 | read `0x0a0000b0` | SMC0 `SMC_DRM_GPU_HW_RST` entry value |
| 4, 5 | read `0x01800024`, then `0x01800020` | Core identity as the vendor's probe reads it |
| 6, 7 | write `0x0a0000b0` ← 2, read back | `sunxi_platform.c:969–970` |
| 8, 9 | write `0x01880014` ← 0, read back | `sunxi_platform.c:973–975` |
| 10, 11 | read `0x01800024`, then `0x01800020` | Core identity after the secure configuration |
| 12, 13 | write `0x01880014` ← row 1's value, read back | Restore |
| 14, 15 | write `0x0a0000b0` ← row 3's value, read back | Restore |

Rows 1 to 3, the readbacks and the restore are the probe's own: the
vendor reads none of these words and reads back neither write. Rows 6 to
15 run only after rows 4 and 5 have returned. If row 4 or 5 stalls, the
next image puts the two writes before the read.

5. A `GPU PROBE result` line reports whether each identity matched
   `36.56.104.183` (`0x240038006800b7`) and whether the restore readbacks
   equal the entry values. `COMPLETE GPU probe read: identity
   36.56.104.183 before and after the secure configuration, restored, GPU
   left powered` (status 0) requires both identities, both restore
   readbacks and no pending asynchronous abort. Anything else ends with
   `FAIL GPU probe read: identity or restore not as expected, GPU left
   powered` (status 1).

The image makes no OFF and no provider park. CORE6 never acknowledged ON,
and turning it and TOP5 off from that state has no board evidence; the
run ends after the restore, and the lifetime ends with the power removal
that the runbook requires anyway.

**The write window restores on every way out.** From the first write
(row 6) to the last restore readback (row 15) the client:

- masks D, A, I and F, so no interrupt, task switch or asynchronous abort
  is taken;
- makes no deadline or clock check and has no failure call, so no
  `bb_power_fail` or `platform_exit` can run;
- points `VBAR_EL1` at `probe_restore_vectors`, sixteen slots that each
  write `drm_ctrl` and then the SMC0 word back from the entry values and
  enter the same slot of the image's own vector table. Before the window
  the client requires `VBAR_EL1` to hold that table, and refuses the
  writes otherwise.

A synchronous exception, the one exception that masking cannot stop, such
as an external abort on the second identity read, therefore restores both
words before the image's fault report returns to the monitor. After the
last restore readback the client restores `VBAR_EL1`, reads `ISR_EL1` and
prints `GPU PROBE window serror_pending`, and only then restores DAIF. An
asynchronous abort signalled during the window is thus taken, if at all,
after both words hold their entry values. The monitor's 2 s recovery
cannot fall inside the window: the window takes microseconds and begins
about 200 ms after monitor entry. The only path that leaves the words
written is an access that never completes, which no software can follow;
then the lifetime ends with the power removal.

What each result means:

- **Both identities `36.56.104.183`.** The core answers in this state
  with CORE6 not reporting ON, so CORE6 status does not gate core register
  access, and the earlier stalls came from the 64-bit load or from an
  absent CORE6 write. The secure configuration does not change the read.
- **First identity correct, second wrong or a fault line in the window.**
  The secure configuration changes what a non-secure CPU read reaches;
  `drm_ctrl` 0 or SMC0 2 may make the core secure-only or reset it. Both
  words were restored.
- **First identity wrong.** The core answers, but not with its identity.
  0 or all ones means that the read reached a block in reset, isolated or
  unpowered; a value with branch 0 is the legacy format, which the vendor
  then reads as `CORE_REVISION` and `CORE_ID` (`rgxbvnc.c:131–132`).
- **No return at row 4 or 5.** The 32-bit probe-time read stalls without
  the secure configuration. The next image makes the two writes before
  the read; the operating point below follows if that read stalls too.
- **No return at row 10 or 11.** The read stalls only after the secure
  configuration, which then blocks non-secure access instead of enabling
  it. The SMC0 word and `drm_ctrl` stay written until the power removal.
- **A restore readback that differs, or `serror_pending=1`.** SMC0 or the
  wrapper refused a write. The asynchronous abort is taken after the
  restore and ends the run through the image's fault report.

**Write exception for coordinator acceptance.** The state is TOP5 ON, the
GPU clock open at 400 MHz from PERI0, DCDC4 at 800 mV, the reset released
and CORE6 not acknowledged. The image writes 2 to SMC0 `0x0a0000b0` and
then 0 to the wrapper's `drm_ctrl` `0x01880014`: the two writes of
`sunxi_secure_config`, in its order and with its values. After the second
identity read it writes both back to the values it read before them. Each
write is a 32-bit Device-nGnRnE store followed by a readback. It also
reads `0x01880010` and the two written words before the writes, and makes
two pairs of 32-bit identity reads, `0x01800024` then `0x01800020`. The
coordinator accepted the SMC0 write as part of the GPU grant for this
variant, because the vendor driver makes it on every GPU power-on. The
A733 user manual places SMC0 at `0x0A000000`–`0x0A00FFFF` in the memory
controller block (MEMC0, interrupt 213, `MEMC0_SMC0`) and describes none
of its registers; the name of offset `0xb0` comes only from the vendor
source.

**Performance design note.** The probe runs once per board lifetime and
has no hot path. Its cost is 15 accesses, three translation walks (two
for read and write), two 35-word snapshots and at most 200 ms of status
observation in one-tick pauses. The client refuses to start unless the
client deadline leaves 200 ms and a 100 ms reserve after the ON begins.
`DSB SY` on both sides of each access orders the progress record in DRAM
against the device access, and each write against its readback
(performance architecture section 1.2). The masked window holds at most
ten accesses and their log lines. A controller, provider or deadline
fault before the window retains the claim, as in section 1.8.

The host check (`check.py --probe-read`, twelve cases) runs the linked
client against modeled controller results. It requires:

- the three pages checked before the first access;
- every access to be the table row that its progress record names, in
  order, after its log line;
- rows 6 to 15 to run with D, A, I and F masked and `VBAR_EL1` at the
  restore vectors, and the other rows without them;
- the restore writes to carry the entry values, the progress record to
  hold each value, and no OFF or provider park.

The cases are the board's state, CORE6 ON during the observation, a first
identity of 0, a second identity that differs, a restore that the
register ignores, a synchronous abort on the second identity read, a
controller refusal, an ON before the reset release, a wrong SMC0
translation and a short client deadline. In the abort case the check
enters the restore vectors as the exception would, and requires both
words written back, `drm_ctrl` first, before the image's handler runs.
Two seeded table mutants, an SMC0 write of 3 and an SMC0 restore that
takes `drm_ctrl`'s entry value, are each refused at that write.

The probe is the last step of its lifetime, after the cold DDR start and
the provider-readiness row (bundle row 11).

### The vendor operating point is the next variable

If the identity read still stalls after both stages, the vendor's
operating point is the next difference to test: 1008 MHz from the GPU PLL
at the highest operating point's voltage. This is a plan; no image sets
it.

- **Source.** `sunxi_get_opp` takes the highest operating point
  (`sunxi_platform.c:754–759`), `sunxi_platform_init` sets its voltage
  (901) and its clock (922). In the pinned `sun60iw2p1.dtsi` that point is
  `opp@1008000000` at lines 4736 onward, with `opp-microvolt-vfdefault`
  960 mV and per-speed-bin values from 860 mV to 960 mV; the vendor picks
  the value through `vf_mapping_table` from the SID speed bin
  (`sunxi_platform.c:643–692, 711–732`). Lines 405–417 of that dtsi hold
  the timer and NMI nodes, not the operating point.
- **Rail.** The board device tree (`sun60i-a733-orangepi-zero3w.dts`,
  cached with the GPU power sources) gives the GPU `gpu-supply =
  <&reg_dcdc4>` (3420–3422): AXP8191 DCDC4, 500–1540 mV, ramp delay
  250 µV/µs, always on (2141–2149).
- **Selector.** DCDC4's selector is PMIC register `0x15`, 7 bits
  (`drivers/power/a733_pmic.c:21`), 10 mV steps from 500 mV up to code 70
  (`a733_pmic_decode`): 800 mV is code 30 (`0x1e`) and 960 mV is code 46
  (`0x2e`). The board's speed bin must be read first, because a lower bin
  voltage gives another code (860 mV is code 36).
- **Clock.** The GPU PLL at CCU `0x0e0` reads `0x41104500` (disabled) on
  port 3-1; the clock word `0xb20` would select it in place of PERI0.

### The delay-row writes stay

The controller keeps the vendor delay-row writes. The board read back the
vendor values at reset in both CORE6's and TOP5's rows, so each write
stores the value its register already holds, and all five passed their
readbacks on the board. Keeping them costs five writes and five reads per
ON. Removing them would change the controller, its native adapter, six
model checks, the reference check and the source-tree check, and would
rebuild both images. They leave with the other diagnostic steps once a
board run selects an explanation.

### What each failed lifetime left

The monitor returns through an `RMR_EL3` warm reset of the CPU, which does
not reset the PCK or CCU.

- **Admission refusal (`81d4a728`).** TOP5 stayed ON with its row
  unreleased. CORE6 kept policy 8, status 0 and PWCR `0x101`, and the GPU
  clock stayed gated with reset asserted.
- **Deadline at the final poll (`4ade8fdf`).** TOP5 stayed ON with its row
  unreleased. CORE6 holds policy 8, status 0 and PWCR 0. The GPU clock
  selects PERI0 400 MHz with its gate open, and the bus gate is open with
  reset released, so the GPU block is clocked and out of reset while CORE6
  reports OFF. Nothing touched the GPU register window, and the PMIC
  provider reported no held, retained or failed request.

- **Deadline at the final poll after the delay row (`ade71834`).** The
  same state as the run before, with the vendor delay row rewritten to the
  values it held from reset.

The queue's quarantine closes these lifetimes. A power removal resets the
PCK and CCU, and CORE6 returns to its power-up state. No run wrote
storage or any persistent setting, so the next lifetime needs nothing
beyond the power removal.

## An unpowered GPU core is the leading hypothesis for both failed GPU runs

Two GPU images started on a board and did not return:

- the power/identity probe, ELF `fd3d5839…`, on port 3-1, observed for
  30 seconds;
- the finite GPU computation, ELF
  `2ab2cb1b3bac150d5eb695793ab0eb39bb453afc7fbd7ecb2f595ebfca3fd08d` from
  `gpu-returning-repeat-fix-20261019/build/return.elf`, on port 3-3, observed
  for 90 seconds.

Both were built before the controller gained CORE6 control (commit
`9bd65b98`). Each powered GPU_TOP (domain 5) and never wrote CORE6 policy 8
before its first access to the GPU register window at `0x01800000`. The
leading hypothesis is that the first GPU register read stalled the CPU because
the core domain was unpowered. A read whose target cannot respond does not
complete and raises no exception, so no bounded failure path runs and no
report is written. The probe's own limits argue against an ordinary bounded
failure: its client deadline is 1.5 seconds from monitor entry and the
returning CPU monitor's deadline is 2 seconds, so a refused step would have
reported and returned well inside the 30-second observation.

The hypothesis is not proven. Neither run produced a native report, so the
stage at which each stopped is unknown, and a stop before the GPU access
(entry, provider readiness or the TOP transition) remains possible. The
corrected power-only run also never saw CORE6 status 8, and one remaining
explanation is that this PPU does not sequence CORE6's switches at all
([The remaining explanations, ranked](#the-remaining-explanations-ranked)).
If that holds, CORE6 status does not show whether the GPU core is powered.

The [power-only variant](../apps/gpu-power-id/README.md#the-power-only-variant-runs-before-any-identity-probe)
of the diagnostic separates the two explanations. It powers TOP and CORE,
checks CORE status 8, powers both OFF and releases them, twice, and never accesses
the GPU register window. Following the lowest-recovery-risk order of
AGENTS.md, it runs before any identity probe. If it returns, CORE power
control works on that board and a later identity probe tests the register
read alone. If it does not return, the failure lies in power control and
involves no GPU register access.

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
This correction adds the omitted CORE operations without changing local reset
order, parent rate or voltage.

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

With immediate acknowledgement, CORE control costs seven CORE writes and
seventeen reads on ON, then one write and two reads on OFF, plus two
release reads. Five of the ON writes and five of the reads are the delay
row and its readbacks. Six of the ON reads are diagnostic: identification
(`PPU_IDR0`, `0xfb0`), the accept and deny inputs (`PPU_MISR`, `0x14`)
before any write and after reset release, and the status after the delay
row, after the PWCR write and after the policy write. The native adapter
permits the two diagnostic offsets for reads only. A waiting poll adds
at most 50 µs of status reads, one controller clock sample per read. An
acknowledgement inside that window completes the step in the same poll
instead of one scheduler tick later. Native writes use the existing shared
register lock. The power structure holds the cached policy, status and PWCR,
the six diagnostic words, the three observation waits and a seen mask; the
mask records completed reads, including a read followed by deadline
rejection, and is not admission authority. The power/identity diagnostic
prints those cached values and the failure step without accessing hardware
again.

Focused evidence is in
`~/.cache/sunstoneos/evidence/gpu-core-power-20261024/`: inherited TOP/CORE pairs,
static-state and deadline refusals, actual native access widths and locks,
resource-release retention, and composition with the actual TOP owner/provider
and rail ledger. Domain4 and shared-gate sentinels remain unchanged. The corrected
power/identity candidate rebuilds every object using the enlarged power/platform
layout and preserves the accepted monitor cleanup checks. These are software
checks, not physical qualification or a location for the failed `fd3d5839` run.
The earlier provider-readiness image remains unchanged and is still the first
proposed board test.

The power-up admission, the CORE6 delay row and the CORE6 observations
have focused results in `~/.cache/sunstoneos/build/gpu-core6-b-1024/checks-1/`.

- **Controller model.** It completes the power-up state at each of five
  points: after the delay row, after the PWCR write, after the policy
  write, after clock/reset progress, and never. For each, it checks the
  recorded status, the wait and the final result; the last case times out
  and retains its leases. It requires the delay row before PWCR 0 on every
  ON. It refuses policy 8 and status 0 with PWCR `0x010101ff`, 0 or `0x100`
  before any write. An observed status of 4 after the PWCR write fails with
  STATE, both from the power-up state and from settled OFF.
- **Owner composition.** It adds the power-up state with TOP5 inherited
  OFF and ON.
- **PCK trace and reference check.** The trace starts from the power-up
  state. The reference check compiles the vendor `sunxi_pd_init` for the
  CORE6 table entry and requires the native CORE6 row writes to equal its
  writes. It also confirms the order: the TOP5 request, the CORE6 row,
  PWCR 0 and policy 8, then the first CCU write.
- **Native check.** It reads the two diagnostic offsets and a row word with
  32-bit accesses, and refuses row writes other than the pinned whole-word
  values.
- **Source-tree check.** `checks/power-domains-tree.py` lists the GPU
  controller as the writer of the domain-6 row and the client as a reader
  of TOP5 words.
