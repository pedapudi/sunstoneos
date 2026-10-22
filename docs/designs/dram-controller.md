# DRAM controller report and memory-bus priority

Status: design for review. No code exists. Physical behavior stays
unqualified until the board evidence in the last section exists.

The component has four parts. A read-only report records the memory
configuration that boot0 left, so evidence records name it. The display
engine's memory-bus (NSI) ports get the vendor priorities, and the GPU and NPU
ports are lowered, so display scan-out is served ahead of bulk masters. One
evidence caller gets a bandwidth and latency window. The DDR temperature
channel gets a passive trip. DRAM training, frequency scaling, voltage and the
boot0 DRAM firewall stay inside boot0.

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
  their rates as `fabric[1..2]` and `fabric_hz[1..2]`. Drivers compare the
  live words with them under mask `0x8700001f` (`a733_media_clock.c`).
- **NSI port fields.** The pinned `sunxi-nsi.c` and `sunxi-nsi.h` place port
  n's block at `0x02020000 + 0x200 × n`. Every field write in the vendor
  driver is a read-modify-write that keeps all other bits:
  - mode: bits [1:0] of `+0x10` (`nsi_port_setmode`);
  - priority: two 2-bit fields, bits [1:0] and [3:2] of `+0x14`, both set to
    the same value, with maximum 3 (`nsi_port_setpri`, `MBUS_PRI_MAX`);
  - select: bit 0 of `+0x18` (`nsi_port_setio`).

  The counters are enable bit 0 of `+0xc0`, clear bit 0 of `+0xc4`, period in
  cycles `+0xc8`, read request, write request, read data and write data counts
  at `+0xcc`, `+0xd0`, `+0xd4` and `+0xd8`, and read latency sum `+0xdc`.
  Ports 32 and 33 count total DRAM traffic, one per channel, in 64-byte units
  and NSI-clock cycles. Port 2 counts in 16-byte units and MBUS cycles
  (`master_clks`).
- **Port sharing.** The `nsi0` node of the pinned `sun60iw2p1.dtsi` lists one
  master per port and gives its power domain: DE 2, DI 4 and G2D 5 in DE_SYS,
  GPU 6 in GPU_TOP, NPU 18 in NPU, and IOMMU0 14 and IOMMU1 15 with no
  domain. The two IOMMU ports carry page-table walks for every translated
  master. Port 13 is named `usb_pcie` and is shared by USB and PCIe.
- **DRAMC registers.** The manual gives only base addresses (`0x0a020000`,
  `0x0a100000`). The component reads no DRAMC register.

## Data flow and ownership

- **Report.** A pure function fills a caller-owned record of at most 256 bytes.
  It decodes the boot0 record described under "Changes outside this
  component" when its magic, version, length, status, epoch and generation
  match the current boot contract; otherwise the memory fields read
  "unavailable". It reads `+0x580`, `+0x584` and `+0x588` once, records the
  NSI gate and reset bits, compares `+0x580` and `+0x588` with the board
  record under the shared mask, and takes the NSI and MBUS rates from
  `fabric_hz[1..2]` only when both match. It makes no NSI access and no
  write. The first caller is the `gpu-display` evidence record.
- **Port table.** A const table in the driver holds, for each written port,
  the port number, its field masks and its target: vendor values for the
  DE_SYS ports, "lower to 1" for GPU and NPU. No separate authority record
  exists.

  | Caller (after its domain is ready) | Ports and targets |
  | --- | --- |
  | `a733_display_framebuffer.c` (domain 1 only) and `apps/g2d/native.c` | IOMMU0 14 and IOMMU1 15 at 3, DE 2 at 2, DI 4 and G2D 5 at 1; mode 0 and select 1 on all five |
  | `a733_gpu_platform.c`, on entry to `A733_GPU_PLATFORM_ON` | GPU 6: lower only |
  | `a733_npu_platform.c`, before the stage becomes `COMPLETE` | NPU 18: lower only |

- **Priority hook.** `a733_dram_priority` takes one table row set. It first
  performs one mapping check of the CCU and NSI pages and reads `+0x580` and
  `+0x584`, which must show both NSI gates set and both resets deasserted. If
  either fails, it makes zero NSI accesses and returns not-admitted; the
  caller proceeds with the inherited priorities and records that. Otherwise,
  for each field it reads the word, compares under the field mask, and only
  if the field differs writes the read value with the field replaced and reads
  the word back. The read-back is compared under the same mask. For a
  lower-only port the hook touches priority alone: each 2-bit subfield above 1
  becomes 1, and subfields at 0 or 1 are left as they are. The caller stores
  each port's inherited and read-back words in its own evidence record.
