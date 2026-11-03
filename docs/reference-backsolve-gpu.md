# GPU reference sequence and historical integration comparison

The A733 GPU power path selects the vendor's 1008 MHz/960 mV operating point.
[The power controller](../drivers/gpu/a733_gpu_power.c) enables PLL_GPU0,
releases bus reset, opens the bus/core clocks and waits for CORE6 ON.
[The supply provider](../drivers/gpu/a733_gpu_platform.c) raises DCDC4 before
that sequence. The disabled-PLL, 400 MHz, 800 mV path belongs to earlier images.

The current [compute queue](../platform/a733/gpu_queue.c) uses workload pages
no larger than 256 KiB; [the VM planner](../drivers/gpu/pvr_gpu_vm.c) rejects
larger pages. Executed-command health, secure writes and first-job MMU-cache
completion are implemented. [GPU status](status/gpu.md) states the current
qualification limits; standalone success does not qualify integrated recognition.

The vendor driver development kit (DDK) supplies the A733 platform reference.
The historical source comparison below describes code through `dc3d1ccd`,
including the removed application-specific OS session. It preserves the source
analysis in `~/.cache/sunstoneos/evidence/ref-gpu-hang-1031/report.md` and
`analysis.md`, which inspect native commit `9b0c7039` and a nine-image bisection
ladder. Their `inputs.json` and `firmware-metadata.json` retain source/image
hashes and firmware metadata. The comparison's gaps describe those frozen
inputs, rather than the current queue implementation.

**Observed** identifies source behavior or a recorded measurement.
**Inferred** identifies a predicted consequence. **Unknown** identifies a
question the cited evidence does not settle. This document requests no board
run. Use [the current handoff](handoff.md) for launch prerequisites and
[the watchdog record](watchdog.md) for failure capture.

## Terms

- **TOP5 and CORE6.** The GPU's two power domains in the PCK-600 power
  controller at `0x07060000`: domain 5 (GPU top, row `0x07065000`) and domain 6
  (GPU core, row `0x07066000`). Each row is an Arm power policy unit (PPU) as
  described in Arm DEN0051E. A row's policy word (`+0x0`) requests a power
  mode, its status word (`+0x8`) reports the mode reached (8 is ON), and its
  PWCR (`+0x20`) enables the device handshakes the PPU waits for.
- **Q-Channel.** The Arm low-power handshake between a PPU and the device in
  its domain. A transition to ON completes only when the device accepts.
- **GPU wrapper.** Allwinner registers at `0x01880000`, inside the GPU's
  `0x01800000`–`0x0188ffff` window, with DVFS, interrupt, DRAM-security and
  Q-Channel override words (`sunxi_dvfs_ctrl.c:113–150`).
- **SMC0.** The DRAM security controller at `0x0a000000`. The A733 manual
  gives its address and no registers; register names come only from vendor
  code.
- **SPC.** The secure peripheral controller at `0x02054000` (manual memory
  map), which decides per bus port whether non-secure software may
  reach a peripheral. Its register layout is not in the manual; BL31's use of
  it is the only reference.
- **SLC.** The GPU system-level cache. CPU cache maintenance alone does not
  transfer data held in this device cache.
- **Stall.** An access that does not complete. Ordinary CPU exception or
  timer recovery may fail. The independent watchdog has a separate reset path;
  a nonreturn alone does not establish a bus stall.

## References and their gaps

`~/.cache/sunstoneos/evidence/reference-backsolve-20261025/gpu/sources.json`
records each file used here with its URL, pinned revision and SHA-256. The
GPU-wrapper inventory of the same day,
`~/.cache/sunstoneos/evidence/gpu-wrapper-reference-20261025/sources.json`,
records the BXM DDK files. Kernel and BSP sources are from
`orangepi-xunlong/linux-orangepi` at `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`
(Linux 6.6.98); boot binaries and the build configuration are from
`orangepi-xunlong/orangepi-build` at `a95129bf30ea11886cc35f08ebb3e69695a26656`.
Citations without a path name these files:

