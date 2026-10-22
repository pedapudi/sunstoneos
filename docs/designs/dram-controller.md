# DRAM controller report and memory-bus priority

Status: design for review. No code exists. Physical behavior stays
unqualified until the board evidence in the last section exists.

The component has four parts. A read-only report records the memory
configuration that boot0 left, so evidence records name it. Each DMA master's
provider sets that master's memory-bus (NSI) priority once, after power-on and
before the master's first DMA, so display scan-out is served ahead of bulk
masters. One evidence caller gets a bandwidth and latency window. The DDR
temperature channel gets a passive trip. DRAM training, frequency scaling,
voltage and the boot0 DRAM firewall stay inside boot0.

## Sources

- **Boot0 parameters.** Boot0 keeps 32 parameter words at monitor header
  offset `0x30..0xac` and the size in MiB at `0x2c`. The FEL DDR wrapper run
  of 2026-10-15 recorded word 0 = `0x7f8` (2040 MHz) and word 1 = 9, which
  the pinned vendor `sunxi_dram_info.c` and the boot0 log identify as LPDDR5.
  Word 24 = `0x13090603` gives trained frequencies of 2040, 1165, 816 and
  408 MHz by the vendor rule 4 × 2040 / (d + 1). The size is 12,288 MiB.
  Words 6, 7 and 30 have no pinned field definition and are recorded raw.
- **Fabric clocks.** `NSI_CLK_REG` (CCU `+0x580`: gate bit 31, reset bit 30),
  the NSI configuration word (`+0x584`: gate bit 0, reset bit 16) and
  `MBUS_CLK_REG` (`+0x588`), per manual section 4.1.6. Each image's sealed
  board session already carries the expected `+0x580` and `+0x588` words and
  their rates as `fabric[1..2]` and `fabric_hz[1..2]`, and drivers compare the
  live words against them under mask `0x8700001f` (`a733_media_clock.c`).
- **NSI ports.** The pinned `sunxi-nsi.c` and the `nsi0` node of
  `sun60iw2p1.dtsi` give each port a block at `0x02020000 + 0x200 × n`. The
  block holds mode at `+0x10`, priority at `+0x14` (a larger value is served
  first) and select at `+0x18`. Its counters are enable `+0xc0`, clear
  `+0xc4`, period in cycles `+0xc8`, read and write request and data counts
  `+0xcc..+0xd8`, and read latency sum `+0xdc`. The dtsi gives each port's
  power domain and the vendor values in the hook table below. Ports 32 and 33
  count total DRAM traffic, one per channel. Ports 6, 32 and 33 count
  NSI-clock cycles and port 2 counts MBUS cycles (`master_clks`). Port counts
  use 16-byte units and totals 64-byte units.
- **DRAMC registers.** The manual gives only base addresses (`0x0a020000`,
  `0x0a100000`). The component reads no DRAMC register.

## Data flow and ownership

- **Report.** A pure function fills a caller-owned record of at most 256 bytes.
  It decodes the boot0 header record if the boot path delivered one bound to
  the current boot contract (magic, version, length 264, status, epoch and
  generation match); otherwise the memory fields read "unavailable". It reads
  the three CCU words once, compares `+0x580` and `+0x588` with the board
  record under the shared mask, and takes the NSI and MBUS rates from
  `fabric_hz[1..2]` only when both match. It records the NSI gate and reset
  bits. It makes no NSI access and no write. The first caller is the
  `gpu-display` evidence record.
