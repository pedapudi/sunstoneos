# The GPU powers on in the vendor's clock, supply and secure state

**Status: implemented and host-checked; ON is board-proven, OFF is not.**
On 2026-10-25 a diagnostic image applied three steps that the vendor stack
performs and SunstoneOS did not, then read the GPU core identity
`36.56.104.183` (session
`~/.cache/sunstoneos/evidence/board-session-20261025-4/port-3-1/s10-gvs-run/`).
The three steps are BL31's cold-boot security writes at EL3, DCDC4 raised to
960 mV, and the GPU core clocked from the GPU PLL at 1008 MHz. Every earlier
image without them stalled at that read. This design makes the three steps
the default GPU power path and removes the diagnostic copies that applied
them. A GPU OFF that follows a GPU ON has never run on a board.

References are those of the GPU reference backsolve
(`docs/reference-backsolve-gpu.md` on branch `work/backsolve-gpu-1025`). Citations without a path name these
files of the pinned board kernel `2ac08e8c7`: `sunxi_platform.c` (BXM DDK
`services/system/rogue/rgx_sunxi/`), `ccu_gate.c`, `ccu_nm.c`,
`ccu_common.c`, `ccu-sun60iw2.c` (BSP `drivers/clk/sunxi-ng/`),
`pck600_domains.c` and `sun60iw2p1.dtsi`. "BL31" is `monitor.fex`, SHA-256
`8af16b86…5b2632`, with file offsets.

## The board run shows what the three steps did

The run's log records every EL3 write with the word read before and after
it:

- The 22 SPC words at `0x02054004 + 0x10·n` and the three R_SPC words read 0
  before and after their `0xffffffff` writes. They behave as write-only
  "set" registers, so a readback cannot confirm them.
- SPC `0x020540e0`, port 14's status word, changed from `0x1060` to
  `0xfffffbff`. BL31 writes this word directly; port 14's set register is
  `0x020540e4`, which BL31 does not write.
- `PRCM_SEC_SWITCH_REG` (`0x07010290`) and `CCMU_SEC_SWITCH_REG`
  (`0x02003f00`) changed from 0 to 7.
- DCDC4's selector changed from `0x9e` to `0xae` (bit 7, set by boot0, kept;
  800 mV to 960 mV).
- PLL_GPU0 locked within four reads each time and read `0xf9105300`
  (1008 MHz, locked). Both GPU clock update bits cleared on the first read.
- CORE6 stayed at status 0 for 50 µs after its PWCR and policy writes and
  reached status 8 once the clocks ran, 3,151 µs after the observation
  started.

Which of the three steps is necessary is not separated. The manual states
that the PLL and PRCM security switches have no effect while the chip is in
non-security mode (manual 4.2.5.26). PLL_NPU has been enabled and locked from
non-secure EL1 without the switch, so these boards behave as non-security
mode, and the switch writes are probably inert. The supply and the GPU PLL
remain the candidates.

## EL3 security setup runs in every image

**Decision.** Every EL3 entry makes every cold-boot security write of
BL31's `sunxi_security_setup`, cold-boot writes 9 to 17 of
`docs/vendor-firmware/tf-a-monitor.md` (branch `work/blob-doc-monitor-1025`),
in BL31's order, before any core enters EL1. SunstoneOS has four EL3
entries into an image:

- `platform/a733/monitor.c`, which the returning images use;
- `apps/storage-read-single-core/monitor.c`, which the single-core
  application images use;
- `platform/a733-smp-cached/boot.c` (`a733_smp_boot`), which the multicore
  application and OS images use through their boot patches. It makes the
  writes after starting the secondaries, which wait for the primary's
  continuation, and before that continuation;
- `platform/a733-smp/boot.c`, the same sequence in the uncached eight-core
  diagnostic `a733-smp-diagnostic`.

