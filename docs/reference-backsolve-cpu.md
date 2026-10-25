# CPU bring-up compared step by step with the reference implementations

This document derives the A733 CPU bring-up sequence from pinned reference
sources and vendor binaries, and compares SunstoneOS with it step by step. It
covers secondary power-up, exception-level setup, GIC-600 initialization, the
generic timer, caches and coherency, floating point, idle, power-down and
DVFS. It is a source comparison only. No fix described here is implemented,
and no board ran during the comparison.

The A733 has six Cortex-A55 cores (0–5, r2p0) and two Cortex-A76 cores (6–7,
r4p1) in one DynamIQ cluster. MPIDR affinity level 1 holds the core index. The
GIC-600 is revision r1p6: the distributor at `0x03400000` reports
`GICD_IIDR = 0x0201643b`, whose product field `0x02` is GIC-600 and whose
variant and revision fields are 1 and 6. Its redistributor frames start at
`0x03460000`, one `0x20000` frame per core.

SunstoneOS starts from the boot ROM at EL3 and runs no vendor EL3 firmware.
It therefore performs itself the per-core work that TF-A or the vendor BL31
would do before an operating system runs, and the comparison treats both as
references for that work.

## The references and their gaps

Every reference file, its URL, revision and SHA-256 is recorded in
`~/.cache/sunstoneos/evidence/reference-backsolve-20261025/cpu/sources.json`.
Citations below give a repository-relative path and line within the pinned
revision, or a binary's name and file offset.

| Reference | Revision | Local copy | Covers |
| --- | --- | --- | --- |
| Trusted Firmware-A (TF-A), Arm's EL3 firmware | `0892ad4d` | `…/reference-backsolve-20261025/cpu/tf-a/` | PSCI `CPU_ON`/`CPU_OFF`, GICv3 and GIC-600 drivers, EL3 entry, EL2 setup for a lower-level OS, A55/A76 errata and power-down |
| Experimental A733 TF-A port | `5bc9605c` | `~/.cache/sunstoneos/sources/cpu-startup/atf-5bc9605c…/` | A733 power controller, entry address, GIC wake bit, build options |
| Vendor BL31 (`monitor.fex`) | sha256 `8af16b86…` | `~/.cache/sunstoneos/sources/cpu-startup/monitor.fex` | The EL3 firmware the vendor ships; loads at `0x48000000` |
| Vendor SCP firmware (`scp.fex`) | sha256 `07e6b976…` | `~/.cache/sunstoneos/evidence/npu-low-power-source-20261024/scp.fex` | RISC-V power-management processor firmware |
| Vendor boot0 | sha256 `829ea8e3…` (and `d2a9bccc…` for the 1800 MHz variant) | `~/.cache/sunstoneos/sources/board-boot/` | First-stage loader: DRAM, CPU clocks, PMIC rails |
| Linux | v6.17, `e5f0a698` | `…/reference-backsolve-20261025/cpu/linux/`, `…/workload-reference-20261025/linux/` | GICv3 CPU init, PSCI client, idle, timer, boot contract |
| Board kernel (Orange Pi) | `2ac08e8c` | `~/.cache/sunstoneos/sources/board-kernel/` | Device tree, CPU PLL driver |
| FreeRTOS-Kernel, `ARM_CR82` port | `97f50c0e` | `~/.cache/sunstoneos/sources/FreeRTOS-Kernel-97f50c0e…/` | IRQ entry, locks, idle |
| Arm GIC architecture, IHI 0069G | G | `~/.cache/sunstoneos/sources/cpu-audit/` | Register semantics |
| Arm CoreLink GIC-600 TRM | r1p6, 100336_0106_00 | `…/reference-backsolve-20261025/cpu/arm-gic600/` | Redistributor power, GICR_PWRR fields |
| Arm GIC-600 errata notice SDEN-892601 | v12.0 | `~/.cache/sunstoneos/evidence/native-local-interrupt-init-20261018/` | Erratum 2384374 and others |
| Cortex-A55 TRM | as cached | `~/.cache/sunstoneos/sources/cpu-continue/` | Power-down and cache behaviour |

The following are closed or unavailable:

- **Vendor EL3, SCP and boot0 source.** Allwinner publishes none of it for the
  A733. The three binaries are the only vendor reference. Their facts below
  come from disassembly and are cited by hash and offset.
- **A76 and DSU technical reference manuals.** Neither is cached. A76
  power-down and errata facts come from TF-A's `cortex_a76.S`; DSU facts come
  from TF-A's DSU macros and from the cluster registers read on the board.
- **The A733 TF-A port's power management.** It implements `CPU_ON`, `CPU_OFF`
  and system off only; its idle-state table is empty
  (`plat/allwinner/sun60i_a733/sunxi_idle_states.c:9`–`11`) and its SCP path is
  disabled (`allwinner-common-ncat2.mk:19`). It corroborates individual
  register sequences only.
- **U-Boot.** The cached Orange Pi U-Boot (`b791be84`) runs after BL31 and
  takes no part in CPU power-up; it is not used here.

Binary citations use the form `monitor.fex@0x8d74`: a file offset in the
named blob, whose SHA-256 is in the table above. The vendor BL31 is TF-A v2.5,
a debug build identified by its version strings at `monitor.fex@0xf3aa` and
`@0xf3c8`; it loads at `0x48000000`, so an offset is the address minus
`0x48000000`. The SCP firmware is RISC-V code linked at `0x40004000`. boot0 is
AArch32 Thumb-2 code that runs at `0x47000`; its offsets are the address minus
`0x47000`. The disassembly listings and scripts are outside Git, under
`~/.cache/sunstoneos/evidence/reference-backsolve-20261025/cpu/monitor/` and
`…/vendor-power/`. This document copies no blob bytes.

## The reference sequence

### Secondary power-up goes through the PSCI path and three CPU-configuration registers

Linux never powers a core on itself. It passes the target MPIDR and a physical
entry address to firmware with `PSCI_CPU_ON`
(`linux/drivers/firmware/psci/psci.c:217`–`233`), and firmware runs the
platform sequence.

1. TF-A `psci_cpu_on_start` takes the target's lock, flushes and sets its
   affinity state to ON_PENDING, and calls the platform hook
   (`tf-a/lib/psci/psci_on.c:79`–`144`).
2. The A733 port programs every core's entry address once, at setup:
   `CPUCFG + 0x1004 + n·0x1000` (low) and `+0x1008` (high), with
   `CPUCFG = 0x08000000` (`plat/allwinner/common/sunxi_pm.c:48`–`61`,
   `include/sunxi_cpucfg_ncat3.h:30`–`37`).
3. Its `pwr_domain_on` sets bit 0 of `CPUCFG + 0x1000 + n·0x1000` (AArch64)
   and waits without a bound for PPU status `0x07052008 + n·0x1000` low nibble
   0 (OFF). It then sets power-mode bit 0 at `0x07050220 + 4n` and
   hotplug-enable bit 0 at `0x07050200 + 4n`, waits without a bound for status
   8 (ON), and clears hotplug enable and power mode in that order
   (`plat/allwinner/common/sunxi_native_pm_ncat2.c:42`–`61`).
