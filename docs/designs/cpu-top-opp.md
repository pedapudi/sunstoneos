# Design: CPU clusters and the DSU default to the vendor's top operating points

Status: **driver changes implemented and host-modeled. Steps 1 to 5 passed
on port 3-3 on 2026-10-25, and step 6, all eight cores at the top points,
passed there the same day (`board-session-cpu-top-opp-10`); its first run
had stopped as designed at bundle 26, which found CPU-B not reset by the ROM
return. Step
7's grant of both clusters is implemented and host-checked; it cannot be
frozen until the integrated OS image has a reviewed physical entry record
and B1 has passed (below).** Every hardware block defaults to the highest
operating point that the vendor uses for the chip's speed bin, with the
vendor's voltage; lower points are optional modes ([AGENTS.md](../../AGENTS.md),
"Implementation"). The user approved CPU clock and voltage changes on
2026-10-24 ([handoff](../handoff.md)). This design derives the top points and
the vendor's rate-change sequence from the references, records how the CPU
clock driver ([cpu-clock.md](../cpu-clock.md)) now follows that sequence, and
orders the board steps that reach the top points on port 3-3.

The A733 has three CPU clock domains. CPU-L is the six Cortex-A55 cores,
CPU-B the two Cortex-A76 cores, and the DSU (DynamIQ Shared Unit) holds the
L3 cache and the snoop logic that all eight cores share. Each domain has its
own PLL. Two outputs of the AXP8191 PMIC supply them: DCDC5 feeds CPU-L and
the DSU, DCDC3 feeds CPU-B.

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
([cpu-clock.md](../cpu-clock.md)). Port 3-1's code has not been read.

**What the shipped kernel runs.** The board kernel configuration builds the
cpufreq driver with the NVMEM bin selection (`CONFIG_AW_CPUFREQ_DT=y`,
`CONFIG_ARM_AW_SUN50I_CPUFREQ_NVMEM=y`, lines 965 and 972) under the
schedutil governor with no clamp (lines 1706 and 878), so both clusters reach
their top points under load. It leaves the DSU driver out
(`# CONFIG_AW_SUNXI_DSUFREQ is not set`, line 992), so the shipped kernel
keeps the DSU at the 494 MHz that the boot firmware leaves. The DSU's top
point comes from Allwinner's table and dsufreq driver; no kernel that Orange
Pi ships applies it. Under the user's rule ("if allwinner uses even higher
clocks, use that"), it is still the default, and its step runs last.

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

## The driver compared with that sequence

| Step | Vendor | Driver before this change | Driver now |
| --- | --- | --- | --- |
| Column | `26m-vf%04x` from the SID code | Same | Same |
| Voltage order | Before the clock when raising, after it when lowering | Same | Same |
| Voltage settle | \|ΔV\| ÷ 250 µV/µs, rounded up to whole ms | Unrounded: 200 µs for 50 mV | Rounded up to whole ms (action plan rank 13) |
| Lock | Three reads 3 µs apart, then 20 µs | First read that shows lock | Three reads at least 3 µs apart, then 20 µs |
| PLL change | N changed in place with modulation (kernel); mux parked on the oscillator (boot0) | Parked, output left on | Same; the output stays on, as the kernel leaves it |
| Mux settle | 20 µs after each switch (boot0) | 2 µs | 20 µs |
| P order | P before or after N by direction | P written while parked | Same; the order has no effect while the domain runs from the oscillator |
| Modulation enables | Set at probe | Refused when set | Same |
| DSU | Rule above; clock only | Never written | Writable: the rule's cap, the CPU-L voltage ceiling, no voltage write |
| Thermal | Passive cooling from 70 °C | THROTTLE: clusters to 1014 MHz | Same; DSU requests held to 780 MHz under THROTTLE |

The divergence that remains is parking. The kernel never parks a running
cluster; it relies on modulation, under which N alone does not define the
rate, so SunstoneOS keeps boot0's park instead. boot0 parks CPU-L while CPU0
executes, which is the only vendor precedent for parking a running cluster.
The kernel's DSU floor for a fast CPU-B and its automatic DSU lowering are
left to the owner: the driver changes the DSU only on request, and it checks
the three-quarters cap only on a DSU request, so the owner lowers the DSU
before a cluster. Step 4 below applies the floor: the vendor raises the DSU
to 910 MHz, the fastest DSU point at 800 mV, just before CPU-B reaches
1488 MHz or more; the driver's cap admits 910 MHz only once CPU-B runs above
1214 MHz, so step 4 raises the DSU right after CPU-B.

## The default path

Once the board steps below pass on a board, an image that owns the CPU
clocks raises each domain to its top point at start, in the vendor order:

1. DCDC3 to CPU-B's top voltage, 1 ms, then CPU-B to its top point.
2. DCDC5 to CPU-L's top voltage, 1 ms, then CPU-L to its top point.
3. The DSU to the rule's top point, with no voltage write.

On port 3-3 that is DCDC3 `0xb7` and CPU-B 2002 MHz (N = 77), DCDC5 `0xb2`
and CPU-L 1794 MHz (N = 69), and the DSU at 1274 MHz (N = 49). The driver
already holds the order and the checks: the model runs these three requests
on the recorded port 3-3 state with 15 clock writes and 2 PMIC writes.

Lower points are optional modes:

- **THROTTLE** (thermal): clusters at 1014 MHz and DSU requests at 780 MHz or
  below, both 800 mV in every bin; CRITICAL ends the session.
- **Power saving**: any row of the bin's column, requested explicitly. A
  lowering follows the vendor order: the DSU first, then the cluster, then
  its voltage.

The integrated OS image carries the default path as approval step `TOP`:

- `apps/os/prepare.py` admits a `TOP` approval only with both clusters at
  the top rows of the board's bin column, as B1 recorded it; it still
  refuses both clusters for steps A1 to A3.
