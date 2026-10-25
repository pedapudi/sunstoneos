# The vendor reads the GPU identity after three steps that SunstoneOS omits

This document reconstructs, from every available vendor reference, what the
vendor software stack does on the Allwinner A733 from cold boot to the first
read of the GPU core identity and on to GPU firmware start. It then compares
that sequence with the SunstoneOS GPU power controller and each diagnostic
variant, and recommends one next board image. It describes analysis only; no
board run, image or code change accompanies it.

Every SunstoneOS image that has read the GPU core identity register
(`RGX_CR_CORE_ID__PBVNC`, `0x01800020`) has failed to return. The last two,
which read it as the vendor does and then also in the vendor's clock and reset
order, stalled as well. The comparison below finds three differences that no
image has yet removed, and that the vendor applies on every boot before that
read:

- **GPU clock source.** The vendor enables the GPU PLL and runs the GPU core
  from it, at 1008 MHz. SunstoneOS leaves the GPU PLL disabled and runs the
  core from the 400 MHz PERI0 output.
- **GPU supply.** The vendor raises DCDC4 to 960 mV before it powers the GPU.
  SunstoneOS keeps the PMIC's power-on value, which reads 800 mV.
- **Secure setup.** The vendor's EL3 firmware (TF-A BL31) marks the peripheral
  security controller ports and the clock controller's PLL, bus and MBUS
  registers non-secure at cold boot. SunstoneOS boots through FEL with its own
  EL3 monitor, which makes none of those writes.

The reference supports removing all three in one image, and the last section
specifies it.

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
- **Stall.** A CPU load that never completes. It raises no exception, so the
  image cannot report, the monitor's recovery timer cannot return it to the
  ROM, and only a power removal recovers the board.

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

The requested references, and what they turned out to be:

- **Power-domain driver for the GPU PPUs.** The only one built is
  `pck600_domains.c` (kernel config 893, `CONFIG_AW_PCK600_DOMAINS=y`), which
  was already cached. The BSP has two other PPU drivers. `gpu_domain.c`
  drives an older Allwinner GPU power block (compatible `allwinner,gpu-pd`,
  power switches, isolation and a GPU power-request interrupt), and
  `pm_domains.c` serves tv303, r528, a523 and v851. Neither is built (kernel
  config 891–892), and no A733 device-tree node matches them. Both were
  fetched and recorded.
- **CCU driver.** `ccu-sun60iw2.c` and its helpers were cached; the PLL
  enable, set-rate and lock-wait helpers are cited below.
- **Board device tree and PMIC bindings.** The kernel dtsi and board dts were
  cached. The AXP8191 regulator driver (`axp2101-regulator.c`, register header
  `axp2101.h`) was fetched. The u-boot device tree of the same build
  (`pack-uboot/sun60iw2/bin/dts/u-boot-current.dts`) was fetched; it is built
  for a T736 demo board, registers the CORE6 domain and gives the GPU node no
  power domain. It configures u-boot; Linux uses the kernel tree.
- **TF-A and SCP.** Only binaries exist: `monitor.fex` (fetched) and
  `scp.fex` (already cached). Both were disassembled. No TF-A or SCP source
  is published for this board.
- **Runtime PM path.** `drivers/base/platform.c`,
  `drivers/base/power/domain.c`, `runtime.c`, `common.c` and `dd.c` were
  fetched at the pinned kernel revision.
- **Also fetched.** The NSI interconnect driver (`sunxi-nsi.c`), which
  attaches a GPU bus-master device to TOP5; the SID driver for the speed-bin
  read; and the BSP SMC call driver (`sunxi-smc.c`).

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

SunstoneOS skips stages 1 to 3 below. It enters from the ROM's FEL mode,
calls three boot0 functions for PMIC, clock and DRAM setup
(`boot/a733/ddr-entry.S:44–66`), and then runs its own EL3 monitor and a
non-secure EL1 image (`platform/a733/monitor.c:96–97`, `SCR_EL3` = `0x505`).

### 1. boot0 sets no GPU state beyond a DCDC4 enable and one PLL bit

boot0 makes no access to the PCK-600, the GPU window, SMC0, R_CCU `+0x1ac`
or the GPU clock words `0xb20`/`0xb24`, and never writes a DCDC4 voltage.
In the PMIC and clock function, which SunstoneOS also calls (offset `0x1fac`):

1. It reads the chip version at `0x03000024` (`0x169f0`).
2. It sets DCDC4's enable, AXP8191 register `0x10` bit 3 (`0x16976`–`0x16990`),
   and bit 7 of registers `0x13`–`0x1a`, which include DCDC4's voltage
   register `0x15`; bit 7 lies outside the 7-bit voltage field. DCDC4
   therefore runs at the PMIC's power-on voltage.