| Short name | File |
| --- | --- |
| `sunxi_platform.c` | BXM DDK `services/system/rogue/rgx_sunxi/sunxi_platform.c` |
| `pck600_domains.c` | BSP `drivers/pm_domain/pck600_domains.c` |
| `ccu-sun60iw2.c`, `ccu_nm.c`, `ccu_gate.c`, `ccu_common.c` | BSP `drivers/clk/sunxi-ng/` |
| `sun60iw2p1.dtsi`, board dts | kernel `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi`, `sun60i-a733-orangepi-zero3w.dts` |
| kernel config | orangepi-build `external/config/kernel/linux-sun60iw2-current-a733.config` |
| manual | A733 User Manual V1.00 (extracted text) |
| boot0 | `boot0_sdcard_orangepizero3w.fex`, SHA-256 `829ea8e3…07fabe3`, loaded at `0x47000`; offsets are file offsets |
| BL31 | `monitor.fex`, SHA-256 `8af16b86…5b2632`, TF-A v2.5 debug build `7dddb263a-dirty`, loaded at `0x48000000`; offsets are file offsets |
| SCP | `scp.fex`, SHA-256 `07e6b976…b42749`, RISC-V; offsets are file offsets |

The effective vendor components are:

- **Power-domain driver for the GPU PPUs.** The only one built is
  `pck600_domains.c` (kernel config 893, `CONFIG_AW_PCK600_DOMAINS=y`).
  The BSP has two other PPU drivers. `gpu_domain.c` drives an older Allwinner
  GPU block (compatible `allwinner,gpu-pd`, power switches, isolation and a
  GPU power-request interrupt). `pm_domains.c` serves tv303, r528, a523 and
  v851. Neither is built (kernel config 891–892), and no A733 node matches them.
- **CCU driver.** `ccu-sun60iw2.c` and its helpers define PLL enabling,
  rate changes and lock waits.
- **Board device tree and PMIC bindings.** The kernel dtsi, board dts and
  AXP8191 regulator driver (`axp2101-regulator.c`, register header `axp2101.h`)
  define the GPU supply. The u-boot device tree of the same build
  (`pack-uboot/sun60iw2/bin/dts/u-boot-current.dts`) is built for a T736 demo
  board, registers CORE6 and gives the GPU node no power domain.
  It configures u-boot; Linux uses the kernel tree.
- **TF-A and SCP.** The pinned inputs contain binaries `monitor.fex` and
  `scp.fex`, with behavior inventories in
  [the vendor firmware record](vendor-firmware/README.md).
  No source is available in the pinned inputs.
- **Runtime PM path.** `drivers/base/platform.c`,
  `drivers/base/power/domain.c`, `runtime.c`, `common.c` and `dd.c` define
  domain attachment and runtime power management at the pinned revision.
- **Interconnect and platform calls.** The NSI driver (`sunxi-nsi.c`) attaches
  a GPU bus-master device to TOP5. The SID driver reads the speed bin;
  `sunxi-smc.c` defines the BSP secure-monitor calls.

Closed or unavailable:

- TF-A, SCP and boot0 sources.
- The SPC and SMC0 register descriptions.
- The DDK firmware `rgx.fw` that the vendor build loads (`config_kernel.h:2`).
  SunstoneOS uses Imagination's open firmware
  `rogue_36.56.104.183_v1.fw` (linux-firmware `8a58f818`, SHA-256
  `1db1c399…f9c0d2`).
- The `allwinner,sunxi-power-domain-test` driver named by the dtsi test
  nodes; it is not in the pinned BSP tree.
- The meaning of PLL_GPU0 bit 24, which boot0 sets and the manual marks
  reserved.

## The vendor sequence from cold boot to firmware start