4. The target starts at the TF-A warm entry, runs the CPU reset function
   (errata) and enables its data cache early, because the port builds with
   `WARMBOOT_ENABLE_DCACHE_EARLY=1` and `HW_ASSISTED_COHERENCY=1`
   (`allwinner-common-ncat2.mk:22`–`23`).
5. `psci_cpu_on_finish` calls the platform hook, which clears bit 1 of the
   hotplug control word (`GIC_WAKEUP_DISABLE`)
   (`sunxi_native_pm_ncat2.c:68`–`73`), then initializes the redistributor and
   CPU interface (`tf-a/lib/psci/psci_on.c:171`–`187`), then programs CNTFRQ
   (`tf-a/lib/psci/psci_setup.c:319`–`323`).

The vendor BL31 performs the same register sequence, with these
differences from the port:

- It returns ALREADY_ON for a core that is on and ON_PENDING for one that is
  starting (`monitor.fex@0x8d74`, `@0x8f88`, `@0x8f90`), and rejects Aff1 above 7
  or a nonzero Aff2 (`@0x8164`).
- It writes the entry words on every `CPU_ON`, with its warm entry
  `0x48000228` (`@0xc4ac`, `@0xc4bc`), after setting the AArch64 bit (`@0xc2ac`).
- The power sequence at `@0xc314` polls for OFF, sets the power-mode bit, sets
  the hotplug-enable bit, polls for ON, then clears enable and mode. No poll
  has a timeout, and no step resets a core, changes a clock, waits a fixed time
  or messages the SCP.
- On the new core (`@0x85c8`) it clears hotplug-control bit 1 (`@0xc23c`),
  writes 1 to the DSU register CLUSTERPWRDN_EL1 (`@0x8240`), powers the
  redistributor (`@0x6a1c`) and enables the CPU interface (`@0x5f38`). It then
  writes SGI and PPI defaults (`@0x3c54`), restores one shared snapshot of SGI
  and PPI configuration that the last core to power down saved (`@0x3cb8`,
  saved at `@0x3db4`), and writes CNTFRQ = 24 MHz (`@0x8bd4`).
- At PSCI setup it clears bits 3:0 of the nine words at
  `0x07051000 + n·0x1000`, n = 0 to 8 (`@0x4e88`). The words for n = 1 to 8
  sit 8 bytes below each core's PPU status word (`0x07052008 + c·0x1000`). In
  Arm's PPU layout, offset 0 is PWPR, whose bits 3:0 are the power policy, and
  offset 8 is the status word PWSR. Under that layout the vendor sets the
  policy of eight core PPUs and one other PPU to OFF. No A733 document confirms
  the layout.

The SCP takes no part in runtime `CPU_ON` or `CPU_OFF`. Its CPU operation
handler acts only when the requested system state is 3, system suspend
(`scp.fex@0x55a0`–`0x55d6`).

### EL3 and EL2 controls prepare a lower-level OS that leaves EL2 unused

- **EL3.** TF-A initializes SCTLR_EL3 with only RES1, I, A and SA, then SCR_EL3,
  MDCR_EL3 (`MTPME` only) and CPTR_EL3 from fixed reset values
  (`tf-a/include/arch/aarch64/el3_common_macros.S:21`–`108`, `:206`–`213`). At
  run time it adds `SCR_EL3.EA`, `SCR_EL3.SIF` and `MDCR_EL3.SDD`
  (`el3_common_macros.S:549`–`572`). For the Nonsecure context it sets NS, RW
  and HCE (`tf-a/lib/el3_runtime/aarch64/context_mgmt.c:314`, `:490`, `:501`).
- **EL2 left unused.** `init_nonsecure_el2_unused` sets HCR_EL2.RW, CPTR_EL2 to
  its RES1 value, CNTHCTL_EL2 with EL1PCEN and EL1PCTEN, CNTVOFF_EL2 to zero,
  VPIDR_EL2 and VMPIDR_EL2 from MIDR and MPIDR, VTTBR_EL2 to zero, MDCR_EL2,
  HSTR_EL2 to zero and CNTHP_CTL_EL2.ENABLE to zero
  (`context_mgmt.c:1110`–`1208`). SCTLR_EL2 gets its RES1 value when HCE is set
  (`context_mgmt.c:1240`–`1253`).
- **Linux boot contract.** CNTFRQ must hold the timer frequency, CNTVOFF
  must be equal on all CPUs, and entry at EL1 requires CNTHCTL_EL2.EL1PCTEN.
  All CPUs must share one coherency domain and one value of SCR_EL3.FIQ.
  ICC_SRE_EL3 and ICC_SRE_EL2 must have Enable and SRE set
  (`Documentation/arch/arm64/booting.rst:195`–`224`, `:270`–`279`).

The vendor BL31 sets these values:

- **EL3 at entry.** SCTLR_EL3 = `0x30c50830`, then I, A and SA added
  (`monitor.fex@0x118`, `@0x138`); SCR_EL3 = `0x238` (`@0x150`); MDCR_EL3 =
  `0x818000` (`@0x158`); PMCR_EL0 = `0xe0` (`@0x164`); CPTR_EL3 = 0 (`@0x170`).
- **SCR_EL3 at Nonsecure EL1 entry.** `0x30631`: NS, the RES1 bits, SIF, RW,
  APK and API. FIQ is added only when a Secure OS has registered
  (`@0x4a5c`, `@0x7db8`), and HCE only for entry at EL2.
- **Unused EL2.** HCR_EL2 = `0x30080000000` (`@0x4854`), CPTR_EL2 = `0x33ff`
  (`@0x485c`), CNTHCTL_EL2 = 3 (`@0x4864`), CNTVOFF_EL2 = 0 (`@0x4868`),
  VPIDR_EL2 = MIDR, VMPIDR_EL2 = MPIDR, VTTBR_EL2 = 0, MDCR_EL2 =
  `0x4020000` with HPMN = PMCR.N (`@0x4890`), HSTR_EL2 = 0 and CNTHP_CTL_EL2 = 0.
- **EL1 at entry.** SCTLR_EL1 = `0x30d00800` (`@0x31b0`), CPACR_EL1 = 0, SPSR
  `0x3c5`, which is EL1h with DAIF masked.
- **Errata.** The A55 reset function reads MIDR and writes nothing
  (`@0x2bc8`). The A76 reset function applies only 1946160, for r3p0 to r4p1
  (`@0x2b8`, revision check `@0x27b4`), and requires SSBS, so CVE-2018-3639
  needs no register write (`@0x2dc8`). The A55 speculative address-translation
  workaround 1530923 is built in (`@0x388c`, `@0x31ac`). The binary never writes
  CPUECTLR, CLUSTERPWRCTLR, CLUSTERECTLR or ACTLR_EL2.

### GIC-600 initialization powers the redistributor before waking it

- **Distributor (once).** TF-A clears the three group enables and waits for
  GICD_CTLR.RWP, sets ARE_S and ARE_NS and waits again, gives every SPI a
  default group, priority and trigger, then enables the configured groups
  (`tf-a/drivers/arm/gic/v3/gicv3_main.c:184`–`217`). The wait is a poll of
  RWP alone (`gicv3_private.h:264`–`268`).
