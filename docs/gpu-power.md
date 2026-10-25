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
  `sunxi_secure_config` writes `0x0a0000b0` and a register in the GPU
  window; it is the secure-configuration callback, outside this sequence.
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

### The remaining explanations, ranked

1. **This PPU does not sequence CORE6's power switches.** They may be
   switched with TOP5 or from inside the GPU block. The evidence is that
   the vendor device tree comments out both CORE6 nodes, the SCP skips only
   CORE6's status poll, and BXM never reads CORE6. MISR bit 0, the
   power-switch accept, read 0, which is consistent but proves nothing: the
   bit also reads 0 between handshakes. CORE6 status would then never be a
   usable acknowledgement, and the GPU core would be powered by TOP5 alone.
   Testing that needs a GPU register access, outside the power-only rule.
2. **The undocumented CORE6 delay words stop the power-switch sequence.**
   DEN0051E Table 4-5 defines the device control delays as finite counts:
   each delay is its field value plus one PPU clock cycle, and the values
   range from 0 to 255, so each is 1 to 256 cycles. A missing value in the
   standard words at `0x170` and `0x174` therefore cannot stall a transition
   forever. Only the vendor words at `0xc00`, `0xc04` and `0xc10`, which
   neither DEN0051E nor the PCK-600 TRM describes, could. The vendor table
   pins a CORE6 row that no vendor path writes, and its reset values are
   unknown. The next probe reads the row and then writes the vendor values.
3. **The reset-time transition is wedged for good.** It may have started
   before TOP5 reached ON. A PPU in transition does not abort on a new
   policy, and no software reset of the PPU is documented. The probe cannot
   clear this state; if it holds, the delay-row write changes nothing and
   the snapshot words look normal.
4. **The PWCR write had no effect during the outstanding transition.**
   DEN0051E calls that write UNPREDICTABLE. The PPU would then wait for the
   GPU's accept, which MISR shows never came. This leaves the same
   question as the first explanation: why the GPU's Q-Channel logic is
   unpowered or unconnected.
5. **GPU PLL, SCP mailbox, PRCM power switch, interrupt mask or PPU
   clock.** None is supported. The GPU clock was in effect from PERI0, the
   SCP block has no CORE6 step, the PRCM has no GPU switch, masks gate no
   transition, and the PPU gate is open.

### The next probe writes the vendor CORE6 row and snapshots the prerequisites

After admission, the controller writes the vendor CORE6 delay row through
its CORE bank, each word read back, and then observes CORE status as after
the other writes: from the power-up state it re-reads for up to 50 µs until
status 8. The PWCR 0, policy 8 and clock sequence follows unchanged. The
native adapter admits each row word only with its pinned value.

The power-only client also takes a read-only snapshot before the first ON
and after each ON attempt, including a failed or expired one. It reads 35
words, none in the GPU register window:

- the CORE6 PPU row: `PWPR`, `PMER`, `PWSR`, `DISR`, `MISR`, `STSR`,
  `PWCR`, `PTCR`, `IMR`, `AIMR`, `ISR`, `AISR`, `EDTR0`, `EDTR1`, the delay
  row and `IDR1`;
- the TOP5 row: policy, status, `DISR`, `MISR`, `PWCR` and the delay row;
- R_CCU `0x1ac` (PPU gate) and `0x250` (VDD isolation gates);
- CCU `0x0e0` (GPU PLL), `0xb20` and `0xb24`.

Each page is checked as an identity Device-nGnRnE mapping first. The
expected results are:

- **Status 8 at the observation after the delay row, or later.** The
  vendor row is the missing step. The row stays in the controller, and a
  later change removes the diagnostic reads.
- **A STATE failure at a delay-row readback.** A CORE6 row word that does
  not hold the written value, for example because it is read-only, fails
  its readback check before any PWCR or policy write. The run returns
  status 1 at once; the failed step and its last value name the word, and
  snapshot 1 shows the whole row.
- **Status 0 throughout.** The first ON fails at the deadline as before.
  The snapshots compare CORE6's row with TOP5's working row and show any
  latched interrupt event. With the row in place, the first, third and
  fourth explanations remain, and the next step is a decision about a GPU
  register access with TOP5 alone.

The CORE6 observations and the snapshot are diagnostic. They stay in the
controller and client only until a board run selects an explanation.

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

The queue's quarantine closes both lifetimes. A power removal resets the
PCK and CCU, and CORE6 returns to its power-up state. Neither run wrote
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