The vendor chain runs boot0, BL31, SCP and Linux. SunstoneOS enters from
ROM FEL, calls boot0 functions for PMIC, clock and DRAM setup through
[the DDR wrapper](../boot/a733/ddr-entry.S), and uses its own EL3/EL1 images.
The [cached EL3 boot](../platform/a733-smp-cached/boot.c) calls
`a733_el3_security_setup()` before entering non-secure EL1. Compare each image's
actual entry path; the vendor sequence alone does not establish inherited state.

### 1. boot0 sets no GPU state beyond a DCDC4 enable and one PLL bit

boot0 makes no access to the PCK-600, the GPU window, SMC0, R_CCU `+0x1ac`
or the GPU clock words `0xb20`/`0xb24`, and never writes a DCDC4 voltage.
In the PMIC and clock function, which SunstoneOS also calls (offset `0x1fac`):

1. It reads the chip version at `0x03000024` (`0x169f0`).
2. It sets DCDC4's enable, AXP8191 register `0x10` bit 3 (`0x16976`–`0x16990`),
   and bit 7 of registers `0x13`–`0x1a`, which include DCDC4's voltage
   register `0x15`; bit 7 lies outside the 7-bit voltage field. DCDC4
   keeps its existing voltage selector.
3. It sets bit 24 of thirteen PLL words, the GPU PLL `0x020020e0` fourth
   (`0x21d6`). This explains the earlier board snapshot's `0x41104500`,
   where the manual gives a default of `0x40104500` (section 4.1.6.16).
   boot0 leaves the PLL disabled; the native GPU power path enables it later.

DRAM initialization (`0x2bdc`, also called by SunstoneOS) sets R_CCU
`0x07010250` bit 0 twice, with 2 ms and 10 ms waits after (`0x4842`,
`0x10924`), and writes NSI interconnect words whose master ports the blob
does not name. The normal boot path, which SunstoneOS skips, adds further
NSI writes after DRAM initialization (`0x1f38`). No firewall or SMC0 setup
is reached; the strings `sunxi_smc_config` and `smc-v2.c` are unreferenced.

### 2. BL31 makes peripherals and clock registers non-secure

BL31's cold-boot platform setup makes these writes, in order, besides UART,
GIC and CPU power setup. Its `sunxi_security_setup` (`0xc3b8`) runs again on
every resume from system suspend.

1. SPC ports 0–13 (`0x02054004`–`0x020540d4`, every `0x10`) and ports 16–23
   (`0x02054104`–`0x02054174`) set to `0xffffffff`. Port 14 is written
   `0xfffffbff` at `0x020540e0`, leaving bit 10 secure; port 15 is not
   written.
2. R_TZMA `0x07003000`, `+0x4`, `+0x8` = 0; R_SPC `0x07002004`, `+0x14`,
   `+0x24` = `0xffffffff`.
3. R_PRCM `0x07010290` |= 7.
4. CCU `0x02003f00` = 7: `CCMU_SEC_SWITCH_REG`, whose bits 0, 1 and 2 make
   the PLL, bus and MBUS clock registers non-secure (manual 4.1.6.278). The
   manual marks `PLL_GPU0_CTRL_REG` secure by default under bit 0 (4.1.6.16).
5. DMA0 `0x04601020` and DMA1 `0x04024020` = `0xffff`; CCU `0x02002800` =
   `0x80000002` and `0x02002850` = `0x10001` (not identified); timer
   `0x03009008` = 0.

BL31 never touches the GPU window, the PCK-600, the GPU clock words, the
NSI or `0x0a0000b0`. Of SMC0 it only reads the secure-region words
(`0x0a000110` + `0x10`·n) and, when SID `0x030060a0` bit 0 is set, the DRM
region words `0x0a000060`/`0x0a000064` (`0xd53c`). Its secure-register SMC
(`0xc000ff06`, handler `0xb1d8`) accepts only the two DMA security words.
It loads the SCP firmware only when a later stage calls SMC `0x8000ff10`
(`0xae88`). Calls numbered in the OP-TEE range, including `SETUP_MIPS`
(`sunxi-smc.c:95`), go to OP-TEE, which this build does not package
(`boot_package.cfg` comments out `optee.fex`).

