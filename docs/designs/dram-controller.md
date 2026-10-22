# DRAM controller report and memory-bus priority

Status: design for review. No code exists yet. Physical behavior will remain
unqualified until the board evidence listed at the end exists.

This component has five jobs. It records the memory configuration that boot0
left, so that evidence records name it. It checks boot0's DRAM firewall against
the OS memory map. It applies a fixed memory-bus priority table, so display
scan-out keeps priority over bulk masters. It measures per-master bandwidth on
request. It also supplies the DDR refresh state to the existing thermal policy.
DRAM training, frequency scaling and DRAM voltage stay inside boot0's opaque
routine and remain a known dependency.

## Sources fix what can be decoded

- **Boot0 DRAM parameters.** Boot0 writes 32 parameter words at monitor header
  offset `0x30` and the DRAM size in MiB at offset `0x2c`. The cold-entry
  recorder already captures this 264-byte header. In the 2026-10-15 capture,
  word 0 is `0x7f8` (2040 MHz) and word 1 is 9 (LPDDR5, as in the pinned vendor
  `sunxi_dram_info.c` and the boot0 log). Word 24 is `0x13090603`; the vendor
  rule 4 × 2040 / (d + 1) gives the four trained frequencies of 2040, 1165,
  816 and 408 MHz. The size word reports 12,288 MiB.
- **Fields without a source stay raw.** Words 6, 7 and 30 (`0xa11a`,
  `0x30001001`, `0x10065`) encode geometry and options, but no pinned A733
  source defines them. The vendor width rule reads `0x30001001` as 16 bits,
  while the manual describes a 32-bit controller. The report therefore records
  these words without naming fields.
- **Clock registers.** Manual section 4.1.6 defines `PLL_DDR_CTRL_REG` (CCU
  `0x0020`), `NSI_CLK_REG` (`0x0580`), `MBUS_CLK_REG` (`0x0588`) and
  `DRAM0_CLK_REG` (`0x0c00`). The report decodes them and checks them against
  word 0.
- **Controller registers.** The manual gives only the DRAMC common block at
  `0x0a020000` and controller 0 at `0x0a100000`. Ranks, refresh interval and
  temperature-derated refresh have no register description. The reviewed boot0
  binary forms addresses from `0x0a100000` at fifteen sites. A field is named
  only after its boot0 write sequence identifies it; until then the report
  records it as unknown.
- **Interconnect (NSI) registers.** The pinned vendor `sunxi-nsi.c` and
  `sunxi-nsi.h` give each port a block at NSI `0x02020000 + 0x200 × n`.
  Each block holds mode (`+0x10`), read and write priority (`+0x14`), QoS source
  (`+0x18`) and performance counters (`+0xc0..+0xe0`). Port 32 counts total
  traffic.

## Data flow and ownership

- **Configuration report.** One boot-time call reads the captured header and at
  most 48 registers, then fills an immutable record. It performs no writes and
  issues no DRAM mode-register reads, because those send commands to the DRAM.
- **Firewall check.** Boot0's board configuration requests a 120 MiB secure
  DRAM region. The decoder traces boot0 to find the registers that hold it. It
  compares each decoded secure range with the image (`0x41000000..0x41f00000`),
  the monitor (`0x42000000..0x42200000`) and the device pools
  (`0x48000000..0x79000000`), using `a733_extents_overlap_or_wrap`. An overlap
  with a nonsecure DMA pool is a refusal that pool admission consumes. Until the
  trace is complete, the verdict is "unverified" and existing pools are unchanged.
- **Priority table.** The values come from the `nsi0` children of the pinned
  `sun60iw2p1.dtsi`. IOMMU0 and IOMMU1 (ports 14 and 15) get priority 3. The
  display engine (port 2), ISP (16) and CSI (17) get priority 2. EINK, DI,
  G2D and the three video engines get priority 1. Every listed port uses
  mode 0, which disables the limiter, and takes QoS from the register. The
  display engine translates through the IOMMU, so the IOMMU's higher priority
  also covers scan-out page-table walks.
- **Ports in switchable power domains.** The vendor driver powers a port's
  domain before it touches that port's registers. A write to an unpowered port
  could stall the bus, and software cannot recover a stalled access. The table
  is therefore applied per power domain. The domain owner calls it after power-on
  and before the master's first DMA. The call writes only differing fields and
  reads each one back. A mismatch keeps the master disabled.
- **Bandwidth monitor.** One owner task programs a window of at most 1 s,
  clears and enables the selected port counters, and blocks until the window
  ends. It then reads the request, data and cycle counts once and disables the
  counters. Results carry the window, the NSI clock and the data units from the
  device tree (16 bytes per port counter and 64 bytes for the total).
- **DDR temperature.** The thermal owner already reads channel 1 (DDR), and
  the policy already treats 110 °C as critical. JEDEC LPDDR5 standard refresh
  is specified up to 85 °C. If the report establishes that temperature-derated
  refresh is enabled, the policy is unchanged. If refresh is fixed or unknown,
  policy initialization adds an 85 °C passive trip with 2 °C hysteresis to
  channel 1. The sensor sits on the SoC, so its offset from the DRAM die
  temperature remains a physical limit.

