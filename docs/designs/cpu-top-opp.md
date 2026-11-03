# Design: CPU clusters and the DSU default to the vendor's top operating points

Status: **implemented**; the CPU clock driver and the clock-owner path for both clusters and the DSU are host-modeled. It is **qualified on the board** for the returning steps 1 to 6 on port 3-3 (CPU-B 2002 MHz, CPU-L 1794 MHz and the DSU 1274 MHz under the eight-core workload) and **open** for the integrated OS image at the top points (B1 and TOP).

Every hardware block defaults to the highest operating point that the vendor uses for the chip's speed bin, with the vendor's voltage; lower points are optional modes ([AGENTS.md](../../AGENTS.md), "Implementation"). This design derives the top points and the vendor's rate-change sequence from the references, states how the CPU clock driver ([cpu-clock.md](../cpu-clock.md)) follows that sequence, and records the board qualification. The records are in [the evidence file](../evidence/cpu-top-opp.jsonl). The OS image placement and owner are in [cpu-a76-dvfs.md](cpu-a76-dvfs.md); the run-time switch between top and low points is in [performance-modes.md](performance-modes.md).

The A733 has three CPU clock domains. CPU-L is the six Cortex-A55 cores, CPU-B the two Cortex-A76 cores, and the DSU (DynamIQ Shared Unit) holds the L3 cache and the snoop logic that all eight cores share. Each domain has its own PLL. Two outputs of the AXP8191 PMIC supply them: DCDC5 feeds CPU-L and the DSU, DCDC3 feeds CPU-B.

## References

All citations are files of the board kernel, linux-orangepi `2ac08e8c`, unless
another source is named. The pinned copies, their SHA-256 and their URLs are
in `~/.cache/sunstoneos/evidence/reference-backsolve-20261025/cpu/sources.json`;
`checks/cpu-clock-check.py` verifies each one it reads against the kernel's
Git tree.

- `arch/arm64/boot/dts/allwinner/sun60iw2p1-cpu-vf.dtsi` V0.9 (SHA-256
  `d5535630…`): the operating-point tables and the speed-bin map.
- `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` and
  `sun60i-a733-orangepi-zero3w.dts`: CPU nodes, the DSU node and the rails.
- `bsp/drivers/cpufreq/sun50i-cpufreq-nvmem.c`,
  `bsp/drivers/cpufreq/cpufreq-linux-6.6/cpufreq-dt.c` and `drivers/opp/core.c`:
  column selection and the order of a cluster change.
- `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2-cpupll.c`, `ccu_nkmp.c` and
  `ccu_common.c`: the PLL fields, the relock and the lock confirmation.
- `bsp/drivers/dsufreq/sunxi-dsufreq.c`: the DSU rate rule.
- `bsp/drivers/power/regulator/axp2101-regulator.c`: the AXP8191 selectors
  and the settle wait.
- The board kernel configuration, orangepi-build `a95129bf`
  `external/config/kernel/linux-sun60iw2-current-a733.config` (SHA-256
  `ae50f80a…`, cached at
  `~/.cache/sunstoneos/sources/board-kernel-config/`): which of those drivers
  the shipped kernel builds.