- **Priority hook.** Each provider calls `a733_dram_priority` with its fixed
  port list after power-on and before its master's first DMA. The hook first
  admits NSI access: one mapping check of the CCU and NSI pages, two CCU reads
  that show both NSI gates set and both resets deasserted, and the
  component's source-backed NSI authority record. If any of these fails, the
  hook makes zero NSI accesses and returns not-admitted; the provider proceeds
  with the inherited priorities and records the result. Otherwise, for each of
  mode, priority and select, the hook reads the inherited value, writes the
  vendor value only if it differs, and reads it back. GPU, NPU and USB_PCIE
  are lowered only: the hook writes priority 1 only when the inherited value
  is above 1, and leaves mode and select untouched. The provider stores the
  inherited and read-back values in its own evidence record.

  | Provider (after power-on) | Power domain | Ports at vendor priority (mode 0, select 1) |
  | --- | --- | --- |
  | `a733_de_sys_power` | DE_SYS; IOMMU ports none | IOMMU0 14 and IOMMU1 15 at 3, DE 2 at 2, DI 4 and G2D 5 at 1 |
  | VE_DEC lease in `a733_video_platform` | VE_DEC | VE0 7 and VE1 8 at 1 |
  | VE_ENC lease in `a733_video_encode_platform` | VE_ENC | VE2 9 at 1 |
  | VI path in `a733_media_clock` | VI | ISP 16 and CSI 17 at 2 |
  | `a733_gpu_power`, `a733_npu_platform`, `a733_pcie` | GPU_TOP, NPU, PCIE | GPU 6, NPU 18, USB_PCIE 13: no vendor values, lowered to 1 only |

  IOMMU0 and IOMMU1 carry the display's page-table walks, so the DE_SYS hook
  writes them; images without DE_SYS leave them inherited.
- **Mismatch.** A read-back that differs from the written value leaves the port
  in an undocumented mode that could throttle its master. The hook stops at
  the first mismatch. Fields already written keep their values, later ports
  stay inherited, and the result lists each port. The mismatched port's master
  does not start: a DE, IOMMU0 or IOMMU1 mismatch keeps scan-out off, and a DI
  or G2D mismatch keeps only that master off. No DMA has started, so the
  failure is a refusal before device access and retains nothing beyond what
  the provider already holds.
- **Bandwidth window.** The single caller is the `gpu-display` output task,
  which already wakes every tick. The ports are DE 2, GPU 6, and totals 32 and
  33; the period P is 50 ms.
  - *Start* compares `+0x580` and `+0x588` with the board record again and
    refuses on a difference. It reads the counter (t0), then issues an ISB.
    For each port, in the vendor order, it writes the period P × f, clears
    the enable bit, sets the clear bit and sets the enable bit, reading each
    word once. Then `a733_mmio_dsb_sy`, an ISB and a counter read give t1.
  - *Collect* returns pending until the counter reaches t1 + P. It reads the
    port 32 data count, the four counts of each port, the port 2 latency sum
    and the port 32 data count a second time, then disables each port. Then
    `a733_mmio_dsb_sy`, an ISB and a counter read give t_end.
  - *Refusal.* Collect refuses if t_end ≥ t0 + 2P, which keeps every read
    inside each port's first period whatever the skew between enable writes.
    It also refuses if the two port 32 reads differ: that shows free-running
    counters, whose correct denominator the board has not yet shown. Accepted
    rates are count / P. Every refusal path disables the counters.
  - *Power.* DE is admitted while `a733_de_sys_power_ready()` holds; DE_SYS
    has no power-off path. GPU port 6 is used only while the caller holds its
    GPU power lease. The caller releases the lease only after collect returns,
    and collect calls `a733_gpu_resources_status` before the first port 6
    access. If the lease is not live, or the status is BUSY, collect makes no
    port 6 access, including the disable, and refuses the GPU result.
- **DDR temperature.** The DDR entry of `trips[]` in `a733_thermal.c` becomes
  {85000, 2000, 110000}. The vendor zone has only the 110 °C critical trip.
  JEDEC specifies LPDDR5 standard refresh up to 85 °C, and no decoded field
  shows derated refresh. A DDR passive trip raises the existing global
  THROTTLE level, which caps both clusters at 1014 MHz.

## Interrupts, memory and barriers