### 3. The SCP firmware touches GPU state only on resume from suspend

u-boot starts the SCP just before Linux: with
`CONFIG_ARISC_DEASSERT_BEFORE_KERNEL` it issues SMC `0x8000ff10` with the
kernel device tree address (u-boot `b791be84`, `arch/arm/lib/bootm.c:407–412`),
and BL31 loads and releases the SCP (`0xae88`). That cold start reaches no
PCK, PLL, GPU or DCDC4 code. Every GPU-related SCP access belongs to the
system-suspend handler at `0x55a0`. It runs only on a CPU-operation message
(type `0x22`, dispatcher `0x6c32`–`0x6d08`) that requests system state 3.
Its GPU-related work is on the resume side:

- **On resume** it ORs policy 8 into PCK rows 0 to 10 and polls status bit 3
  of every row except CORE6, with no timeout (`0x5ec6`). It never accesses
  PWCR or the delay words of any row.
- **PLL table.** It saves 14 PLL words on suspend, the GPU PLL `0x0e0` among
  them. After the PCK loop it restores each with output gate bit 27 clear,
  sets enable bits 31 and 30 and then lock-enable bit 29, polls lock bit 28,
  waits, and sets bit 27 (`0x5ef4`–`0x5f80`).
- **R_CCU `0x07010250`.** Resume clears bits 3 and 8 (`0x5e9a`, mask
  `~0x108`). Bit 0 is left set.
- **SMC0.** It saves and restores eight secure-region words and writes
  `+0x4`, `+0x18`, `+0x70`–`+0x84` on resume (`0x57e4`–`0x581c`,
  `0x69a2`–`0x6a40`, `0x555c`). None is GPU-specific.

The SCP never touches the GPU window, the GPU clock words or DCDC4's voltage
register.

u-boot itself touches no GPU, PCK or SMC0 register, has no power-domain
driver, and sets no DCDC4 voltage (its `power_sply` node is absent). Linux
receives the kernel's device tree, loaded by `boot-sun60iw2.cmd:45` and passed
by `bootm` (line 61).

### 4. The kernel brings up shared resources before the GPU module loads

1. **Critical GPU bus gates.** The CCU driver keeps the GPU AHB gate
   (`0x020025c0` bit 7) and MBUS gate (`0x020025e0` bit 16) on as
   `CLK_IS_CRITICAL` (`ccu-sun60iw2.c:587–590, 658–661`). The boot script
   passes `clk_ignore_unused` (`boot-sun60iw2.cmd:38`), so unused-clock
   cleanup changes no clock; the GPU PLL is also `CLK_IGNORE_UNUSED`
   (`ccu-sun60iw2.c:187–189`), and its auto-gate word `0x02003914` keeps its
   reset value.
2. **PCK-600 domains.** At `subsys_initcall` (`pck600_domains.c:633`), the
   driver writes each registered domain's delay row (126–142; values 547–559)
   and registers it with `is_off` false (283), so the generic domain code
   records it as ON without touching the hardware
   (`drivers/base/power/domain.c:2067`). It enables the PPU clock
   (R_CCU `+0x1ac`, 476) and refuses every power-off for 10 s (488–490,
   174–178). CORE6 is not registered: its node is commented out
   (`sun60iw2p1.dtsi:769–773`), so its delay row is not written. No
   `GENPD_FLAG_PM_CLK` is set (282), so domain transitions switch no device
   clocks (`domain.c:2084`).
3. **TOP5.** TOP5 reads policy 8 and status 8 from reset (port 3-3 snapshot,
   `docs/gpu-power.md`). Its first power-on request through the domain code
   is a no-op, because the domain is recorded ON (`domain.c:761–766`).
   A power-on after an earlier power-off writes policy 8, issues `DSB SY`
   and polls status for at most 10 ms (`pck600_domains.c:104–124`).
