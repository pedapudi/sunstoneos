# A733 hardware facts

Code asserts values that the hardware presents, and host models restate the same values. Three board failures
came from one wrong assumption in both places: the Non-secure priority byte of the shared timer (PPI 27),
the read-back of `ICC_PMR_EL1` after an unmasked write, and an NPU interrupt bind condition that is still
open. The facts file `docs/hardware/a733-facts.json` records what the hardware presents, with the view, the
core and the source register of every value. A probe image measures the facts in one board run. Host models
and checks read the file instead of restating a value.

## Each fact carries its view

A fact is one decoded field of one register. Its entry in the JSON file has these members.

| Member | Meaning |
| --- | --- |
| `name` | A dotted name, for example `gic.cpuif.icc_ctlr_el1.pribits` or `gic.dist.spi97.priority`. |
| `source` | The register, with the byte or bit when the fact is a field of a larger register. |
| `view` | `el`, `security` and `interface`. The probe runs at EL1 in the Non-secure state that the cached monitor enters (`SCR_EL3.NS` set at entry). `SCR_EL3` is not readable at EL1, so the state is stated, not read back. |
| `core` | The core that read the value, `0` for shared facts that core 0 reads once, `all` for an unmeasured per-core fact, `null` for a fact without a core. |
| `status` | `measured` (read by the probe on the board), `observed` (seen on a board before the probe), or `unmeasured`. |
| `value`, `written` | The value, and for the write tests the value that was written. |
| `queue_entry`, `image_sha256`, `port` | The board run that produced the value. |

The GIC has two security states, so the Non-secure value of a priority byte or of `ICC_PMR_EL1` differs from
the Secure value that EL3 wrote (Linux `irq-gic-v3.c` lines 95-115; IHI0069 section 4.8.1). Every GIC fact
therefore names its view. Facts read on the virtual board (`checks/qemu-os-boot.py`) carry the platform
`qemu-os-board` in the table header and the decoder refuses to merge them.

## The probe image

`apps/hw-facts` is an OS application of the CPU-return lifetime. The OS configuration is
`SUNSTONE_A733_OS_CPU_RETURN`, `A733_A76_ADMITTED`, `SUNSTONE_A733_OS_CPU_CLOCK`, `SUNSTONE_A733_CPU_TOPOLOGY`
and `SUNSTONE_A733_HW_FACTS`, with `SUNSTONE_APPLICATION=hw-facts`. Every core calls `hwf_capture`
(`platform/a733/hw_facts.c`) from `ss_cpu_observe_boot`, at EL1, before the scheduler starts. The application waits for all eight cores, then returns the 9792-byte table with
`ss_finish`. The returned table has this layout (`include/a733_hw_facts.h`, little-endian).

| Offset | Content |
| --- | --- |
| 0 | 64-byte header: magic `A733HWF1`, version, size, platform, core count, slot counts and offsets, mask of finished cores. |
| 64 | 96 shared slots of 16 bytes, written by core 0. |
| 1600 | Eight blocks of 64 core slots, one block per core. |

A slot holds the fact identifier (16 bits), the core, flags (read, skipped, anomaly), an auxiliary 32-bit
value and a 64-bit value. The slot order comes from `tools/hw_facts.py`, which writes
`include/a733_hw_facts_table.inc`; `checks/hw-facts.py` compares the two.

### What the probe measures

| Scope | Facts |
| --- | --- |
| Each core | `MPIDR_EL1`, `MIDR_EL1`, `REVIDR_EL1`, `CNTFRQ_EL0`, `CTR_EL0`, `CLIDR_EL1`, `DCZID_EL0`; `CCSIDR_EL1` for `CSSELR_EL1` 0 to 7; `ID_AA64PFR0/1`, `DFR0/1`, `AFR0`, `ISAR0/1/2`, `MMFR0/1/2/3`, `ZFR0`; `PMCR_EL0`. |
| Each core, GIC CPU interface | `ICC_CTLR_EL1` (PRIbits, IDbits and the other fields), `ICC_PMR_EL1` on entry, `ICC_RPR_EL1` with no interrupt active, `ICC_SRE_EL1`, `ICC_IGRPEN0/1_EL1`, `ICC_BPR0/1_EL1`; the read-back of `ICC_PMR_EL1` after writing 0xff and after writing the FreeRTOS API mask. |
| Each core, redistributor | `GICR_TYPER`, `GICR_WAKER`, `GICR_IIDR`; in the SGI and PPI frame the group, group modifier, enable, pending, active and configuration words, and the priority byte of every SGI and PPI (INTID 0 to 31, including PPI 27). |
| Core 0, distributor | `GICD_CTLR` (DS, ARE and the enables), `GICD_TYPER`, `GICD_IIDR`, `GICD_TYPER2`, `GICD_PIDR2`. |
| Core 0, SPIs | For INTID 56 to 61, 67, 69, 70, 71, 80, 81, 95, 97, 143, 149, 153, 161, 185, 187, 200, 201, 228 and 242: priority, group, group modifier, enable, pending, active, configuration and route, with the route the image requested, so that the configured Secure value and the Non-secure read-back sit side by side. |
| Core 0, PPI writes | The priority byte of PPI 27 and PPI 30 read back after writing the value the platform writes next. |
| Core 0, device words | The NPU admission words `0x070101ac`, `0x020025c0`, `0x020025e0`, `0x07064000`, `0x07064008`. The two PPU words are read only after the three gates are open, as the runner requires; otherwise the slot is marked skipped. |

