# DRAM controller report and memory-bus priority

Status: design for review. No code exists yet. Physical behavior remains
unqualified until the board evidence listed at the end exists.

This component records the memory configuration that boot0 left, so evidence
records name it. It applies a fixed memory-bus priority table with read-back,
so display scan-out keeps priority over bulk masters. It gives one board
evidence caller a bandwidth and latency window over the ports that caller
loads. The DDR thermal channel gets a fixed passive trip. DRAM training,
frequency scaling, DRAM voltage and the boot0 DRAM firewall stay inside boot0
and are listed as dependencies.

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
  (`+0x0c00`), per manual section 4.1.6 and the pinned `ccu-sun60iw2.c`. In
  `NSI_CLK_REG` and `MBUS_CLK_REG`, bits [26:24] select the parent and bits
  [4:0] hold the divider minus one. NSI parents are 24 MHz, pll-ddr,
  pll-peri0-800m, pll-peri0-600m, pll-peri0-480m and pll-de-3x. MBUS parents
  are 24 MHz, pll-peri0-600m, pll-ddr, pll-peri0-480m, pll-peri0-400m and
  pll-npu. The parent PLL words are `PLL_DDR` (`+0x0020`), `PLL_PERI0`
  (`+0x00a0`), `PLL_NPU` (`+0x02a0`) and `PLL_DE` (`+0x02e0`), each with
  fractional-mode pattern words at `+8` and `+12`.
- **Controller registers.** The manual gives only the DRAMC common block at
  `0x0a020000` and controller 0 at `0x0a100000`, with no field descriptions.
  The report reads no DRAMC register. A DRAMC field is added only after the
  boot0 write-sequence trace names it, and this design then lists its offset
  and meaning.
- **Interconnect (NSI) registers.** The pinned `sunxi-nsi.c`,
  `bus-sunxi-nsi.h` and the `nsi0` node of `sun60iw2p1.dtsi` give each port a
  block at `0x02020000 + 0x200 × n`. The block holds mode bits [1:0] at
  `+0x10`, the priority field at `+0x14`, the select bit 0 at `+0x18`, and
  counters at `+0xc0` (enable), `+0xc4` (clear), `+0xc8` (period in cycles),
  `+0xcc..+0xd8` (read and write request and data counts) and `+0xdc`/`+0xe0`
  (accumulated read and write latency). A larger priority value is served
  first. The A733 ports are GMAC 0, MSI_LITE0 1, DE 2, EINK 3, DI 4, G2D 5,
  GPU 6, VE0–VE2 7–9, GIC 10, MSI_LITE1/2 11–12, USB_PCIE 13, IOMMU0/1 14–15,
  ISP 16, CSI 17 and NPU 18. Total DRAM traffic is port 32 plus port 33, one
  per channel. Ports 6, 32 and 33 count NSI-clock cycles; ports 2, 14 and 15
  count MBUS-clock cycles. Port counters use 16-byte units and totals 64-byte
  units. The sources do not define what mode 0 and select 1 mean, so the table
  records them as vendor values.

## Data flow and ownership

- **Header handoff.** No current boot path delivers the header to the OS. The
  FEL DDR wrapper leaves the words in SRAM for the host. The cold-entry report
  at `0x47008000` has never executed on a board, its payload parks without
  starting FreeRTOS, and the report survives a warm reset. The path that
  starts FreeRTOS must copy the 32 words and the size word into a record bound
  to the current boot contract's epoch and generation. The report decodes it
  only if magic, version, size, status 1, header length 264, epoch and
  generation all match; otherwise the memory-configuration part is refused.
- **Configuration report.** One boot call reads the five fixed CCU words and
  the word of each parent PLL that `NSI_CLK_REG` or `MBUS_CLK_REG` selects. A
  parent is decoded only if it passes the integer-PLL rule that
  `a733_media_clock.c` already applies: enable and lock bits set, both pattern
  words disabled, N in range. A 24 MHz parent needs no PLL word. Any other
  parent leaves that clock undecoded. The report stores the NSI and MBUS
  frequencies, the header decode and the always-on port values below in one
  record, and sends no DRAM mode-register commands.