3. It sets bit 24 of thirteen PLL words, the GPU PLL `0x020020e0` fourth
   (`0x21d6`). This explains why that word reads `0x41104500` on the board,
   where the manual gives a default of `0x40104500` (section 4.1.6.16). The
   PLL stays disabled.

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
  `~0x108`). Bit 0 is left set. `docs/gpu-power.md` describes this
  instruction as clearing bits 0, 3 and 8; bit 0 is not cleared.
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
(`rgxstartstop.c:859–860`), runs the soft-reset sequence (775–816), SLC,
MIPS wrapper, AXI-ACE and BIF setup (867–945), takes the MIPS core out of
reset (947–979) and sets `SYS_BUS_SECURE` again (984–986). The firmware is
the DDK's `rgx.fw`.

## The GPU driver itself powers CORE6, and nothing waits for it

The CORE6 writers are:

- **Kernel GPU driver.** `sunxi_platform.c` writes CORE6 PWCR 0 and policy 8
  at probe (72–91, 915–916), policy 8 on each power-on (352) and policy 0 on
  each power-off (377). It never reads CORE6.
- **Kernel PPU driver.** `pck600_domains.c` would drive CORE6 like TOP5 if
  its node were present, but the node and its test node are commented out
  (`sun60iw2p1.dtsi:769–773, 3416–3422`). The older `gpu_domain.c`, which
  sequences GPU power switches itself, is not built (kernel config 891).
- **boot0 and BL31.** No access to the PCK-600.
- **SCP.** Only on resume from system suspend: policy 8 into every row, with
  CORE6's status poll skipped (`0x5ec6`). Nothing at cold boot.

No reference shows CORE6 status reaching 8, and no reference reads the GPU
core with CORE6 acknowledged, because no reference reads CORE6 status at all.

## Clock, supply and secure state remain different after four board runs

The SunstoneOS sequence is the power-only ON of
`drivers/gpu/a733_gpu_power.c:29–77` with its resource admission in
`drivers/gpu/a733_gpu_resources.c:95–118`, plus the variant clients in
`apps/gpu-power-id/`. The board records are:

| Set | Session | Image and change | Result |
| --- | --- | --- | --- |
| `board-bundles-20261024-11` | `board-session-20261024-11/port-3-1` | power-only ON with the CORE6 delay row (`ade71834`) | Returned; CORE6 status 0 throughout; snapshot shows PLL_GPU0 `0x41104500` (disabled), `0xb20` `0x83000000` |
| `board-bundles-20261024-14` | `board-session-20261024-14/port-3-1` | power-only ON, then four wrapper reads | Returned; wrapper answers with TOP5 ON and CORE6 at 0 |
| `board-bundles-20261025-3` | `board-session-20261025-1/port-3-1` | power-only ON, wrapper `+0x14`/`+0x10`, SMC0 `+0xb0`, then the identity as two 32-bit loads (bundle 17) | Did not return within 925 s; no write made |
| `board-bundles-20261025-4` | `board-session-20261025-3/port-3-1` | vendor clock and reset order, identity only (`3615f340…`, bundle 18) | Did not return: the host recorded "FEL did not return with access before the deadline" at 10:40:22 |

`s7-ddr` and the provider-readiness run (`s9-gpr-run`) passed earlier in the
last lifetime. The last two rows differ in the reads before the identity and
in the clock and reset order; both stalled. The identity read itself, with
CORE6 unacknowledged, 400 MHz from PERI0, 800 mV and no secure setup, is
therefore the common stalling access.

Divergence classes: **missing step** (the reference does it, we do not),
**order**, **value**, **missing wait**, **extra step** (we do it, the
reference does not). Risk is the likelihood that the difference causes the
stalled identity read.