### What the probe writes

The probe changes no clock, voltage, reset or power domain and runs at the boot operating point. It reads
registers and makes these writes, each of a value that every image writes in the same boot, and each restored.

| Register | Value | Why it is allowed |
| --- | --- | --- |
| `CSSELR_EL1` | 0 to 7 | Selects a cache for `CCSIDR_EL1`, as `ss_cpu_observation_capture` does; the entry value is restored. |
| `ICC_PMR_EL1` | 0xff, then the API mask | The two values the FreeRTOS port writes; the entry value is restored and a mismatch sets the anomaly flag. |
| `GICR_IPRIORITYR` for PPI 27 and PPI 30, core 0 | 0xd8 and the lowest usable priority | The cached platform writes these two bytes with these values next; the entry value is restored. |

The image also installs, at EL3 and through the ordinary route installer, routes for INTID 71 (thermal) and
for INTID 97, 143 and 95 with priority 0xd8 on core 0, with disabled stub handlers
(`tools/sunstone-manifest.py`, `SUNSTONE_A733_HW_FACTS`). The NPU, audio and GPU images route the same INTIDs
with the same priority. No interrupt is enabled.

Not measured, with the reason:

- Whether a Non-secure write to a group, enable or configuration field is ignored. No image writes those
  words from EL1 with a value that changes them, so a test would be a new write.
- PMIC rail voltages. Reading them needs TWI transactions, which write the controller. The rail helper reads
  them before launch and the file records those readings as `runner.rail.*`.
- CPU PLL, mux and divider words, and the boot clock words that the entry guard checks. They are device reads
  of the clock unit; the first probe stays with the GIC, the CPU and the NPU admission words.
- `GICD_IGROUPR` for INTIDs that no image routes. The probe reads the state as left; the unrouted state is
  not a value any image asserts.

## Run and decode

Build the image:

```sh
cfg=$HOME/.cache/sunstoneos/bin/cfg.sh; build=$HOME/.cache/sunstoneos/build/hw-facts-1101/board
"$cfg" "$PWD" "$build" -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON -DSUNSTONE_APPLICATION=hw-facts \
  -DSUNSTONE_A733_OS_CPU_RETURN=ON -DA733_A76_ADMITTED=ON -DSUNSTONE_A733_OS_CPU_CLOCK=ON \
  -DSUNSTONE_A733_CPU_TOPOLOGY=ON -DSUNSTONE_A733_HW_FACTS=ON
cmake --build "$build" --target sunstone-a733-os-hw-facts -j12
python3 tools/hw_facts.py expect expect.json
```

The queue entry uses `--kind cpu-return` and `--expect expect.json`, which fixes the layout, the slot
identities and the flag range of every slot and leaves the values free. After the run, decode the returned
output (`output.bin` in the run directory) and merge it:

```sh
python3 tools/hw_facts.py decode output.bin --merge docs/hardware/a733-facts.json \
  --queue-entry ENTRY_ID --image-sha256 SHA256 --port 3-3
```

Merging replaces the unmeasured and observed entries of the facts the table covers, adds the queue entry to
each, and refuses a table whose header names the virtual board. Record the run with
`tools/evidence.py add ENTRY_ID --subsystem hardware-facts ...` afterwards.

## Observed before the probe