- **Always-on priorities.** The same call writes IOMMU0 and IOMMU1 (priority
  3, mode 0, select 1; vendor values). GMAC has no vendor value, so the call
  lowers its priority to 1 only if the inherited value is above 1. These ports
  have no power domain. The record holds each field's inherited and read-back
  value.
- **Per-domain priorities.** A port in a switchable domain is written only by
  the provider that powered it, after power-on and before its master's first
  DMA, because an access to an unpowered port can stall the bus. Each owned
  field is read, written and read back. The hook returns the inherited and
  read-back values and a per-port result to the provider, which stores them in
  its own evidence record; the boot report stays immutable.

  | Provider (hook after power-on) | Ports and priority |
  | --- | --- |
  | `a733_de_sys_power` (display and G2D) | DE 2 at 2; DI 4 and G2D 5 at 1 |
  | `a733_video_platform` (VE_DEC lease) | VE0 7 and VE1 8 at 1 |
  | `a733_video_encode_platform` (VE_ENC lease) | VE2 9 at 1 |
  | `a733_media_clock` (VI path) | ISP 16 and CSI 17 at 2 |
  | `a733_gpu_power` (GPU_TOP) | GPU 6, lowered to 1 only |
  | `a733_npu_platform` (NPU) | NPU 18, lowered to 1 only |
  | `a733_pcie` (PCIE domain) | USB_PCIE 13, lowered to 1 only |

  The vendor table gives mode 0 and select 1 for the first four rows, and
  those fields are written. GPU, NPU and USB_PCIE have no vendor values. Their
  hooks read only the priority field and write 1 only if the inherited value
  is above 1, so a hook never raises a bulk master above the unchanged
  MSI_LITE ports. EINK (3) has no OS driver and is not written.
- **Hook results.** A hook called before the report is published returns
  not-ready and makes no NSI access; the provider refuses power-on, because
  that order breaks the startup sequence. A hook called after the report
  published a refused NSI admission returns not-admitted and makes no NSI
  access; the provider proceeds with the inherited priorities and records the
  result, since inherited priorities are what the OS runs with today. A
  read-back mismatch disables only the mismatched port's master: a DI or G2D
  mismatch leaves DE scan-out running. Fields already written keep their new
  values, the result lists each one, and the hook does not retry in that boot.
- **Bandwidth window.** The single caller is the board evidence check in the
  `gpu-display` output task, which already polls once per tick. The port set
  is fixed: DE 2 (admitted while `a733_de_sys_power_ready()` holds; DE_SYS
  has no power-off path), GPU 6 (only when the caller passes the GPU power
  lease it already holds), IOMMU0/1 14–15 and totals 32–33. A port whose clock
  the report left undecoded is refused. Start writes each port's period for
  P = 0.5 s from the report's NSI or MBUS frequency, then sets the clear and
  enable bits by read-modify-write, as the vendor driver does. After the last
  enable write, one `a733_mmio_dsb_sy()` and an ISB-ordered counter read give
  the start time. Collect returns pending until one tick after P. It then
  reads the request and data counts of each port and the read latency of
  port 2, reads the counter time again, and disables the counters. If that
  second time is at or past 2P, collect refuses the result, because a latch
  and restart may have fallen between two counter reads. Every refusal path
  disables the counters. The 2P bound covers both possible counter behaviors,
  stopping at the period or latching and restarting; which one the hardware
  shows is unverified. More ports are added only when a caller needs them.
- **Evidence-only reads.** The window's initialization reads mode, priority
  and select of GMAC 0, MSI_LITE0–2 (1, 11, 12), EINK 3 and GIC 10 once per
  evidence run. Production boots do not read them, because no boot decision
  uses them.
- **DDR temperature.** The DDR entry in the static `trips[]` table of
  `a733_thermal.c` becomes {85000, 2000, 110000}, and the table comment
  changes to say that DDR has a project passive trip at 85 °C. The vendor
  `ddr_thermal_zone` has only the 110 °C critical trip. JEDEC LPDDR5 standard
  refresh is specified up to 85 °C, and no decoded field shows derated refresh.
  A passive trip raises the global THROTTLE level, and `a733_cpu_clock.c` then
  caps both clusters at 1014 MHz. The sensor is on the SoC, so DRAM retention
  above 85 °C stays unqualified.