- **Redistributor power (per core).** GIC-600 redistributors are powered down
  at reset (GIC-600 TRM section 3.6.1). Each core waits until the group bits
  RDGPD and RDGPO of `GICR_PWRR` agree, writes `RDPD = 0` with `RDAG = 0`, and
  repeats until RDPD reads 0 (TRM 3.6.1; `tf-a/drivers/arm/gic/v3/gic-x00.c:61`–`86`).
  TF-A applies this only when `GICR_IIDR` names GIC-600 or GIC-700
  (`gic-x00.c:122`–`133`).
- **Redistributor wake.** `gicv3_rdistif_mark_core_awake` does nothing if
  ProcessorSleep is already clear; otherwise it waits for ChildrenAsleep,
  clears ProcessorSleep and waits for ChildrenAsleep to clear
  (`gicv3_helpers.c:36`–`61`). Linux clears ProcessorSleep and waits for
  ChildrenAsleep to clear, with a 1 s timeout (`irq-gic-v3.c:367`–`399`).
- **SGI and PPI defaults.** TF-A writes all ones to GICR_ICENABLER0, waits for
  GICR_CTLR.RWP, writes all ones to IGROUPR0 and zero to IGRPMODR0, writes the
  default priority to every IPRIORITYR word and makes every PPI level-triggered
  (`gicv3_helpers.c:286`–`336`). Linux writes all ones to IGROUPR0, clears
  active and enable state, writes one priority to every SGI and PPI, and then
  waits for RWP (`irq-gic-v3.c:1341`–`1346`; `irq-gic-common.c:121`–`142`).
  Neither reads GICR_CTLR for anything except RWP, and neither compares any
  GIC register with a saved value.
- **CPU interface.** TF-A wakes the redistributor and sets ICC_SRE_EL3 to
  SRE, DFB, DIB and Enable. It switches to the Nonsecure bank to set
  ICC_SRE_EL2 and ICC_SRE_EL1, then sets the Secure ICC_SRE_EL1. It writes PMR
  `0xff` from Secure state, enables Group 0 and Secure Group 1, restores
  SCR_EL3, and finishes with ISB and DSB (`gicv3_main.c:275`–`336`). Linux at EL1 writes PMR, BPR1 = 0
  and ICC_CTLR_EL1, zeroes every implemented active-priority register
  (ICC_AP0Rn and ICC_AP1Rn), and enables Group 1 (`irq-gic-v3.c:1202`–`1281`).
- **Errata.** TF-A applies GIC-600 erratum 2384374 when the distributor
  reports GIC-600 at or below r1p6 (`gic-x00.c:198`–`230`). The workaround
  sets and clears the three GICR_CTLR.DPG bits after the CPU interface group
  enables are cleared, before the redistributor is put to sleep
  (`gic-x00.c:183`–`196`, `gicv3_main.c:343`–`383`). The build default enables it
  with GIC-600 support (`tf-a/drivers/arm/gic/v3/gicv3.mk:15`), and the A733
  port selects GIC-600 support (`allwinner-common-ncat2.mk:16`).

The board reads `GICR_PWRR = n << 8` on frame `n`
(`docs/a733-smp-supervised.md`, "ROM reads identify all interrupt frames").
In the GIC-600 TRM's field layout, bits 23:16 are RDG, the redistributor number,
and bits 15:8 are RDGO, the core's offset within it (TRM section 4.4.5, table
4-26). All eight frames therefore report redistributor 0: one redistributor
block serves all eight cores, and the group bits RDGPD and RDGPO describe that
one shared block.

The vendor BL31 follows the TF-A sequence:

- **Distributor** (`monitor.fex@0x602c`): clear the group enables, wait for
  RWP (poll at `@0x5cb0`), set ARE_S and ARE_NS, wait again, then make every
  SPI Group 1 Nonsecure, priority `0x80` and level-triggered. It leaves
  GICD_CTLR = `0x30`, with no group enabled, and configures no Secure
  interrupt (driver data at `@0xf010`).
- **Redistributor power and wake** use the TRM loop (`@0x6a1c`, off at
  `@0x69d8`) and the TF-A wake and sleep helpers (`@0x6994`, `@0x6978`).
- **CPU0's SGI and PPI defaults** (`@0x6744`) disable all, poll GICR_CTLR.RWP
  (`@0x6764`), and write IGROUPR0 and ICFGR1. GICR_CTLR is read only in that
  poll and never compared.
- **CPU interface** (`@0x5f38`): ICC_SRE_EL3 gains `0xf`, both banks of
  ICC_SRE_EL1 get 1, PMR = `0xff` from EL3 (`@0x600c`), Group 0 is enabled and
  ICC_IGRPEN1_EL3 gains Secure Group 1 only. ICC_CTLR_EL3 is never written.

### The generic timer needs only CNTFRQ, CNTHCTL and CNTVOFF from firmware

TF-A writes CNTFRQ_EL0 on every warm entry (`psci_setup.c:319`–`323`),
CNTHCTL_EL2 with EL1PCEN and EL1PCTEN, CNTVOFF_EL2 = 0 and CNTHP_CTL_EL2.ENABLE
= 0 (`context_mgmt.c:1142`–`1148`, `:1208`). Linux programs its timer as
CVAL = counter + delta (`drivers/clocksource/arm_arch_timer.c:741`) and enables
a 10 kHz event stream in CNTKCTL so that every WFE wait ends within 100 µs
(`arm_arch_timer.c:915`–`956`; `include/clocksource/arm_arch_timer.h:68`–`70`).

### DynamIQ cores are coherent in hardware and need no SMPEN

Cortex-A55 and Cortex-A76 have no SMPEN bit. TF-A's reset functions for them
apply errata only (`tf-a/lib/cpus/aarch64/cortex_a55.S:26`–`122`,
`cortex_a76.S:495`–`537`), and the A733 port declares hardware-assisted
coherency (`allwinner-common-ncat2.mk:23`). With that option TF-A keeps data
caches on until power-down and lets hardware clean them
(`tf-a/lib/psci/psci_common.c:1313`–`1325`).

### Floating point needs every trap control clear

TF-A's CPTR_EL3 reset value leaves TFP clear, CPTR_EL2 gets its RES1 value
with TFP clear (`context_mgmt.c:1130`), and the OS sets CPACR_EL1.FPEN.

### Idle is DSB followed by WFI

Linux's default idle is `dsb(sy); wfi()` (`linux/arch/arm64/kernel/idle.c:23`–`30`).
Deeper idle states go through `PSCI_CPU_SUSPEND` (`drivers/cpuidle/cpuidle-psci.c`).
The A733 port has no idle states.

The board device tree defines two PSCI idle states, both marked
`local-timer-stop`: a core state (parameter `0x0010000`; entry 46 µs, exit
59 µs, minimum residency 3570 µs) and a cluster state (`0x1010000`; 47 µs,
74 µs, 5000 µs) (`sun60iw2p1.dtsi:301`–`321`). Its timer node lists PPIs 13,
14, 11 and 10, which are INTIDs 29, 30, 27 and 26, at 24 MHz
(`sun60iw2p1.dtsi:397`–`406`). The vendor BL31's standby sets SCR_EL3.IRQ and
FIQ, runs ISB, DSB SY and WFI, then restores SCR_EL3 (`monitor.fex@0x8284`).
Its core-level power-down suspend performs only the GIC teardown and writes no
hotplug register (`@0x864c`, `@0x81a4`). System suspend messages the SCP
(type `0x22`, `@0x1948`); the SCP then gates the CPU PLLs, moves the CPU muxes
to 24 MHz and switches rails off (`scp.fex@0x5844`–`0x5900`).