The two assembly interrupt probes (`boot/a733/primary-interrupt-probe.S`
and `secondary-interrupt-probe.S`) also enter EL1, only to take a timer or
software interrupt; they access no peripheral behind the SPC and are left
unchanged. One definition, `include/a733_el3_security.h`, serves all four. The two
build pins of the SMP cached boot source (`cmake/a733-storage-maintenance.cmake`
and `cmake/a733-smp-observe.cmake`) name its new SHA-256; every boot patch
applied to it still applies unchanged. QEMU models (`MONITOR_QEMU`) omit the
writes, because the QEMU machine has none of these blocks. This is rank 1 of the vendor action plan
(`docs/vendor-firmware/action-plan.md`, branch `work/vendor-actions-1025`).

**Justification.**
- BL31 makes the writes on every cold boot and on every resume from suspend,
  whatever device the OS uses. No device condition exists in the reference
  to copy.
- The SPC, TZMA, PRCM, CCU and DMA controllers keep their state through the
  monitor's `RMR_EL3` warm reset; only a power removal resets them. With the
  writes in GPU images only, every later image of a lifetime would run in a
  state that depends on whether a GPU image ran first. With the writes in
  every image, each image starts in one state.
- The board run made writes 9, 10, 12, 13, 14 and 17 and returned to the
  ROM, and the ROM answered.

**The writes and what they open.** Each write only grants non-secure access.

| BL31 write | Address and value | `monitor.fex` offset | Opens to non-secure software | SunstoneOS users and conflict check |
| --- | --- | --- | --- | --- |
| 9, 10 | SPC ports 0–13 and 16–23 set registers, `0x02054004 + 0x10·n` = `0xffffffff` | `0xc3cc`, `0xc3e0` | The peripherals behind those SPC ports; the port-to-device layout is undocumented | Every SunstoneOS device driver runs at non-secure EL1 and reached its device before these writes (board records), so none loses access. The registers read 0 before and after on the board |
| 11 | S_TZMA `0x07003000`, `+0x4`, `+0x8` = 0 | `0xc408` | SRAM areas | The monitors keep their state in DRAM (`0x41000000` and `0x42005000`), not SRAM. The NPU's shared SRAM is routed by SYSCTRL `0x03000008`, which no write touches; the vendor makes this write before Linux runs the NPU |
| 12 | S_SPC `0x07002004`, `+0x14`, `+0x24` = `0xffffffff` | `0xc41c` | Always-on peripherals; the port layout is undocumented | SunstoneOS's always-on drivers (the S_TWI0 PMIC bus, R_CCU, RTC) already reach their devices from EL1. The registers read 0 before and after on the board |
| 13 | `PRCM_SEC_SWITCH_REG` `0x07010290` \|= 7 | `0xc438` | CPUS bus clock, PRCM PLL and power registers | The PPU clock gate (R_CCU `+0x1ac`) is already written from EL1. Board: 0 to 7 |
| 14 | `CCMU_SEC_SWITCH_REG` `0x02003f00` = 7 | `0xc448` | PLL, bus and MBUS clock registers, including `PLL_GPU0_CTRL_REG` and `NSI_CLK_REG` | Clock drivers already wrote PLL_NPU and the display PLLs from EL1. Board: 0 to 7 |
| 15 | DMAC0 `DMA_SEC_REG` `0x04601020` = `0xffff` | `0xc458` | All 16 DMA0 channels | DMA0 is SunstoneOS's DMA controller, used from non-secure EL1, so non-secure channels are what it needs. `0xffff` is the register's reset value (manual 13.1.7.1) |
| 16 | DMAC1 `DMA_SEC_REG` `0x04024020` = `0xffff` | `0xc460` | All 16 DMA1 channels | SunstoneOS does not use DMA1; its ownership stays unresolved (`docs/dma.md`). The NPU uses neither controller |
| 17 | SPC port 14 status word `0x020540e0` = `0xfffffbff`, written directly (the set register is `0x020540e4`) | `0xc470` | Port 14 except bit 10; port 15 is never written | Board: `0x1060` to `0xfffffbff` |