- `apps/os/admission.c` accepts a grant of both clusters.
- The clock owner (`apps/os/cpu.c`) requests CPU-B, then CPU-L, each once its
  own sensor has a reading, and, for a grant of both clusters only, then the
  DSU at the point that `a733_cpu_clock_dsu_top` derives by the rule above
  from the rates that CPU-L and CPU-B reached. A one-cluster grant leaves the
  DSU at its boot point.

One change remains: each grant needs an approval record for one session. The
default needs a standing record per board for its top points, which the
resident lifetime also needs.

## Prerequisites that remain from the A76 design

- **B1 on port 3-3.** The supervised A76 OS image with a grant of no cluster
  has not run. It needs, in one power cycle, ranks 1 to 5 and 9, the rank 7
  and rank 8 bundles refrozen with the status-masked comparison that passed
  ten times on 2026-10-25, the supervised OS image built with
  `A733_A76_ADMITTED` and `SUNSTONE_A733_OS_CPU_CLOCK`, and its record of
  schema `a733-os-cpu-clock-b1-v1`. B1 gates the OS steps A1 to A3 and the
  default path; the returning steps 1 to 5 below power no A76 core and run
  no recognizer, so they do not need it.
- **A bound approval record per run.** OS steps need schema
  `a733-os-cpu-clock-approval-v1`. A returning step carries schema
  `cpu-operating-point-approval-v1` in its bundle: the row, the port, the
  image's SHA-256, the user's approval text and date, the domains, points,
  selectors and the expected board state. `tools/prepare-board-bundles.py`
  refuses a row that names an approval schema without such a record.

## Board steps on port 3-3

Each step is a returning CPU0 image on the provider path of bundle 15
([board-bundles-next.md](board-bundles-next.md)). It reads one set of the
five temperature sensors and inspects the CPU domains read-only. It then
makes the step's change, inspects again, reads a second sensor set and
returns to the ROM. Each step's admission refuses, before any write, a board whose bin,
supplies, rates or modulation enables differ from the record, or whose plan
would take a rail above the bin's top voltage for that rail (DCDC5 1000 mV
and DCDC3 1050 mV in vf0400). The steps change one variable at a time.

| Step | Change | Prediction |
| --- | --- | --- |
| 1 | CPU-B 1014 → 1404 → 1014 MHz at 800 mV, cores 6 and 7 off (bundle 20) | CPU-B PLL `0xf8803600`, then `0xf8802700`; mux `0x03000000` throughout; DCDC3 800 mV before and after; parked under 1 ms per change; CPU-L and DSU words unchanged |
| 2 | CPU-L 494 → 1196 MHz at 800 mV, then the DSU 494 → 780 MHz | CPU-L PLL `0xf8802e00`, mux `0x03000000`; CPU0's cycle counter measures 1196 MHz ± 1% (494 MHz before); DSU PLL `0xf8801e00`, mux `0x03000105`; no PMIC write |
| 3 | DCDC3 800 → 1050 mV and back, CPU-B held at 1014 MHz | DCDC3 selector `0xb7`, then `0x9e`; every clock word unchanged; temperatures within 1 °C |
| 4 | DCDC3 to 1050 mV, CPU-B 1014 → 2002 MHz, then the DSU to 910 MHz; then the DSU to 598 MHz, CPU-B to 1014 MHz and DCDC3 to 800 mV, cores 6 and 7 off; the DSU stays at 598 MHz | CPU-B PLL `0xf8804d00`, DCDC3 `0xb7`, DSU PLL `0xf8802300`, mux `0x03000105`; then DSU PLL `0xf8802e00`, mux `0x03010105`, CPU-B `0xf8802700`, DCDC3 `0x9e` |
| 5 | DCDC5 to 1000 mV, CPU-L → 1794 MHz, then the DSU → 1274 MHz | DCDC5 1000 mV (`0xb2`); CPU-L PLL `0xf8804500`, CPU0 measures 1794 MHz ± 1%; DSU PLL `0xf8803100`, mux `0x03000105`; the hottest sensor below 50 °C |
| 6 | The all-core returning workload with CPU-B at 2002 MHz, CPU-L at 1794 MHz and the DSU at 1274 MHz, set by the workload's own CPU0 before any secondary starts | 32 rounds on every core, 32 ticks on CPU0 and 33 SGIs on each secondary, as at the boot rates; background progress about 3.6 times the boot-rate count on cores 0 to 5 and about 2 times on cores 6 and 7; the relock record reads the three top-point PLL words; every sensor below 70 °C before and after |
| 7 | B1, then the OS image with both clusters and the DSU at their top points | Recognition on the A76 pair at about 2.10 s in B1 and about 1.20 s at the top points for the bench recording ([cpu-a76-dvfs.md](cpu-a76-dvfs.md), A2) |

Step 1 changes nothing that a running core uses, so it tests the park,
relock and lock rule on real hardware at the lowest risk. Step 2 is the first
park of the cluster that runs the driver itself, as boot0 parks it. Step 3
is the first DCDC3 selector write; the PMIC selector-write path has run once,
when the GPU vendor-state probe raised DCDC4 on port 3-1 and read `0xae` back. Steps 4 and 5 reach the top points. A
non-return in step 2 or 5 leaves CPU0 on the oscillator or on an unproven
PLL, and only a power removal recovers it.

Step 1 passed on port 3-3 (`board-session-20261025-17/port-3-3`, record
`c1-opp1-run`): CPU-B read `0xf8803600` at 1404 MHz and `0xf8802700` back at
1014 MHz, the longest park was 59 µs, DCDC3 stayed at 800 mV and the hottest
sensor read 39.7 °C before and 39.6 °C after.