### CPU_OFF disables the CPU interface, sleeps and powers off the redistributor, then the core

TF-A `psci_do_cpu_off` (`tf-a/lib/psci/psci_off.c:47`–`188`):

1. `psci_pwrdown_cpu_start` sets CPUPWRCTLR.CORE_PWRDN_EN through the CPU
   driver (`psci_common.c:1297`–`1333`). On the A76 that is followed by DSB
   (erratum 2743102, through r4p1) and ISB (`cortex_a76.S:466`–`471`,
   `:543`–`554`); on the A55 by ISB (`cortex_a55.S:128`–`132`).
2. `gic_cpuif_disable` clears the Group 0 and Group 1 enables, runs ISB and DSB,
   applies the 2384374 DPG toggle and sets ProcessorSleep, waiting for
   ChildrenAsleep (`gicv3_main.c:343`–`383`; `gicv3_helpers.c:67`–`75`).
3. `gic_pcpu_off` writes `GICR_PWRR.RDPD = 1` after the group check
   (`gic-x00.c:88`–`107`, `:151`–`161`).
4. The A733 `pwr_domain_off` clears power-mode bit 0 and sets
   `GIC_WAKEUP_DISABLE` (`sunxi_native_pm_ncat2.c:33`–`40`, `:63`–`66`).
5. The affinity state becomes OFF, and the core runs DSB SY followed by WFI
   (`psci_off.c:155`–`186`; `psci_common.c:1345`–`1359`).

The vendor BL31's `CPU_OFF` (`monitor.fex@0x9370`) runs, in order:

1. CPUPWRCTLR_EL1 bit 0 set, then ISB (`@0x2aa8`, `@0x2be0`). There is no DSB,
   so the vendor omits the A76 2743102 workaround that upstream TF-A applies.
2. CLUSTERPWRDN_EL1 = 0 (`@0x81e0`), then a save of this core's SGI and PPI
   configuration into the shared snapshot (`@0x3db4`).
3. ICC_SRE_EL3 gains DIB and DFB; Group 0 and both Group 1 enables cleared;
   ISB.
4. ProcessorSleep set and ChildrenAsleep awaited, then `GICR_PWRR.RDPD = 1`.
5. CLUSTERPWRDN_EL1 = 0 again (`@0x8500`).
6. Power-mode bit 0 cleared (`@0xc304`) and hotplug-control bit 1 set
   (`@0xb2bc`).
7. Affinity state OFF (`@0x9414`), then DSB SY and WFI (`@0x3790`). No software
   cache maintenance runs.

Nothing in the binary toggles the GICR_CTLR.DPG bits, so the vendor BL31 does
not apply the GIC-600 erratum 2384374 workaround either. Without the DSU TRM,
the meaning of CLUSTERPWRDN_EL1 bit 0 is not established.

### DVFS raises voltage before frequency

The CPU rails and clocks:

- **Rails.** CPU-L and the DSU share AXP8191 DCDC5; CPU-B uses DCDC3
  (`sun60i-a733-orangepi-zero3w.dts:2919`–`2932`). DCDC2 to DCDC5 range from
  500 to 1540 mV and ramp at 250 µV/µs (same file, lines 2123–2157). The board reads
  DCDC3 and DCDC5 enabled at 800 mV (`docs/handoff.md`, bundle 15).
- **boot0 writes no CPU rail.** It sets DCDC2 to 800 or 840 mV
  (`boot0_sdcard_orangepizero3w.fex@0x1ff6`–`0x1ffa`) and enables DCDC4
  (`@0x16940`); its PMIC write sets the voltage code before the enable bit,
  with no delay (`@0x16760`).
- **boot0 CPU clocks** (`@0x21b8`). If bit 14 of `0x07090160` is set, boot0
  leaves the CPU PLLs alone. Otherwise, for CPU-L, CPU-B and the DSU in turn,
  it switches the mux to 24 MHz, gates the output, writes N and waits 20 µs.
  It then sets the update bit, waits for it to clear and for three lock reads
  (no timeout), waits 20 µs, ungates and waits 20 µs (`@0x2098`, `@0x2114`). It
  then selects the PLL in each mux, with 20 µs after each. The 1800 MHz variant
  has the same CPU clock and PMIC code.
- **Unresolved rate.** The board reads CPU-B N = 39 (`0xf8802700`), which
  matches one of boot0's two targets. CPU-L and the DSU read N = 38
  (`0xf8802600`), which matches neither target as the disassembly reads them.
  `docs/cpu-clock.md` decodes CPU-L as 26 MHz × 38 ÷ 2 = 494 MHz.
- **Registers.** PLL_CPU_L, PLL_CPU_B and PLL_CPU_DSU are at `0x08871000`,
  `0x08872000` and `0x08873000`; each mux is at `+0x1c`
  (`ccu-sun60iw2-cpupll.c:22`–`43`). Rate = reference × N ÷ 2^P, with P in mux
  bits 17:16 (`ccu_nkmp.c:154`–`196`).
- **Kernel order.** In the board kernel, the OPP core raises voltage before frequency and lowers it
  after (`drivers/opp/core.c:1216`–`1258`). The AXP regulator driver rounds each
  settle time up to whole milliseconds, so any change of 250 mV or less waits
  1 ms (`bsp/drivers/power/regulator/axp2101-regulator.c:383`–`409`). The PLL
  driver changes N in place with linear frequency modulation, polls the update
  bit every 100 µs for 10 ms and lock for 70 ms
  (`ccu_nkmp.c:234`–`317`; `ccu-sun60iw2-cpupll.c:270`–`308`). The DSU clock is
  lowered before a cluster change and raised after it
  (`bsp/drivers/cpufreq/cpufreq-linux-6.6/cpufreq-dt.c:84`–`109`;
  `bsp/drivers/dsufreq/sunxi-dsufreq.c:298`–`368`).
- **Operating points.** CPU-L runs at up to 1008 MHz at 800 mV in every speed
  bin, and CPU-B up to 1392 MHz at 800 to 850 mV (`sun60iw2p1-cpu-vf.dtsi:20`–`322`,
  `:623`–`971`).
- **Firmware.** The vendor BL31 has no CPU clock or DVFS handler, and the SCP
  changes CPU clocks only around system suspend.

## SunstoneOS compared step by step

"Ours" means the cached eight-core image (`platform/a733-smp-cached/`, the
portable controllers in `platform/a733/`) and the returning workload overlay
(`boot/a733/`). Line numbers are on `bringup` at `95620a1b`. Each divergence
has one class: missing step, different order, different value, missing wait or
barrier, or extra check (a check or step the reference does not perform).

