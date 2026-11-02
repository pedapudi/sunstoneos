# A733 hardware facts

This page gives the values that code and host models need. Every line cites its source: a facts entry in
[a733-facts.json](a733-facts.json), a source line, or a specification section. Facts marked "from source" are set
by our own monitor and installers; "measured" values come from the probe run on queue entry
`1793590887901610846-fc9d445f` (port 3-1). [The probe](a733-facts-probe.md) explains how the table is read and decoded.

## Security state

- The OS runs at EL1 in the Non-secure state. The monitor writes `SCR_EL3` = 0x535: NS, FIQ, HCE and RW set
  (`platform/a733-smp-cached/continue.S:98-99`; facts `monitor.scr_el3`, `.ns`, `.fiq`; from source). EL2 is unused.
- The GIC keeps two security states: the boot stops if `GICD_CTLR.DS` is set (`platform/a733-smp-cached/boot.c:95`;
  `monitor.gicd_ctlr.ds`), and it enables affinity routing for both states and Group 0 and Non-secure Group 1
  (`boot.c:103-104`).
- `SCR_EL3.FIQ` is set, so Group 0 belongs to EL3 and a Non-secure EL1 access to a Group 0 CPU interface register
  traps to the monitor (IHI0069, `ICC_IGRPEN0_EL1`; Linux `irq-gic-v3.c` `gic_has_group0`).

## Priority views

EL3 programs priorities in the stored (Secure) view. Non-secure EL1 sees `(stored << 1) & 0xff`; a Non-secure write of
`v` stores `((v >> 1) | 0x80) & 0xf8` (five priority bits, `gic.cpuif.icc_ctlr_el1.pribits` = 4; IHI0069 4.8.1; Linux
`irq-gic-v3.c:95-115`). `ICC_PMR_EL1` and the redistributor and distributor priority bytes follow the same rule.
Use `include/a733_gic_priority.h` or `tools/hw_facts.py` (`ns_view`, `stored_from_ns`); `checks/hw-facts.py` refuses
other comparisons of a priority with a literal.

| Case | Stored | Non-secure view | Source |
| --- | --- | --- | --- |
| Route priority of INTID 97, 71, 143 and 95 | 0xd8 | 0xb0 | measured `gic.dist.spi97.priority`; from source `config.spi97.stored_priority` |
| Unmasked write of `ICC_PMR_EL1` (0xff) | 0xf8 | 0xf0 | measured `gic.cpuif.icc_pmr_el1.readback_after_write_ff` |
| FreeRTOS API mask, written as 0xb0 | 0xd8 | 0xb0 | measured `...readback_after_write_api_mask` |

An interrupt may call the FromISR API when its stored priority is at least 0xd8, the stored value of the mask.

## Interrupts the OS uses

| INTID | Use | Group | Stored priority | Non-secure view | Route | Trigger |
| --- | --- | --- | --- | --- | --- | --- |
| 27 | Shared timer (CNTV) | Non-secure 1 (`continue.c:120-121`) | 0xe8 when the OS writes 0xd8 (`platform.c:121`); 0xd8 where EL3 sets it (NPU finite image, `apps/npu-finite/deadline.c:15-20`) | 0xd0 (`config.intid27.stored_priority_written_ns`, measured `priority_after_write`); 0xb0 in the NPU finite image (entry `1793587317962103239-5bbfc48d`) | Per core; core 0 handles it | Level (`continue.c:122-124`) |
| 30 | Scheduler tick (CNTP) | Non-secure 1 (`cpu_continue.c:115`) | 0xf0 for the OS write of 0xe0 (`platform.c:116`) | 0xe0 (measured `priority_after_write`) | Per core; core 0 handles it | Level (`cpu_continue.c:118`) |
| 97 | NPU | Non-secure 1 (`irq_table.c:70-71`) | 0xd8 (`sunstone-manifest.py:347`) | 0xb0 (measured) | Core 1, `OS_NPU_CORE` (`apps/os/npu.h:6`, `irq_table.c:76`) | Level (`irq_table.c:72`) |
| 187 | USB2 host | Non-secure 1 | 0xd8 (`sunstone-manifest.py:346`) | 0xb0 (derived, `config.spi187.ns_view_priority`) | Core 0 | Level |
| 71, 143, 95 | Thermal, DMA0, GPU | Non-secure 1 | 0xd8 (`sunstone-manifest.py:345-350`) | 0xb0 | Core 0 (GPU: application core) | Level |

Every route stores at least 0xd8, the stored value of the API mask; the route installer refuses a lower priority
(`A733_IRQ_TABLE_MIN_PRIORITY`, `platform/a733-smp-cached/irq_table.h`).

## CPU identity (every core read at EL1)

| Cores | `MIDR_EL1` | `CTR_EL0` | L1 I and D, L2, L3 | `ID_AA64ISAR0` atomics |
| --- | --- | --- | --- | --- |
| 0 to 5, Cortex-A55 | 0x412fd050 | 0x84448004 | 32 KiB each, 128 KiB, 1 MiB shared | 2 (LSE) |
| 6 and 7, Cortex-A76 | 0x414fd0b1 | 0x9444c004 | 64 KiB each, 256 KiB, 1 MiB shared | 2 (LSE) |

`MPIDR_EL1` = 0x81000000 \| core << 8, `REVIDR_EL1` = 0, `CNTFRQ_EL0` = 24000000 on every core, `CLIDR_EL1` =
0xc3000123 (facts `cpu.midr_el1`, `cpu.mpidr_el1`, `cpu.revidr_el1`, `cpu.cntfrq_el0`, `cpu.ctr_el0`, `cache.ccsidr_el1.selN`).
EL1 reads `MIDR_EL1` and `MPIDR_EL1` through `VPIDR_EL2` and `VMPIDR_EL2`, which the monitor copies from the hardware
registers (`continue.S:129-131`).

## What traps or reads as zero at Non-secure EL1

- Trap to EL3: `ICC_IGRPEN0_EL1` (measured, entry `1793590176547518274-e819ce89`, fact
  `gic.cpuif.icc_igrpen0_el1.read_traps_to_el3`), and by the same rule `ICC_BPR0_EL1`, `ICC_AP0Rn_EL1`, `ICC_IAR0_EL1`,
  `ICC_EOIR0_EL1` and `ICC_HPPIR0_EL1`. The probe never reads them (`checks/hw-facts.py` disassembles it).
- `GICD_CTLR` reads in the Non-secure layout: 0x12 is EnableGrp1A and ARE_NS; `DS`, `ARE_S`, EnableGrp0 and
  EnableGrp1S are not visible (IHI0069, `GICD_CTLR`, "Non-secure access when GICD_CTLR.DS == 0").
- RAZ/WI: `GICD_IGROUPR`, `GICD_IGRPMODR`, `GICR_IGROUPR0`, `GICR_IGRPMODR0` and every field of a Secure interrupt
  (`platform/a733/npu_irq.c` comment; facts with status `raz-in-view`). SGIs 1 to 15 read priority 0 because only SGI 0
  is written (`cpu_continue.c:120`); they are Non-secure Group 1 (`cpu_continue.c:115`).