4. **NSI.** The interconnect driver attaches a GPU bus-master device to TOP5
   (`sun60iw2p1.dtsi:1514–1517`; `sunxi-nsi.c:1191–1219, 1346–1371`) and takes
   a runtime reference only while it sets QoS (1137–1189). It writes no GPU
   port register on this SoC; its GPU bandwidth limits are for sun50iw10
   only (`sunxi-nsi.c:827–848`).
5. **DCDC4.** The board marks DCDC4 always-on and boot-on, 500–1540 mV, ramp
   250 µV/µs (board dts 2141–2149), and assigns it as `gpu-supply` (3420–3422).
   The selector is register `0x15`, 7 bits, 10 mV steps from 500 mV to code
   `0x46` (`axp2101.h:985`; `axp2101-regulator.c:1297–1300, 1344–1347`).

### 5. The GPU probe powers, clocks and raises the voltage before the identity read

The BXM module `pvrsrvkm.ko` is built with `PVR_SYSTEM=rgx_sunxi` and
`SUPPORT_LINUX_DVFS` (`config_kernel.h:37`). `platform_probe` first attaches
the device to TOP5 with power-on requested (`drivers/base/platform.c:1398`;
`domain.c:2774–2778`), which is a no-op while TOP5 is recorded ON. Then
`PVRSRVCommonDeviceCreate` calls `SysDevInit` (`pvrsrv.c:2258`), which runs
`sunxi_platform_init` (`sysconfig.c:281`):

1. **Operating point.** `sunxi_get_opp` takes the highest operating point,
   1008 MHz (`sunxi_platform.c:754–759`; `sun60iw2p1.dtsi:4736`). Its voltage
   is the speed-bin entry named by `vf_mapping_table`, or `vfdefault` when the
   device tree has no such table (`sunxi_platform.c:649–653, 729–732`). The
   kernel tree has no table, so the voltage is `opp-microvolt-vfdefault`,
   960 mV (`sun60iw2p1.dtsi:4739`). The u-boot tree's table maps the codes 0,
   1 and 2 to `vf0000`, `vf0100` and `vf0200`, which are also 960 mV. No speed
   bin read changes the voltage on this board.
2. **Supply.** `regulator_enable` (899), then `sunxiSetVoltage(960 mV)`
   (901): DCDC4 code `0x2e`, a ramp delay of 160 mV at 250 µV/µs rounded up
   to 1 ms (`axp2101-regulator.c:383–410`), then `udelay(200)`
   (`sunxi_platform.c:473–485`).
3. **TOP5.** `pm_runtime_enable` and `pm_runtime_get_sync` (911–912); TOP5 is
   already ON.
4. **CORE6.** `0x07066020` (PWCR) = 0, then `0x07066000` (policy) = 8
   (915–916, functions 72–91). Nothing reads CORE6 status.
5. **Clocks and reset**, `sunxi_enable_clks_wrap` (920, body 167–223):
   1. Enable the GPU PLL (173). The enable helper clears output gate bit 27,
      sets LDO bit 30, sets enable bit 31, sets lock-enable bit 29, polls lock
      bit 28 every 3 µs until three consecutive reads show it (at most 100
      reads), then sets bit 27 (`ccu_gate.c:157–203`; `ccu_common.c:52–61`).
      The factors are those inherited from boot0: N = 70, P0 = 2, 840 MHz
      from 24 MHz (manual 4.1.6.16).
   2. Enable the PERI0 800, 600, 400, 300 and 200 MHz outputs (177–198).
   3. Deassert the GPU reset, `0x02002b24` bit 16 (203;
      `ccu-sun60iw2.c:2105`).
   4. Open the GPU bus gate, `0x02002b24` bit 0 (208; 1045–1047).
   5. Open the core clock gate, `0x02002b20` bit 31, with the update bit 27
      (213; 1030–1043). The clock keeps its reset selection, source 0, the
      GPU PLL (manual 4.1.6.121), with no cycle masking.
   6. Read `0x02002b20` and write it back with bit 27 set (217–220).