Steps 2 to 5 are bundles 22, 21, 23 and 24, built from the same source as
step 1 with `-DOPP_BUNDLE=n` (below). They passed on port 3-3 in one lifetime
(`board-session-20261025-19/port-3-3`), every clock word as predicted and the
hottest sensor at 42 °C. Every image's `before` lines showed CPU-L and the
DSU at 494 MHz and CPU-B at 1014 MHz. The ROM return resets CPU-L and the
DSU; CPU-B read 1014 MHz only because steps 1 and 4 had lowered it again
themselves (next section).

## Steps 2 to 5 share one lifetime

The four steps run in one port 3-3 lifetime, in the order of their recovery
risk, each returning to the ROM before the next starts:

1. Step 3 (bundle 21): DCDC3 to 1050 mV and back. Only a rail of the
   powered-off cluster changes.
2. Step 2 (bundle 22): the first park of the cluster that runs the image, at
   800 mV.
3. Step 4 (bundle 23): CPU-B at its top point and the DSU at 910 MHz, then
   CPU-B and DCDC3 back to their entry values.
4. Step 5 (bundle 24): DCDC5 and CPU-L to their top points and the DSU to
   1274 MHz.

They can share a lifetime because each one returns with its rails back at
800 mV, except step 5, which runs last, and each admits the state the
earlier ones leave. Every step requires DCDC3 and DCDC5 at 800 mV and CPU-B
at 1014 MHz, and admits CPU-L and the DSU at any rate on their PLLs. A step
that does not return ends the lifetime, and the later steps then need a new
one; they would also have been meaningless after that failure. The order puts the
voltage-only step first and the step that leaves the board at its top
points last. The ROM return is not known to keep the CPU PLLs: each step's
`before` lines record what the previous step left.

Each step reads a sensor set before any write and after each raise. The
thermal stop is 70 °C, the board kernel's first passive trip of both CPU
zones (`sun60iw2p1.dtsi:535`, `:568`). A step that finds a sensor at or above it
before any write returns REFUSED. After a write it lowers what it raised (the DSU to
598 MHz, then the clusters to 1014 MHz, then DCDC3 to 800 mV) and returns
STOPPED; both are controlled outcomes. The driver's own THROTTLE level
stays at 90 °C. CPU0's rate is measured from its cycle counter against
CNTPCT over 1 ms, with the PMU code that bundle 4 ran on port 3-3, before
and after each CPU-L change. The cycle counter counts core clocks during
memory stalls too, so no step depends on the memory bus clock, which the
clock audit finds at 400 MHz after each ROM return.

In step 4 the DSU goes to 910 MHz, the rate at which the vendor's dsufreq
holds the DSU while CPU-B runs at 1488 MHz or more; the vendor sets it just
before the CPU-B change, and the driver's three-quarters cap admits it only
once CPU-B runs above 1214 MHz, so the step sets it right after. The DSU then
goes to 598 MHz before CPU-B is lowered, in the vendor order: 598 MHz is the
fastest 800 mV DSU row at or below three quarters of 1014 MHz (754 MHz), so
the cap admits it whether CPU-L runs at 494 or 1196 MHz.

**A ROM return with CPU-L above 494 MHz has never run.** It first happens
after bundle 22, with CPU-L at 1196 MHz, and again after bundle 24, at
1794 MHz. The prediction is that the ROM answers both times: the next
survey's responsive-ROM check and the Read bundle after bundle 24 show it. If
the ROM times its USB or FEL delays by counting CPU cycles, those delays
shrink by 494/1196 and 494/1794, and a return can then fail to enumerate; a
missing FEL instance after bundle 22 or 24 is that failure, and only a power
removal recovers it. After bundle 24 the ROM idles with CPU-L at 1794 MHz,
the DSU at 1274 MHz and DCDC5 at 1000 mV, and nothing reads the sensors, so
the runbook removes power promptly after the Read bundle that follows it.

Step 3 writes DCDC3 through the PMIC provider itself, because the driver
changes a voltage only together with a clock. It holds CPU-B's clock lease
exclusively, so CPU-B stays at 1014 MHz, which both voltages cover, and an
exclusive DCDC3 lease. The provider requires the inherited selector `0x1e`
before the write, keeps bit 7 and reads the byte back.

The frozen set is
`~/.cache/sunstoneos/evidence/cpu-top-opp-20261025/steps2-5-set-r2/`: power
removal, cold DDR start, the base image, then bundles 21, 22, 23 and 24, each
with its approval record, then the Read bundle 14 as the host-side capture.

The host model runs the three driver steps in sequence with the bundles'
grants (`lifetime_cases` in `checks/cpu-clock-model.c`), followed by a
thermal stop of step 5 that returns DCDC5 to 800 mV.

## The FEL ROM return resets CPU-L and the DSU, not CPU-B

The first run of step 6 (`board-session-cpu-top-opp-7/port-3-3`, set
`step6-set-2`) showed it. Bundle 25 left CPU-B at 2002 MHz, CPU-L at
1794 MHz and the DSU at 1274 MHz, with DCDC3 at 1050 mV and DCDC5 at
1000 mV. After its ROM return, bundle 26 read CPU-L and the DSU back at
`0xf8802600` (494 MHz) but CPU-B still at `0xf8804d00` (2002 MHz), and
refused with `boot clock words`; the Read bundle 14 that followed read the
same CPU-B word. The rails kept their voltages.

The boot ROM explains it. Its clock setup programs two PLLs from its
literal pool: `0x08871000` (CPU-L) and `0x08873000` (DSU), through the
routine at `0x1898` that calls the PLL writer at `0x1774` once for each
with N = 38, then selects both muxes at `0x1af0`. The pool holds no
`0x08872000`, and no routine writes CPU-B (`a8e31821…`, literals at
`0x1f78`–`0x1fa0`; [boot ROM and boot0](../vendor-firmware/boot-rom-and-boot0.md)).
CPU-B's boot word `0xf8802700` (N = 39, 1014 MHz) comes from boot0's board
setup, which the cold DDR start runs once per lifetime. Every earlier
lifetime had lowered CPU-B to 1014 MHz before its return (steps 1 and 4),
so the ROM never had a raised CPU-B to keep.