| Fact | Value | Source |
| --- | --- | --- |
| `gic.redist.intid27.priority`, Non-secure read of the shared timer's priority byte after the image wrote 0xd8 | 0xb0 | Queue entry `1793587317962103239-5bbfc48d` accepted this value in the finite NPU deadline setup. Whether the write took effect is open. |
| `gic.cpuif.icc_pmr_el1.readback_after_write_ff` | 0xf8 | Commit `55a35b4d` describes the read-back. The queue entry that showed it is not recorded. |
| `runner.mmio.*` for the five NPU admission words | `0x070101ac` 0x1, `0x020025c0` 0x110103ff, `0x020025e0` 0xf1055803, `0x07064000` 0x8, `0x07064008` 0x8 | The `npu_on_admission` record of the same entry. |
| `runner.rail.*`: DCDC3 and DCDC5 800 mV, DCXO 26 MHz, CPU voltage bin 4 | as listed | The `rails` record of the same entry. |

The open NPU interrupt bind condition is `bind_refusal` in `platform/a733/npu_irq.c`; the probe records every
register it reads (clauses 3 to 9) for INTID 97 in the state a routed image leaves at EL1.

Two observations do not fit one model. The read-back of `ICC_PMR_EL1` is not shifted (an unmasked write reads
back 0xf8 with five priority bits), yet the Non-secure priority of PPI 27 reads 0xb0 after the image wrote
0xd8. The host model takes the PMR view from the table, and takes the priority read-back of an interrupt with
an observed value from the table. The probe's PPI write tests and the Non-secure priority reads of every
SGI and PPI decide between a write that is ignored and a different view.

## Inventory of asserted and refused hardware values

Code sites assert or refuse on the value; model sites restate it. "Fact" names the facts entry that holds the
measurement.