6. **Rate.** `sunxiSetFrequency(1008 MHz)` (922, 405–417) selects the GPU PLL
   as parent and sets the PLL to 1008 MHz. N must lie in 53–105
   (`ccu-sun60iw2.c:178`), so the PLL takes N = 84, P0 = 2 (VCO 2016 MHz).
   With `CONFIG_AW_STANDARD_CCU=y` (kernel config 49) the set-rate path clears
   output gate bit 27, writes N (bits 15:8 = `0x53`) and P0 (bits 22:20 = 1),
   clears and sets lock-enable bit 29, waits for lock and sets bit 27 again
   (`ccu_nm.c:250–345`). The PLL word becomes `0xe9105300` with boot0's bit 24
   kept.
7. **Identity read.** `RGXRegisterDevice` (`pvrsrv.c:2348`) reaches
   `RGXBvncInitialiseConfiguration` (`rgxinit.c:5267`), which finds the system
   powered and reads `RGX_CR_CORE_ID__PBVNC` with `OSReadHWReg64`
   (`rgxbvnc.c:119, 694`): two 32-bit loads, `0x01800024` and `0x01800020`
   (`osfunc.h:1086–1095`). Because the branch is above 20 it then reads
   `RGX_CR_MULTICORE_SYSTEM` and the SLC size (`rgxbvnc.c:704, 709`).

### 6. Firmware start follows the secure configuration

On each later power-on, `RGXPostPowerState` reads `RGX_CR_TIMER`
(`0x01800160`, `rgxtimecorr.c:300`) and then calls `RGXDoStart`
(`rgxpower.c:1010–1037`, 865–889). That runs `sunxi_secure_config`
(`sysconfig.c:371`; `sunxi_platform.c:964–979`): SMC0 `0x0a0000b0` = 2, then
wrapper `drm_ctrl` `0x01880014` = 0. `RGXStart` then writes
`RGX_CR_SYS_BUS_SECURE` (`0x0180a100`) = 0 and reads it back
(`rgxstartstop.c:859–860`), runs the soft-reset sequence (775–816), initializes
the system-level cache (SLC), MIPS wrapper and AXI-ACE interface (867–945),
then releases the MIPS GARTEN reset (947–979).

The Bus Interface (BIF) initialization routine returns without register programming for MIPS
(`rgxstartstop.c:643–657`). Firmware boot uses MIPS boot/data/exception remaps
and the BRN 63553 address-zero remap (`rgxstartstop.c:287–456`). FWCORE_BOOT
programming belongs to the RISC-V path. The final `SYS_BUS_SECURE` enable is
conditional on `SUPPORT_TRUSTED_DEVICE` without `SUPPORT_SECURITY_VALIDATION`
(`rgxstartstop.c:983–986`). The selected BXM `config_kernel.h` does not enable
trusted-device support, so that enable is absent in this build.

The DDK uses its own `rgx.fw` and firmware ABI. SunstoneOS uses the pinned open
firmware container; its structures and command identifiers follow that interface.
Generic upstream confirms the MIPS BIF exclusion and remaps
(`pvr_fw_startstop.c:173`, `pvr_fw_mips.c:97` at the revision below).

## Current power sequencing matches the vendor operating point

**Observed:** `a733_gpu_power.c:on_steps` clears CORE6 PWCR, enables and locks
PLL_GPU0, releases bus reset, opens bus/core gates, selects 1008 MHz and
requests TOP5/CORE6 ON. It requires CORE6 status 8 before engine access.
This follows the vendor's later power-on ordering
(`sunxi_platform.c:342–352`), with an additional acknowledgement check.
The vendor probe's initial order remains documented in section 5 above.