- **Why each written port is quiet.** The DE_SYS callers run the hook once,
  on the step where `a733_de_sys_power_poll` first returns ready and before
  `a733_iommu_open`. At that point each caller's IOMMU admission states every
  real IOMMU master excluded and drained (`excluded_masters =
  A733_IOMMU_REAL_MASTERS`, sourced from the sealed isolation record), and
  the caller has started no DE, DI or G2D DMA. No master on ports 2, 4, 5, 14
  or 15 is therefore active. Ports 6 and 18 have one master each, which has
  not started when its platform reaches ON or COMPLETE. GPU_TOP and NPU can
  power off and on again, so their hook runs after every power-on.
- **Mismatch.** A read-back that differs under the mask leaves the port in an
  undocumented mode. The hook stops at the first mismatch and returns one
  failure; fields already written keep their values. The caller takes the
  same path as a failed power-on of its domain: the DE_SYS caller fails before
  IOMMU open, and the GPU or NPU platform fails before its first job. Each
  image links one DE_SYS caller, which owns every DE_SYS master that image
  uses, so this one check covers all of them. No master already running uses
  a written port, so a mismatch stops no running traffic.
- **Bandwidth window.** The single caller is `gpu_display_observation_poll`,
  which the `gpu-display` output task runs once per loop. The ports are DE 2
  and totals 32 and 33; the period P is 50 ms. Initialization performs one
  mapping check of the NSI page. Start makes zero NSI accesses unless the
  report recorded both fabric words matching and the NSI gates set and resets
  deasserted. `a733_npu_pll_scopes` refuses an NPU PLL change while MBUS
  selects pll-npu or pll-ddr, or NSI selects pll-ddr or pll-de-3x, and no
  other OS code writes those clocks, so the report's single compare holds for
  the session.
  - *Start* reads the counter (t0) and issues an ISB. For each of ports 2, 32
    and 33, in the vendor order: write the period P × f (1 write), read the
    enable word and write it with bit 0 clear (1 read, 1 write), read the
    clear word and write it with bit 0 set (1 read, 1 write), write the saved
    enable word with bit 0 set (1 write). That is 6 accesses per port, 18 in
    total. Then `a733_mmio_dsb_sy`, an ISB and a counter read give t1.
  - *Collect* returns pending until the counter reaches t1 + P. It reads the
    four counts of port 32 (the first sample of its read data count), the four
    counts of port 33, the four counts and the latency sum of port 2, and port
    32's read data count again: 14 reads. It then disables each port with one
    write of the enable word saved at start, bit 0 clear: 3 writes. The window
    is the only writer of these counter registers, so the saved word is
    current. Then `a733_mmio_dsb_sy`, an ISB and a counter read give t_end.
  - *Refusal.* Collect refuses if t_end ≥ t0 + 2P, which keeps every read
    inside each port's first period whatever the skew between enable writes.
    It also refuses if the two port 32 reads differ: that shows free-running
    counters, whose correct denominator the board has not yet shown. Both
    refusals happen after the disable writes, so every collect leaves the
    three ports disabled. Accepted rates are count / P.
  - *Power.* DE_SYS has no power-off path, and ports 32 and 33 have no power
    domain. Start runs only while `a733_de_sys_power_ready()` holds.
- **DDR temperature.** The DDR entry of `trips[]` in `a733_thermal.c` becomes
  {85000, 2000, 110000}. The vendor zone has only the 110 °C critical trip.
  JEDEC specifies LPDDR5 standard refresh up to 85 °C of die temperature, and
  no decoded field shows derated refresh. The input is the SoC THS "DDR"
  channel, a proxy for the die temperature with an unknown offset. A DDR
  passive trip raises the existing global THROTTLE level, which caps both
  clusters at 1014 MHz.

## Interrupts, memory and barriers

The component installs no interrupt, timer or task. The NSI interrupt
(SPI 43) has no qualified delivery and stays masked. The window adds no
wake-up: it runs in the existing `vTaskDelay(1)` loop of the output task in
`apps/gpu-display/display.c`, as performance architecture section 1.3 allows
for a device without qualified interrupt delivery. When that loop moves to
vsync or notification wake-ups, the window moves to the new wake source, or
to SPI 43 once its delivery is qualified. It never blocks or busy-waits.