CPU-B at 2002 MHz across a return is safe only while DCDC3 stays at
1050 mV. Two fixes were weighed:

- **Accept a raised CPU-B after a return** (bundle 26 and the relock accept
  the top word with the rail at 1050 mV). The raised state would then
  persist through every later image of the lifetime, and each of them would
  need to know it: an image that lowers DCDC3, B1's baseline, which is
  defined at boot0's rate, and the step images, which admit only CPU-B at
  1014 MHz.
- **Never return with CPU-B raised** (chosen). Every image that raises
  CPU-B lowers it to 1014 MHz before a successful return, so the state that
  boot0 leaves holds at every such ROM return, as every other image
  assumes. Bundle 25 does not change CPU-B: it raises DCDC5 through CPU-L's
  top point and DCDC3 with CPU-B held at 1014 MHz, the change step 3
  qualified. The workload's relock raises CPU-B, and on the completed path,
  after all seven secondaries are OFF, CPU0 lowers CPU-B to 1014 MHz
  (N = 39) at EL3 with the relock's own sequence before the return
  (`top_opp_lower_b` in `boot/a733/top-opp-relock.c`). Bundle 27 then only
  reads the boot words. Steps 1 and 4 lowered CPU-B themselves, as before.

Only a failed run can return with CPU-B raised: a deadline return of the
workload after its relock, a completed workload whose lowering refused or
failed (the run then reports a failure), or a step image that fails between
raising and lowering CPU-B. DCDC3 is then at least at the raised point's
voltage, because every raise writes the voltage first, and each such
outcome ends the lifetime with a power removal before any other image runs.
The paths that could lower DCDC3, or that assume CPU-B at 1014 MHz, refuse
a raised CPU-B:

- the CPU clock driver lowers DCDC3 only after lowering CPU-B in the same
  transition and never below the rail's first reading in the image; with
  CPU-B inherited at 2002 MHz it lowers CPU-B first with no PMIC write
  (`checks/cpu-clock-model.c`, inherited CPU-B cases);
- every step image admits CPU-B only at 1014 MHz, and its direct DCDC3
  write (`dcdc3_change`) runs with the CPU-B clock lease held and requires
  that admitted rate; no plan with a DCDC3 write changes CPU-B;
- bundles 26 and 27 refuse any clock word other than the boot words;
- the integrated OS image refuses to start unless CPU-B reads boot0's
  1014 MHz (`os_cpu_admit` in `apps/os/cpu.c`), so B1 and steps A1 to A3
  and `TOP` start from boot0's point;
- the workload's relock reads DCDC3 itself and refuses unless it is at
  1050 mV, and its lowering refuses unless CPU-B reads the relock's word.

The performance-mode toggle of the default path follows the same rule: an
image may raise CPU-B for its own run, and lowers it before any ROM return.
The integrated OS image ends at a terminal halt and a power removal, so it
needs no lowering.

## Step 6: the workload relocks its own clocks

Every launch starts from the ROM, which resets CPU-L and the DSU on each
return and leaves CPU-B as the last image set it. A setter image cannot
hand CPU-L and the DSU to the workload with no ROM return in between,
because the host loads every image through the ROM. The workload's own
launch must therefore set the clocks. The rails are expected to differ:
neither ROM description lists a PMIC access
([boot ROM and boot0](../vendor-firmware/boot-rom-and-boot0.md)), so a voltage
that an earlier image writes should survive the return; the first run of
step 6 confirmed it for DCDC3 and DCDC5. Step 6 still relies on it only
after bundle 26 has read it on the board:

1. **Bundle 25** raises DCDC5 to 1000 mV and CPU-L to 1794 MHz, then the
   DSU to 1274 MHz, through the driver, then DCDC3 to 1050 mV with CPU-B
   held at 1014 MHz, and leaves the rails. Its ROM return resets CPU-L and
   the DSU.
2. **Bundle 26** reads, with no write, that DCDC5 is still at 1000 mV and
   DCDC3 at 1050 mV and that the three domains read the boot PLL and mux
   words that bundle 14 recorded (CPU-L `0xf8802600`/`0x03010000`, CPU-B
   `0xf8802700`/`0x03000000`, DSU `0xf8802600`/`0x03010105`). It refuses,
   and the lifetime ends before the workload, if a rail or a word differs.
3. **The workload** (`bundle-deadline-top-5`) is the returning deadline
   profile on the refrozen native image `6fd7af86` with one change: before
   it releases any secondary, CPU0 at EL3 relocks CPU-B to 2002 MHz, CPU-L
   to 1794 MHz and the DSU to 1274 MHz (`boot/a733/top-opp-relock.c`). The
   relock follows the driver's sequence. Before its first clock write it
   checks the board itself: the DCXO enabled and stable at 26 MHz, speed-bin
   code `0x04`, every domain on its locked PLL with modulation off, and
   DCDC3 at 1050 mV and DCDC5 at 1000 mV, which it reads from the AXP8191
   over S_TWI0 with the PMIC provider's bus checks and phase order (page 0,
   chip 3 and revision 0 before the selectors). Any difference ends the run
   with a refusal code through the deadline unit before any secondary
   starts. It records the entry and final words, the parked time, the lock
   reads, the oscillator and SID words and the seven PMIC bytes in the
   identity header at `0x42018180`. The overlay's 16 KiB reservation had
   288 bytes left, so the 2,647-byte relock and its tables sit in the
   3.5 KiB gap between the 512-byte base overlay and the extension.