**Observed:** the supply provider accepts or raises DCDC4 to 960 mV and waits
for settlement (`a733_gpu_platform.c:supply_raise`). The firmware clock field
is set to 1008 MHz by `a733_gpu_backend_bind()` in
[the backend](../platform/a733/gpu_backend.c). Clock source and supply are
resolved differences for this path; they do not identify the current failure.

**Observed board scope:** standalone firmware start/health/stop/restart,
eight compute jobs and two exact-sum shapes pass under their frozen inputs.
The records include `sol-resume-20261028/port-3-1/gpu-exact-sum/` under
`~/.cache/sunstoneos/evidence/`. Complete GPU recognition and non-idle recovery remain open.

## The firmware-only diagnostic stops in its compatibility callback

The recorded `ladder-wdt3/2-firmware` image on port 3-1 stopped in `compat()`
at `platform/a733/gpu_backend.c:375` in that frozen build. The first-failure
record retains caller `0x41088a50` and value `0x04040201`. Evidence is under
`~/.cache/sunstoneos/evidence/dev-runs-1031/3-1-gpu-return-204909/` in
`dev-run.json` and `run/watchdog-report.json`.

That diagnostic comes from the unmerged GPU-reference branch. Its line numbers
and behavior do not describe the merged backend. The stop localizes the
callback; it does not prove a firmware ABI mismatch or a workload page fault.
The omissions below describe the frozen source comparison.

## Historical service omissions relative to the standalone probe

The October 31 analysis compares native source with the selected DDK and
upstream Imagination driver at
`cbbddb6b08cc9900a87cccaa9b22bbe06aa4f50a`.
Its cached upstream root is
`~/.cache/sunstoneos/evidence/ref-gpu-hang-1031/linux/drivers/gpu/drm/imagination/`.
Paths and symbols below identify the compared revision, including files since
removed from the OS. Upstream line citations use the pinned source.

| Boundary | Observed behavior in the compared revision | Reference and inferred consequence |
| --- | --- | --- |
| Health completion | `drivers/gpu/pvr_gpu_service.c` calls `pvr_gpu_control_init()` but never binds `pvr_gpu_control_count_executed()`. Without the callback, the control requires a health return word of 1. | The standalone `apps/gpu-power-id/fw-start.c` binds executed-count completion. Upstream `pvr_power.c:165–196` checks executed progress and submits HEALTH_CHECK without a return-slot waiter. **Inferred:** firmware can start and execute health while service startup times out before any job. |
| Vendor secure writes | `platform/a733/gpu_backend.c:power_poll` proceeds from identity/capability checks to active state without `0x0a0000b0=2`, then `0x01880014=0`. | DDK `sunxi_platform.c:964–979` and standalone `fw-start.c:secure_config` perform these ordered 32-bit writes. **Inferred:** missing secure preparation can block firmware memory access. Its necessity in inherited board state is **unknown**. |
| Initial timer correlation | The backend publishes clock/timer correlation during later performance changes, without an initial sample before boot. | DDK `rgxpower.c:1010–1037` and standalone `fw-start.c` publish the initial correlation before firmware start. **Inferred:** missing correlation can affect timing interpretation; evidence for a permanent stall is weaker. |
| MMU-cache invalidation | `drivers/gpu/pvr_gpu_flush.c` implements MMUCACHE, command 102, but the service never invokes it before the first workload. CPU cleaning publishes tables without invalidating GPU translation caches. | DDK `rgxfwutils.c:4065`, upstream `pvr_job.c:742` and `pvr_mmu.c:147`, and standalone `fw-job.inc:367` perform pre-job invalidation. **Inferred:** stale translations can fault the first job independently of page size. |
| SLC ownership | `platform/a733/gpu_mimus.c` keeps device caching enabled for shared job, arena and output mappings. `pvr_compute_mimus.c` performs CPU cache maintenance without a device-SLC ownership transfer. | DDK `rgxcompute.c:1283` supports SLC flush. Upstream `pvr_mmu.c:2601` and standalone `fw-job.inc:158` support bypass. **Inferred:** output can remain stale after timeline progress, or later jobs can reuse stale arguments. |
| Workload page sizes | `platform/a733/gpu_mimus.c` uses shift 21, meaning 2 MiB, for models, arena and output. Firmware separately uses flat 4 KiB MIPS translations. | DDK `rgxinit.c:4948` rejects 1 MiB and 2 MiB workload pages for BRN 71317. The analysis branch uses 256 KiB; upstream implements 4 KiB and 64 KiB (`pvr_mmu.c:33`). **Inferred:** workload accesses can fault even after successful firmware boot. |
| Register access | The backend uses 64-bit MMIO for 64-bit operations and reads additional capability registers before firmware start. | DDK `osfunc.h:1086–1095` and the successful standalone probe split accesses into 32-bit halves; generic upstream uses `ioread64`. A733 behavior for the backend's accesses is **unknown**. |
| Close and failure | `pvr_gpu_service.c` omits legacy application context CLEANUP 106; failure retains ownership. `pvr_gpu_stop.c` requires idle before power removal. | Upstream `pvr_queue.c:1118` cleans contexts; standalone `fw-start.c` has bounded idle-checked recovery. **Inferred:** an ordinary timeout can prevent return without a bus hang. Timeout does not prove drain. |