Records and the window object live in normal write-back memory that no device
reads, so they need no cache maintenance. Register accesses use plain
`a733_mmio_read32` and `a733_mmio_write32`. Priority writes need no barrier:
Device-nGnRnE accesses to one peripheral complete in program order, so a
read-back returns after its write reached the port, and the caller's
DMA-enable store depends on that comparison. The window issues one `DSB SY`
per phase, because the action that follows, a counter read, is not a memory
access (performance architecture, section 1.2). t0 needs no DSB, because the
Device writes after its ISB cannot be performed earlier. Collect's first
reads need none either: Device reads are not speculative, and they depend on
the branch that tested the counter.

## Synchronization across cores

There is no shared mutable state. Each written port has one writer: the
DE_SYS caller for ports 2, 4, 5, 14 and 15 (each image links one), the GPU
platform for port 6 and the NPU platform for port 18. These callers write
disjoint port blocks, and CCU accesses are single-word reads. The window
touches only counter registers of ports 2, 32 and 33, which no hook writes.
The window object is file-static in `apps/gpu-display/observation.c`, and the
output task is its only caller. The component adds no lock and no
acquire-release pair.

## Reuse

The component reuses the board fabric record and its masked compare, the
`a733_npu_pll_scopes` refusal, the shared MMIO and mapping helpers, the
callers' power-on paths, failure paths and evidence records, the IOMMU
admission's exclusion record, the output task's existing loop, the thermal
trip table and the evidence snapshot tools. It decodes no parent PLL and has
no boot-time writer. The vendor sysfs interface, governor and limiters are
not ported.

## Performance budgets

Access counts come from host models that run the compiled code against
counting register files. Times come from the architectural counter on the
board. RTF is recognition wall time divided by audio duration.

| Metric | Target | Measurement | Baseline |
| --- | --- | --- | --- |
| Report | 3 CCU reads, 0 NSI accesses, 0 writes, 0 barriers | Host model; counter around the call | No report |
| Hook | 2 CCU reads; per field 1 NSI access if already at target, else 3; ≤ 9 per DE_SYS port, ≤ 45 per DE_SYS call; ≤ 3 per GPU or NPU call; 0 barriers | Host model per caller | 0 OS writes |
| Steady state | 0 tasks, timers, interrupts and accesses | Linked-symbol check; host model | 0 |
| Window | Start: 0 CCU and 18 NSI accesses, 1 DSB, 2 ISB. Collect: 17 NSI accesses, 1 DSB, 1 ISB. ≤ 20 µs per phase; 20 windows/s | Host model per phase; counter around each phase | None |
| Counter headroom | No 32-bit wrap in P | Port data ≤ 2.4 × 10⁸ units at 76.8 GB/s (a 64-bit bus at 9,600 MT/s, above any LPDDR5 or LPDDR5X rate); period ≤ 3 × 10⁷ cycles at 600 MHz; latency sum below 2³² while mean DE reads outstanding < 2³² / (P × f_MBUS), which is 143 at 600 MHz | Not applicable |
| Scan-out | 0 TCON underflows with the table; mean DE read latency (sum / requests), over the run and in the worst window, lower with the table; mean total of ports 32 and 33 ≥ 70 % of the calibration peak, else inconclusive | 60 s at 1280×720 XRGB8888, 60 Hz, GPU rendering plus a CPU copy loop on all eight cores; images with and without the table. The calibration peak is the highest 50 ms total of ports 32 and 33 in a 10 s run of the same load without scan-out, in the same board session | Not measured |
| Lowered masters | GPU frames/s ≥ 0.95 × no-table; NPU inference time ≤ 1.05 × no-table | Scan-out run above; NPU runtime image, and under scan-out where an image runs both; hooks record inherited GPU and NPU priority | Not measured |
| DDR trip | RTF ≤ 1.05 × without the trip; time with DDR ≥ 85 °C and CPUB < 90 °C ≤ 60 s of 600 s | OS image recognizing recorded input for 10 min, 25 °C ambient, fan off, with and without the trip. Also recorded: total time at the 1014 MHz cap and RTF over capped intervals only, as the baseline for a later per-cluster action | Measured before the change: RTF, DDR − CPUB per sample, time each channel is ≥ 85 °C |
| Code size | ≤ 4 KiB text, ≤ 512 B data | `llvm-size` | 0 |

If every inherited GPU or NPU priority subfield is 1 or below, the hook writes
nothing and the lowered-master bound holds trivially; the evidence states
this. The window records the mean number of DE reads outstanding per window
(latency sum divided by P × f_MBUS). P is halved if any window exceeds a
quarter of the wrap bound, because no pinned source gives DE's
outstanding-read limit. Each 50 ms window averages three frames, so the worst
window approximates per-frame peaks.