The two DMA writes are made only when the controller's bus gate and reset
(CCU `0x704` for DMA0, `0x70c` for DMA1, bits 0 and 16) are both open. A
FEL-booted lifetime starts with both controllers gated and in reset (board
read of 2026-10-17, `supervised-master-state-20261017-193555`). A write to a
gated controller is not known to complete, the project reads no gated
register, and a reset release would discard the write anyway. In that state
the channels hold their reset value `0xffff`, which equals BL31's value, so
the condition changes no channel state. When an earlier image of the
lifetime left a controller running, the write restores BL31's value before
EL1 starts; the DMA startup then admits only a quiet controller and does not
read `DMA_SEC_REG`. BL31's TIMER0 writes (18 to 20) configure no security
and are left out; no SunstoneOS code uses TIMER0.

**Effects on other devices.**
- **NPU.** The NPU state read of 2026-10-25 read 21 words, none of which is
  written here; it should read the same values. The NPU's open finding,
  shared SRAM routed to the CPU (SYSCTRL `0x03000008` bit 1), is outside
  these writes. The NPU has its own DMA engine and uses neither DMA
  controller.
- **Display, video, USB, MMC and the GPU wrapper.** Each was reached from
  non-secure EL1 before these writes existed, so each keeps its access.
- **Clock registers.** The display and NPU PLL writes already worked from
  EL1, consistent with the switches having no effect in non-security mode.
- **Secure-only registers.** Port 14 bit 10 and port 15 stay as the reset
  left them. The SPC layout is undocumented, so the writes may expose
  registers that were secure-only. A driver that relies on such a register
  faulting or reading zero would change behaviour; the documents name one,
  DMA0 `+0x20`, which the DMA startup keeps outside its allowlist.
- **Board-bundle supply snapshot (row 6).** In a lifetime where a GPU image
  has run, DCDC4 reads 960 mV (next section). Row 6 accepts DCDC4 at
  `0xff9e` (800 mV) or `0xffae` (960 mV), with every DCDC4 snapshot equal to
  the first, so it can run before or after a GPU image.

## DCDC4 960 mV is a GPU operating point of the shared PMIC path

**Decision.**
- `include/a733_gpu_opp.h` holds the vendor device tree's 1008 MHz operating
  point (`sun60iw2p1.dtsi:4736–4760`, `opp@1008000000`) with its 22 speed-bin
  voltages, 860–960 mV. SunstoneOS uses its `opp-microvolt-vfdefault`,
  960 mV. The vendor kernel's device tree has no `vf_mapping_table`, so
  `sunxi_get_opp` takes `vfdefault` on every board (`sunxi_platform.c`
  `sunxi_get_opp`); the u-boot tree maps speed-bin codes 0, 1 and 2 to
  `vf0000`, `vf0100` and `vf0200`, which are also 960 mV.
- The PMIC provider (`drivers/power/a733_pmic.c`) admits a DCDC4 selector
  write under an exclusive DCDC4 lease when the target decodes to one of
  that row's voltages. The CPU rails keep their 800–1050 mV rule.
- The GPU platform raises DCDC4 once, before its first ON: an exclusive
  DCDC4 lease, a snapshot read, a write from 800 mV to 960 mV with its
  readback, and 1.2 ms of settling (the regulator's 1 ms ramp for 160 mV at
  250 µV/µs, `axp2101-regulator.c:383–410`, plus the vendor's 200 µs,
  `sunxiSetVoltage`). A DCDC4 already at 960 mV is not written.
- The power-domain owner's boot check accepts DCDC4 at 800 mV (power-on) or
  960 mV (left by an earlier image of the lifetime). The supply consumer
  samples DCDC4 through the provider after the raise and requires 960 mV,
  and so does the GPU resource manager, which no longer checks 800 mV.
