# Design: CPU clusters and the DSU default to the vendor's top operating points

Status: **driver changes implemented and host-modeled. Step 1 passed on port
3-3 on 2026-10-25; steps 2 to 5 are built and frozen in one lifetime and have
not run.** Every hardware block defaults to the highest
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

Three changes outside the driver remain for the default path, after the
board steps:

- The integrated OS image's owner (`apps/os/cpu.c`) requests each granted
  cluster's maximum; it must also request the DSU after CPU-L.
- Preparation and admission refuse a grant that names both clusters
  ([cpu-a76-dvfs.md](cpu-a76-dvfs.md)); the default needs both, and the
  record format already carries them.
- Each grant needs an approval record for one session. The default needs a
  standing record per board for its top points, which the resident lifetime
  also needs.

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
| 6 | The all-core workload with the top point applied before the tasks start | Workload passes with the status-masked capture; core 6 measures 2002 MHz and core 0 1794 MHz; the hottest sensor stays below 90 °C |
| 7 | B1, then the OS image with both clusters and the DSU at their top points | Recognition on the A76 pair at about 1.2 s for the bench recording ([cpu-a76-dvfs.md](cpu-a76-dvfs.md), A2) |

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
step 1 with `-DOPP_BUNDLE=n` (below). Step 6 needs the driver in the all-core workload
image and is not built.

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
`~/.cache/sunstoneos/evidence/cpu-top-opp-20261025/steps2-5-set/`: power
removal, cold DDR start, the base image, then bundles 21, 22, 23 and 24, each
with its approval record, then the Read bundle 14 as the host-side capture.

The host model runs the three driver steps in sequence with the bundles'
grants (`lifetime_cases` in `checks/cpu-clock-model.c`), followed by a
thermal stop of step 5 that returns DCDC5 to 800 mV.

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