4. **The workload's completed path**, after all seven secondaries are OFF,
   lowers CPU-B to 1014 MHz with the same sequence and records it in the
   identity header at `0x42018160` (result, entry and final words, parked
   time and lock reads); the runner's decoder refuses a completed run
   without it.
5. **Bundle 27** reads, with no write, the temperatures, the rails and the
   boot words of all three domains, and refuses on any other word. It runs
   only after the workload completed with DRAM restored; every other
   outcome of the workload ends the lifetime with a power removal.

The workload reads no temperature itself: it lasts about 32 ticks, shorter
than one conversion period of the temperature sensor, about 100 ms. The
sensor sets of bundles 25, 26 and 27 bracket it in the same lifetime, and
the 70 °C thermal stop applies in each of them. The relock and the workload
do not use the memory bus clock; the workload's own clocks are the three it
sets.

Predictions, against the ten boot-rate runs on port 3-3 of 2026-10-25
(`board-session-20261025-3/port-3-3`, `tw1` to `tw10`):

| Quantity | Boot rates | Top points |
| --- | --- | --- |
| Rounds per core, ticks on CPU0, SGIs per secondary | 32, 32, 33 | Same: the rounds wait on the tick |
| Time per round | One tick | One tick |
| Background progress, cores 0 to 5 | 82,600 to 88,200 (run 1: 187,600 to 196,700) | About 3.6 times, 300,000 to 320,000, less where memory bounds the loop |
| Background progress, cores 6 and 7 | 130,100 to 137,300 | About 2 times, 255,000 to 270,000 |
| Relock record | — | Result 1; final words `0xf8804d00`/`0x03000000`, `0xf8804500`/`0x03000000`, `0xf8803100`/`0x03000105`; at least three lock reads per domain; parked about 60 µs each |
| Hottest sensor, bundles 25 to 27 | 42 °C in step 5; 37 °C in bundle 25 of the first step 6 run | Below 50 °C |
| CPU-B lowering record | — | Result 1; entry `0xf8804d00`/`0x03000000`, final `0xf8802700`/`0x03000000`; at least three lock reads; parked about 60 µs |
| Bundle 27 | — | Boot words in all three domains, DCDC5 1000 mV and DCDC3 1050 mV |

Board 3-3 has run neither secondary at a top point. A non-return of the
workload ends the lifetime with the A76 cores possibly running at 2002 MHz
and CPU0 at 1794 MHz; only a power removal recovers it. A deadline return
resets CPU0 into the ROM, which resets CPU-L and the DSU but not CPU-B: a
secondary still running on CPU-B runs at 2002 MHz with DCDC3 at 1050 mV,
and the lifetime also ends with a power removal, before any other image.
A deadline return with every secondary completed (mask `0xfe`) means the
lowering refused or failed: no secondary runs, CPU-B is at 2002 MHz or
parked on the oscillator at 1050 mV, the lowering record at report offset
`0x160` gives the code, and the lifetime ends with a power removal. The
deadline unit returns CPU0 1.5 s after the launch (the 1 s startup deadline
plus 0.5 s), and the runner waits 10 s after its `exe` transaction for the
returned FEL instance. The operator therefore removes power at once when the
runner stops with `No new FEL instance`, or 12 s after the launch if it has
not reported by then.

The frozen set is
`~/.cache/sunstoneos/evidence/cpu-top-opp-20261025/step6/step6-set-5/` for one
fresh port 3-3 lifetime: power removal, cold DDR start, the base image,
bundles 25 and 26, the workload (`../bundle-deadline-top-5/`, its own runbook,
binding script and root-review draft, with `bundle-deadline-top-5.sha256`),
bundle 27, the Read bundle 14 and a power removal. The workload's source is
the archive `step6/source-3244504c`; the overlay built without `--top-opp`
equals the frozen deadline overlay byte for byte. Bundle 27's order entry
names the workload as a gate (`after_external` in the set's lifetime order).

The binding script is `tools/top-opp-workload-bind.py`, frozen byte for
byte as the bundle's `cpu-prepare.py` with `binding.json` beside it (the
source archive and bundle 26's frozen row with the SHA-256 of its uploaded
`probe.bin` and of its `image.elf`). It refuses port 3-1, whose bin,
oscillator and supplies have not been read, and requires that bundle 26's
run record and every later record belong to the current survey's instance
chain: each launch starts on the instance of the record before it, and the
survey's node and identity are the ones bundle 26's run returned to. Bundle
26's run record names the uploaded `probe.bin`, as every run record does;
the script takes that hash from the row's `artifacts.json` and requires the
file and `binding.json` to agree with it. It copies the read-only frozen
template by contents, so the prepared files are writable.
`checks/top-opp-workload-bind.py` requires the frozen script to equal the
tree script at the commit `binding.json` names, runs it on a real session's
records into a new directory, and requires eight refusals: port 3-1, a
bundle 26 record named by its ELF hash, a survey on another instance and a
list without bundle 26, each on changed copies of the records; and, in a
writable copy of the bundle, a `binding.json` probe pin, a row `probe.bin`
or a row `artifacts.json` that disagrees, and an ELF pin equal to the probe
pin. The tree script now refuses through explicit checks that `python -O`
keeps and makes every check before it creates the output directory; set 5
keeps the frozen copy from `5a334288`, whose checks are `assert`
statements, run without `-O`. A binding that stops after creating
`<stem>-prepare` needs that directory moved to the trash before a rerun;
the workload runbook says so (`step6/step6-set-5-runbook-corrected.json`
records the runbook's and the hash list's previous and new SHA-256).