| Reference step | Citation | Our step | Citation | Class | Risk |
| --- | --- | --- | --- | --- | --- |
| GPU PLL enabled (840 MHz, lock polled) before the core clock | `sunxi_platform.c:172–173`; `ccu_gate.c:157–203` | GPU PLL left disabled | `a733_gpu_power.c:64–67`; set-11 snapshot `0x0e0` = `0x41104500` | missing step | high |
| Core clock opened on source 0, the GPU PLL, then the PLL set to 1008 MHz | `sunxi_platform.c:212–220, 922, 405–417`; `ccu_nm.c:250–345` | Core clock on source 3, PERI0 400 MHz | `a733_gpu_power.c:64–67`; `a733_gpu_power.h:7` | value | high |
| DCDC4 960 mV, then 1 ms ramp and 200 µs, before TOP5 and CORE6 | `sunxi_platform.c:899–901, 473–485`; `sun60iw2p1.dtsi:4739` | DCDC4 checked at the power-on 800 mV, unchanged | `a733_gpu_power.h:8`; `a733_gpu_resources.h:76` | value, missing wait | high |
| SPC ports set non-secure at EL3 | BL31 `0xc3b8` | No SPC write | `platform/a733/monitor.c:96–97` (no security setup in the tree) | missing step | medium (wrapper reads in the same window returned) |
| `CCMU_SEC_SWITCH_REG` = 7, so PLL, bus and MBUS registers accept non-secure writes | BL31 `0xc3b8`; manual 4.1.6.278, 4.1.6.16 | Not written; all GPU clock writes are non-secure EL1 | `platform/a733/monitor.c:96–97` | missing step | high, as the prerequisite of the GPU PLL rows |
| R_PRCM `0x07010290` \|= 7, R_SPC and R_TZMA writes | BL31 `0xc3b8` | Not written | none | missing step | medium |
| SCP firmware loaded and running | BL31 `0xae88` (on request) | SCP never started | none | missing step | low (its cold start touches no GPU state) |
| CORE6 PWCR 0, policy 8, no status read | `sunxi_platform.c:915–916` | Same writes; status observed but not required | `a733_gpu_power.c:58–63, 76` | none | not a divergence; status 0 in every run |
| CORE6 delay row never written | `sun60iw2p1.dtsi:769–773` | Five delay writes and readbacks of the values held from reset | `a733_gpu_power.c:47–56` | extra step | low (set 11: inert) |
| Reset deasserted, then bus gate, then core gate | `sunxi_platform.c:202–215` | Gates, 1 µs, then reset; reordered in the vendor-clk variant | `a733_gpu_power.c:64–72`; `vendor-clk-power.c:65–72` | order | low (set `20261025-4` used the vendor order and still stalled) |
| Identity read as two 32-bit loads | `osfunc.h:1086–1095` | One 64-bit load in `fd3d5839`; two 32-bit loads since | `main.c:127–139`; `probe-read.c:64–79`; `vendor-clk.c:35, 54` | value | low (both later forms stalled) |
| No access before the identity read | `rgxbvnc.c:694` | Wrapper `+0x14`, `+0x10` and SMC0 `+0xb0` read first | `probe-read.c:64–67` | extra step | low (set `20261025-4` stalled without them) |
| No wait for CORE6 before the identity read | `sunxi_platform.c:915–922` | 200 ms observation of CORE6 status | `vendor-clk.c:75–80` | extra step | low |
| TOP5 ON from reset, recorded ON | `pck600_domains.c:283`; `domain.c:2067` | Owner adopts the inherited ON | `docs/gpu-power.md` (owner) | none | low |
| PPU clock R_CCU `+0x1ac` on | `pck600_domains.c:476` | Checked on | `a733_gpu_power.c:32` | none | low |
| PERI0 outputs enabled | `sunxi_platform.c:177–198` | PERI0 400 MHz checked on, through the shared lease | `a733_gpu_resources.c:99–118` | none | low |
| Normal-boot NSI writes after DRAM init | boot0 `0x1f38` | Skipped (FEL path) | `ddr-entry.S:44–66` | missing step | low (QoS words of memory ports) |
| SMC0 `+0xb0` = 2 and `drm_ctrl` = 0 before `RGXStart` | `sunxi_platform.c:964–979` | Not in the firmware start | `pvr_gpu_start.c:92` | missing step | none for the identity read; required at firmware start |
| DDK firmware `rgx.fw` | `config_kernel.h:2` | Open firmware `rogue_36.56.104.183_v1.fw` | `docs/gpu.md:92` | value | none for the identity read |

## The GPU clock source ranks first among the divergences

1. **The GPU PLL and the core clock source.** In every board run the GPU PLL
   was disabled and the core ran from PERI0. The vendor never reads the core
   without the GPU PLL running. The manual ties GPU clock updates to a
   request and acknowledge handshake with the GPU (4.1.6.121, bit 27), so
   GPU-side logic takes part in clock selection. If any GPU-side power or
   Q-Channel logic is clocked from the GPU PLL directly, a disabled PLL
   leaves CORE6's handshake unanswered and the core window unpowered, which
   matches CORE6 status 0, `PPU_MISR` 0 and a stalled read. This is an
   inference from register behaviour; no reference states the clock tree
   inside the GPU.