| Reference step | Citation | Our step | Citation | Divergence class | Risk |
| --- | --- | --- | --- | --- | --- |
| Program every core's entry address once | `sunxi_pm.c:48`–`61` | Program the target's entry words at each power-on | `platform/a733/cpu_start.c:68`–`75` | Different order | None |
| Set AArch64, wait for OFF, set power mode then hotplug enable, wait for ON, clear enable then mode | `sunxi_native_pm_ncat2.c:42`–`61` | Same writes in the same order; the OFF and clear-request states are required at entry instead of awaited; polls have a deadline | `cpu_start.c:57`–`95` | Extra check | Low: a refusal is reported, and the target is not started |
| Target clears `GIC_WAKEUP_DISABLE` before GIC setup | `sunxi_native_pm_ncat2.c:68`–`73`; `psci_on.c:171` | Target clears hotplug-control bit 1 after the frame scan, before redistributor power | `platform/a733/cpu_continue.c:84`–`85` | None | None |
| SCR_EL3 for Nonsecure: NS, RW, HCE, FIQ by routing; EA and SIF set | `context_mgmt.c:314`, `:490`, `:501`; `el3_common_macros.S:556`–`559` | `0x535`: NS, FIQ, HCE, RW; EA clear; SIF cleared later so that EL3 can fetch Nonsecure-mapped code | `platform/a733-smp-cached/continue.S:88`–`89`, `:213`–`229` | Different value | Low |
| VPIDR_EL2 and VMPIDR_EL2 from MIDR and MPIDR | `context_mgmt.c:1154`–`1155` | Not written; EL1 reads of MIDR and MPIDR return their reset values | `continue.S:101`–`117` | Missing step | Low: recorded EL1 MPIDR values match the hardware |
| CNTHP_CTL_EL2.ENABLE = 0 | `context_mgmt.c:1208` | Not written; the field is UNKNOWN after a warm reset | `continue.S:112`–`114` | Missing step | Medium-low: an enabled EL2 timer asserts PPI 26, and the cleanup and drain checks then park the core |
| HCR_EL2.RW, CPTR_EL2 RES1, CNTHCTL_EL2 EL1PCEN and EL1PCTEN, CNTVOFF 0, VTTBR 0, MDCR_EL2.HPMN, HSTR 0, SCTLR_EL2 RES1 | `context_mgmt.c:1120`–`1199`, `:1253` | Same values | `continue.S:101`–`117` | None | None |
| CNTFRQ_EL0 on every warm entry | `psci_setup.c:319`–`323` | 24 MHz on every entry, with CNTPS, CNTP and CNTV disabled | `platform/a733-smp-cached/start.S:24`–`30`; `continue.S:21`–`27` | None | None |
| Floating point: CPTR_EL3 and CPTR_EL2 TFP clear, CPACR_EL1.FPEN = 3 | `context_mgmt.c:1130` | CPTR_EL3 = 0, CPTR_EL2 = `0x33ff`, CPACR_EL1 = `3 << 20` | `continue.S:92`, `:110`–`111`, `:137`–`138` | None | None |
| Distributor: groups off and RWP wait, ARE and RWP wait, SPI defaults, groups on | `gicv3_main.c:184`–`217` | The production image requires the boot state (ARE_S, ARE_NS, Group 0 and Nonsecure Group 1 enabled, DS clear); the returning overlay disables all SPIs; no SPI group, priority or trigger defaults | `cpu_continue.c:66`–`68`; `boot/a733/secondary-distributor-probe.c:21`–`37` | Missing step | Low while no SPI is enabled |
| Distributor write pending: poll GICD_CTLR.RWP until clear | `gicv3_private.h:264`–`268`; `irq-gic-v3.c:344`–`359` | A set RWP bit is a failure: at every core's first controller phase, at the overlay's distributor preparation, and before and after SPI routing; the overlay's distributor restoration compares the whole GICD_CTLR word | `cpu_continue.c:67`; `secondary-distributor-probe.c:26`, `:51`; `platform/a733-smp-cached/irq_table.c:44`, `:77` | Extra check | Medium: a concurrent distributor write retains the starting core, and the image stops silently |
| Find the own frame by walking frames to the Last bit | `gicv3_main.c:1357`–`1414` | Read all eight frames; require unique affinities and the Last bit on frame 7 only | `cpu_continue.c:69`–`81` | Extra check | Low: fixed hardware facts, observed on the board |
| Redistributor power: wait for equal group bits, write RDPD = 0, repeat until RDPD reads 0 | `gic-x00.c:73`–`86`; GIC-600 TRM 3.6.1 | Same loop, with a deadline | `cpu_continue.c:89`–`95` | None | None |
| Wake: return if ProcessorSleep is clear; wait for ChildrenAsleep, clear ProcessorSleep, wait for ChildrenAsleep clear | `gicv3_helpers.c:36`–`61` | Same, with a deadline; a ProcessorSleep that reads set afterwards is a failure | `cpu_continue.c:97`–`99` | None | None |
| SGI and PPI defaults: disable all, RWP wait, all Group 1 Nonsecure, all priorities, PPIs level | `gicv3_helpers.c:306`–`336`; `irq-gic-v3.c:1341`–`1346` | Disable all and wait for RWP, clear pending and active, require no active interrupt, all Group 1 Nonsecure, PPIs 29 and 30 level, SGI 0 priority `0xf0` only, enable SGI 0 and wait, then read back group, modifier, enable and priority | `cpu_continue.c:100`–`121` | Extra check; different value (one priority byte) | Low |
| Never compare a GIC register with a saved value; poll RWP only | `irq-gic-v3.c:344`–`365`; `gicv3_helpers.c:310`–`311` | Baseline and restoration compare GICR_CTLR, including RWP (bit 3) and UWP (bit 31), with the saved word | `boot/a733/primary-local-cache-probe.c:83`–`87`, `:106`–`107`; `boot/a733/secondary-local-probe.c:92`–`93`, `:115`–`116` | Extra check | Observed: the cause of the port 3-3 workload failures |
| (as above) | (as above) | The same comparisons include GICR_PWRR's group status bits, GICR_WAKER.ChildrenAsleep and GICR_ISPENDR0; the workload drain refuses any pending PPI other than SGI 0 and PPI 30 | same lines; `boot/a733/all-core-first-task-probe.S:205`–`224` | Extra check | Medium-low |
| EL3 CPU interface: SRE, DFB, DIB, Enable; Secure PMR `0xff`; Group 0 and Secure Group 1 enabled | `gicv3_main.c:275`–`336` | ICC_SRE_EL3 = `0xf`; ICC_CTLR_EL3 = 0; Group 0 and both Group 1 enables cleared; Secure PMR `0xff` with readback | `continue.S:70`–`86` | Different value | Low: SunstoneOS uses no Secure interrupt |
| EL1 CPU interface: PMR, BPR1 = 0, ICC_CTLR_EL1, all active-priority registers zeroed, Group 1 enabled | `irq-gic-v3.c:1202`–`1281` | ICC_CTLR_EL1 = 0, BPR1 = 0, PMR = 255, Group 1 enabled; active priorities not written | `continue.S:172`–`178` | Missing step | Low: each launch enters through a warm reset, which zeroes them (IHI 0069G 12.2.2) |
| EL3 PMR before Nonsecure entry | `gicv3_main.c:275`–`336` | Present in the cached image; absent in the uncached image's continuation | `platform/a733/cpu_continue.S:60`–`128` | Missing step | Low unless the uncached image delivers interrupts |
| Tick: CVAL = counter + delta | `arm_arch_timer.c:741` | CVAL = previous CVAL + interval | `platform/a733-smp-cached/platform.c:124`–`128` | Different value | Low: a late tick fires again at once; bounded |
| 10 kHz event stream for WFE waits | `arm_arch_timer.c:915`–`956` | CNTKCTL_EL1 = 3, no event stream | `continue.S:139`–`140` | Missing step | Low-medium: a lost event leaves a core in WFE with interrupts masked |
| Lock release: store, DSB, SEV | Arm's recommended sequence; upstream FreeRTOS has SEV before DSB (`portable/GCC/ARM_CR82/port.c:1566`–`1580`) | Store, DSB, SEV | `patches/freertos-smp-c.patch:248`–`256` | None | None |
| Caches: hardware coherency, data cache enabled early on warm entry, no SMPEN | `allwinner-common-ncat2.mk:22`–`23` | EL3 MMU and caches enabled after GIC setup and an all-core barrier | `platform/a733-smp-cached/continue.c:115`–`125`; `platform/a733/cpu_coherence.S:303`–`363` | Different order | None |
| A55 r2p0: no reset errata; the port selects 1530923 | `cortex_a55.S:26`–`122`; `allwinner-common-a55.mk:10` | No A55 control change; speculative-AT sequence required | `platform/a733/cpu_coherence.c:23`–`29` | None | None |
| A76 r4p1: 1946160, 2356586, 3888013 when selected; no static CVE-2018-3639 mitigation on cores with SSBS; the A733 port selects none | `cortex_a76.S:428`–`480`, `:495`–`537`; `allwinner-common.mk:85`–`89` | 1946160, 2356586 and 3888013, plus CPUACTLR2 bit 16 (load-pass-store disabled) | `cpu_coherence.c:34`–`42` | Extra step | None for stalls; it costs A76 store-to-load forwarding |
| Power-down: CORE_PWRDN_EN, DSB on the A76 (2743102), ISB | `cortex_a55.S:128`–`132`; `cortex_a76.S:543`–`554` | Same, with DSB on every core | `boot/a733/secondary-identity-probe.S:138`–`143` | None | None |
| Power-down GIC: clear group enables, ISB and DSB, 2384374 DPG toggle, ProcessorSleep and wait | `gicv3_main.c:343`–`383`; `gic-x00.c:183`–`196` | Group enables required clear, local state restored, ProcessorSleep restored and ChildrenAsleep awaited; no DPG toggle | `secondary-local-probe.c:79`–`90` | Missing step | Low with SPIs and LPIs disabled; medium once SPIs are routed |
| Power-down: redistributor RDPD = 1 | `gic-x00.c:88`–`107` | Not written | — | Missing step | Low |
| Platform off: clear power mode, set `GIC_WAKEUP_DISABLE` | `sunxi_native_pm_ncat2.c:33`–`40` | Same, by the dying core | `secondary-identity-probe.S:144`–`148` | None | None |
| Terminal DSB and WFI loop | `psci_common.c:1345`–`1359` | DSB and WFI loop | `secondary-identity-probe.S:152`–`155` | None | None |
| Idle: DSB SY, WFI | `linux/arch/arm64/kernel/idle.c:23`–`30` | DSB SY, WFI at EL0 with SCTLR_EL1.nTWI set | `include/a733_cpu_power.h:23`–`25`; `continue.S:118`–`124` | None | None |

