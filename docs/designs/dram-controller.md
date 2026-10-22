# DRAM controller report and memory-bus priority

Status: design for review. No code exists yet. Physical behavior remains
unqualified until the board evidence listed at the end exists.

This component records the memory configuration that boot0 left, so evidence
records name it. It applies a fixed memory-bus priority table with read-back,
so display scan-out keeps priority over bulk masters. It also gives one board
evidence caller a per-port bandwidth window. The DDR thermal channel gets a
fixed passive trip. DRAM training, frequency scaling, DRAM voltage and the
boot0 DRAM firewall stay inside boot0 and are listed as dependencies.

## Sources fix what can be decoded

- **Boot0 DRAM parameters.** Boot0 keeps 32 parameter words at monitor header
  offset `0x30..0xac` and writes the DRAM size in MiB at `0x2c`. The values
  below come from the FEL DDR wrapper run on 2026-10-15
  (`ddr-20261015-223537/dram-parameters.bin` and `session.json`). Word 0 is
  `0x7f8` (2040 MHz). Word 1 is 9, which the pinned vendor `sunxi_dram_info.c`
  and the boot0 log identify as LPDDR5. Word 24 is `0x13090603`; the vendor
  rule 4 × 2040 / (d + 1) gives the trained frequencies 2040, 1165, 816 and
  408 MHz. The size is 12,288 MiB.
- **Fields without a source stay raw.** Words 6, 7 and 30 (`0xa11a`,
  `0x30001001`, `0x10065`) encode geometry and options that no pinned A733
  source defines. The report records them without naming fields.
- **Clock and reset registers.** The CCU at `0x02002000` holds
  `PLL_DDR_CTRL_REG` (`+0x0020`), `NSI_CLK_REG` (`+0x0580`: gate bit 31,
  `RST_BUS_NSI` bit 30), the NSI configuration word (`+0x0584`: gate bit 0,
  `RST_BUS_NSI_CFG` bit 16), `MBUS_CLK_REG` (`+0x0588`) and `DRAM0_CLK_REG`
  (`+0x0c00`), per manual section 4.1.6 and the pinned `ccu-sun60iw2.c`.
- **Controller registers.** The manual gives only the DRAMC common block at
  `0x0a020000` and controller 0 at `0x0a100000`, with no field descriptions.
  The first report reads no DRAMC register. A DRAMC field is added only after
  the boot0 write-sequence trace names it, and this design then lists its
  offset and meaning.
- **Interconnect (NSI) registers.** The pinned `sunxi-nsi.c`, `sunxi-nsi.h`
  and the `nsi0` node of `sun60iw2p1.dtsi` give each port a block at
  `0x02020000 + 0x200 × n`. The block holds mode bits [1:0] at `+0x10`, read
  and write priority bits [3:0] at `+0x14`, the select bit 0 at `+0x18` and
  counters at `+0xc0..+0xe0`. The A733 ports are GMAC 0, MSI_LITE0 1, DE 2,
  EINK 3, DI 4, G2D 5, GPU 6, VE0–VE2 7–9, GIC 10, MSI_LITE1/2 11–12,
  USB_PCIE 13, IOMMU0/1 14–15, ISP 16, CSI 17 and NPU 18. The node declares
  two channels; total traffic is port 32 plus port 33. Ports 6, 18, 32 and
  33 count NSI-clock cycles, port 10 the GIC clock, and every other port the
  MBUS clock. Port counters use 16-byte units and totals 64-byte units. The
  sources do not define what mode 0 and select 1 mean, so the table records
  them as vendor values. The CPU direct channels are separate blocks at
  `0x0a030000` (port 19) and `0x0a040000` (port 20).

## Data flow and ownership

- **Header handoff.** No current boot path delivers the header to the OS. The
  FEL DDR wrapper leaves the words in SRAM for the host. The cold-entry report
  at `0x47008000` has never executed on a board, its payload parks without
  starting FreeRTOS, and the report survives a warm reset. The path that
  starts FreeRTOS must copy the 32 words and the size word into a record bound
  to the current boot contract's epoch and generation. The report decodes it
  only if magic, version, size, status 1, header length 264, epoch and
  generation all match; otherwise the memory-configuration part is refused.
- **Configuration report.** One call reads five CCU words and the inherited
  mode, priority and select of the always-on ports (0, 1, 10, 11, 12, 14 and
  15). It fills one immutable record. It sends no DRAM mode-register commands.