- OFF leaves DCDC4 at 960 mV. The vendor lowers no GPU voltage at runtime
  power-off and disables the regulator only at system suspend
  (`sunxiPostPowerState`); the board marks DCDC4 always-on.

## The GPU controller follows the vendor order

The sequences below compare the vendor's first power-on (`sunxi_platform_init`),
its later power-ons (`sunxiPrePowerState`) and power-offs
(`sunxiPostPowerState`) with the default controller
(`drivers/gpu/a733_gpu_power.c`).

**ON.** The controller runs one sequence for every ON:

1. Admission (ours only): the AHB and MBUS master gate words recorded,
   PPU clock, GPU clock word at
   gate closed, source 0 (PLL_GPU0), divider 1, GPU bus gate and reset
   closed, PLL_GPU0 disabled with input divider M1 1, which the factor write
   keeps.
2. CORE6 entry words read and admitted (ours only), then PWCR 0. The vendor
   writes PWCR 0 at its first power-on only; it stays 0 afterwards. TOP5 is
   ON from reset, so CORE6's PPU answers before any domain request.
3. PLL_GPU0 enabled as `ccu_pll_gate_helper_enable` does (`ccu_gate.c:157–203`):
   output gate cleared, LDO, enable and lock enable set, lock read every 3 µs
   until three consecutive reads show it (at most 100,
   `ccu_common.c:47–61`), 20 µs, output gate set.
4. GPU bus reset released, bus gate opened, core clock gate opened with the
   update bit, then the update bit written again (`sunxi_enable_clks_wrap`).
5. PLL_GPU0 set to 1008 MHz as `ccu_nm_set_rate` does (`ccu_nm.c:250–345`):
   output gate cleared, N 84 and P0 2, lock enable cleared and set, the lock
   wait, 20 µs, output gate set.
6. TOP5 ON through the power-domain owner's row, with the GPU clocked, as
   `sunxiPrePowerState` requests the domain after `sunxi_enable_clks_wrap`.
   On the first ON the owner adopts TOP5, which is ON from reset.