## Access authority, interrupts and memory

Before any access, one mapping check per window confirms Device attributes
for the CCU page and the NSI ports. `platform_init` maps the first GiB as
Device-nGnRnE, so this check is a single admission test; a failure refuses
the report. An EL1 synchronous abort ends the system through `platform_exit`,
so the component never probes. CCU reads cite the clock registry's existing
fabric authority. NSI access needs a source-backed record, as DMA0 has. Its
sources are the vendor driver's nonsecure EL1 access and the secure-peripheral
state this boot path leaves. Without that record, or unless both NSI gates are
set and both resets deasserted, the report publishes a refused NSI admission,
makes zero NSI accesses and applies no priority. It never enables the gates;
that request goes to the fabric owner.

The component installs no interrupt, timer or task. The NSI interrupt (SPI 43)
stays masked. The window never blocks and never busy-waits.

The record is at most 256 bytes of normal write-back memory that no device
reads, so it needs no cache maintenance. Register accesses use plain
`a733_mmio_read32` and `a733_mmio_write32`. Priority writes need no barrier.
Device-nGnRnE accesses to one peripheral complete in program order, so the
read-back returns only after the write has reached the port. The provider
compares the read-back value before its DMA-enable store, and a store cannot
become visible before the branch that depends on that load resolves.

## Synchronization across cores

The report runs once in the startup task after the scheduler starts, before
media providers start. CCU reads are single words with no read-modify-write,
so they take no mutex. The task publishes the record with a release store of
its state word, as admitted or refused; readers on any core use an acquire
load. NSI ports do not share words, and each port has one writer (the boot
call or one provider), so priority calls need no added lock.
`a733_iommu_open` refuses until the state word shows publication, which
orders the IOMMU0/1 writes before any IOMMU translation. The window structure
is a static object in the `gpu-display` evidence source file, and no other
task holds a pointer to it. Its initialization checks the caller once with
`a733_task_context_owner_el1`; start and collect check nothing per call.

## Reuse and removal

The component reuses the shared MMIO, range, mapping and task-context helpers,
the integer-PLL rule of the media clock driver, the provider power-on paths,
the provider evidence records, the thermal trip table and the evidence
snapshot tools. It adds no capture path or report format. The OS has no
memory-bus code to remove. No OS code writes `NSI_CLK_REG`, `MBUS_CLK_REG` or
their parent PLLs after boot0; the video, USB, DMA and media-clock drivers
only read them against fixed values. The window therefore takes its
frequencies from the report and rereads no CCU word. The vendor sysfs
interface, its frequency governor and its limiter settings are not ported.

## Performance budgets

Host models run the compiled code against register files that count every
access. Instruction counts come from AArch64 emulation of the compiled object.
Times come from the architectural counter on the board. RTF (real-time
factor) is recognition wall time divided by audio duration.

| Metric | Target | Measurement | Baseline |
| --- | --- | --- | --- |
| Boot report | ≤ 11 CCU reads (5 fixed, ≤ 3 per decoded parent); ≤ 14 NSI reads, ≤ 7 writes; 0 barriers; ≤ 2,500 instructions | Host model; emulation; counter around the call | None |
| Priority per port | 9 accesses (3 fields, each read, written, read back); lowered-only port: 1 read, plus 1 write and 1 read-back when above 1; 0 barriers | Host model per provider | 0 OS writes |
| Steady state | 0 tasks, timers, interrupts and accesses | Linked-symbol check; host model | 0 |
| Bandwidth window | Start: 5 accesses per port, 1 DSB. Collect: 6 per port, plus 1 latency read on port 2. Six ports: 30 and 37. 0 CCU reads, 0 blocking or busy-wait, ≤ 20 µs per phase. Initialization: 18 reads once | Host model counts per phase; counter around start and collect | None |
| Counter headroom | No 32-bit wrap in P = 0.5 s | Port data: 2³² × 16 B at 38.4 GB/s wraps after 1.79 s; totals after 7.2 s. Period: clock < 8.59 GHz. Port 2 latency sum: below 2³² while mean read latency stays under 2,480 cycles at 3.46 M reads/s | Not applicable |
| Scan-out under load | Each window's port 32 + 33 total ≥ 70 % of measured peak, else inconclusive. 0 TCON underflow events with the table. Port 2 mean read latency (latency sum / read requests), over the run and for the worst window, lower with the table than without, at totals within 5 % | 60 s at 1280×720 XRGB8888, 60 Hz; load from GPU rendering plus a CPU copy loop on every core; peak from a calibration run of that load alone; images built with and without the table | Latency and peak not measured |
| DDR trip cost | RTF with the trip ≤ 1.05 × RTF without; added time at THROTTLE ≤ 60 s of 600 s | Integrated OS image recognizing recorded input for 10 min at 25 °C ambient, fan off, with and without the trip | Measured before the change: RTF, DDR minus CPUB per sample (mean, max), time each channel spends at or above 85 °C |
| Code size | ≤ 4 KiB text, ≤ 512 B data | `llvm-size` | 0 |

