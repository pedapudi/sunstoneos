# Eighteen SunstoneOS actions follow from the vendor-binary analysis

Status: **plan**. This document turns the analysis of the A733 vendor binaries
and the three reference backsolves into SunstoneOS changes. It changes no code
and records no board run. Every action names the reference behaviour that
grounds it, the SunstoneOS code it changes, and the check that verifies it.
Line numbers are on `bringup` at `25bf0af7`.

[`AGENTS.md`](../../AGENTS.md) sets two rules that shape the plan. Hardware
sequences come from the references before a board run, so every board step
below tests a stated prediction. Replacing opaque firmware waits until the
hardware works and is tuned, so firmware replacement appears only in the
[deferred section](#deferred-work-replacing-opaque-firmware).

## Sources

The four vendor-binary documents describe each binary's behaviour with blob
offsets; the backsolves compare the vendor sequence with SunstoneOS step by
step. Each document is merged into `bringup` or still on the branch shown.

| Document | Covers | Location until merged |
| --- | --- | --- |
| [Boot ROM and boot0](boot-rom-and-boot0.md) | ROM reset, FEL, shared-SRAM routing, CPU PLLs, PMIC rails, DRAM training, boot-package handoff | branch `work/blob-doc-boot-1025` |
| [TF-A monitor](tf-a-monitor.md) | Vendor EL3 firmware (BL31): cold-boot security writes, PSCI, GIC, errata, SMC services, SCP start | branch `work/blob-doc-monitor-1025` |
| [SCP firmware](scp-firmware.md) | E902 firmware: suspend, resume, power-off, wake sources, DRAM self-refresh | branch `work/blob-doc-scp-1025` |
| [GPU and NPU binaries](gpu-and-npu-binaries.md) | GPU firmware container and host interface; NPU libraries, driver commands, feature database | branch `work/blob-doc-accel-1025` |
| [CPU backsolve](../reference-backsolve-cpu.md), [GPU backsolve](../reference-backsolve-gpu.md), [NPU backsolve](../reference-backsolve-npu.md) | Vendor sequence compared with SunstoneOS, divergences ranked | merged |

Each document's curated JSON inventories are under [`data/`](data/) on its
branch.

## Work already applied or in flight

These items are excluded from the ranking. The gaps found in them are ranked
actions.

| Work | State | Gap that this plan ranks |
| --- | --- | --- |
| GPU vendor state: BL31 security writes, DCDC4 at 960 mV, GPU PLL at 1008 MHz | The diagnostic image read BVNC `36.56.104.183` on port 3-1 ([handoff](../handoff.md)). The writes live in copies under `apps/gpu-power-id/` on branch `work/gpu-vendor-default-1025` | The production monitor makes none of the security writes (action 1). The firmware start lacks the vendor's secure configuration (action 5) |
| Shared SRAM routed to the NPU | Merged as a variant of the finite numerical image; both jobs matched the reference on port 3-3 | The production startup sequencer and monitor do not route it (action 2) |
| Multicore correctness: GIC status masks, RWP waits, unused-EL2 setup, active priorities | Branch `work/mc-correct-1025`, not merged | The timer event stream and GIC-600 erratum 2384374 are outside that change (actions 7 and 8) |
| Multicore performance: lock bookkeeping, counter clock, newlib state, IRQ exit, tick | Counter clock and newlib state merged; the rest on branch `work/mc-perf-1025` | None from the vendor binaries |

## How the actions are ranked

Each action carries one category:

- **correctness**: SunstoneOS omits or changes a step that the hardware needs;
- **performance**: the vendor runs a clock or setting that SunstoneOS does not,
  and the reference shows it is in specification;
- **power**: the vendor gates a clock or domain that SunstoneOS leaves running;
- **robustness**: a step that bounds or records a failure;
- **feature**: an interface that a planned capability needs.

**Value** is what fails without the action or what it gains. **Risk** is the
chance that the change stalls a board. A stall ends the board's lifetime, and
only the user's power removal recovers it. The rank orders value over risk. A read-only step
ranks high because it costs almost nothing and decides later writes.

**Verification** is either a host check (a model, an instruction harness or a
comparison against a curated inventory) or a board prediction with the run
that tests it. Board steps follow the board-test rule of `AGENTS.md`: within a connection
window, the run most likely to lose the board runs last.

Performance actions also pass the gate in
[performance architecture](../performance-architecture.md) section 4: a short
design with budgets, a review, and measurements before and after. Every clock
or voltage change on a board needs the user's approval for that change
([CPU clocks](../cpu-clock.md#a-session-must-grant-each-change)).

## Ranked actions

| Rank | Action | Category | Value | Risk | Tranche |
| ---: | --- | --- | --- | --- | --- |
| 1 | Make BL31's cold-boot security writes in the production EL3 monitor | correctness | high | low | 3 |
| 2 | Route the shared SRAM to the NPU in the production NPU path | correctness | high | low | 5 |
| 3 | Raise CPU-L to 1014 MHz and CPU-B to 1196 MHz at the inherited 800 mV | performance | high | low | 7 |
| 4 | Read the vendor-written state on both boards in one read-only row | robustness | medium | very low | 2 |
| 5 | Add the vendor's secure configuration to the GPU firmware start | feature | high | low | 6 |
| 6 | Run the NPU at 852 MHz at the inherited 800 mV | performance | high | medium | 5 |
| 7 | Work around GIC-600 erratum 2384374 in images that route SPIs | correctness | medium | low | 4 |
| 8 | Enable the generic-timer event stream | robustness | medium | low | 4 |
| 9 | Run boot0's interconnect setup after DRAM initialization | performance | medium | low | 1, then 8 |
| 10 | Scale the DSU clock to 780 MHz with the clusters | performance | medium | medium | 7 |
| 11 | Complete NPU jobs by interrupt and suspend its internal clock when idle | performance, power | medium | low | 5 |
| 12 | Find the writer that sets MBUS to 400 MHz in some lifetimes | performance | medium | none | 1, then 8 |
| 13 | Wait at least 1 ms after each CPU rail write | robustness | medium | none | 1 |
| 14 | Measure the A76 load-pass-store control | performance | low | low | 7 |
| 15 | Write the CPUIDLE policy words only if they differ from the vendor value | robustness | low | low | 4 |
| 16 | Let the redistributor power down after the last core sleeps | power | low | low | 4 |
| 17 | Correct six statements that the analysis contradicts | robustness | low | none | 1 |
| 18 | List the NPU models that need loader-generated transposes | feature | low | none | 1 |

### 1. Make BL31's cold-boot security writes in the production EL3 monitor

**Finding.** The vendor BL31 runs `sunxi_security_setup` at every cold boot
and every resume ([TF-A monitor](tf-a-monitor.md#cold-boot-register-writes),
writes 9 to 17). It makes the peripheral security controller ports, the
always-on peripherals and the clock controller's PLL, bus and MBUS registers
Nonsecure, and it opens every channel of both DMA controllers to Nonsecure
software. The manual makes `PLL_GPU0_CTRL_REG` and `NSI_CLK_REG` Secure by
default (sections 4.1.6.16 and 4.1.6.74).

**SunstoneOS today.** Absent. The monitor enters Nonsecure EL1 with SCR_EL3 =
`0x505` and no security write (`platform/a733/monitor.c:96`–`97`). The GPU
diagnostic applies 28 of the writes from a monitor copy, and that image
returned with the GPU identity read. Three records depend on the missing
writes:

- the [DMA startup](../dma-startup.md) requires an external record of
  Nonsecure channel state that nothing supplies (lines 75–82);
- the [memory-bus priority design](../designs/dram-bus-priority.md) lists
  Nonsecure access to the NSI ports and `NSI_CLK_REG` as unproven;
- the GPU PLL writes need the clock controller's switch.

**Change.** In the monitor's cold path, before the SCR_EL3 write, make BL31's
writes in its order:

1. SPC ports 0–13 and 16–23 (`0x02054004` + `0x10`·n, `0x02054104` + `0x10`·n)
   = `0xffffffff`;
2. S_SPC `0x07002004`, `+0x14`, `+0x24` = `0xffffffff`;
3. `PRCM_SEC_SWITCH_REG` `0x07010290` |= 7;
4. `CCMU_SEC_SWITCH_REG` `0x02003f00` = 7;
5. DMA0 `0x04601020` and DMA1 `0x04024020` = `0xffff`;
6. SPC port 14 `0x020540e0` = `0xfffffbff`, leaving bit 10 Secure and port 15
   unwritten as the vendor does.

Read back `0x02003f00`, `0x07010290` and the two DMA words, and record them in
the monitor result so that the DMA startup can cite them. Leave out the TIMER0
writes, whose purpose is unknown and which no SunstoneOS code uses. Decide the
S_TZMA writes (`0x07003000`–`0x07003008` = 0, SRAM Nonsecure) after reviewing
which SRAM the monitor keeps for itself. Share one write table with the GPU
diagnostic's monitor copy.

**Verification.** Host: a check compares the monitor's write list with
[`tf-a-monitor-mmio.json`](data/tf-a-monitor-mmio.json) in order, address and
value, and names each documented omission. Board, port 3-3 first, then port
3-1: rerun one qualified returning image built with the new monitor.
Prediction: it returns as before, `0x02003f00` reads 7, the PRCM switch reads
bits 2:0 set, and both DMA words read `0xffff`.

### 2. Route the shared SRAM to the NPU in the production NPU path

**Finding.** The boot ROM routes the 512 KiB shared SRAM to the CPU, and
U-Boot clears SYSCTRL `0x03000008` bit 1 to give it to the NPU
([NPU backsolve](../reference-backsolve-npu.md#boot-firmware)). With the bit
cleared while the NPU resets were held, both numerical jobs matched the
reference; without it, both outputs were wrong ([handoff](../handoff.md)).

**SunstoneOS today.** Implemented on `work/npu-production-1025`
([design](../designs/npu-sram-route.md)); not yet run on a board. The startup
sequencer routes the SRAM and powers domain 4 first, both NPU backends refuse
register access outside READY, the finite image's EL3 return path sets bit 1
before its ROM return, and the register pass adopts a running PLL_NPU. The
re-read of the master gates and the PCK clock at the final check (backsolve
fix 4) is not part of that change.

**Change.** Move the variant's three steps into production:

1. clear bit 1 with readback in the startup sequencer before the first write
   of CCU `+0xb04`, and set it again after local stop;
2. refuse NPU window access in the adapter unless the routing step has run and
   the startup is READY;
3. set bit 1 at EL3 before `monitor_reset()` whenever it reads clear.

Add the remaining backsolve fixes that the variant left out:

- request or adopt domain 4 ON before the local open table, the order that
  runs on the vendor board (`a733_npu_startup.c:324`–`339`; backsolve fix 2);
- re-read CCU `+0x5c0` bit 6, `+0x5e0` bit 18 and R_CCU `+0x1ac` bit 0 at the
  final check (fix 4);
- adopt a PLL_NPU that is already enabled and locked at the profile factors.
  The vendor's rate path accepts an enabled PLL. Today a second NPU startup in
  one lifetime is refused and needs a power removal.

**Verification.** Host: retarget the variant's linked-instruction check
(`apps/npu-finite/sram-route-check.py`) at the production objects. Board,
port 3-3: the production numerical image. Prediction: both outputs match the
reference as in the variant run, a FEL read after the ROM return finds
`0x03000008` = 2, and with PLL adoption a second startup in the same lifetime
reaches READY.

### 3. Raise CPU-L to 1014 MHz and CPU-B to 1196 MHz at the inherited 800 mV

**Finding.** Boot0 leaves CPU-L at 494 MHz (N=38, divider 2), CPU-B at
1014 MHz and the DSU at 494 MHz ([boot0](boot-rom-and-boot0.md#clock-and-pll-setup)).
The vendor raises them later through Linux cpufreq and dsufreq; neither BL31
nor the SCP sets a CPU rate ([CPU backsolve](../reference-backsolve-cpu.md#dvfs-raises-voltage-before-frequency)).
The pinned operating-point table gives 800 mV to CPU-L at 1014 MHz and to
CPU-B at 1196 MHz in every speed-bin column. The board reads DCDC3 and DCDC5
at 800 mV.

**SunstoneOS today.** The driver exists and has never run
(`drivers/clock/a733_cpu_clock.c:41`–`74`; [CPU clocks](../cpu-clock.md)). It
is compiled only with `SUNSTONE_A733_OS_CPU_CLOCK`, and no session carries a
grant.

**Change.** Neither raise needs a voltage change. Write the performance
design with its budgets, then prepare a session whose grant names one cluster
and one change, with the user's approval record. CPU-L first, then CPU-B.

**Verification.** Host: the driver's existing model with a grant for each
target. Board, port 3-3, one cluster per run. Prediction for CPU-L: the PLL
word reads N=39 (`0xf8802700`), the mux divider field reads 0, the workload
passes, and the A55 recognition time falls toward 494/1014 = 0.49 of its
present value, less where memory bounds the work. Prediction for CPU-B: N=46
(`0xf8802e00`) and an A76 time ratio near 1014/1196 = 0.85.

### 4. Read the vendor-written state on both boards in one read-only row

**Finding.** Several later actions depend on words that the vendor stack
writes and that no SunstoneOS record holds.

**SunstoneOS today.** The NPU read row (`tools/prepare-board-bundles.py:455`)
already reads the speed bin, domain 4 and `0x03000008`. Port 3-1's speed bin
is not recorded; port 3-3's reads `0x496`, code 4, column vf0400.

**Change.** Add one FEL read row with these words:

- the nine CPUIDLE policy words `0x07051000` and `0x07052000`–`0x07059000`;
- the nine interconnect words that boot0 and the SCP write: `0x02023a14`,
  `0x02023c14`, `0x02023e14`, `0x02024014`, `0x02024214`, `0x0202446c`,
  `0x0202466c`, `0x0202200c`, `0x0202220c`;
- the security words `0x02003f00`, `0x07010290`, `0x04601020`, `0x04024020`;
- `NSI_CLK_REG` `0x02002580` and `MBUS_CLK_REG` `0x02002588`;
- the R_PRCM auto-gating words `0x07010338`, `0x0701033c`, `0x07010360`;
- the speed-bin word `0x0300624c`.

FEL reads run in the ROM's Secure state, so Secure-default words read their
true values.

**Verification.** Board, both ports, first in a connection window. Predictions:

- every interconnect word differs from the vendor value, because the FEL path
  skips boot0's interconnect routine;
- `0x02003f00` reads other than 7, because no BL31 has run;
- bits 3:0 of the eight core policy words read 0, the value BL31 writes,
  since cores power off on both boards;
- MBUS reads 400 or 600 MHz.

The row makes no write.

### 5. Add the vendor's secure configuration to the GPU firmware start

**Finding.** Before `RGXStart`, the vendor GPU driver writes SMC0
`0x0a0000b0` = 2 and the GPU wrapper's `drm_ctrl` `0x01880014` = 0, both by
plain stores from Nonsecure EL1 (`sunxi_platform.c:964`–`979`;
[GPU backsolve](../reference-backsolve-gpu.md#6-firmware-start-follows-the-secure-configuration)).

**SunstoneOS today.** Absent. The start sequence writes `RGX_CR_SYS_BUS_SECURE`
only (`drivers/gpu/pvr_gpu_start.c:91`–`93`).

**Change.** Add the two writes, each with a readback, before the soft-reset
steps of the start sequence.

**Verification.** Host: the start-sequence reference check gains the two
steps in the vendor order. Board, port 3-1, after the GPU vendor state is the
production default: the first firmware-start image. Prediction: SMC0 `+0xb0`
reads 2, the firmware's boot status reaches the state that the open driver
waits for, and the first health-check command completes.

### 6. Run the NPU at 852 MHz at the inherited 800 mV

**Finding.** The vendor NPU driver selects 1008 MHz and never changes DCDC2
([NPU backsolve](../reference-backsolve-npu.md#npu-driver-probe), step 11).
Its own table lists 1008 MHz at 900–1030 mV, so that setting runs outside the
table, and SunstoneOS does not copy it. The table's 26 MHz columns, which apply
to this board's oscillator, list 852 MHz at 800 mV for vf0000, vf0200, vf0201,
vf0204, vf0400, vf0500 and vf0501 (`sun60iw2p1-cpu-vf.dtsi:2069`–`2091`).
Boot0 programs DCDC2 in its DRAM supply setup, so a voltage raise would also
change the DRAM supply; the plan rules it out.

**SunstoneOS today.** 492 MHz: N=82, divider 4 (`drivers/npu/a733_npu_startup.c:14`–`24`).

**Change.** After action 2, a second profile at N=71 and divider 2 (24 MHz ×
71 ÷ 2 = 852 MHz), admitted only for a speed-bin code whose column lists
800 mV.

**Verification.** Host: the startup model with the second profile. Board,
port 3-3, whose column lists 800 mV: the numerical image at 852 MHz. Port 3-1
runs it only after action 4 records its bin.
Prediction: both outputs are byte-identical to the 492 MHz run, and the job
time falls toward 492/852 = 0.58 of its value.

### 7. Work around GIC-600 erratum 2384374 in images that route SPIs

**Finding.** The GIC is r1p6, inside the erratum's range. Upstream TF-A
toggles GICR_CTLR.DPG at power-down; the vendor BL31 does not, which is no
evidence that the erratum is absent ([TF-A monitor](tf-a-monitor.md#new-insights-for-sunstoneos),
item 9). With SPIs routed and PMR masking, a higher-priority SPI can wait
without bound ([CPU backsolve](../reference-backsolve-cpu.md#divergences-ranked-by-their-chance-of-stopping-a-core),
item 6).

**SunstoneOS today.** Absent. The OS, I2S and NPU images route SPIs
(`platform/a733-smp-cached/irq_table.c:39`), and the FreeRTOS port masks with
ICC_PMR_EL1 (`patches/freertos-smp-c.patch:108`).

**Change.** Toggle the three DPG bits after clearing the group enables and
before setting ProcessorSleep, as `gic-x00.c:183`–`196` does. Add one of the
erratum notice's remedies for PMR masking: a periodic DPG toggle, or masking
through GICD_ICENABLER.

**Verification.** Host: the redistributor model observes the toggle in order.
Board: the backsolve's run 6, last in its window. Prediction: without the
remedy, the higher-priority SPI's latency has no bound; with it, the toggle
period bounds the latency.

### 8. Enable the generic-timer event stream

**Finding.** Linux enables a 10 kHz event stream so that every WFE ends within
100 µs; a lost event cannot then leave a core waiting with interrupts masked
([CPU backsolve](../reference-backsolve-cpu.md#divergences-ranked-by-their-chance-of-stopping-a-core),
item 5).

**SunstoneOS today.** Absent. CNTKCTL_EL1 = 3 (`platform/a733-smp-cached/continue.S:139`–`140`,
`platform/a733/cpu_continue.S:121`).

**Change.** Write CNTKCTL_EL1 = `0xa7`: EL0 counter access, EVNTEN, and
EVNTI = 10, an event every 2^11 counter ticks (85 µs at 24 MHz).

**Verification.** Host: the instruction harness reads `0xa7` on every core.
Board: no dedicated run. Prediction for the next eight-core workload run:
an unchanged pass and unchanged task timings.

### 9. Run boot0's interconnect setup after DRAM initialization

**Finding.** Boot0's main calls a routine (`0x49050`) that writes six NSI
interconnect words through `0x48f38` and then a barrier
([boot0](boot-rom-and-boot0.md#what-boot0-deliberately-does-not-do)). The SCP
writes the same six words on every resume, and three more
([SCP firmware](scp-firmware.md#resume), step 9):

| Address | SCP value | Written by boot0 |
| --- | --- | --- |
| `0x02023a14` | `0x3f` | yes |
| `0x02023c14` | `0x7f` | yes |
| `0x02023e14` | `0xf` | no |
| `0x02024014`, `0x02024214` | `0xf1` | yes |
| `0x0202446c`, `0x0202466c` | `0x40a05` | yes |
| `0x0202200c`, `0x0202220c` | 1 | no |

In the vendor NSI layout (`sunxi-nsi.h:354`–`365`; base `0x02020000`, `0x200`
per port), the first seven addresses are offsets `+0x14` and `+0x6c` of ports
29 to 35, beyond the 21 master ports. The last two are offset `+0xc` of the
ISP and CSI ports. The meaning of the port 29 to 35 words is unknown.

**SunstoneOS today.** Absent. The RAM wrapper calls only board setup and
`init_DRAM` (`boot/a733/ddr-entry.S:48`, `:65`).

**Change.** Tranche 1, host: decode routine `0x48f38` to record the values
boot0 writes, and confirm from `main`'s call order that the routine runs after
`init_DRAM`, as the boot0 document states. Tranche 8: if action 4 shows
different board values, call `0x49051` from the wrapper in `main`'s position.

**Verification.** Host: the recorded values equal the SCP's for the shared
addresses. Board: a cold DDR run with the call. Prediction: the words read the
vendor values afterwards and DRAM training still reports 12,288 MiB. The DRAM
latency window of the memory-bus priority design measures the effect.

### 10. Scale the DSU clock to 780 MHz with the clusters

**Finding.** The vendor's dsufreq lowers the DSU clock before a cluster
change and raises it after (`sunxi-dsufreq.c:298`–`368`). The pinned table
gives the DSU 780 MHz at 800 mV in every column. Boot0 leaves it at 494 MHz,
which bounds L3 and snoop bandwidth for all eight cores.

**SunstoneOS today.** Absent. The driver refuses DSU writes
(`drivers/clock/a733_cpu_clock.c:145`) and covers the DSU only by the CPU-L
voltage.

**Change.** After action 3, make the DSU a writable domain with the vendor
order. Use N=30 (26 MHz × 30 = 780 MHz) and the PLL sequence the driver
already uses for the clusters.

**Verification.** Host: the model refuses a DSU raise before the cluster
raise. Board, after action 3 passes: one DSU change. Prediction: the DSU PLL
reads N=30, the workload passes, and the eight-core soak's memory-bound phases
speed up more than its compute phases.

### 11. Complete NPU jobs by interrupt and suspend its internal clock when idle

**Finding.** The vendor enables every NPU interrupt (`+0x014` = `0xffffffff`),
completes jobs by interrupt, gates the internal clock at idle and ungates it
before a submit (`vip_drv_pm.c:157`–`209`;
[NPU backsolve](../reference-backsolve-npu.md#job-completion-and-shutdown)).

**SunstoneOS today.** `+0x014` = `0xc0000001`. Completion is polled with GIC
interrupt 97 masked, and full idle is required
(`drivers/npu/a733_npu_controller.c:30`–`32`, `:192`–`238`). The internal clock
is never suspended.

**Change.** After action 2, take completion from interrupt 97 and wake the
waiting task. Add the vendor's clock suspend and resume pulses around jobs.

**Verification.** Host: the controller model completes on the interrupt path
and keeps the fault decoding. Board: the numerical image. Prediction:
identical outputs, and the CPU is idle during the job.

### 12. Find the writer that sets MBUS to 400 MHz in some lifetimes

**Finding.** A board read found `MBUS_CLK_REG` = `0x84000000` (400 MHz) in one
lifetime where an earlier lifetime ran 600 MHz ([handoff](../handoff.md)).
Boot0's statically resolved accesses include `NSI_CLK_REG` but not `+0x588`
([`boot0-mmio.json`](data/boot0-mmio.json)), so the writer is a computed
access, probably in the DRAM code.

**Change.** Tranche 1, host: search boot0's full inventory under
`~/.cache/sunstoneos/evidence/vendor-firmware-20261025/boot/` for computed CCU
accesses in `init_DRAM`, and record the condition that selects 400 or
600 MHz. Tranche 8: pin the vendor's rate in the wrapper if the condition is
not a board property.

**Verification.** Host: the writer and condition are named with blob offsets.
Board: the read of action 4 on both ports gives the rate per lifetime.

### 13. Wait at least 1 ms after each CPU rail write

**Finding.** The vendor regulator driver rounds every settle time up to whole
milliseconds (`axp2101-regulator.c:383`–`409`).

**SunstoneOS today.** |ΔV| ÷ 250 µV/µs: a 100 mV raise waits 400 µs
(`drivers/clock/a733_cpu_clock.c:26`, `:438`).

**Change.** Wait the larger of that value and 1,000 µs.

**Verification.** Host: the driver model records a settle of at least 1 ms
for a 10 mV step. No board run until a voltage-raising change.

### 14. Measure the A76 load-pass-store control

**Finding.** SunstoneOS sets CPUACTLR2_EL1 bit 16 on the A76, a static
CVE-2018-3639 mitigation that disables store-to-load forwarding. Upstream
TF-A and the vendor BL31 leave it clear on r4p1 because the core implements
SSBS ([TF-A monitor](tf-a-monitor.md#cpu-errata-and-implementation-defined-registers)).

**SunstoneOS today.** Set (`platform/a733/cpu_coherence.c:35`).

**Change.** Measure A76 cycles for the recognition workload with the bit set
and clear. The security review decides whether the gain justifies clearing it.

**Verification.** Board: a timed A76 workload, both settings in one lifetime.
Prediction: identical results; the cycle count falls by the share of
store-to-load forwarding in the workload.

### 15. Write the CPUIDLE policy words only if they differ from the vendor value

**Finding.** BL31 clears bits 3:0 of the nine words at
`0x07051000`–`0x07059000` at PSCI setup. Their layout as PPU policy registers
is inferred ([TF-A monitor](tf-a-monitor.md#new-insights-for-sunstoneos), item 1).

**SunstoneOS today.** Never written; core power-off works with the inherited
value.

**Change.** After action 4, write them in the CPU startup only if a board read
differs from the vendor value.

**Verification.** Board: action 4's read decides; no write if the values
match.

### 16. Let the redistributor power down after the last core sleeps

**Finding.** TF-A writes `GICR_PWRR.RDPD` = 1 after the sleep handshake
(`gic-x00.c:88`–`107`). One redistributor block serves all eight cores, so the
block powers down only when every core sets RDPD.

**SunstoneOS today.** Never written (`boot/a733/secondary-local-probe.c:99`).

**Change.** Write RDPD = 1 after ChildrenAsleep, in images that power cores
off.

**Verification.** Host: the redistributor model. Board: none until a power
measurement exists.

### 17. Correct six statements that the analysis contradicts

- [Watchdog](../watchdog.md): the vendor stack never arms the CPUS watchdog,
  because its writes omit the key ([SCP firmware](scp-firmware.md#the-watchdog-setup-has-no-effect)).
- [Hardware matrix](../hardware.md), always-on processors (lines 224–244):
  system suspend needs DRAM PHY retention (13,532 registers and 32 KiB of PHY
  memory), and the vendor E902 firmware idles without `wfi`.
- [NPU backsolve](../reference-backsolve-npu.md#boot-firmware), item 4: the
  domain-ON loop's caller is the SCP's resume path, so it never runs at cold
  boot.
- The same document's "Sources and their gaps": `libNBGlinker.so` does import
  a register read and reads the identity registers; the conclusion that the
  libraries add no sequence stands.
- `drivers/npu/a733_npu_startup.c:43`–`48` and `:348`: the comments state
  clock-before-power and no source for the 2 ms settle; the vendor board runs
  power first, and `vip_drv_device_platform.c:1386`–`1387` gives the settle.
- [GPU MIPS](../gpu-mips.md): the open firmware's errata are BRN 44079 and
  63553, while the DDK header lists 63553 and 71317. No 71317 workaround
  belongs in the open-firmware path.

**Verification.** Host: the documentation index check.

### 18. List the NPU models that need loader-generated transposes

**Finding.** The A733 NPU has no tensor processor. `libVIPhal.so` generates
transpose descriptors at run time for copies that the loader splits into
64 KiB and 128 KiB pieces ([GPU and NPU binaries](gpu-and-npu-binaries.md#models-are-command-templates-that-the-loader-completes)).

**SunstoneOS today.** The assembler accepts compiled transpose steps in the
directory format (`drivers/npu/nbg_assembly.c:165`–`168`) and has no runtime
generator.

**Change.** Run `tools/inspect-nbg.py` and `nbinfo` over the twelve SDK models
and the mimus models, and list the ones that reach the runtime transpose path.
Implement a generator only for a model on that list that SunstoneOS needs.

**Verification.** Host: the list, with the evidence for each model.

## Tranches avoid the in-flight branches

Each tranche names its files and the work it waits for.

| Tranche | Actions | Files | Waits for |
| ---: | --- | --- | --- |
| 1 | Host analysis and records: 9 (decode), 12 (search), 13, 17, 18 | `drivers/clock/a733_cpu_clock.c`, documents, cache analysis | Nothing |
| 2 | Read-only state row: 4 | `tools/prepare-board-bundles.py` | The GPU vendor-state work, which edits the same file |
| 3 | Production EL3 security setup: 1; the monitor's SRAM-routing restore from action 2 | `platform/a733/monitor.c` | The GPU vendor-state work, so that one write table serves both monitors |
| 4 | CPU startup robustness: 7, 8, 15, 16 | `platform/a733-smp-cached/continue.S`, `platform/a733/cpu_continue.S`, `irq_table.c`, the GIC probes, `patches/freertos-smp-c.patch` | Multicore correctness (`continue.S`, `irq_table.c`, the probes) and multicore performance (the FreeRTOS patch); action 15 also waits for tranche 2 |
| 5 | NPU production: 2, 6, 11 | `drivers/npu/`, `platform/a733/npu_native.c` | Tranche 3 for the EL3 restore; action 6 on port 3-1 also needs its bin from tranche 2 |
| 6 | GPU firmware start: 5 | `drivers/gpu/pvr_gpu_start.c`, its reference check | The GPU vendor state becoming the production default |
| 7 | CPU and DSU clocks: 3, 10, 14 | `drivers/clock/a733_cpu_clock.c`, `apps/os/cpu.c`, `platform/a733/cpu_coherence.c` | Tranche 1 for the settle wait; the performance design gate; the user's approval of each change |
| 8 | DRAM interconnect: 9 (call), 12 (pin) | `boot/a733/ddr-entry.S`, `tools/prepare-ddr.py` | Tranches 1 and 2 |

Tranches 1 and 7 both edit the CPU clock driver; tranche 7 starts after
tranche 1 merges. Tranches 3 and 5 meet at the monitor's ROM-return path,
which tranche 3 owns.

## Findings that need no change

- **PMIC voltage writes preserve bit 7.** Boot0 sets bit 7 of the DCDC
  selectors; the PMIC driver writes only the 7-bit field
  (`drivers/power/a733_pmic.c:324`).
- **Inherited PLL bit 24 is preserved.** Boot0 sets it on ten PLL words; the
  NPU sequencer keeps it (`a733_npu_startup.c:22`).
- **The A76 power-down DSB is present.** The vendor omits erratum 2743102;
  SunstoneOS follows upstream TF-A.
- **The CPU_ON register sequence matches** BL31 steps 1 to 8, with bounded
  polls (`platform/a733/cpu_start.c`).
- **Idle matches the vendor.** The vendor's core power-down suspend writes no
  CPUIDLE word, so its cores idle in WFI as SunstoneOS's do.
- **No CPU, GPU or NPU interconnect priority is reference-grounded.** The
  vendor device tree assigns priorities only to the display engine (2), the
  IOMMU ports (3), ISP and CSI (2), and the other media masters (1). The
  display and IOMMU part is the existing [memory-bus priority hook](../designs/dram-bus-priority.md),
  which no image links yet.
- **DRAM timing is the vendor's.** SunstoneOS runs boot0's own training, which
  selects parameter set 5 at 2040 MHz.
- **The NPU identity check stays strict.** The vendor selects its feature row
  by product only; SunstoneOS also requires revision `0x9202`, which both
  boards report.
- **The NPU completion event number** (vendor 4, SunstoneOS 0) is
  self-consistent in both.
- **Thermal management has no firmware reference.** Neither BL31 nor the SCP
  reads a thermal sensor; SunstoneOS's thermal policy stays its own.
- **System off and reset.** BL31 forwards both to the SCP and has no reset
  register of its own. SunstoneOS's watchdog reset path is unaffected.

## Interfaces SunstoneOS will need later

- **SCP message protocol.** Channel 3 of both message boxes, a header word and
  a count word, and types `0x11`, `0x22`, `0x24`–`0x26`, `0x60`–`0x62` and `0x64`
  ([`scp-messages.json`](data/scp-messages.json)). Needed only if SunstoneOS
  runs the vendor SCP for suspend or power-off.
- **E902 start.** BL31's nine-step loader: the device-tree address in
  `GP_DATA_REG3`, the image copy to SRAM A2, `RISCV_BGR` = `0x10003`, the start
  address, the CPUIDLE bit and the 24 MHz gate. `GP_DATA_REG3` does not overlap
  SunstoneOS's FEL flag in `GP_DATA_REG2`.
- **PMIC power-off and restart.** AXP8191 register `0x55` bits 7 and 6. Their
  meaning rests on log strings and needs a board check before use.
- **eFuse keys.** BL31 reads named keys (`chipid`, `npu_status`) through the
  SID read control at `0x03006040`. SunstoneOS already reads the speed-bin
  word directly. Programming is irreversible and out of scope.
- **Wake sources.** The SCP routes the RTC alarm, GPIO banks L and M, USB
  standby and IR receive. A SunstoneOS suspend design needs the same routing
  and must restore the system counter from the RTC, as the SCP does.

## Deferred work: replacing opaque firmware

`AGENTS.md` defers these until hardware support and performance tuning are
complete. Each candidate states what it would replace, how feasible that is
with the present references, and what it would gain.

### SCP firmware on the E902

The firmware performs system suspend and resume, power-off and reset through
the PMIC, wake-source routing, and DRAM self-refresh with PHY retention.
SunstoneOS uses none of these and has no system suspend.

- **Run the vendor blob unchanged.** Feasible: the start sequence and the
  message protocol are fully recorded. The blob conflicts with SunstoneOS
  owners of the CPUS bus clocks (it sets APBS0 to a source the PMIC provider
  refuses), S_TWI0, S_UART0, S_TIMER0/1 and the message boxes. It also polls
  without `wfi`, so it draws E902 power continuously. Gain: system suspend and
  PMIC power-off with RTC or GPIO wake. This is the cheapest path when a
  suspend requirement appears.
- **A small SunstoneOS E902 service** for power-off, restart and wake
  routing. Feasible: it needs S_TWI0, a message box and the wake interrupts,
  all recorded. Gain: it idles in `wfi`, and SunstoneOS decides which of those
  resources it owns. Its value depends on a feature that needs it.
- **A full suspend reimplementation.** Low feasibility. The DRAM controller
  re-initialisation and the three PHY register lists exist only in the blob,
  and the DRAM controller registers are undocumented. Gain: suspend power,
  unmeasured.

### Boot0's board setup and DRAM initialisation

SunstoneOS calls boot0's board setup and `init_DRAM` from its RAM wrapper.

- **Board setup** (CPU PLLs, PLL bit 24, DCDC2 voltage, a pad write): feasible
  to reimplement, since every write is mapped. The gain is small while
  `init_DRAM` still runs from the blob.
- **LPDDR5 training:** low feasibility. The controller and PHY are
  undocumented, boot0 carries three populated parameter sets and trains four
  P-states of the selected one, and no A733 DRAM source is published. Gain: no opaque code in the cold boot,
  and control of DRAM frequency scaling over the trained P-states (2040, 1165,
  816 and 408 MHz) for power. Neither gain is measured, and the boards work
  with the blob.

### NPU network compiler

SunstoneOS runs no vendor NPU code on the board; its loader, relocation and
command assembly are native. Models are compiled on the host by the vendor's
ACUITY 6.30.22 toolchain, a 7.9 GB x86 container
([NPU compiler](../npu-compiler.md)).

- **An open compiler.** Mesa's etnaviv NPU support generates neural-network
  descriptors for other Vivante cores, and SunstoneOS's assembler cites that
  layout for its descriptor fields. Support for this core (VIP9000NANODI_PLUS,
  product `0x1000003b`, eight neural cores, 512 KiB SRAM) is not established.
  An open compiler would need tiling for the SRAM, weight formats,
  quantisation and transposes, each checked against the vendor compiler's
  output. Feasibility: medium to low. Gain: reproducible model builds without
  the proprietary container, and room for tuning. The vendor tool is an
  adequate host development tool today, so `AGENTS.md` defers this until a
  stated need.

### GPU firmware

The GPU firmware's license forbids disassembly, so it cannot be analysed and
no replacement can be derived from it. SunstoneOS already uses Imagination's
redistributable firmware with the open driver's host interface. No
replacement candidate exists.

### Boot ROM and BL31

The boot ROM is mask ROM. SunstoneOS already runs its own EL3 monitor in place
of BL31; action 1 closes the largest remaining difference in cold-boot
writes.

## Open questions that limit these actions

- The meaning of the interconnect words at ports 29 to 35 (action 9).
- The layout of the CPUIDLE policy words and of CLUSTERPWRDN_EL1, whose DSU
  manual is not cached (action 15).
- Which peripherals sit behind SPC port 14 bit 10 and port 15 (action 1).
- Which of the three GPU vendor-state differences is necessary; the GPU
  vendor-state work owns this question.
- The MBUS writer and its condition (action 12).