| # | Value or behaviour | Code site | Host model site | Fact |
| --- | --- | --- | --- | --- |
| 1 | PPI 27 priority byte 0xd8 written, Non-secure read-back | `platform/a733-smp-cached/platform.c` (a733_smp_primary_el1), `apps/npu-finite/deadline.c` | `platform/qemu-os/platform.c` local_init, `checks/npu-deadline-setup.py`, GIC models | `gic.redist.intid27.priority[_after_write]` |
| 2 | PPI 30 priority byte | `platform/a733-smp-cached/platform.c` | `platform/qemu-os/platform.c` | `gic.redist.intid30.priority[_after_write]` |
| 3 | `ICC_PMR_EL1` unmasked read-back (0xf0 asserted, 0xf8 seen) | `platform/a733/platform.c` configASSERT, `apps/os/cpu-return.c` core_can_schedule, `apps/os/cpu-return-el0.S` | `GicModel` in `tools/hw_facts.py`; the five Unicorn models | `gic.cpuif.icc_pmr_el1.readback_after_write_ff` |
| 4 | `ICC_PMR_EL1` read-back of the API mask | `patches/freertos-smp-c.patch` (xPortStartScheduler) | same | `...readback_after_write_api_mask` |
| 5 | Secure PMR bit 7 after EL3 writes 0xff | `platform/a733-smp-cached/continue.S` | `secure_pmr` in the Unicorn models | `gic.cpuif.icc_pmr_el1.inherited` |
| 6 | `ICC_CTLR_EL1` PRIbits 5 (only `ICC_AP0R0` and `ICC_AP1R0` exist), IDbits | `platform/a733-smp-cached/continue.S` comments and active priority clears | `checks/a733-smp-cached.py` | `gic.cpuif.icc_ctlr_el1.pribits`, `.idbits` |
| 7 | `ICC_RPR_EL1` at idle 0xff | the FreeRTOS port (`patches/freertos-smp-c.patch`) | `checks/a733-supervised-scheduler.py`, `checks/a733-smp-cached.py` | `gic.cpuif.icc_rpr_el1.idle` |
| 8 | `GICD_CTLR` ARE_S and ARE_NS set and group enables clear at route time | `platform/a733-smp-cached/irq_table.c` (a733_irq_table_route) | `checks/a733-irq-table.c` | `gic.dist.gicd_ctlr.*` |
| 9 | `GICD_TYPER` line count bounds each routed INTID | `platform/a733-smp-cached/irq_table.c` | `checks/a733-irq-table.c` | `gic.dist.gicd_typer.itlinesnumber` |
| 10 | INTID 97 group, configuration, enable, pending, active, priority, route (bind clauses 3 to 9) | `platform/a733/npu_irq.c` bind_refusal | `checks/npu-board-providers.py` | `gic.dist.spi97.*` |
| 11 | INTID 187 configuration and priority 0xb0 | `apps/board/host_irq.c` board_host_irq_bind | `checks/usb-host-irq.c`, `checks/usb-host-irq-route.py` | `gic.dist.spi187.*` |
| 12 | INTID 71 and 143 routes at priority 0xd8 | `tools/sunstone-manifest.py` route table, `apps/os/thermal.c`, `apps/os/audio.c` | `checks/a733-irq-table.c` | `gic.dist.spi71.*`, `spi143.*` |
| 13 | Other device SPIs: 56 to 61, 67, 69, 70, 80, 81, 95, 149, 153, 161, 185, 200, 201, 228, 242 | `drivers/spi`, `include/a733_pwm.h`, `a733_lradc.h`, `a733_adc.h`, `a733_gpu_backend.h`, `a733_ce.h`, `drivers/rtc/a733_rtc_boot.c`, `apps/isp0/irq.h`, `apps/pcie-*/irq.c` | per-driver checks | `gic.dist.spiN.*` |
| 14 | SGI 0 priority and group (Secure 0xf0, Non-secure view) | `platform/a733-smp-cached/continue.c`, port | `checks/a733-supervised-scheduler.py`, `checks/all-core-task-workload-probe.py` | `gic.redist.intid0.priority` |
| 15 | `GICR_TYPER`, `GICR_WAKER` words read at pre-release admission | `platform/a733-smp-cached/continue.c` | `checks/all-core-first-task-probe.py` ADMISSION_OFFSETS | `gic.redist.gicr_typer`, `gicr_waker` |
| 16 | Redistributor group, enable, pending, active words of SGI and PPI frames | `platform/a733-smp-cached/continue.c` | the Unicorn models (`0x10080`, `0x10100`) | `gic.redist.sgi_ppi.*` |
| 17 | `CNTFRQ_EL0` is 24 MHz | `platform/a733/counter_clock.c`, `platform/a733-smp-cached/start.S` (writes it), `include/` conversions by 24 | `platform/qemu-os/platform.c` (declares 24 MHz), `checks/a733-smp-cached.py` | `cpu.cntfrq_el0` |
| 18 | `MPIDR_EL1` affinity: core in Aff1, Aff0, Aff2 and Aff3 zero | `platform/a733/cpu_start.c` a733_cpu_index | `checks/a733-smp-cached.py` (`0x81000000 | c << 8`) | `cpu.mpidr_el1.*` |
| 19 | `MIDR_EL1` masked part number: Cortex-A55 on cores 0 to 5, Cortex-A76 on cores 6 and 7 | `platform/a733/cpu_coherence.c`, `platform/a733/cpu_entry.c` | `checks/a733-smp-cached.py`, `checks/a733-supervised-scheduler.py` (`0x412fd050`, `0x414fd0b1`) | `cpu.midr_el1.*` |
| 20 | `REVIDR_EL1` selects errata workarounds | `platform/a733/cpu_entry.c` | the same models | `cpu.revidr_el1` |
| 21 | `CTR_EL0`, `CLIDR_EL1`, `CCSIDR_EL1` per cache: line size, sets, ways, levels | `arch/aarch64/cache.S`, `platform/a733/cpu_observation.c`, `kernel/scheduler/topology.c` | `checks/a733-smp-cached.py` (`ctr_el0`, `clidr_el1`) | `cpu.ctr_el0.*`, `cpu.clidr_el1`, `cache.ccsidr_el1.selN` |
| 22 | `ID_AA64ISAR0_EL1` atomics (LSE), `ID_AA64MMFR2_EL1.CCIDX` | `platform/a733/cpu_observation.c` | `checks/a733-smp-cached.py` | `cpu.id_aa64isar0_el1.atomic`, `cpu.id_aa64mmfr2_el1.ccidx` |
| 23 | `ID_AA64MMFR0_EL1` physical address range, granules | `arch/aarch64`, `platform/a733-smp-cached/memory.c` table geometry | `checks/a733-smp-cached.py` (`id_aa64mmfr0_el1: 2`) | `cpu.id_aa64mmfr0_el1.*` |
| 24 | `ID_AA64PFR0_EL1` EL levels, GIC system registers, FP and AdvSIMD | the cached boot and the port | the models | `cpu.id_aa64pfr0_el1.*` |
| 25 | `PMCR_EL0.N` event counters | `platform/a733/cpu_pmu.c`, `continue.S` MDCR_EL2.HPMN | `checks/cpu-pmu.py` | `cpu.pmcr_el0` |
| 26 | NPU admission words: PCK clock gate, two fabric gates, PCK4 policy and status (both 8) | `tools/board_images.py` validate_npu_on, `tools/board.py` npu_on_state, `drivers/power/a733_power_domains.c` | `checks/board-npu-on-admission.py` | `mmio.0x070101ac`, `0x020025c0`, `0x020025e0`, `0x07064000`, `0x07064008`; runner copies in `runner.mmio.*` |
| 27 | PMIC rails DCDC3 and DCDC5 at 800 mV, DCXO 26 MHz, voltage bin | rail helper (`tools/board_state.py`), `drivers/power/a733_pmic.c` | `platform/qemu-os/mmio.c`, `checks/a733-pmic.c` | `runner.rail.*` (not in the probe) |
| 28 | TWI controller idle state `0xf8` status, control `0x40`, divider | `drivers/power/a733_pmic.c`, `drivers/i2c/a733_twi.c` | `platform/qemu-os/mmio.c`, `checks/board-bundles-apps-model.c` | not a probe fact: the probe makes no TWI access |