- boot0 (`boot0_sdcard_orangepizero3w.fex`, SHA-256 `829ea8e3…`), through the
  [CPU backsolve](../reference-backsolve-cpu.md#dvfs-raises-voltage-before-frequency):
  the only vendor code that parks a CPU mux.


## The vendor's top operating points

**Column.** The cpufreq driver reads the speed-bin code from the SID
(`sunxi_get_soc_dvfs`), maps it through `vf_mapping_table` and names the
column `26m-vf%04x` when the oscillator runs at 26 MHz
(`sun50i-cpufreq-nvmem.c:712`–`735`, the name at `:728`). The DSU driver uses
the same index and name (`sunxi-dsufreq.c:370`–`384`). Port 3-3 reads SID
word `0x0300624c` = `0x496`: code `0x04`, column `26m-vf0400`, and a 26 MHz
oscillator. A row whose voltage is 0 in the column is unusable, because the
OPP core marks any point outside the regulator's 500–1540 mV range
unavailable (`drivers/opp/core.c:1843`–`1866`, `:2009`–`2012`; ranges in
`sun60i-a733-orangepi-zero3w.dts:2132`–`2157`).

**Rails.** `cpu0` (CPU-L, `cluster0_opp_table`) takes `cpu-supply =
<&reg_dcdc5>`, the `dsufreq` node (DSU, `dsu_opp_table`) `dsu-supply =
<&reg_dcdc5>`, and `cpu6` (CPU-B, `cluster1_opp_table`) `cpu-supply =
<&reg_dcdc3>` (`sun60i-a733-orangepi-zero3w.dts:2919`–`2929`;
`sun60iw2p1.dtsi:136`–`137`, `:238`–`239`, `:519`–`524`). Every DCDC5 voltage
must therefore cover both CPU-L and the DSU.

**Bin vf0400 at 26 MHz** (`sun60iw2p1-cpu-vf.dtsi`; each row's line, then the
line of its `opp-microvolt-26m-vf0400` value):

| Domain, rail | Points in MHz at mV | Top point |
| --- | --- | --- |
| CPU-L, DCDC5 | 416, 780, 1014 and 1196 at 800; 1404 at 840; 1508 at 880; 1612 at 920; 1716 at 960; 1794 at 1000 | **1794 MHz at 1000 mV** (`:599`, `:615`) |
| CPU-B, DCDC3 | 416, 780, 1014, 1196 and 1404 at 800; 1508 at 820; 1612 at 840; 1716 at 870; 1794 at 920; 1898 at 970; 1950 at 1000; 2002 at 1050 | **2002 MHz at 1050 mV** (`:1340`, `:1356`) |
| DSU, DCDC5 | 312, 598, 780 and 910 at 800; 1066 at 840; 1144 at 880; 1222 at 920; 1248 at 960; 1274 at 1000 | **1274 MHz at 1000 mV** (`:1989`, `:2005`) |

The highest points at the 800 mV that boot0 leaves on both rails are CPU-L
1196 MHz (`:438`, `:454`), CPU-B 1404 MHz (`:1110`, `:1126`) and the DSU
910 MHz (`:1851`, `:1867`).

**Every bin.** The DSU column applies the vendor rule below with both
clusters at their top points.

| Column | CPU-L top | CPU-B top | DSU top | At 800 mV: CPU-L, CPU-B, DSU |
| --- | --- | --- | --- | --- |
| vf0000 | 1794 at 1000 | 1976 at 1050 | 1352 at 1000 | 1404, 1508, 1066 |
| vf0100 | 1612 at 1050 | 1794 at 1050 | 1222 at 1050 | 1092, 1300, 832 |
| vf0200 | 1794 at 1050 | 2002 at 1050 | 1274 at 1050 | 1300, 1508, 988 |
| vf0300 | 1404 at 860 | 1612 at 900 | 1066 at 860 | 1196, 1196, 780 |
| vf0400 | 1794 at 1000 | 2002 at 1050 | 1274 at 1000 | 1196, 1404, 910 |
| vf0500 | 1794 at 950 | 2002 at 1000 | 1274 at 950 | 1404, 1508, 1066 |
| vf0600 | 1404 at 840 | 1612 at 850 | 1066 at 840 | 1196, 1508, 910 |
| vf0401 | 1794 at 950 | 2002 at 1000 | 1274 at 950 | 1404, 1508, 1066 |
| vf0501 | 1794 at 900 | 2002 at 960 | 1274 at 900 | 1404, 1508, 1066 |

Code 0, which an unprogrammed fuse also reads, uses the driver's envelope
([cpu-clock.md](../cpu-clock.md)). Port 3-1 reads word `0x495`, also code `0x04`.

**What the shipped kernel runs.** The board kernel configuration builds the
cpufreq driver with the NVMEM bin selection (`CONFIG_AW_CPUFREQ_DT=y`,
`CONFIG_ARM_AW_SUN50I_CPUFREQ_NVMEM=y`, lines 965 and 972) under the
schedutil governor with no clamp (lines 1706 and 878), so both clusters reach
their top points under load. It leaves the DSU driver out
(`# CONFIG_AW_SUNXI_DSUFREQ is not set`, line 992), so the shipped kernel
keeps the DSU at the 494 MHz that the boot firmware leaves. The DSU's top
point comes from Allwinner's table and dsufreq driver; no kernel that Orange
Pi ships applies it. Because Allwinner's table uses it, it is the default.

## The vendor's rate-change sequence

**Cluster change.** `set_target` lowers the DSU before
`dev_pm_opp_set_rate` and raises it after (`cpufreq-dt.c:85`–`109`); this
applies only when dsufreq is built. The OPP core writes the regulator before
the clock when raising and after it when lowering
(`drivers/opp/core.c:1212`–`1258`).

**Voltage.** The AXP8191 DCDC3 and DCDC5 selectors are bits 6:0 of registers
`0x14` and `0x16`: 500 mV plus 10 mV per step up to `0x46`, then 20 mV per
step (`axp2101-regulator.c:1292`–`1305`, `:1341`–`1351`; u-boot `b791be84`
`pmu_axp8191.c:53`–`57`). The core writes the field only, so bit 7, which
reads set on port 3-3 (`0x9e`), is kept: 800 mV is `0x9e`, 1000 mV `0xb2`
and 1050 mV `0xb7`. After each change the driver waits
`DIV_ROUND_UP(|ΔµV|, 250)` µs rounded up to whole milliseconds
(`axp2101-regulator.c:383`–`409`, the rounding at `:407`–`408`), so every
change of up to 250 mV waits 1 ms.

**PLL relock.** The kernel changes N in place. It writes a larger post-divider
P before N and a smaller one after the update (`ccu_nkmp.c:293`–`311`), sets
the update bit 26 and polls it clear every 100 µs for up to 10 ms
(`:302`–`306`), then waits for lock (`:317`). With
`CONFIG_AW_STANDARD_CCU=y`, which the board configuration sets (line 49), the
lock wait reads the lock bit every 3 µs until three consecutive reads show
it, at most 100 reads, and then waits 20 µs (`ccu_common.c:47`–`64`); the
other build polls for up to 70 ms (`:68`). At probe the driver enables the
triangular pattern and linear frequency modulation in each CPU PLL
(`ccu-sun60iw2-cpupll.c:270`–`303`); no Linux kernel has run on our boards,
and bundle 14 read those enables clear on port 3-3.

**Mux.** The CPU nodes take the PLLs themselves as their clocks
(`sun60iw2p1.dtsi:136`, `:238`), and the probe selects each PLL in its mux
once (`ccu-sun60iw2-cpupll.c:306`–`308`), so the kernel never reparents a
running cluster. boot0, which runs on CPU0 in CPU-L, parks each CPU mux on
the oscillator before programming its PLL (boot0 `@0x21b8`, `@0x2098`,
`@0x2114`). It parks the mux and gates the PLL output, writes N and waits
20 µs, sets update and waits for it to clear and for three lock reads, waits
20 µs, ungates the output and waits 20 µs. It then selects each PLL, with
20 µs after each switch.

**DSU rule** (`sunxi-dsufreq.c`). The DSU target is three quarters of the
faster cluster's rate, rounded down to a multiple of 26 MHz (`:284`–`291`),
and at most the fastest DSU point whose voltage equals that of CPU-L's
operating point (`:197`–`228`, `:293`–`295`). When CPU-B runs at 1488 MHz or
more, the DSU stays at least at the fastest DSU point of the lowest voltage,
910 MHz in vf0400 (`:316`–`323`, `:490`–`496`). The DSU is lowered before a
cluster change and raised after it (`:298`–`368`), and only its clock is set:
DCDC5 follows CPU-L's operating point (`:178`–`195`). With both clusters at
their vf0400 top points the rule gives min(1482, 1274) = 1274 MHz.

**Thermal.** The kernel's CPU zones are passive at 70 °C and 90 °C and
critical at 110 °C (`sun60iw2p1.dtsi:528`–`595`). The SunstoneOS board policy
throttles at 90 °C and stops at 110 °C ([thermal.md](../thermal.md)).
## The driver and the vendor sequence

| Step | Vendor | Driver |
| --- | --- | --- |
| Column | `26m-vf%04x` from the SID code | Same |
| Voltage order | Before the clock when raising, after it when lowering | Same |
| Voltage settle | \|ΔV\| ÷ 250 µV/µs, rounded up to whole ms | Same |
| Lock | Three reads 3 µs apart, then 20 µs | Same |
| PLL change | N changed in place with modulation (kernel); mux parked on the oscillator (boot0) | Parked; the output stays on, as the kernel leaves it |
| Mux settle | 20 µs after each switch (boot0) | 20 µs |
| P order | P before or after N by direction | P written while parked; the order has no effect while the domain runs from the oscillator |
| Modulation enables | Set at probe | Refused when set |
| DSU | Rule above; clock only | Writable under the rule's cap and CPU-L's voltage ceiling; no voltage write |
| Thermal | Passive cooling from 70 °C | THROTTLE: clusters to 1014 MHz; DSU requests held to 780 MHz |

Parking is the one divergence. The kernel never parks a running cluster; it relies on modulation, under which N alone does not define the rate, so the driver keeps boot0's park. boot0 parks CPU-L while CPU0 executes, the only vendor precedent for parking a running cluster. The kernel's DSU floor for a fast CPU-B and its automatic DSU lowering are left to the owner: the driver changes the DSU only on request and checks the three-quarters cap only on a DSU request, so the owner lowers the DSU before a cluster. The vendor raises the DSU to 910 MHz, the fastest DSU point at 800 mV, just before CPU-B reaches 1488 MHz or more; the driver's cap admits 910 MHz only once CPU-B runs above 1214 MHz, so the owner raises the DSU right after CPU-B.

Host budgets (`checks/cpu-clock-model.c`, `BUDGET` lines): the driver adds waits and no accesses. A frequency change takes 5 clock register writes; the default path on port 3-3 takes 15 clock writes and 2 PMIC writes; the parked time of a raise with a 20 µs model lock is 69 µs. See `cpu-top-opp-host-budgets`.

## The default path

An image that owns the CPU clocks raises each domain to its top point at start, in the vendor order:

1. DCDC3 to CPU-B's top voltage, 1 ms, then CPU-B to its top point.
2. DCDC5 to CPU-L's top voltage, 1 ms, then CPU-L to its top point.
3. The DSU to the rule's top point (`a733_cpu_clock_dsu_top`), with no voltage write.

On port 3-3 that is DCDC3 `0xb7` and CPU-B 2002 MHz (N = 77), DCDC5 `0xb2` and CPU-L 1794 MHz (N = 69), and the DSU at 1274 MHz (N = 49).

Lower points are optional modes. THROTTLE (thermal) holds the clusters at 1014 MHz and DSU requests at 780 MHz or below, both 800 mV in every bin; CRITICAL ends the session. Power saving requests any row of the bin's column; a lowering follows the vendor order: the DSU first, then the cluster, then its voltage.

The OS image's clock owner (`apps/os/cpu.c`) requests CPU-B, then CPU-L, each once its own sensor has a reading, and, for a grant of both clusters only, then the DSU at the point that `a733_cpu_clock_dsu_top` derives from the rates that CPU-L and CPU-B reached. A one-cluster grant leaves the DSU at its boot point. `apps/os/admission.c` accepts a grant of both clusters. Each grant is bound to one session; a standing per-board record for the top points, which the resident lifetime also needs, is not implemented.

## Invariants for CPU-B across a ROM return

The FEL boot ROM programs the CPU-L and DSU PLLs (N = 38) and selects their muxes, and never writes CPU-B: its literal pool holds `0x08871000` and `0x08873000`, not `0x08872000` (boot ROM image `a8e31821…`; [boot ROM and boot0](../vendor-firmware/boot-rom-and-boot0.md)). CPU-B's boot word `0xf8802700` (N = 39, 1014 MHz) comes from boot0's board setup, which the cold DDR start runs once per lifetime. After a return, CPU-L and the DSU read their boot words, CPU-B keeps whatever the last image set, and the rails keep their voltages. The rule that follows:

- Every image that raises CPU-B lowers it to 1014 MHz before a successful return. The workload's relock raises CPU-B; on the completed path, after all seven secondaries are OFF, CPU0 lowers it at EL3 with the relock's own sequence (`top_opp_lower_b` in `boot/a733/top-opp-relock.c`).
- Only a failed run can return with CPU-B raised (a deadline return after the relock, a completed workload whose lowering refused or failed, or a step image that fails between raising and lowering). DCDC3 is then at least at the raised point's voltage, because every raise writes the voltage first, and each such outcome ends the lifetime with a power removal.
- Paths that lower DCDC3 or assume CPU-B at 1014 MHz refuse a raised CPU-B. The driver lowers DCDC3 only after lowering CPU-B in the same transition and never below the rail's first reading in the image. Step images admit only CPU-B at 1014 MHz and hold its clock lease during a direct DCDC3 write. The board bundles refuse any clock word other than the boot words. The integrated OS image refuses to start unless CPU-B reads 1014 MHz (`os_cpu_admit`). The workload's relock reads DCDC3 and refuses unless it is at 1050 mV, and its lowering refuses unless CPU-B reads the relock's word.
- The performance-mode toggle follows the same rule: an image may raise CPU-B for its own run and lowers it before any ROM return. The integrated OS image ends at a terminal halt and a power removal, so it needs no lowering.
- The rail-return helper (board bundle 33) lowers DCDC3 with CPU-B held, then DCDC5 with CPU-L and the DSU held, after checking the boot clock words; a rail already at 800 mV gets no write. `tools/dev-run.py` runs it before the launch and after a normal return for an image that links the CPU-return entry guard (`checks/cpu-rails-return.c`, `checks/os-cpu-return-rails.py`).

## The returning workload relocks its own clocks

Every launch starts from the ROM, which resets CPU-L and the DSU, so the workload sets its clocks itself. Before it releases any secondary, CPU0 at EL3 relocks CPU-B, CPU-L and the DSU to their top points (`boot/a733/top-opp-relock.c`), following the driver's sequence. Before its first clock write it checks the board itself: the DCXO enabled and stable at 26 MHz, speed-bin code `0x04`, every domain on its locked PLL with modulation off. It also checks DCDC3 at 1050 mV and DCDC5 at 1000 mV, read from the AXP8191 over S_TWI0 with the PMIC provider's bus checks and phase order. Any difference ends the run with a refusal code through the deadline unit before any secondary starts. The relock and lowering records sit in the identity header (`0x42018180` and `0x42018160`) and the runner's decoder refuses a completed run without the lowering record. The relock and its tables fit in the gap of the overlay's 16 KiB reservation. `tools/top-opp-workload-bind.py` binds the prepared bundle to the current survey's instance chain and refuses port 3-1 until its supplies are read.

## Board qualification

Each returning step is a CPU0 image on the provider path of bundle 15 ([board-bundles-next.md](board-bundles-next.md)): it reads two sets of the five temperature sensors around its change, and inspects the CPU domains read-only. It refuses before any write unless the board matches the record, stops and lowers at 70 °C, and returns to the ROM. Every step passed on port 3-3 on 2026-10-25.

| Step | Change | Result | Record |
| --- | --- | --- | --- |
| 1 | CPU-B 1014 → 1404 → 1014 MHz at 800 mV, cores 6 and 7 off | Longest park 59 µs; DCDC3 stays 800 mV; 39.7 °C | `cpu-top-opp-step1` |
| 2 to 5 | CPU-L 494 → 1196 MHz and the DSU to 780 MHz; DCDC3 800 ↔ 1050 mV; CPU-B to 2002 MHz with the DSU at 910 then 598 MHz; DCDC5 1000 mV, CPU-L 1794 MHz and the DSU 1274 MHz | Every clock word as predicted; hottest sensor 42 °C | `cpu-top-opp-steps2-5` |
| 6 | All-core workload at CPU-B 2002, CPU-L 1794, DSU 1274 MHz | 32 rounds on every core; relock parks 55, 70 and 69 µs; CPU-B lowered in 54 µs; 37.1 °C | `cpu-top-opp-step6-run` |
| 6 | Background-task progress at the top points | 3.85 times on the A55 cores, 2.06 times on the A76 cores against the same image at the boot rates | `cpu-top-opp-step6-background` |

Measured at the top points against the boot rates, a CPU-bound loop runs about 3.85 times faster on the A55 cores and 2.06 times faster on the A76 cores. The A76 core runs 1.10 to 1.12 times as many iterations per cycle as an A55 core. The shipped Orange Pi kernel leaves the DSU driver out and keeps the DSU at 494 MHz, so the DSU's top point comes from Allwinner's table and driver alone.

## Limitations

- The integrated OS image has not run at the top points; recognition time at the top points (about 1.20 s predicted against about 2.10 s at the boot rates) is unmeasured.
- No source proves parking a running cluster's mux on the oscillator beyond boot0's precedent; the board steps are the evidence.
- Restoration of the rails after a fatal or deadline outcome has no board run. The qualified steps ran on port 3-3; port 3-1 reads speed-bin word `0x495` (code 4), as [performance-modes.md](performance-modes.md) records.
- The ROM return with CPU-L above 494 MHz answered on port 3-3 after steps 2 and 5; no other board has been tried. After step 5 the ROM idles with CPU-L at 1794 MHz and the DSU at 1274 MHz and DCDC5 at 1000 mV, so power removal follows promptly.

## Open work

1. **B1, then TOP** on the integrated OS image ([cpu-a76-dvfs.md](cpu-a76-dvfs.md), prediction in `cpu-top-opp-step7-prediction`). Needs rank 5, the refrozen rank 7 and rank 8 bundles, a reviewed physical entry record, templates with the CPU trip set to 70000, a producer of the B1 record, and a power cycle (the boards cannot be power-cycled until further notice).
2. A standing per-board approval record for the top points, needed by the resident lifetime.
3. Board run of the rail-return helper after a fatal or deadline outcome.