2. **The secure setup.** Linux reads the GPU from non-secure EL1 only after
   BL31 has opened every SPC port but one bit. SunstoneOS's reads of the GPU
   wrapper inside the same window returned, which argues that the wrapper's
   port is already non-secure; the SPC granularity inside the GPU window is
   not documented, so the core registers may sit behind another port. The
   clock-controller switch matters regardless: without bit 0, the PLL rows
   above may be ignored when written from non-secure EL1.
3. **The supply.** 800 mV is the dtsi's voltage for 400 MHz
   (`sun60iw2p1.dtsi:4655`, `opp@400000000`), so it is in specification for our
   clock. It ranks below the clock because no reference ties CORE6's power
   switches to a supply level, but the vendor never brings the GPU up at
   800 mV, and the user has approved the change.
4. **The SCP firmware.** Low. Its cold start, the only part that precedes
   the vendor's identity read, touches no PCK, PLL or GPU state.
5. **Everything else** is ruled out by board records or is inert: reset
   order and the extra reads by set `20261025-4`, the delay row by set 11,
   the access width by sets `20261025-3` and `20261025-4`.

## The next image applies the vendor's state at once

### Contents

The image keeps the vendor-clk variant's structure (power-only ON, CORE6
observation, two 32-bit identity loads, exit with the GPU left powered) and
changes these steps, in this order:

1. **At EL3, before entering non-secure EL1:** BL31's security writes of
   section 2: the SPC port words, R_SPC, R_PRCM `0x07010290` \|= 7 and CCU
   `0x02003f00` = 7. Read back `0x02003f00`. The R_TZMA words, the DMA words,
   the unidentified CCU `0x2800`/`0x2850` writes and the timer write are left
   out: none concerns a peripheral that the image reads, and R_TZMA governs
   secure memory, which the GPU identity read does not use.
2. **DCDC4 to 960 mV:** register `0x15` field `0x7f` = `0x2e`, keeping bit 7;
   read back; wait 1.2 ms. This replaces the 800 mV admission check of
   `a733_gpu_resources.c` for this image only.
3. **TOP5** adopted ON, as today.
4. **CORE6** PWCR 0 then policy 8, as today, without the delay row.
5. **GPU PLL enable** at the inherited factors, by the helper sequence of
   section 5 step 5.1, with the lock poll bounded at 100 reads 3 µs apart and
   its result recorded.
6. **Reset, bus gate, core gate** in the vendor order, with `0xb20` written
   as gate bit 31, source 0, divider 0 and update bit 27; read back until
   bit 27 clears, bounded; then the read-modify-write of bit 27.
7. **GPU PLL to 1008 MHz** by the set-rate sequence of section 5 step 6;
   expected word `0xe9105300`, lock recorded.
8. **CORE6 observation** of at most 200 ms, recorded, not required.
9. **Identity:** `0x01800024`, then `0x01800020`.

The vendor's own TOP5-before-voltage order is not reproduced: TOP5 is ON
from reset in both stacks, so raising DCDC4 first changes no domain state.

### One image tests the reference state, where single variables have not

- **Each run costs a lifetime.** A stall leaves no report, keeps the host
  waiting for its deadline and needs the user's power removal, which is
  available every several hours. The GPU runs of sets 11, 14, `20261025-3`
  and `20261025-4` used four lifetimes. Each changed one difference, and the
  last two stalled with the clock, supply and secure state unchanged.
- **The reference state is the only one known to read the identity.**
  Vendor Linux reads it on these boards with all three differences removed.
  If an image that matches that state returns the identity, the remaining
  SunstoneOS state is adequate. That run is then a returning baseline, and
  later images can drop one change at a time without risking a stall. If it
  stalls, the cause lies outside the GPU sequence, in state that SunstoneOS
  still does not reproduce, such as the BL31 writes left out above or the
  vendor's DRAM-controller configuration.
- **The three changes are coupled.** The vendor never runs the GPU at
  1008 MHz below 960 mV, and the PLL writes may need the clock controller's
  security switch. Testing the clock without the voltage or the switch would
  either run the GPU out of its operating point or test writes that may be
  ignored.

### Prerequisites and open points

- The user's approval covers the GPU clock and voltage. The EL3 security
  writes change the monitor that every image shares; they need the
  coordinator's acceptance for this image's monitor build.
- Whether non-secure EL1 can write PLL_GPU0 is unknown. The image reads
  `0x02003f00` and the PLL word after each write and stops before the
  identity read if the PLL word did not take, so a refused write ends the
  run with a report instead of a stall.
- The image adds a GPU PLL owner. The resource manager must hold the PLL and
  the DCDC4 change as GPU-exclusive for the image's lifetime.
- Performance budget: the additions cost about 40 register accesses, one
  PMIC write and readback, at most two 300 µs lock polls and a 1.2 ms supply
  wait, once per lifetime.