Step 6's second run (`board-session-cpu-top-opp-8/port-3-3`, set
`step6-set-4`) passed bundles 2, 25 and 26: bundle 26 read CPU-B at
`0xf8802700` (1014 MHz), CPU-L and the DSU at their boot words and DCDC5
1000 mV and DCDC3 1050 mV. The workload did not launch: that set's binding
compared bundle 26's record with the ELF hash, and its copy of the frozen
template kept the read-only modes, so the replacement admission record
could not be written. The check above binds that session's real records
with `bundle-deadline-top-5` (`step6/binding-check-5b`). `step6-set-5`
differs from `step6-set-4` only in the workload's runbook path and the
session directory; every image is byte-identical. It replaces
`step6-set-4`, `step6-set-3`, `step6-set-2` and `step6-set`, and their
workload bundles; those must not run.

The host models: `checks/top-opp-relock-check.c` runs the relock against
modeled PLLs and a modeled AXP8191 behind S_TWI0 (the order, the 20 µs
waits, three lock reads 3 µs apart, P before the select, both supplies read
before any clock write, 23 refusals with no clock write and the timeouts,
then CPU-B's lowering with no PMIC access, its refusals and a lock
timeout), and `checks/workload-bisect-probe.py` runs the overlay with the
native image in the instruction model, with modeled CPU PLLs and PMIC bus,
in all nine deadline cases; the model allows the completed path's CPU-B
accesses only after every secondary is observed OFF.
`checks/workload-bisect-host.py` validates the relock and lowering records
in the runner's decoder.

## Step 6 passed on port 3-3

This design and the step 6 sets name the step images bundles 25, 26 and
27. In the tree they are now bundles 30, 31 and 32 (`OPP_BUNDLE`,
`tools/prepare-board-bundles.py`), because bringup's GPU and clock-audit
rows took 25 to 29; a later set prints the new numbers.

Set `step6-set-5` ran in one fresh port 3-3 lifetime on 2026-10-25
(`~/.cache/sunstoneos/evidence/board-session-cpu-top-opp-10/port-3-3/`),
with the frozen binding of `bundle-deadline-top-5`:

| Phase | Records | Wall time (end) | Result |
| --- | --- | --- | --- |
| Cold DDR start | `s7-ddr` | 22:01:46 | 12,288 MiB |
| Bundle 2 | `c6-bb2-*` | 22:01:49 | Kernel check passed |
| Bundle 25 | `c6-opp25-*` | 22:02:07 | CPU-L 1794 MHz at DCDC5 1000 mV in 10.98 ms (the longest park 143 µs), the DSU 1274 MHz in 5.94 ms, DCDC3 `0xb7` (1050 mV) with CPU-B at 1014 MHz; the hottest sensor 35.7 °C |
| Bundle 26 | `c6-opp26-*` | 22:02:24 | Boot words in all three domains, DCDC5 1000 mV, DCDC3 1050 mV |
| Capture, cache-off, survey, binding | `c6-tw-state-before`, `c6-tw-cache-off`, `c6-tw-survey`, `c6-tw-prepare` | 22:02:36 | All passed; the binding refused nothing |
| Workload | `c6-tw-run` | 22:02:43 to 22:02:50 | `complete`, `rom_usable` and `dram_restored` true, completed mask 254 |
| Bundle 27 | `c6-opp27-*` | 22:03:13 | Boot words in all three domains, both rails held; the hottest sensor 37.1 °C |
| Read bundle 14 | `bb14-read` | 22:03:14 | Passed; then the power removal |

The workload's own record, in 24 MHz counter ticks:

- **Relock.** Entry words: CPU-B `0xf8802700`/`0x03000000`, CPU-L
  `0xf8802600`/`0x03010000`, DSU `0xf8802600`/`0x03010105`, the boot
  words. Final words: CPU-B `0xf8804d00` (N 77, 2002 MHz), CPU-L
  `0xf8804500` (N 69, 1794 MHz), DSU `0xf8803100`/`0x03000105` (N 49,
  1274 MHz). Its own reads: DCXO word `0x983fb0f7` (26 MHz), SID `0x496`
  (bin 4), PMIC page 0, chip 3, revision 0, enables `0xff`, DCDC3 `0xb7`
  and DCDC5 `0xb2`, all seven reads with the idle status `0xf8`, S_TWI0
  divider `0x28`, 400 kHz, which the route check admits only from the
24 MHz PLL_REF. From the first board
  read to the last select took 25,901 ticks (1.08 ms), most of it the PMIC
  reads. Each domain was parked 1308, 1668 and 1651 ticks (55, 70 and
  69 µs), with 7, 3 and 3 lock reads.
- **Workload.** 32 rounds on every core; CPU0 32 ticks and 1 SGI; 33 SGIs
  on each secondary; no failure.
- **Lowering.** After every secondary was OFF: entry `0xf8804d00`, final
  `0xf8802700`/`0x03000000` (1014 MHz), parked 1292 ticks (54 µs), 12 lock
  reads.

### The background-progress prediction used the wrong image

The predicted counts (300,000 to 320,000 on cores 0 to 5, 255,000 to
270,000 on cores 6 and 7) scaled the ten boot-rate runs of
`board-session-20261025-3` by the clock ratios. Those runs used the native
image `762237cd…`, not `6fd7af86…`, which step 6 runs. The two images'
background loops are identical, an exclusive increment of the core's
counter between two calls of `ucPortGetCoreID`, but that routine differs:
in `762237cd…` it is a supervisor call from the EL0 task, and in
`6fd7af86…` it reads `TPIDRRO_EL0` and returns. At the boot rates the old
image spends about 188 cycles per iteration on the A55 cores and 248 on the
A76 cores, the new one about 23 and 20, assuming the 32 ms window of 32
ticks. The unit, the counter and the 32-tick window are the same in both.