7. The master gates recorded again and required open (ours only): a TOP5
   OFF closed the AHB gate on a board (`docs/gpu-power.md`, "The first
   OFF on a board closed the GPU master gates").
8. CORE6 policy 8, then status polled until 8.

The vendor's first power-on writes policy 8 before the clocks; its later
power-ons write it after them. At power-up CORE6's policy already reads 8
with its transition outstanding, so on the first ON the write in step 8
changes no request: the transition completes when the clocks run, as on the
board. On a later ON, from policy 0, the write comes after the clocks and
TOP5, as in the vendor's later power-ons. The status poll is our check; the
vendor never reads CORE6 status. Every PLL_GPU0 write is read back (ours
only), and the controller fails with `PLL`, `LOCK` or `UPDATE` when a write
does not take, the PLL does not lock, or an update bit stays set. The CORE6
delay-row writes are removed: no reference writes them, and the board read
the same values at reset.

**OFF.** The controller runs `sunxiPostPowerState`'s order:

1. CORE6 policy 0, then status polled until 0 (the poll is ours only).
2. TOP5 OFF through the row; the owner waits for domain status 0.
3. GPU bus gate closed.
4. Core clock gate closed with the update bit, as `ccu_gate_helper_disable`
   writes it for a key-field clock.
5. PLL_GPU0 disabled as `ccu_pll_gate_helper_disable` does (`ccu_gate.c:40–69`):
   output gate, lock enable, enable, then LDO cleared, each read back.
6. GPU bus reset asserted, then the resource release.

Both domain-5 transitions run with the GPU clocked, as the vendor runs them,
and the power-domain owner admits a domain-5 transition whatever the GPU
clock, bus gate and reset read. The owner used to require them closed. That
rule rested on TOP5 having reached ON at power-up with the GPU gated and in
reset, which shows nothing: TOP5 is ON from reset, so no TOP5 power-up
handshake has ever run without the GPU clock, and none has run with it. The
board run showed that CORE6's handshake depends on the GPU clock; TOP5's
may too, and the vendor order keeps the clock running through both. The
OFF sequence and a TOP5 transition from OFF have not run on a board.

**Failure states.** A failure stops the controller at its step, leaves the
state below, retains the leases and the TOP5 row, and makes no further
access. The controller reports the error and the step index, and the GPU
platform prints them with the CORE6 and clock snapshots.

| Failed step | GPU clocks and PLL_GPU0 | TOP5 | CORE6 |
| --- | --- | --- | --- |
| ON 1–2 (admission, PWCR) | Gated, reset asserted, PLL disabled | ON from reset, untouched | Entry state; PWCR 0 if written |
| ON 3–5 (PLL, clocks, rate) | As far as the step got; the gate and reset as written | ON from reset, untouched | PWCR 0, policy unchanged |
| ON 6 (TOP5 refused, failed or timed out) | Clocked at 1008 MHz | The owner's answer: refused rows write nothing, a failed row keeps its rail pin | PWCR 0, policy unchanged |
| ON 7 (master gates closed after TOP5 ON) | Clocked at 1008 MHz | ON | PWCR 0, policy unchanged |
| ON 8 (policy 8 or status poll) | Clocked at 1008 MHz | ON | Policy 8, status as last read |
| OFF 1 (policy 0 or status poll) | Clocked at 1008 MHz | ON | Policy 0, status as last read |
| OFF 2 (TOP5 OFF) | Clocked at 1008 MHz | The owner's answer, row retained | OFF |
| OFF 3–6 (clocks, PLL, reset) | As far as the step got | OFF | OFF |

Each state keeps the GPU clocked while a power handshake is outstanding, so
a later attempt or a reset can complete it; no state leaves the GPU
unclocked with a domain transition outstanding.

**PERI0.** The GPU core clock runs from PLL_GPU0, so the GPU no longer
leases the PERI0 400 MHz output or checks its rate. The resource manager
keeps the PERI0 lease for the NPU and its other users, and the GPU platform
acquires every resource except that one.

## The diagnostic copies are removed

The image that ran on the board linked copies of the monitor, PMIC provider,
supply consumer, GPU resource manager, controller and native register
adapter. With the default changed, these copies, their unit check and the
approval record of the diagnostic are removed. The vendor-clock-order
diagnostic, a controller copy that the board run superseded, is removed with
its board-bundle row. The identity, power-only, wrapper-read and probe-read
diagnostics remain: they now run the default path. The identity diagnostic
reads the identity as two 32-bit loads, as the vendor does, in each of two
ON/OFF cycles. The controller keeps the CORE6 entry, PWCR and policy
observations, and records the PLL lock and clock-update read counts.

## Cost

GPU power transitions are not a hot path: an image powers the GPU on once
per GPU service start and off once per stop, so no section 3 budget of
[the performance architecture](../performance-architecture.md) applies.
The counts below come from the controller model and the monitor check;
[the measurements](gpu-vendor-default-measurements.md) give them with their
inputs.

| Cost | Before | After |
| --- | --- | --- |
| Controller register writes per ON / OFF | 11 / 4 | 16 / 8 |
| Controller callbacks per ON in the model (one clock sample per µs) | 82 | 182 |
| PMIC transactions before the first ON of a platform | 0 | 3 (DCDC4 snapshot, write with readback, consumer sample) |
| PMIC transactions before a later ON | 0 | 0 |
| Fixed waits per ON | 1 µs settle | two 20 µs PLL waits; 1.2 ms once per platform for DCDC4 |
| EL3 accesses per image start | 0 | 31 writes and 3 reads; 33 writes when an earlier image left DMA running |

The ON adds the 10 PLL_GPU0 writes of the enable and set-rate helpers, each
read back, two lock waits (four reads each on the board) and two update-bit
waits (one read each on the board); it removes the five CORE6 delay-row
writes and their read-backs. The lock and update waits run inside one poll,
at most 100 reads each; the board needed 12 µs per lock wait. The heavier
form is the bounded busy wait, which the reference's `udelay(3)` loop also
uses; it runs once per transition.

Failure classes are unchanged: a refused or failed step stops the
controller in one of the states of the failure table above, retains the
leases and the TOP5 row, and makes no further access. A failed DCDC4 write retains the exclusive lease and the PMIC
request, so the programmed voltage stays uncertain until recovery. A DCDC4
snapshot that completes at a voltage other than 800 or 960 mV made no
write: the platform reaps it, releases the lease and fails, so the power
service keeps its supply records.

## The next GPU board steps

Each step is one bounded, returning image on port 3-1, after a cold DDR
start and the frozen provider-readiness checkpoint of the same lifetime.
Within a lifetime the steps run in the order below, lowest recovery risk
first. A failure of any step is reported and returns; only a GPU-window
read with the core unpowered can stall, and every identity read follows a
CORE6 status of 8.

### 1. OFF and a second ON in one lifetime

Images: the power-only diagnostic (bundle row 12), then the identity
diagnostic (row 13), both two ON/OFF cycles on the default path. The
power-only image makes no GPU-window access, so it tests OFF and the second
ON alone. Set `~/.cache/sunstoneos/evidence/board-bundles-20261025-9/port-3-1/`
freezes the lifetime: the provider-readiness checkpoint `3adfa4ad…`
(`s11-gpr`), then power-only `b163b8fc…` (`s11-gpo`), then identity
`01849e62…` (`s11-gpi`), with its runbook at `port-3-1/runbook.md`.

Both images link 23 objects of the checkpoint and compile 14. Two bringup
changes altered structures that `struct bb_power` embeds, the counter
clock's and the power service's, so the builder now also compiles the
board-bundle power service, its trace helpers, the counter clock and the
PMIC native adapter. Built against the checkpoint's `power.c`, the client
read every power-service field after the clock 8 bytes from where it is
written; the host checks caught it at the first deadline check.

**Images that cannot follow a GPU image.** The power service of an image
built before this branch verifies DCDC4 only at 800 mV, so after a GPU
image has raised DCDC4 to 960 mV it leaves DCDC4 unverified. The only such
image on a GPU lifetime is the provider-readiness checkpoint `3adfa4ad…`,
which therefore runs first, as every port 3-1 lifetime orders it. Every
other row is built from this branch when a set is prepared, and its power
service accepts either voltage; board bundle 6 accepts either snapshot. The
NPU images on port 3-3 verify DCDC4 the same way, but no GPU image runs on
port 3-3.

Predictions, per image:

- The monitor makes the security writes and the run returns to a responsive
  ROM. DCDC4 reads selector `0x9e` before the first image's raise and
  `0xae` (960 mV) after it; the second image finds `0xae` and writes
  nothing.
- Cycle 1 ON: CORE6 entry policy 8, status 0, PWCR `0x101`; `GPU clock`
  `pll=0xf9105300`, about four reads per lock wait, one read per update
  bit; CORE6 status 8 before the ON deadline, `misr_late=0x100`.
- Cycle 1 OFF: CORE6 status 0 within the first poll and TOP5 status 0
  within the owner's 10 ms, both with the GPU still clocked; PLL_GPU0 then
  reads `0x01105300` (bit 24 kept, control bits clear); the resource
  release passes. This is the first TOP5 transition from ON on a board.
- Cycle 2 ON: TOP5 goes from OFF to ON with the GPU clocked, the first TOP5
  power-up handshake on a board.
- Cycle 2 ON: CORE6 entry policy 0, status 0, PWCR 0, which admission
  accepts as settled OFF; PLL_GPU0 locks directly at 1008 MHz; CORE6
  reaches 8 after the policy write; snapshot 2 shows `0x07066008` = 8.
- The identity image prints `GPU identity cycle=1` and `cycle=2`, each
  `raw=0x240038006800b7 matches=1`, and `COMPLETE GPU power identity:
  36.56.104.183 in two ON/OFF cycles, provider parked`.

What each departure would mean:

- **Cycle 1 OFF times out at the CORE6 status poll** (OFF step 1): CORE6
  does not complete OFF even while clocked. TOP5 and the clocks stay on, as
  in the vendor-state image.
- **TOP5 OFF fails or times out** (OFF step 2): TOP5 does not complete OFF
  even while clocked. The GPU stays clocked and CORE6 OFF, which the
  snapshot after the failed OFF shows.
- **Cycle 2 ON fails**: the first OFF left the GPU in a state from which
  this ON order does not recover; the controller's error, step and the
  snapshot after the failed ON name the step and the CORE6 and PLL words.

### The EL3 setup on port 3-3

[The EL3 qualification runbook](../board-runbooks/el3-qualification.md)
runs in one fresh port 3-3 lifetime after this branch merges. It runs the
kernel check with the security writes including S_TZMA (write 11), a read
of six of the written words, and the six runs of the refreeze sequence on
the refrozen native image `6fd7af86…`, whose pre-scheduler overlay calls
the native setup (set `board-bundles-20261025-10/port-3-3/`). Its
predictions: each run returns, both security switches read 7, the three
S_TZMA words read 0, SPC `0x020540e0` reads `0xfffffbff`, and every 91-word
capture equals the clean capture.

### 2. Firmware start through the open-source host driver

The GPU firmware's license (`LICENSE.powervr` line 14) forbids reverse
engineering, decompiling or disassembling it. The firmware start is
therefore derived only from the open-source BXM DDK host driver
(`rgxstartstop.c`, `rgxinit.c`, `rgxtimecorr.c`, `rgx_sunxi/sunxi_platform.c`
at kernel `2ac08e8c7`), its published interface headers and the firmware
container fields that the host driver itself reads. Board observation uses
the same host interfaces: GPU registers, the firmware-started flag and the
firmware's shared host structures.

The diff of the vendor sequence against the [firmware start](../gpu-start.md)
gives three changes before any board run:

1. **Secure configuration.** `RGXDoStart` calls `sunxi_secure_config`
   before `RGXStart`: SMC0 `0x0a0000b0` = 2, then wrapper `drm_ctrl`
   `0x01880014` = 0 (`sunxi_platform.c:964–979`). The SunstoneOS start
   lacks both (action 5 of the vendor action plan).
2. **Core clock in the firmware configuration.** The firmware state still
   gives the firmware 400 MHz (`a733_gpu_reserved_prepare` callers and the
   GPU backend's admission of `A733_GPU_INITIAL_HZ`); the core now runs at
   `A733_GPU_CORE_HZ`, 1008 MHz, which the firmware's timer correlation
   needs.
3. **Timer read.** `RGXPostPowerState` reads `RGX_CR_TIMER` (`0x01800160`)
   before `RGXDoStart` (`rgxtimecorr.c:300`); the SunstoneOS start makes no
   such read.

Board steps, each after step 1 has returned:

- **The probe-read diagnostic on the default path** (row 17): the identity,
  the two secure-configuration writes with their read-backs, the identity
  again, and the restores. Prediction: both identities `36.56.104.183`,
  SMC0 `+0xb0` reads 2 and `drm_ctrl` 0 after the writes, both restored,
  and a return. A return with these values makes the secure configuration
  safe to add to the firmware start.
- **The firmware start**: the default ON, the secure configuration, the
  timer read and the 35-step start of `pvr_gpu_start.c`. Prediction: the
  firmware-started byte at system-init offset 352 becomes 1 within the
  start deadline, followed by the firmware's first host-visible status in
  its shared structures. A start that times out leaves the MIPS wrapper
  and reset registers to read through the host interface; the result names
  the step and the register values, and no firmware code is examined.