The component installs no interrupt, timer or task, and the NSI interrupt
(SPI 43) stays masked. The window adds no wake-up: it runs inside a task that
already polls every tick, and it never blocks or busy-waits.

Records and the window object live in normal write-back memory that no device
reads, so they need no cache maintenance. Register accesses use plain
`a733_mmio_read32` and `a733_mmio_write32`. Priority writes need no barrier:
Device-nGnRnE accesses to one peripheral complete in program order, so a
read-back returns after its write reached the port, and the provider's
DMA-enable store depends on that comparison. The window issues one `DSB SY`
per phase, because the action that follows, a counter read, is not a memory
access (performance architecture, section 1.2). t0 needs no DSB, because the
Device writes after its ISB cannot be performed earlier. Collect's first
reads need none either: Device reads are not speculative, and they depend on
the branch that tested the counter.

## Synchronization across cores

There is no shared mutable state. Each port has one writer, the provider of
its domain. The build lists show that each image links one DE_SYS instance:
`a733_display_framebuffer.c` in the display images and `apps/g2d/native.c` in
the G2D image. Providers on different cores write disjoint port blocks, and
CCU accesses are single-word reads. A hook finishes before its provider
reports ready, and the window touches only counter registers, so the two never
share a register. The window object is static in the `gpu-display` evidence
file; initialization checks the caller once with
`a733_task_context_owner_el1`. Nothing is published across cores, so the
component adds no lock and no acquire-release pair.

## Reuse

The component reuses the board fabric record and its masked compare, the
shared MMIO, mapping and task-context helpers, the provider power-on paths and
evidence records, the GPU lease status call, the thermal trip table and the
evidence snapshot tools. It decodes no parent PLL and has no boot-time writer.
`a733_npu_pll_scopes` refuses an NPU PLL change while MBUS selects pll-npu or
pll-ddr, or NSI selects pll-ddr or pll-de-3x, so no OS write changes the
fabric rates; the window's compare at start covers any other writer. The
vendor sysfs interface, governor and limiters are not ported.

## Performance budgets

Access counts come from host models that run the compiled code against
counting register files. Times come from the architectural counter on the
board. RTF is recognition wall time divided by audio duration.

| Metric | Target | Measurement | Baseline |
| --- | --- | --- | --- |
| Report | 3 CCU reads, 0 NSI accesses, 0 writes, 0 barriers | Host model; counter around the call | No report |
| Hook | 2 CCU reads; ≤ 9 NSI accesses per vendor-table port (1 per field already at its value); ≤ 3 per lowered-only port; ≤ 45 for DE_SYS; 0 barriers | Host model per provider | 0 OS writes |
| Steady state | 0 tasks, timers, interrupts and accesses | Linked-symbol check; host model | 0 |
| Window | Start: 2 CCU and 24 NSI accesses, 1 DSB, 2 ISB. Collect: 22 NSI accesses, 1 DSB, 1 ISB. ≤ 20 µs per phase; 20 windows/s | Host model per phase; counter around each phase | None |
| Counter headroom | No 32-bit wrap in P | Port data ≤ 2.4 × 10⁸ units at 76.8 GB/s (a 64-bit bus at 9,600 MT/s, above any LPDDR5 or LPDDR5X rate); period ≤ 3 × 10⁷ cycles at 600 MHz; latency sum below 2³² while mean DE reads outstanding < 2³² / (P × f_MBUS), which is 143 at 600 MHz | Not applicable |
| Scan-out | 0 TCON underflows with the table; mean DE read latency (sum / requests), over the run and in the worst window, lower with the table; totals ≥ 70 % of measured peak, else inconclusive | 60 s at 1280×720 XRGB8888, 60 Hz, GPU rendering plus a CPU copy loop on every core; images with and without the table | Not measured |
| Lowered masters | GPU port 6 data per window and GPU frames/s ≥ 0.95 × no-table; NPU inference time ≤ 1.05 × no-table | Scan-out run above; NPU runtime image, and under scan-out where an image runs both; hooks record inherited GPU and NPU priority | Not measured |
| DDR trip | RTF ≤ 1.05 × without the trip; time with DDR ≥ 85 °C and CPUB < 90 °C ≤ 60 s of 600 s | OS image recognizing recorded input for 10 min, 25 °C ambient, fan off, with and without the trip | Measured before the change: RTF, DDR − CPUB per sample, time each channel is ≥ 85 °C |
| Code size | ≤ 4 KiB text, ≤ 512 B data | `llvm-size` | 0 |