- **Always-on priorities.** The same call writes IOMMU0 and IOMMU1 (priority
  3, mode 0, select 1) and GMAC (priority 1). These ports have no power
  domain, so the call runs once at boot, after NSI admission and before any
  provider enables IOMMU translation. MSI_LITE0–2 and GIC are recorded and
  left unchanged.
- **Per-domain priorities.** A port in a switchable domain is written only by
  the provider that powered it, after power-on and before its master's first
  DMA, because an access to an unpowered port can stall the bus. The call is a
  read-modify-write of each owned field, then one read-back per field. The
  first read is recorded as the inherited value. A mismatch keeps the master
  disabled. Calls per provider:

  | Provider (hook after power-on) | Ports and priority |
  | --- | --- |
  | `a733_de_sys_power` (display and G2D) | DE 2 at 2; DI 4 and G2D 5 at 1 |
  | `a733_video_platform` (VE_DEC lease) | VE0 7 and VE1 8 at 1 |
  | `a733_video_encode_platform` (VE_ENC lease) | VE2 9 at 1 |
  | `a733_media_clock` (VI path) | ISP 16 and CSI 17 at 2 |
  | `a733_gpu_power` (GPU_TOP) | GPU 6 at 1, priority only |
  | `a733_npu_platform` (NPU) | NPU 18 at 1, priority only |
  | `a733_pcie` (PCIE domain) | USB_PCIE 13 at 1, priority only |

  The vendor table gives mode 0 and select 1 for every listed port. GPU, NPU,
  USB_PCIE and GMAC have no vendor values, so only their priority field is
  owned and the inherited mode and select are recorded. EINK (3) has no OS
  driver and is recorded as unused.
- **Bandwidth window.** The single caller is the board evidence check in the
  `gpu-display` output task, which already polls once per tick. A start call
  admits DE (2) only while `a733_de_sys_power_ready()` holds; DE_SYS has no
  power-off path. IOMMU0/1 (14, 15) and the totals (32, 33) are always on.
  Any other port is admitted only while the caller holds that domain's lease
  and holds it until collection. Unknown or unpowered ports are refused. Start
  programs each port's period from its own decoded clock, then clears and
  enables the counters. A collect call returns pending until one tick after
  the period. It reads request and data counts, disables the counters and
  returns the programmed cycles and clock. A collection after twice the
  period is refused. That bound gives a complete period whether the hardware
  stops at the period or latches and restarts; which one it does is
  unverified. A changed NSI or MBUS clock word between start and collect also
  refuses the result.
- **DDR temperature.** The DDR entry in the static `trips[]` table of
  `a733_thermal.c` becomes {85000, 2000, 110000}. JEDEC LPDDR5 standard
  refresh is specified up to 85 °C, and no decoded field shows derated
  refresh. A passive trip raises the global THROTTLE level, and
  `a733_cpu_clock.c` then caps both clusters at 1014 MHz. The sensor is on the
  SoC, so DRAM retention above 85 °C stays unqualified. The trip is revisited
  only when the boot0 trace names a field showing derated refresh enabled.

## Access authority, interrupts and memory

Before any access, one mapping check per window confirms Device attributes
for the CCU page and the NSI ports. `platform_init` maps the first GiB as
Device-nGnRnE, so this check is a single admission test; a failure refuses
the report. An EL1 synchronous abort ends the system through `platform_exit`,
so the component never probes. CCU reads cite the clock registry's existing
fabric authority. NSI access needs a source-backed record, as DMA0 has. Its
sources are the vendor driver's nonsecure EL1 access and the secure-peripheral
state this boot path leaves. Without that record, or unless both NSI gates are
set and both resets deasserted, the component makes zero NSI accesses and
applies no priority. It never enables the gates; that request goes to the
fabric owner.

The component installs no interrupt, timer or task. The NSI interrupt (SPI 43)
stays masked. The window never blocks and never busy-waits.

The record is at most 256 bytes of normal write-back memory that no device
reads, so it needs no cache maintenance. Register accesses use plain
`a733_mmio_read32` and `a733_mmio_write32`; Device order within one peripheral
suffices. Each priority call ends with one `a733_mmio_dsb_sy()` after its last
read-back. That orders the NSI write before the provider's DMA enable in a
different peripheral, and `a733_mmio.h` has no weaker barrier.

## Synchronization across cores