The correct baseline is the same image and profile at the boot rates:
`board-session-20261025-8`, runs `r4` to `r6` (`6fd7af86…`, overlay
manifest `582c76f5…`), with 687,505 to 697,371 on the A55 cores (mean
692,204) and 1,583,985 to 1,598,607 on the A76 cores (mean 1,589,962).
Against it:

| Cores | Boot rates | Top points | Ratio | Clock ratio |
| --- | --- | --- | --- | --- |
| 0 to 5, A55 | 692,204 | 2,664,229 (2,660,665 to 2,671,333) | 3.85 | 1794/494 = 3.63 |
| 6 and 7, A76 | 1,589,962 | 3,275,263 (3,273,698 and 3,276,827) | 2.06 | 2002/1014 = 1.97 |

The counts scale about 6% and 4% above the clock ratios. A fixed amount of
other work per window explains it: the workers' rounds, the tick and the
SGIs cost a number of cycles, which take less time at the higher rate and
leave more of the window to the background task. Fitting count = k (f W −
O) to both rates with W = 32 ms gives O = 1.21 million cycles (2.4 ms at
494 MHz) on the A55 cores and 2.62 million cycles (2.6 ms at 1014 MHz) on
the A76 cores, and 21 and 19 cycles per iteration. Both core types lose
about the same 2.5 ms at the boot rates, which fits clocks of 1794 and
2002 MHz; the clocks themselves come from the relock's final PLL words,
locked and read back. The A76/A55 count ratio is 2.30 at the boot rates
(clock ratio 2.05) and 1.23 at the top points (clock ratio 1.12): at both
rates an A76 core runs 1.10 to 1.12 times as many iterations per cycle as
an A55 core. The predicted 0.85 came from the wrong baseline.

The prediction for cores 0 to 5 and 6 and 7 was wrong because of that
baseline, not because of the clocks or the measurement. Every other
prediction held: rounds, ticks and SGIs, the relock and lowering words,
parks under 100 µs, and the hottest sensor below 50 °C.

## Step 7: B1, then the OS image at the top points

Step 7 is two supervised runs of the integrated OS image built with
`A733_A76_ADMITTED` and `SUNSTONE_A733_OS_CPU_CLOCK`, each in its own power
cycle, because the image takes over USB0 and ends at a terminal halt:

1. **B1**: a grant of no cluster, the recognizer on core 6 and its worker on
   core 7, at the boot rates.
2. **TOP**: approval step `TOP`, citing B1's record; the clock owner raises
   CPU-B to 2002 MHz at 1050 mV, CPU-L to 1794 MHz at 1000 mV and the DSU to
   1274 MHz before the timed recognition.

Neither run can be frozen yet. `apps/os/prepare.py` needs a reviewed
physical entry record: the thermal owner's facts from a fresh reading and the
DDR reservation of the recognition inputs ([os-image.md](../os-image.md)).
No such record exists, and no integrated OS image has run on a board. The
other B1 prerequisites of [cpu-a76-dvfs.md](cpu-a76-dvfs.md) also remain for
the same power cycle on port 3-3:

- rank 5, the forced-stall return;
- the rank 7 and rank 8 bundles refrozen with the status-masked comparison
  (`~/.cache/sunstoneos/evidence/refreeze-el3-20261025/`);
- after B1, its record of schema `a733-os-cpu-clock-b1-v1`, which the `TOP`
  approval must cite.

The 70 °C stop is not yet built for these runs. The OS thermal policy's
trips are compiled constants, the vendor's THROTTLE at 90 °C and CRITICAL at
110 °C (`drivers/thermal/a733_thermal.c`). The session's hardware alarm
thresholds are not inputs to the policy. A 70 °C stop therefore needs a
policy change before step 7 is frozen: a CRITICAL trip at 70 °C on every CPU
sensor for these runs, which ends the session. The run's record must also
carry the thermal task's readings, taken about every 100 ms, through the
timed recognition.

Predictions for the bench recording, from the A2 estimate of
[cpu-a76-dvfs.md](cpu-a76-dvfs.md) (14.894 s × 494 ÷ (2 × 2 × f) for the
linear stage plus 0.283 s for the other stages, with f the CPU-B rate in
MHz):

| Run | CPU-B, CPU-L, DSU | Recognition time | Other |
| --- | --- | --- | --- |
| B1 | 1014, 494, 494 MHz | About 2.10 s | Text and CRCs equal to the host reference |
| TOP | 2002, 1794, 1274 MHz | About 1.20 s, 1.75 times faster | The same text and CRCs; `CLOCK` lines show the three top-point PLL words; every sensor below 70 °C |

A ratio below 1.75 shows a limit outside the A76 cores. The DSU at 1274 MHz
instead of 494 MHz should raise the ratio if L3 bandwidth bounds the linear
stage.

The host checks of the `TOP` grant: `checks/os-image-launch.py` prepares a
`TOP` bundle and refuses a `TOP` approval with one cluster, a `TOP` approval
below the bin's top points and both clusters for step A2 (48 cases);
`checks/cpu-clock-check.py` checks the DSU rule's points, including 1274 MHz
for vf0400 with CPU-L at 1794 MHz and CPU-B at 2002 MHz.
`checks/os-cpu-clock-native.py` passes on the same build but covers the
register adapter only; no check yet drives the owner's request order for a
grant of both clusters.

## Next steps