The vendor binaries add the following rows.

| Reference step | Citation | Our step | Citation | Divergence class | Risk |
| --- | --- | --- | --- | --- | --- |
| Clear bits 3:0 (PPU power policy, under Arm's layout) of nine words at `0x07051000 + n·0x1000` at setup | `monitor.fex@0x4e88` | Not written; the inherited policy is used | — | Missing step | Low: power-on and power-off work on both boards with the inherited value, which no record captures |
| CLUSTERPWRDN_EL1 = 1 on the new core; 0 twice at power-down | `monitor.fex@0x8240`, `@0x81e0`, `@0x8500` | Not written | — | Missing step | Unknown meaning; low while CPU0 stays powered |
| A76 reset errata: 1946160 only | `monitor.fex@0x2b8`, `@0x2db4` | 1946160, 2356586, 3888013 and CPUACTLR2 bit 16 | `cpu_coherence.c:34`–`42` | Extra step | None for stalls |
| Power-down with no DSB before ISB on the A76 | `monitor.fex@0x2aa8` | DSB present on every core | `secondary-identity-probe.S:141`–`143` | None: SunstoneOS follows upstream TF-A | None |
| SGI and PPI configuration restored from a snapshot that another core saved | `monitor.fex@0x3cb8`, `@0x3db4` | Each core writes its own configuration | `cpu_continue.c:105`–`112` | None: the vendor quirk has no reason to copy | None |
| Voltage settle: at least 1 ms per change | `axp2101-regulator.c:383`–`409` | Wait \|ΔV\| ÷ 250 µV/µs | `drivers/clock/a733_cpu_clock.c:26`; `docs/cpu-clock.md` step 6 | Different value | Medium for DVFS runs: a 100 mV raise waits 400 µs instead of 1 ms |
| Scale the DSU clock with the clusters | `sunxi-dsufreq.c:298`–`368` | The DSU clock is never written; the CPU-L voltage covers the DSU point | `drivers/clock/a733_cpu_clock.c:145`, `:234`–`241` | Missing step | None for stalls |
| PLL change: boot0 parks the mux on 24 MHz; the kernel changes N in place | `boot0_sdcard_orangepizero3w.fex@0x2114`; `ccu_nkmp.c:234`–`317` | Park on the DCXO, bounded update and lock waits | `docs/cpu-clock.md` step 7 | None: SunstoneOS follows boot0 | None |

## Divergences ranked by their chance of stopping a core

The ranking orders divergences by how likely each is to stop a core in a
park loop or a silent hang on the current images. Every failed check in the
returning overlay and in the cached image ends in a park loop. CPU0 parks as
well, so a refusal and a hardware hang look the same from the host unless the
CPU0 return deadline is built in (`docs/board-runbooks/rank-7-diagnosis.md`,
"Every refusal in the diagnostic looks like a hang").

1. **GICR_CTLR is compared with a saved value, including its status bits**
   (extra check). CPU0's restoration rewrites its SGI and PPI words and then
   requires every snapshot word, GICR_CTLR included, to equal the saved one
   (`boot/a733/primary-local-cache-probe.c:80`–`87`); the secondaries'
   cleanup does the same after the sleep handshake
   (`boot/a733/secondary-local-probe.c:87`–`93`). Bit 3 of GICR_CTLR is RWP and
   bit 31 is UWP, both read-only write-pending status (IHI 0069G 12.11.2).
   Linux and TF-A poll RWP and never compare GICR_CTLR
   (`irq-gic-v3.c:362`–`365`; `gicv3_helpers.c:310`–`311`). This divergence
   explains three port 3-3 records in `docs/handoff.md`: the workload that did
   not return for 6.7 hours (`u2-tw-prepare`), the deadline return of run 5
   (`tw5-prepare`) and the deadline return after the first RWP wait
   (`tw1-prepare`, second lifetime). With bits 3 and 31 masked, the workload
   passed ten times in one lifetime. It is also consistent with the port 3-1
   rank 7 non-return of 2026-10-24 in a clean lifetime. That diagnostic runs
   the same CPU0 restoration after the secondaries have started, where a
   refusal parks CPU0 (`docs/board-runbooks/rank-7-diagnosis.md`, check 4). No
   record of that run identifies the failing check. The masking is on `work/workload-bisect-1025`
   (`456c47ab`); `bringup` and the frozen rank 7 and rank 8 bundles still carry
   the full comparison.

   The board record shows RWP set again after a completed wait, with no CPU0
   write in between. The GIC-600 TRM suggests a mechanism that the architecture
   text does not. Every frame reads `GICR_PWRR = n << 8`, so RDG (bits 23:16)
   is 0 on all eight frames: one redistributor block serves all eight cores
   (TRM 4.4.5, table 4-26). The secondaries write GICR_ICENABLER0 in their own
   frames during their cleanup, which can overlap CPU0's restoration. If the
   shared block reports those writes in every frame's RWP, CPU0 observes RWP
   without writing. This is a hypothesis; the TRM does not state how RWP is
   scoped. Run 1 below tests it.

2. **GICD_CTLR.RWP is treated as a failure** (extra check). Every core's
   first controller phase refuses a set RWP (`platform/a733/cpu_continue.c:67`),
   and so do the overlay's distributor preparation
   (`boot/a733/secondary-distributor-probe.c:26`) and the SPI routing in the
   devices image (`platform/a733-smp-cached/irq_table.c:44`, `:77`). The
   overlay's distributor restoration compares the whole GICD_CTLR word, RWP
   included (`secondary-distributor-probe.c:51`). TF-A and Linux wait for RWP
   to clear (`gicv3_private.h:264`–`268`; `irq-gic-v3.c:344`–`359`). The
   mechanism is the one in item 1. In the cached image a refusal marks the
   core failed and parks it (`platform/a733-smp-cached/continue.c:32`–`36`,
   `:109`–`111`); the other cores then fail their arrival barrier, and the
   image stops with no return. No board record shows this failure. The current
   overlay finishes its distributor writes before it starts any secondary
   (`boot/a733/secondary-identity-probe.c:49`–`51`), so the race needs a
   concurrent distributor writer: a devices image that routes SPIs while a
   secondary starts, or any later change in that order.

3. **Other volatile words are compared exactly** (extra check). The same
   baseline and restoration comparisons include GICR_PWRR, whose bits 2 and 3
   report the shared redistributor's group power state; GICR_WAKER, whose
   bit 2 is ChildrenAsleep; and GICR_ISPENDR0, which shows any asserted
   level-sensitive PPI (`primary-local-cache-probe.c:63`–`70`, `:106`–`107`;
   `secondary-local-probe.c:59`–`66`, `:112`–`116`). The secondaries' cleanup
   also requires no pending SGI or PPI after disabling them
   (`secondary-local-probe.c:83`), and the workload drain refuses any pending
   PPI other than SGI 0 and PPI 30 (`all-core-first-task-probe.S:216`–`224`).
   The references perform none of these checks. No board record has shown a
   difference in these words; a timer left enabled by reset state would change
   ISPENDR0 (item 4).

4. **CNTHP_CTL_EL2 is never cleared** (missing step). CNTHP_CTL_EL2.ENABLE is
   UNKNOWN after a warm reset. TF-A clears it when EL2 is unused
   (`context_mgmt.c:1208`); SunstoneOS writes CNTHCTL_EL2 and CNTVOFF_EL2 only
   (`platform/a733-smp-cached/continue.S:112`–`114`). An enabled EL2 physical
   timer asserts PPI 26, a level-sensitive interrupt. It is never delivered,
   because all PPIs except 30 stay disabled, but it is pending, and the checks
   in item 3 then park the core. The vendor BL31 clears it as TF-A does
   (`monitor.fex@0x4854`–`0x4890`). Every board record so far shows pending 0,
   so the reset value has been benign on these boards.

5. **No event stream bounds WFE waits** (missing step). FreeRTOS kernel locks
   wait in `SEVL; WFE` (`portable/GCC/ARM_CR82/port.c:1676`–`1681`) with
   interrupts masked. SunstoneOS's release is store, DSB, SEV
   (`patches/freertos-smp-c.patch:248`–`256`), which follows Arm's sequence, so
   a waiter is woken without an event stream. Linux also enables a 10 kHz
   event stream (`arm_arch_timer.c:915`–`956`), which ends every WFE within
   100 µs even if an event is lost. Without it a lost event leaves a core in
   WFE with interrupts masked, permanently. No board record points at this.

6. **GIC-600 erratum 2384374 is not worked around** (missing step). The GIC
   is r1p6 (`GICD_IIDR = 0x0201643b`), inside the range for which TF-A applies
   the workaround (`gic-x00.c:198`–`230`), and the A733 port's options enable it
   (`gicv3.mk:15`; `allwinner-common-ncat2.mk:16`). The vendor BL31 does not
   apply it. SunstoneOS's power-down sets ProcessorSleep without toggling the
   DPG bits (`secondary-local-probe.c:89`–`90`). The erratum (SDEN-892601 v12.0,
   "Failure to forward highest priority interrupt") concerns SPIs and LPIs,
   which the workload leaves disabled. It has two parts. In the first, a
   higher-priority SPI waits behind an unsent lower one until an unblocking
   event; when software masks with the PMR, as every FreeRTOS critical section
   does, the notice states that the system may hang. In the second, an SPI
   recalled while a CPU's group enable is cleared delays later SPIs to that
   CPU. TF-A's toggle at power-down addresses the second part only; the notice
   gives a periodic DPG toggle, or masking with GICD_ICENABLER instead of the
   PMR, for the first. The risk applies to every image that routes SPIs.

7. **VPIDR_EL2 and VMPIDR_EL2 are never written** (missing step). With EL2
   implemented, EL1 reads of MIDR_EL1 and MPIDR_EL1 return these registers.
   TF-A and the vendor BL31 copy the hardware values
   (`context_mgmt.c:1154`–`1155`; `monitor.fex@0x4854`–`0x4890`). SunstoneOS
   derives its core index at EL1 from MPIDR (`platform/a733-smp-cached/continue.c:25`–`30`;
   `platform/a733-smp-cached/continue.S:419`–`421`). The board records show EL1 MPIDR equal to the
   hardware value, so the reset values are correct on these cores today.

8. **Active-priority registers are not zeroed** (missing step). Linux zeroes
   ICC_AP0Rn and ICC_AP1Rn before enabling Group 1 (`irq-gic-v3.c:1245`–`1281`).
   A nonzero active priority would mask every interrupt at that priority and
   below. Each SunstoneOS launch enters through a warm reset, which resets
   them to zero (IHI 0069G 12.2.2), so this matters only for an entry without
   a reset.

9. **The redistributor is never allowed to power down** (missing step).
   TF-A writes `GICR_PWRR.RDPD = 1` after the sleep handshake
   (`gic-x00.c:88`–`107`). SunstoneOS leaves RDPD clear. Because one block
   serves all eight cores, the block stays powered while any core leaves RDPD
   clear; this costs power and does not stop a core.

10. **The uncached image lacks the EL3 PMR write** (missing step). The cached
    continuation sets the Secure PMR before Nonsecure entry
    (`platform/a733-smp-cached/continue.S:82`–`86`); the uncached image's
    continuation does not (`platform/a733/cpu_continue.S:60`–`128`). With
    `SCR_EL3.FIQ = 1` and a Secure PMR below `0x80`, Nonsecure PMR writes are
    ignored (IHI 0069G 4.8.1), and no interrupt is delivered. This matters only
    if that image is used for interrupt delivery.

11. **The PPU power policy is inherited** (missing step, from the vendor
    binary). The vendor BL31 sets the policy field of nine PPUs to OFF at
    setup (`monitor.fex@0x4e88`). SunstoneOS never writes it. A policy left
    at ON would keep a core powered after its power-down WFI, and the
    primary's wait for OFF would then expire. Both boards power cores off and
    on today, so the inherited value works; no record contains it.

12. **The voltage settle wait is shorter than the vendor kernel's**
    (different value). The SunstoneOS CPU clock driver waits
    |ΔV| ÷ 250 µV/µs after a voltage write
    (`drivers/clock/a733_cpu_clock.c:26`); the vendor kernel waits at least
    1 ms (`axp2101-regulator.c:383`–`409`). A frequency raise that starts
    before the rail settles can fault or hang a core at the new rate. This
    affects only the pending CPU clock and voltage runs.

## Fixes grounded in the reference

Each fix replaces a SunstoneOS step with the reference step it diverges from.
The numbers refer to the ranked list above.

1. **Poll write-pending bits; never compare them** (items 1 and 2). Merge the
   GICR_CTLR masking of `work/workload-bisect-1025` and keep its RWP waits.
   Apply the same rule to GICD_CTLR: replace the refusals at
   `cpu_continue.c:67`, `secondary-distributor-probe.c:26` and
   `irq_table.c:44` with a bounded wait for RWP to clear, as
   `gicv3_private.h:264`–`268` does, and drop bit 31 from the restoration
   comparison at `secondary-distributor-probe.c:51` and from the final check at
   `irq_table.c:77`. Refreeze the rank 7 and rank 8 bundles from the result.
2. **Compare configuration fields only** (item 3). In the baseline and
   restoration snapshots, compare GICR_PWRR bits 1:0 rather than the whole
   word, compare GICR_WAKER bit 1 (ProcessorSleep) and wait for bit 2, and
   drop GICR_ISPENDR0 from the equality. Keep the raw words in the records.
   The references write configuration and poll status; they do not refuse a
   pending level interrupt that is disabled.
3. **Clear CNTHP_CTL_EL2 and set VPIDR_EL2 and VMPIDR_EL2 at EL3** (items 4
   and 7), as `init_nonsecure_el2_unused` does (`context_mgmt.c:1154`–`1155`,
   `:1208`). Add the three writes beside CNTHCTL_EL2 in
   `platform/a733-smp-cached/continue.S:112`–`114`.
4. **Enable the event stream** (item 5). Set CNTKCTL_EL1.EVNTEN with a
   divider near 10 kHz at `continue.S:139`–`140`, as
   `arm_arch_timer.c:915`–`956` does. At 24 MHz, EVNTI = 11 triggers on bit 11
   of the counter, every 2^12 ticks (171 µs, about 5.9 kHz); EVNTI = 10 gives
   85 µs (11.7 kHz).
5. **Work around 2384374 before any image routes SPIs** (item 6). Toggle
   the DPG bits after clearing the group enables and before ProcessorSleep,
   as `gic-x00.c:183`–`196` does, and add one of the notice's two remedies for
   PMR masking: a periodic DPG toggle or GICD_ICENABLER masking.
6. **Zero the active-priority registers at EL3 entry** (item 8): ICC_AP0R0_EL1,
   then the Secure and Nonsecure ICC_AP1R0_EL1, in the order IHI 0069G 12.2.2
   requires. The warm reset already zeroes them, so the fix matters only for an
   entry without a reset.
7. **Write RDPD = 1 after the sleep handshake** (item 9), as `gic-x00.c:88`–`107`
   does. This is a power fix and can wait.
8. **Record the PPU policy words before changing them** (item 11). Read the
   nine words at `0x07051000`–`0x07059000` in run 2; write a policy only if
   one reads other than the value the vendor writes.
9. **Wait at least 1 ms after each CPU rail write** (item 12), the vendor
   kernel's settle time, before the dependent frequency raise.

## Board runs, each testing one prediction

Each run tests one prediction from the comparison. The order follows the
board rule in `AGENTS.md`: within a connection window, the run most likely to
lose the board runs last. Every run uses the CPU0 return deadline, so a failed
check returns a record instead of a silent park.

1. **Locate the source of the stray RWP** (port 3-3, read-only addition to the
   qualified workload). Prediction: CPU0's GICR_CTLR.RWP is set only while
   at least one secondary has a GICR_ICENABLER0 write in flight in its own
   frame, because one redistributor block serves all eight cores. Sample
   CPU0's raw GICR_CTLR with a counter timestamp through the window in which
   the secondaries run their cleanup, and record each secondary's
   ICENABLER0 write time. If RWP on frame 0 never coincides with a secondary
   write, the hypothesis is false and the stray RWP stays unexplained; the
   masking fix does not depend on the answer.
2. **Record the inherited EL2 and CPU-interface state** (both ports, read-only
   addition at EL3 entry on every core). Prediction: CNTHP_CTL_EL2,
   CNTHV_CTL_EL2, VPIDR_EL2, VMPIDR_EL2, ICC_AP0R0_EL1, ICC_AP1R0_EL1 and
   ICC_RPR_EL1 hold benign values on every core (timers disabled or masked,
   virtual identity equal to the hardware identity, no active priority). Read
   CLUSTERPWRDN_EL1 and the nine PPU policy words at `0x07051000`–`0x07059000`
   in the same pass. A timer with ENABLE set and IMASK clear on any core shows
   that fix 3 corrects an active fault on that board.
3. **Rerun port 3-1 rank 7 with the status-masked comparison** (after fix 1).
   Prediction: the diagnostic that did not return on 2026-10-24 now returns,
   because the GICR_CTLR comparison was its unrecorded refusal. A deadline
   return names another check; a pass ties the port 3-1 record to item 1.
4. **Race the distributor on purpose** (port 3-3, after run 3). Prediction:
   an image in which CPU0 writes GICD_ICENABLER for the already-disabled SPI
   banks while the secondaries run their first controller phase retains at
   least one secondary with phase 0 and error 3 on the unfixed controller, and
   passes with fix 1's RWP wait. Build both variants from one source; run the
   fixed one first.
5. **Ten workload runs per board with fixes 1 to 4** (both ports). Prediction:
   ten of ten pass on each board, including port 3-1, with every 91-word
   capture equal to the clean capture.
6. **SPI latency under PMR masking** (last in its window, highest recovery
   risk; after fix 5). The image routes two SPIs of different priority to one
   core while FreeRTOS critical sections mask with the PMR. Prediction: without
   an unblocking event, the higher-priority SPI's delivery latency has no
   bound; with a periodic DPG toggle, the toggle period bounds it. Record the latency of every delivery and stop
   at the image's return deadline.
7. **CPU clock and voltage steps** (after run 5, the board that passed it
   first). Prediction: with the 1 ms settle of fix 9, each raise within the
   800 mV operating points (CPU-L to 1008 MHz) completes and the workload
   passes at the new rate. Run raises that need a higher voltage only after
   that, one cluster at a time, voltage first.