The report runs once in the startup task after the scheduler starts, before
media providers start. CCU reads are single words with no read-modify-write,
so they take no mutex. The task publishes the record with a release store of
its state word; readers on any core use an acquire load. Ports do not share
words, and each port has one provider, so priority calls need no added lock.
The window's state belongs to its single caller, checked with
`a733_task_context_owner_el1`; there is no service task or request interface.

## Reuse and removal

The component reuses the shared MMIO, range, mapping and task-context helpers,
the provider power-on paths, the thermal trip table and the evidence snapshot
tools. It adds no capture path or report format. The OS has no memory-bus code
to remove. The vendor sysfs interface, its frequency governor and its limiter
settings are not ported.

## Performance budgets

Host models run the compiled code against register files that count every
access. Instruction counts come from AArch64 emulation of the compiled object.
Times come from CNTVCT_EL0 on the board.

| Metric | Target | Measurement | Baseline |
| --- | --- | --- | --- |
| Boot report | ≤ 33 reads, 7 writes, 1 barrier, ≤ 3,000 instructions | Host model; emulation; CNTVCT around the call | None |
| Priority per port | 9 accesses (mode, priority, select); 5 for priority only; 1 barrier per call | Host model per provider | 0 OS writes |
| Steady state | 0 tasks, timers, interrupts and accesses | Linked-symbol check; host model | 0 |
| Bandwidth window | ≤ 8 accesses per port plus 6 CCU reads; 0 blocking or busy-wait; ≤ 20 µs per phase | Host model counts; CNTVCT around start and collect | None |
| Counter headroom | No 32-bit wrap in a 1 s window | Port data: 2³² × 16 B at 38.4 GB/s wraps after 1.79 s. Totals: 7.2 s. Period: 1 s needs < 4.29 GHz | Not applicable |
| Scan-out under load | 0 TCON underflow or lost-data events; DE bytes within one frame of demand per window | 60 s at 1280×720 XRGB8888, 60 Hz, GPU rendering; images built with and without the table | None |
| DDR trip cost | Recognition real-time factor and time at THROTTLE, with and without the 85 °C trip | OS image recognizing recorded input for 10 min at 25 °C ambient, fan off | CPU throttles at 90 °C; DDR not measured |
| Code size | ≤ 4 KiB text, ≤ 512 B data | `llvm-size` | 0 |

The boot report reads five CCU words and three fields of seven ports, then
reads back the seven fields it writes. The 38.4 GB/s bound assumes four
transfers per 2400 MHz clock on a 32-bit bus, which exceeds this board under
any reading of word 0. A 720p XRGB frame is 3,686,400 bytes, so scan-out needs
221.2 MB/s. A window not aligned to vsync can hold one partial frame, so the
tolerance is one frame plus the measured blanking and prefetch margin. IOMMU
walk traffic on ports 14 and 15 is reported separately. The scan-out row
covers DE and GPU; adding G2D, VE and NPU load needs an image that composes
them. The trip row also records how often channel 1 reaches 85 °C.

## Deliberately left out

- **Training, frequency scaling and voltage.** Boot0's opaque routine owns
  them; replacing it is deferred by project policy.
- **DRAM firewall check.** Boot0 requests a 120 MiB secure region, but no
  trace identifies its registers, which are likely secure-only. Pools stay
  unchanged. Once the trace yields the range, the check is one boot-time
  `a733_extents_overlap_or_wrap` test of that range against
  `0x40000000..0x48000000` (every OS-mapped CPU range) and
  `0x48000000..0x79000000` (both device pools). A host check fails when a
  reservation leaves those spans. An overlap is a bounded boot refusal that
  withholds the device pools and is recorded.
- **Mode-register reads, limiters, CPU direct channels, NSI ECC and runtime
  priority changes.** No board use needs them; a limiter waits for a measured
  underrun.
- **Full-capacity memory testing.** A separate board test owns it.

## Dependencies and changes outside this component

- The primary agent's boot path delivers the bound header record described
  above.
- Each provider in the priority table calls the per-domain function after
  power-on and before its master's first DMA.
- The boot0 trace that names the refresh, derating and firewall fields is a
  prerequisite owned by this component.

## Board evidence that would confirm the design

- A report whose clock, type and size agree with the wrapper log.
- Read-back on each powered domain, and the inherited values of every port.
- The scan-out row with and without the table, and the counter period behavior.
- A DDR temperature series with the trip-cost measurement.