- **Step 7, the sustained load.** The OS-image runs above (B1, then `TOP`)
  wait for three things. First, a 70 °C stop in the OS thermal policy: a
  session field for a CRITICAL trip at or below the vendor's, validated by
  `apps/os/prepare.py` and used by the policy as the lower of the two.
  Second, B1: rank 5, the reviewed physical entry record, and a producer of
  the `a733-os-cpu-clock-b1-v1` record. Third, power cycles: the integrated
  OS image ends at a terminal halt that only a power removal clears, and
  the boards cannot be power-cycled until further notice. Review the
  policy change, the session format change and the clock owner's order for
  a grant of both clusters, which no check drives yet, before any freeze.
  Until power cycles return, a CPU set must continue an existing lifetime
  and return on every reachable outcome with the clocks and rails at boot
  values. Step 6's sets left DCDC3 at 1050 mV and DCDC5 at 1000 mV for the
  power removal, so a continuing set also needs a last image that lowers
  both rails with the clocks at their boot rates. For DCDC5 that needs a
  write like `dcdc3_change` with CPU-L and the DSU held, which does not
  exist yet.
- **CPU-B after a ROM return.** The ROM resets CPU-L and the DSU but not
  CPU-B. Every image that raises CPU-B lowers it to 1014 MHz before a
  successful return; a failed return can leave it raised, with DCDC3 at
  the raised point's voltage. Step images admit CPU-B only at 1014 MHz,
  the OS image refuses any other rate, and the CPU clock driver lowers
  CPU-B before DCDC3.
- **The workload binding.** `tools/top-opp-workload-bind.py` has the
  `-O`-proof checks and names the rail read-back image bundle 31; the next
  freeze takes it, with `binding.json` beside it, and
  `checks/top-opp-workload-bind.py` then runs against that freeze.
- **The returning overlay no longer links on bringup.** Bringup's MBUS
  restore in `identity_primary_run` (`01308e9e`, `3f09ec2c`) grows the overlay's
  `identity-c.o` by about 870 bytes. The return-deadline workload profile
  now exceeds its 16 KiB reservation, with or without `--top-opp`. That
  profile's next freeze needs a new layout, for example the extension
  placed in the gap after the base overlay when no relock or vector table
  uses it, or the MBUS window moved into the native image the overlay
  calls.
- **The performance-mode toggle.** The default path is CPU-B, CPU-L and the
  DSU at the top points through the driver, in the order and with the
  waits above. The toggle lowers through the same driver: the DSU first,
  then the clusters, then their rails. In a returning image it must lower
  CPU-B to 1014 MHz before the return. In the OS it needs the `TOP` grant,
  which `apps/os/prepare.py` admits, and a standing per-board approval
  record instead of the per-session one. With the boot0 rates as the
  baseline, step 6 measured about 3.85 times the A55 throughput and 2.06
  times the A76 throughput of a CPU-bound loop at the top points.

## Step 1: bundle 20

`apps/board-bundles-power/cpu-opp-step.c` runs on bundle 15's image with the
CPU clock driver, the thermal driver, its native adapter and the ADC native
binding added (`checks/board-bundles-apps.py`). The grant names CPU-B up to
1,404 MHz and two changes. The client:

1. waits for the PMIC provider and the 5,160 µs supply settle, starts the
   thermal driver and policy and reads one set of the five sensors;
2. inspects CPU-L, the DSU and CPU-B through the driver: the PLL, mux,
   pattern and LFM words of each, and DCDC5 and DCDC3 snapshots. These are
   the first board reads of the DSU's pattern and LFM words;
3. returns `REFUSED board bundle 20:` with the converter disabled and the
   provider parked, before any write, unless the level is NORMAL, the bin is
   vf0400, both rails read 800 mV, CPU-B runs at 1014 MHz and every pattern
   and LFM enable is clear. Each refusal is a controlled outcome, a warm
   return;
4. requests 1404 MHz, then 1014 MHz, each with a 120 ms deadline, driving the
   driver in 200 µs bursts once per tick;
5. requires after each transition that DCDC3 kept 800 mV, so no selector
   write was planned;
6. inspects the three domains again and requires every PLL and mux word and
   both rails to read as before the step, reads the second sensor set,
   disables the converter, parks the provider and prints
   `COMPLETE board bundle 20`.

Every request deadline is clamped to the session's 1.5 s, so a park cannot
start after the session ends.

A fault after the park ends with a `FAIL` line and leaves CPU-B on the
oscillator or its PLL; cores 6 and 7 are off, so CPU0 keeps running and the
monitor returns.

The frozen set is
`~/.cache/sunstoneos/evidence/cpu-top-opp-20261025/step1-set/`. It starts a
new port 3-3 lifetime: power removal, cold DDR start `s7-ddr`, the base image
(`kernel-check-counter-option`, whose monitor and platform objects bundle 20
links), then bundle 20, then the Read bundle 14 as a host-side capture of the
clock words. `port-3-3/cpu-b-800mv-step/bundle/approval.json` binds the
values to port 3-3 and the image.

## Budgets and measurement

The driver changes add waits and no accesses.

| Metric | Before | After | Measured by |
| --- | --- | --- | --- |
| Clock register writes per frequency change | 5 | 5 | `cpu-clock-model`, `BUDGET` lines |
| PMIC writes in the A2 raise | 1 | 1 | Same |
| Parked time of the A2 raise, 20 µs model lock | 23 µs | 69 µs | Same |
| Owner ticks inside the parked window, 20 µs lock | 0 | 0 | Same |
| Wait after a 50 mV raise | 200 µs | 1 ms | Same |
| Step 1: clock writes, PMIC writes | — | 10, 0 | Same, on the recorded port 3-3 state |
| Default path on port 3-3: clock writes, PMIC writes | — | 15, 2 | Same |
| Default OS images | — | Loadable bytes unchanged | Integration gate and image comparison |

The driver is linked only into images with `SUNSTONE_A733_OS_CPU_CLOCK` or
into bundle 20, so the default images do not change. A parked window of
about 70 µs fits the OS owner's 200 µs burst, and the 1 ms settle only delays
a frequency change, which the owner's 120 ms deadline covers.

On the board, step 1 measures the lock time, as the parked time less the
three settles, and confirms the CPU-B words after each change.