If the inherited GPU or NPU priority is 1 or below, the hook writes nothing
and the lowered-master bound holds trivially; the evidence states this. The
window records the mean number of DE reads outstanding per window (latency
sum divided by P × f_MBUS). P is halved if any window exceeds a quarter of the
wrap bound, because no pinned source gives DE's outstanding-read limit. Each
50 ms window averages three frames, so the worst window approximates
per-frame peaks.

The DDR trip stays even if it exceeds its bound, because refresh behavior
above 85 °C is unknown. Exceeding the bound makes the next step blocking for
this component: decode boot0's derated-refresh setting from the boot0
write-sequence trace, or show that the DRAMC applies LPDDR5 MR4 refresh-rate
changes. The trip is raised only on that evidence.

## Deliberately left out

- **Training, frequency scaling and voltage.** Boot0's opaque routine owns
  them; replacing it is deferred by project policy.
- **DRAMC fields, including LPDDR5 temperature-based refresh state.** No
  pinned source names them. The boot0 write-sequence trace, owned by this
  component, is the prerequisite; until it exists the report records the
  state as unavailable.
- **DRAM firewall check.** Boot0 requests a 120 MiB secure region whose
  registers no trace identifies. When the trace yields the range, one
  boot-time `a733_extents_overlap_or_wrap` test checks it against the OS
  memory map.
- **Unwritten ports.** GMAC 0, MSI_LITE 1, 11 and 12, EINK 3 and GIC 10 are
  neither written nor read. No OS driver uses GMAC or EINK, and no pinned
  source gives the power domain of GMAC, GIC or MSI_LITE.
- **Limiters, CPU direct channels, runtime priority changes and window ports
  without a budget.** No board use needs them; a limiter waits for a measured
  underflow.

## Changes outside this component, and host checks

- Each provider in the hook table calls the hook as specified, keeps a
  mismatched master off and proceeds on not-admitted. The framebuffer and G2D
  drivers check their port results before their first DMA.
- `apps/gpu-display/observation.c` holds the window object and calls the
  report and the window.
- Primary agent: the boot path that starts FreeRTOS copies the 32 parameter
  words and the size word into a record bound to the boot contract's epoch and
  generation.
- `checks/thermal-policy.c` sets DDR to code 1000 (108 °C) at line 113 and
  keeps that reading for the rest of the sequence. The same commit as the
  `trips[]` change moves that DDR reading below 85 °C. The new
  `checks/a733-thermal-ddr-trip.c` expects THROTTLE at code 1394 (85.02 °C),
  holds it at 1428 (83.01 °C) and releases it at 1429 (82.95 °C).
- New host-model cases, all in owned `checks/dram-*` files:
  - a not-admitted hook makes zero NSI accesses;
  - a mismatch at each port position leaves later ports untouched;
  - a GPU lease released between start and collect gives zero port 6
    accesses in collect;
  - enable writes skewed across the start phase, moving port 32 counts and
    t_end ≥ t0 + 2P each refuse or accept as specified.

## Board evidence that would confirm the design

- A report whose type, frequency and size match the wrapper log.
- Inherited and read-back values from every hook.
- The scan-out and lowered-master rows with and without the table, and whether
  the counters hold, latch or run past the period.
- The DDR trip row with its baseline.