The health omission has direct earlier board evidence:
`~/.cache/sunstoneos/evidence/board-session-20261025-18/port-3-1/s18-gfx-run/kernel.log`
records executed count 1 with return word 0 and a health timeout.
That observation supports a service-startup prediction; it does not establish
the cause of the later complete-image nonreturn.

## Firmware and job interfaces must follow the selected ABI

**Observed:** the native loader validates and stages ELF segments and zero tails
within a 16 MiB firmware heap at `0xc0000000` (`pvr_firmware.c`, `pvr_fw_state.c`).
Connection, OS-init and system-init occupy `0xc0fd0000`, `0xc0fe0000` and
`0xc0ff0000`. MIPS boot remaps and flat firmware translations are independent
of the GPU catalogue/directory/leaf tables used by workloads.

**Observed:** the kernel command ring has 128 slots; the firmware ring has
32 slots. Both use 88-byte commands (`include/pvr_ccb.h`). The legacy application
submits COMPUTE 205 and UPDATE 213 before KICK 101, then schedules through
`0x01800b00=0`
(`pvr_mimus_service.c`, `pvr_cccb.c`, `pvr_ccb.c`). Context creation publishes
objects and pointers; no separate CREATE command is missing. Completion uses
the timeline, with IRQ status/clear at `0x8a8/0x8b0` (`pvr_gpu_control.c`).
Timeline progress alone does not establish correct output or safe retirement.

**Observed:** the DDK lists errata BRN 63553 and BRN 71317. Open-firmware
metadata additionally advertises BRN 44079 and enhancements with different
bit numbering.
The analysis directory's `firmware-metadata.json` and
[the container inventory](vendor-firmware/data/gpu-firmware-container.json)
retain the decode and image hash. **Unknown:** whether native shader allocations
reach the resource limits associated with those additional flags.

## Compatibility must succeed before health or workload diagnosis

The frozen firmware-only diagnostic stops in the compatibility
callback. Resolve that boundary before interpreting its later health or
workload stages. The earlier executed-count/return-slot evidence still supports
the health-binding gap in the compared service; it does not identify this stop.

Only after startup succeeds can a 256 KiB versus 2 MiB comparison test
BRN 71317. A shared startup failure is uninformative about page size.
First-job failures also leave the MMUCACHE and SLC ownership gaps unresolved.
The current fixes and remaining qualification work are recorded in
[GPU status](status/gpu.md); historical images have no new launch authority.