The inventory holds 28 rows. Rows 1 to 4 and 10 are the facts behind the three board failures; rows 5 to 9 and
11 to 25 are the other GIC and CPU values that code asserts or that a model restates; rows 26 to 28 are the
runner and PMIC values.

## Adoption plan

Host models first, driver code second. This round converts the models of rows 3 to 7 and 14; no driver
behaviour changes.

Done in this round:

- `tools/hw_facts.py` provides `Facts` and `GicModel`. `GicModel` takes the number of implemented priority
  bits from `ICC_CTLR_EL1.PRIbits`, selects the shifted or unshifted Non-secure view that reproduces the
  recorded `ICC_PMR_EL1` read-back, fails with `ContradictsTable` when neither does, and returns an observed
  Non-secure priority for an interrupt that has one.
- `checks/a733-smp-cached.py`, `checks/a733-supervised-scheduler.py`, `checks/all-core-first-task-probe.py`,
  `checks/all-core-task-workload-probe.py`, `checks/primary-cache-probe.py` and
  `checks/secondary-interrupt-probe.py` use `GicModel` for the priority mask, the Non-secure writes, the
  Non-secure reads and the `ICC_PMR_EL1` and `ICC_RPR_EL1` views.
- `checks/hw-facts.py` fails when one of those models hard-codes the mask, the shift or a Non-secure priority
  literal, when the table and the layout of the C code differ, and when the facts file is malformed.

Remaining, in order:

1. After the probe run: merge the table, then re-run `checks/hw-facts.py`. A model whose rule the table
   contradicts fails there, not on a board.
2. `platform/a733/platform.c` and `apps/os/cpu-return.c`: replace the literal `0xf0` and `0xff` comparisons
   by a constant generated from the table (`gic.cpuif.icc_pmr_el1.readback_after_write_ff`).
3. `apps/board/host_irq.c`, `platform/a733/npu_irq.c`, `apps/npu-finite/deadline.c`: replace the expected
   Non-secure priority byte by `NS_VIEW(route priority)` taken from the measured pair for INTID 187, 97 and 27.
4. `platform/qemu-os/platform.c` and the QEMU board registers: seed `CNTFRQ_EL0`, `MIDR_EL1`, `MPIDR_EL1` and
   the PMR read-back from the table where QEMU allows it, and state where QEMU differs.
5. `checks/a733-smp-cached.py` ID registers, `CTR_EL0`, `CLIDR_EL1` and `MIDR_EL1` (rows 17 to 24): load the
   per-core values.
6. Rows 26 and 27: `tools/board_images.py` reads its gate masks and expected policy values from the table.
7. Rows 8, 9, 11 and 13: compare the route installer's checks with the measured Secure and Non-secure fields.

## Host tests

- `checks/qemu-os-boot.py` builds the facts image for the virtual board, runs it, decodes the table the
  application returns and checks the decoded values and the board expectation. The table names the virtual
  board.
- `checks/hw-facts.py` decodes synthetic tables (valid, skipped slots, malformed, incomplete, wrong core,
  virtual board), checks the expectation, the JSON file and `GicModel` against the file and against
  contradicting files, and scans the host models.
- The Unicorn models cannot run end to end on this tree: `checks/a733-supervised-scheduler.py` stops at
  `id_aa64isar0_el1` before any change here, and the others need cached evidence. They compile and import.