The 38.4 GB/s bound assumes four transfers per 2400 MHz clock on a 32-bit
bus, which exceeds this board under any reading of word 0. A 720p XRGB frame
is 3,686,400 bytes, so scan-out needs 221.2 MB/s, or 3.46 M reads/s if each
read carries 64 bytes. The window records read requests and data, so the
evidence shows the real read size; P is halved if a measured latency sum
exceeds 2³¹. The hardware gives only a latency sum, so the worst-window mean
stands in for a maximum. If the table shows no lower DE latency at qualifying
load, it stays as the vendor configuration and the result is recorded.

If the trip exceeds either DDR bound, the trip stays, because the unknown
refresh state makes it a safety requirement. The regression is recorded as
explained, and decoding the boot0 derated-refresh field becomes the blocking
follow-up for raising or removing the trip.

## Deliberately left out

- **Training, frequency scaling and voltage.** Boot0's opaque routine owns
  them; replacing it is deferred by project policy.
- **DRAM firewall check.** Boot0 requests a 120 MiB secure region, but no
  trace identifies its registers, which are likely secure-only. Once the trace
  yields the range, one boot-time `a733_extents_overlap_or_wrap` test runs
  against two spans. An overlap with `0x48000000..0x79000000` (both device
  pools) withholds those pools and is recorded. An overlap with
  `0x40000000..0x48000000` (the running image, the monitor at
  `0x42000000..0x42200000` and the mimus arenas) is a fatal boot refusal in
  the primary boot path, unless the range equals the monitor region, which is
  recorded as expected. A host check fails when a reservation leaves those
  spans.
- **Mode-register reads, limiters, CPU direct channels, NSI ECC, runtime
  priority changes and window ports without a caller.** No board use needs
  them; a limiter waits for a measured underrun.
- **Full-capacity memory testing.** A separate board test owns it.

## Dependencies and changes outside this component

- The primary agent's boot path delivers the bound header record and handles
  the fatal firewall refusal.
- Each provider in the priority table calls its hook after power-on and
  before its master's first DMA, keeps a mismatched master disabled, refuses
  on not-ready and proceeds on not-admitted.
- `a733_iommu_open` refuses until the report state is published. Host checks
  cover open before publication, a hook before publication and a hook after a
  refused admission, each with zero NSI accesses.
- `checks/thermal-policy.c` asserts NORMAL with DDR at code 1000 (108 °C) and
  states that DDR has no passive trip; that case moves the DDR reading below
  85 °C. A new `checks/a733-thermal-ddr-trip.c` raises only DDR to code 1394
  (85.02 °C) and expects THROTTLE, holds it at code 1428 (83.01 °C), and
  expects NORMAL at code 1429 (82.95 °C). `docs/thermal.md` updates the DDR
  trip row.
- The media clock's integer-PLL test becomes a shared clock-driver function
  so the report does not copy it.
- The boot0 trace that names the refresh, derating and firewall fields is a
  prerequisite owned by this component.

## Board evidence that would confirm the design

- A report whose clock, type and size agree with the wrapper log.
- Read-back on each powered domain, inherited values from every hook, and the
  evidence-only port values.
- The scan-out row with and without the table, and the counter period behavior.
- The DDR trip row, with its baseline run before the change.