## Interrupts and polling

The component installs no interrupt, timer or task. The NSI interrupt (SPI 43)
stays masked; the vendor driver does not use it either. The report and the
priority writes are straight-line code. The bandwidth monitor blocks with
`vTaskDelayUntil` and performs no status polling. The DDR reading arrives
through the thermal owner's existing deferred interrupt and 1 s update.

## Memory placement and cache policy

The report is one record of at most 256 bytes in normal write-back memory. No
device reads or writes it, so it needs no cache maintenance. Registers are read
through the existing Device mappings with `a733_mmio.h` accessors. Before the
first access, `a733_mapping_check.h` confirms Device attributes for the CCU,
NSI and DRAMC windows. A missing DRAMC mapping refuses the controller fields,
and the remaining report is still published. NSI and DRAMC accesses also need
nonsecure access authority from the platform, as other drivers already require.
A nonsecure access to a secure-only block can raise an external abort.

## Synchronization across cores

The boot CPU fills the report and publishes it with a release store of its
state word. Readers on any core use an acquire load and read only a published
record. Per-port NSI registers do not share words with other ports, so priority
calls need no lock between ports. The domain lease serializes calls for one
domain. The bandwidth monitor admits only its owner task through
`a733_task_context_owner_el1`. CCU reads take the common register mutex, as the
thermal driver does.

## Reuse and removal

The component reuses the cold-entry header capture, the shared MMIO, range,
mapping and task-context helpers, the thermal policy and the evidence snapshot
tool. It adds no capture path or report format. The OS has no memory-bus code
to remove. The vendor's sysfs interface, its 100 ms frequency governor and its
limiter settings chosen from framebuffer size and chip revision are not ported.

## Performance budgets

Host models run the compiled decoder and table code against modeled register
files that count every access. Instruction counts come from AArch64 emulation
of the compiled object, as existing checks do for other drivers.

| Metric | Target | Measurement | Current baseline |
| --- | --- | --- | --- |
| Boot report | ≤ 48 register reads, 0 writes, ≤ 5,000 instructions | Host model access count; emulated instruction count | None; configuration is known only from the boot0 log |
| Priority per port | ≤ 9 accesses (three read-modify-writes, three read-backs); 11 ports | Host model per domain | 0 OS writes; inherited values are unknown |
| Steady-state cost | 0 tasks, timers, interrupts and register accesses | Linked-image symbol check; host model | 0 |
| Bandwidth window | ≤ 12 accesses per port, ≤ 50 µs CPU time outside the window, 0 busy-wait iterations | Host model; emulated instruction count | None |
| Counter headroom | No 32-bit overflow within 1 s | Analysis: 38.4 GB/s in 16-byte units overflows after 1.79 s | Not applicable |
| Thermal coupling | 0 added accesses per policy update; DDR throttle within the existing 1 s update | Extended `checks/thermal-policy.c` | DDR has no passive trip |
| Code size | ≤ 4 KiB text, ≤ 512 B data | `llvm-size` on the AArch64 object | 0 |

The 38.4 GB/s bound assumes four transfers per 2400 MHz controller clock on a
32-bit bus. That rate exceeds this board's configuration under any reading of
the clock word. Scan-out demand is recorded beside the measured bandwidth.
One 1920×1080 XRGB8888 layer at 60 Hz needs 497.7 MB/s; one 3840×2160 layer
needs 1.99 GB/s.

## Deliberately left out

- **Training, frequency scaling and voltage.** These belong to boot0's opaque
  routine; replacing it is deferred by project policy.
- **Mode-register reads.** They issue DRAM commands; the report stays passive.
- **Bandwidth limiters.** The vendor configuration disables them for every
  listed port. A limiter is added only after a measured display underrun.
- **CPU direct channels.** Ports 19 and 20 use separate registers at
  `0x0a030000`, and the vendor configuration sets no priority for them.
- **NSI ECC, runtime priority changes and the NSI interrupt.** No board use
  needs them.
- **Full-capacity memory testing.** This component reports capacity; testing
  all 12 GiB is a separate board test.

## Changes outside this component

- The power-domain owner calls the per-domain priority function after power-on
  and before enabling a master's DMA.
- Device-pool admission consumes the firewall verdict.
- If nonsecure EL1 cannot read the DRAMC or NSI blocks, the EL3 cold-entry
  recorder must snapshot those words. The primary agent owns that code.

## Board evidence that would confirm the design

- A captured report whose clock, type and size agree with the boot0 log.
- A firewall decode and a nonsecure DMA transfer inside each device pool.
- Priority read-back on each powered domain.
- A bandwidth window during scan-out that matches the computed demand within
  one data unit per window.
- A DDR temperature series beside the decoded refresh state.