The DDR trip stays even if it exceeds its bound, because refresh behavior
above 85 °C is unknown. Exceeding the bound makes the next step blocking for
this component: decode boot0's derated-refresh setting from the boot0
write-sequence trace, or show that the DRAMC applies LPDDR5 MR4 refresh-rate
changes. The trip is raised only on that evidence.

## Left out, and the condition that adds each item

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
- **VE 7, 8 and 9, ISP 16 and CSI 17.** The vendor values exist, but no
  budget shows that they help scan-out, and ISP and CSI at 2 would equal DE.
  They are added when a scan-out run with those masters active shows an
  underflow or a DE latency regression.
- **USB_PCIE 13.** The port is shared by USB and PCIe, and in the PCIe images
  the board's USB link (`apps/pcie/usb0.c`) is live when PCIe powers on. No
  source maps the port to one controller, so it stays inherited.
- **GPU port 6 in the window.** GPU_TOP power belongs to the render task's
  lease, and the render task powers the GPU off on its own schedule. The
  vendor driver powers a master's domain before touching its port block, so
  an access from the output task could reach an unpowered port. GPU load is
  measured by frames per second instead.
- **The DSI scan-out image.** It powers DE through the GPU resource manager
  and has no DE_SYS caller, so its ports stay inherited until a scan-out run
  there shows an underflow.
- **Other unwritten ports.** GMAC 0, MSI_LITE 1, 11 and 12, EINK 3 and GIC 10
  are neither written nor read. No OS driver uses GMAC or EINK, and no pinned
  source gives the power domain of GMAC, GIC or MSI_LITE.
- **Limiters, CPU direct channels and runtime priority changes.** No board use
  needs them; a limiter waits for a measured underflow.

## Changes outside this component, and host checks

- `a733_display_framebuffer.c` (domain 1 instance) and `apps/g2d/native.c`
  call the hook once when DE_SYS first reports ready and before IOMMU open,
  proceed on not-admitted and fail on mismatch. `a733_de_sys_power.c` is not
  changed, so the domain 9 and 10 instances never call the hook.
- `a733_gpu_platform.c` calls the hook on entry to ON and `a733_npu_platform.c`
  before COMPLETE, with the same result handling.
- `apps/gpu-display/observation.c` holds the window object and calls the
  report and the window.
- Primary agent: the boot path that starts FreeRTOS writes a 168-byte boot0
  record, little-endian:

  | Offset | Field |
  | --- | --- |
  | 0 | magic, `u32` |
  | 4 | version, `u32` (1) |
  | 8 | length, `u32` (168) |
  | 12 | status, `u32` (0 copied, otherwise the failure) |
  | 16 | boot contract epoch, `u64` |
  | 24 | boot contract generation, `u64` |
  | 32 | parameter words 0..31, 32 × `u32`, from header `0x30..0xac` |
  | 160 | size in MiB, `u32`, from header `0x2c` |
  | 164 | reserved, `u32` (0) |

- `checks/thermal-policy.c` sets DDR to code 1000 (108 °C) at line 113 and
  keeps that reading for the rest of the sequence. The same commit as the
  `trips[]` change moves that DDR reading below 85 °C. The new
  `checks/a733-thermal-ddr-trip.c` expects THROTTLE at code 1394 (85.02 °C),
  holds it at 1428 (83.01 °C) and releases it at 1429 (82.95 °C).
- New host-model cases, all in owned `checks/dram-*` files:
  - a not-admitted hook makes zero NSI accesses;
  - a mismatch at each port and field position leaves later fields untouched;
  - words seeded with non-zero unrelated bits keep them after every write, and
    a mismatch outside the field mask is not reported;
  - unequal priority subfields, such as 3 and 1, or 2 and 0, give the
    specified result for vendor and lower-only ports;
  - a start before the report admitted the fabric makes zero NSI accesses;
  - enable writes skewed across the start phase, moving port 32 counts and
    t_end ≥ t0 + 2P each refuse or accept as specified, and every collect
    ends with three disable writes.

## Board evidence that would confirm the design

- A report whose type, frequency and size match the wrapper log.
- Inherited and read-back words from every hook.
- The scan-out and lowered-master rows with and without the table, and whether
  the counters hold, latch or run past the period.
- The DDR trip row with its baseline and capped-interval figures.
- The offset between the THS DDR channel and the DRAM die: a thermocouple on
  the LPDDR5 package top read alongside the channel across the DDR trip run,
  and an MR4 reading once the boot0 trace identifies the mode-register read.
