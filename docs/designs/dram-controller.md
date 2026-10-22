# DRAM controller report and memory-bus priority

Status: design for review. No code exists. Physical behavior stays
unqualified until the board evidence in the last section exists.

The component has four parts. A read-only report decodes the memory
configuration that boot0 recorded and checks the fabric clocks. The display
engine's memory-bus (NSI) ports get the vendor priorities, and in the one image
that also renders on the GPU the GPU port is lowered, so scan-out is served
ahead of bulk traffic. One evidence image measures bandwidth and DE read
latency in fixed windows. The DDR temperature channel gets a passive trip.
DRAM training, frequency scaling, voltage and the boot0 DRAM firewall stay
inside boot0.

## Sources

- **Boot0 parameters.** Boot0 writes 32 parameter words at monitor header
  offset `0x30..0xac` and the size in MiB at `0x2c`. The FEL DDR wrapper run
  of 2026-10-15 recorded word 0 = `0x7f8` (2040 MHz) and word 1 = 9, which
  the pinned vendor `sunxi_dram_info.c` and the boot0 log identify as LPDDR5.
  Word 24 = `0x13090603` gives trained frequencies of 2040, 1165, 816 and
  408 MHz by the vendor rule 4 × 2040 / (d + 1). The size is 12,288 MiB.
  Words 6, 7 and 30 have no pinned field definition and are recorded raw.
- **Cold report.** On every cold entry, `boot/a733/cold-entry.S` clears the
  512-byte report at `0x47008000` and copies all 264 monitor header bytes to
  report offsets 192..455, so header byte h is report byte 192 + h. After a
  DSB it publishes magic `0x41533733434f4c44`, version 1, size 512 and the
  status. It runs with the MMU and caches off (`docs/cold-entry.md`).
- **Stage-2 report.** The stage-2 loader never writes the cold report. Its
  own report at `0x47008200` ends with status 6 (entered), the armed counter
  at offset 112 and the slot generation at offset 48. The OS session it
  enters carries the same two values as its epoch and generation. Both reports
  lie in the reserved monitor interval `0x47000000..0x47400000`, outside every
  image section and device pool.
- **Fabric clocks.** `NSI_CLK_REG` (CCU `+0x580`: gate bit 31, reset bit 30),
  the NSI configuration word (`+0x584`: gate bit 0, reset bit 16) and
  `MBUS_CLK_REG` (`+0x588`), per manual section 4.1.6. Each image's sealed
  board session carries the expected `+0x580` and `+0x588` words and rates as
  `fabric[1..2]` and `fabric_hz[1..2]`, which drivers compare with the live
  words under mask `0x8700001f`.
- **NSI port fields.** The pinned `sunxi-nsi.c` and `sunxi-nsi.h` place port
  n's block at `0x02020000 + 0x200 × n`. Every vendor field write is a
  read-modify-write that keeps all other bits: mode, bits [1:0] of `+0x10`;
  priority, two 2-bit fields at bits [1:0] and [3:2] of `+0x14`, both set to
  the same value, maximum 3; select, bit 0 of `+0x18`. The counters are enable
  bit 0 of `+0xc0`, clear bit 0 of `+0xc4`, period in cycles `+0xc8`, read
  request, write request, read data and write data counts at `+0xcc..+0xd8`,
  and read latency sum `+0xdc`. Ports 32 and 33 count total DRAM traffic, one
  per channel, in 64-byte units and NSI cycles. Port 2 counts in 16-byte units
  and MBUS cycles.
- **Port sharing.** The `nsi0` node of the pinned `sun60iw2p1.dtsi` lists one
  master per port and its power domain: DE 2, DI 4 and G2D 5 in DE_SYS, GPU 6
  in GPU_TOP, NPU 18 in NPU, and IOMMU0 14 and IOMMU1 15 with no domain. The
  IOMMU ports carry page-table walks for every translated master.
- **DRAMC registers.** The manual gives only base addresses. The component
  reads no DRAMC register.

## Data flow and ownership

- **Report.** One call per image fills a caller-owned record. It first checks
  one translation of the `0x47000000` block (normal write-back, EL1 readable),
  which the EL1 maps of `platform/a733/platform.c` and
  `a733_cpu_coherence_tables` already provide. EL3 wrote the reports with
  caches off and the image maps them cacheable, so the report cleans and
  invalidates the six lines it reads to the point of coherency and issues one
  `DSB SY`. The lines are cold report lines 0, 3, 4 and 5, which hold the
  report header and header bytes `0x2c..0xac`, and stage-2 report lines 0
  and 1. The memory fields are decoded only if the cold report shows magic,
  version 1, size 512 and status 1, and the stage-2 report shows status 6
  with armed counter and generation equal to the image's session epoch and
  generation. A FEL-loaded image runs no cold entry, so a report left by an
  earlier cold boot can remain in DRAM across a warm reset; the stage-2 match
  rejects it. Otherwise the memory fields read "unavailable" with the failed
  condition. The report reads `+0x580`, `+0x584` and `+0x588` once, records
  the NSI gate and reset bits, and takes the NSI and MBUS rates from
  `fabric_hz[1..2]` only when both words match under the mask. It writes
  nothing.
- **Port table.** A const table holds each written port's number, field masks
  and target: IOMMU0 14 and IOMMU1 15 at 3, DE 2 at 2, DI 4 and G2D 5 at 1,
  with mode 0 and select 1, for the DE_SYS callers; "lower to 1" for GPU 6.
- **Priority hook.** `a733_dram_priority(ports, io, evidence)` makes every
  access through an `io` pair, `read(context, address, &value)` and
  `write(context, address, value)`, each returning 1 on success, so the host
  models run it against counting register files. Each call reads `+0x580` and
  `+0x584` and requires both NSI gates set and both resets deasserted, because
  those bits can change between power-ons. If they are not, it makes zero NSI
  accesses and returns not-admitted; the caller proceeds with the inherited
  priorities and records that. Otherwise, for each field it reads the word and,
  only if the field differs from the target, writes the read value with the
  field replaced and reads the word back, comparing under the field mask. For
  the lowered GPU port each priority subfield above 1 becomes 1, and subfields
  at 0 or 1 stay. The evidence holds each port's inherited and final fields.
- **Mapping checks, once per caller.** Each `io` implementation checks its
  pages once, at its own initialization: the CCU page `0x02002000` and each
  NSI page it touches must translate to themselves as Device-nGnRnE (MAIR
  attribute `0x00`). The DE_SYS callers use `a733_dram_native_io` in
  `drivers/dram/`, whose address allowlist is the table's words. The GPU
  native adapter checks the NSI page in `a733_gpu_platform_native_init`. The
  page tables are sealed before device admission and no later code changes
  them; a change of the tables is the event that makes these checks stale.
  When the table owner publishes a mapping generation (performance
  architecture section 1.5), each `io` re-checks on a generation change.
- **DE_SYS callers.** `a733_display_framebuffer.c` (domain 1 instance, linked
  by the gpu-display, HDMI and DisplayPort images) and `apps/g2d/native.c`
  run the hook on the step where `a733_de_sys_power_poll` first returns ready,
  before `a733_iommu_open`. Each sets a per-instance done flag when the hook
  succeeds or returns not-admitted, because the poll returns ready on every
  later call. At that point the caller's IOMMU admission states every real
  IOMMU master excluded and drained (`excluded_masters =
  A733_IOMMU_REAL_MASTERS`) and the caller has started no DE, DI or G2D DMA,
  so no master on ports 2, 4, 5, 14 or 15 is active.
- **GPU caller.** `a733_gpu_platform.c` gains a `bus_priority` pointer, zero
  after the required zeroing of its storage. Only gpu-display sets it, because
  only that image has scan-out to protect while the GPU runs; the GPU-render
  and accelerator images keep the inherited priority. When set, the platform
  runs the hook on each entry to `A733_GPU_PLATFORM_ON`, before the GPU's first
  job, through a shim that maps NSI addresses to a new bank `A733_GPU_NSI` of
  its existing ops. The native adapter's allowlist gains reads of CCU `0x580`
  and `0x584` and of port 6 `+0x14`, and writes of port 6 `+0x14`.
- **Mismatch.** A read-back that differs under the mask leaves the port in an
  undocumented mode. The hook stops at the first mismatch; fields already
  written keep their values. The DE_SYS caller fails before IOMMU open and the
  GPU platform fails before its first job, as for a failed domain power-on. No
  running master uses a written port, so a mismatch stops no traffic.
- **Bandwidth window.** Ports DE 2 and totals 32 and 33; the period P is a
  compile-time constant of 50 ms. It runs only in the load image described
  under "Measurement". Its initialization checks the NSI counter pages once,
  as above. Start makes zero NSI accesses unless the report found both fabric
  words matching and the NSI gates and resets admitted, and
  `a733_de_sys_power_ready()` holds. `a733_npu_pll_scopes` refuses an NPU PLL
  change while MBUS or NSI selects a PLL it would affect, and no other OS code
  writes those clocks, so the report's single compare holds for the session.
  Every time value comes from an ISB followed by `MRS CNTPCT_EL0`, with a
  per-window monotonic compare; the window takes no lock.
  - *Start* reads the counter (t0). For each of ports 2, 32 and 33, in the
    vendor order: write the period P × f; read the enable word and write it
    with bit 0 clear; read the clear word and write it with bit 0 set; write
    the saved enable word with bit 0 set. That is 6 accesses per port, 18 in
    total. Then `DSB SY` and a counter read give t1.
  - *Collect* returns pending until the counter passes t1 + P. It reads the
    four counts of port 32 (the first sample of its read data count), of port
    33, and of port 2 with its latency sum, then port 32's read data count
    again: 14 reads. It disables each port with one write of the saved enable
    word, bit 0 clear: 3 writes. The window is the only writer of these
    registers, so the saved word is current. Then `DSB SY` and a counter read
    give t_end.
  - *Refusal.* Enables happen between t0 and t1, so the reads fall after the
    end of each port's first period. Collect refuses if t_end ≥ t0 + 2P, which
    keeps them before the end of each port's second period. It refuses if the
    two port 32 reads differ, which shows free-running counters whose
    denominator the board has not yet shown. That test works only if channel
    0 carries traffic between the two reads. Collect therefore also refuses a
    window whose port 32 read data count is below 1,024 units (64 KiB), as a
    proxy for that traffic; scan-out alone moves about 11 MB per window, or
    5.5 MB per channel if the two channels share it evenly. Every collect
    ends with the three disable writes.
- **DDR temperature.** The DDR entry of `trips[]` in `a733_thermal.c` becomes
  {85000, 2000, 110000}. The vendor zone has only the 110 °C critical trip.
  JEDEC specifies LPDDR5 standard refresh up to 85 °C die temperature, and no
  decoded field shows derated refresh. The input is the SoC THS "DDR" channel,
  a proxy with an unknown offset. A DDR passive trip raises the existing global
  THROTTLE level, which caps both clusters at 1014 MHz.

## Interrupts, memory and barriers

The component installs no interrupt, timer or task. The NSI interrupt
(SPI 43) has no qualified delivery and stays masked. The window adds no
wake-up: it runs in the output task's existing `vTaskDelay(1)` loop in
`apps/gpu-display/display.c`, which performance architecture section 1.3
allows for a device without qualified interrupt delivery. Its pending check is
one ISB, one counter read and one compare per loop pass, about 1,000 per
second. When that loop moves to vsync wake-ups, the window moves with it. It
never blocks or busy-waits.

Records and window state live in normal write-back memory that no device
reads, so they need no cache maintenance. The report's line maintenance is
the only exception, for the reason given above. The hook issues no barrier:
Device-nGnRnE accesses to one peripheral complete in program order, so a
read-back returns after its write reached the port, and the caller's DMA
enable depends on that comparison. The GPU adapter's existing write path keeps
its own DSBs. The window issues one `DSB SY` per phase, because the next
action, a counter read, is not a memory access (performance architecture
section 1.2). Device reads are not speculative and collect's reads depend on
the branch that tested the counter, so they need no leading barrier.

## Synchronization across cores

Each written port has one writer: the DE_SYS caller for ports 2, 4, 5, 14 and
15 (each image links one) and the GPU platform for port 6. They write disjoint
port blocks, and CCU accesses are single-word reads. The window touches only
counter registers of ports 2, 32 and 33, which no hook writes. Window state and
aggregates are written and read by the output task alone, which also fills the
observation reply. The load image adds two atomics: a phase word the output
task stores with release and the render and copy tasks load with acquire, and
a render count the render task stores with release and the output task loads
with acquire. The component adds no lock.

## Reuse

The component reuses the cold report, the stage-2 report, the session's epoch
and generation, the board fabric record and its masked compare, the
`a733_npu_pll_scopes` refusal, the shared MMIO and mapping helpers, the
callers' power-on and failure paths, the IOMMU admission's exclusion record,
the output task's loop, the observation reply, the HDMI underflow check, the
device-memory allocator and the thermal trip table. It adds no boot record and
needs no change from the primary agent.

## Measurement

The window has one producer, a load mode of gpu-display in
`apps/gpu-display/dram-controller-load.c`, compiled only with
`SUNSTONE_GPU_DISPLAY_DRAM_LOAD=ON`. Two images are built from it: one with
the port table and one with the hooks compiled out. The normal gpu-display
image keeps its two frames and three presentations.

- **Session.** The image runs inside the existing 20 s diagnostic window
  (`HDMI_WINDOW_TICKS`), and the output task stops HDMI at `stop_at`, 2 s
  before the deadline. After the third presentation, slot 0 stays on scan-out
  at 1280×720 XRGB8888, 60 Hz. Phase A, calibration, lasts 2 s (40 windows)
  with the copy load only. Phase B runs the copy load and the GPU loop until
  `stop_at` − 1 s. Phase B needs at least 8 s (160 windows); a shorter phase B
  is recorded as inconclusive, and lengthening the diagnostic window for this
  image is then a reviewed change. A missed deadline fails the run as any
  session failure does.
- **Copy load.** Eight tasks at priority 1, below every existing task, so
  they use only idle time. Each is pinned with `vTaskCoreAffinitySet` to one
  core: A55 cores 0–5 and A76 cores 6–7. A 32 MiB CPU-only lease from the
  write-back device pool gives each task its own 2 MiB source and 2 MiB
  destination, which exceeds the caches; no device is granted the lease.
- **GPU loop.** The GPU is closed before scan-out starts, so in phase B the
  render task opens the GPU service again and repeats BEGIN, WRITE and SUBMIT
  of frame 2's input, waiting for COMPLETE, without READ, then closes it at the
  end of phase B. The render count over phase B gives GPU renders per second.
  The GPU service host model must accept OPEN after STOPPED; until it does,
  the GPU rows stay unmeasured.
- **Calibration with scan-out on.** DE_SYS powers only on the way to scan-out
  in this image, so phase A includes scan-out at 221 MB/s, under 1 % of the
  76.8 GB/s bound below, and is not separated from it.
- **Transport.** The observation reply's `terminal.reserved[136]` becomes a
  136-byte evidence block, and the reply version becomes 2; the 1,024-byte
  size and its static assert stay. The normal image fills the report and port
  fields; the load image fills all of them.

  | Offset | Fields |
  | --- | --- |
  | 0 | `u16` ×8: windows started, accepted, refused late, refused idle, refused free-running, windows above a quarter of the wrap bound, phase A windows, worst-window index |
  | 16 | `u32` ×5: phase A maximum of ports 32 + 33 data, port 32 maximum, port 33 maximum, port 2 read data maximum, worst-window port 2 read requests |
  | 36 | `u32`: GPU renders completed in phase B |
  | 40 | `u64` ×7: phase B sums of port 32 data, port 33 data, port 2 read data, port 2 read requests and port 2 latency; worst-window latency sum; phase B length in µs |
  | 96 | `u16` ×12: inherited and final fields of ports 2, 4, 5, 14, 15 and 6 (mode bits 1–0, priority bits 5–2, select bit 6) |
  | 120 | `u32` ×3: CCU `+0x580`, `+0x584`, `+0x588` |
  | 132 | `u32`: report result (memory decode state, fabric match, NSI admitted, hook results) |

  Data means read plus write data counts. The worst window is the accepted
  phase B window with the highest mean DE read latency, compared by cross
  multiplication. The host tool derives means, rates (count / P) and the mean
  DE reads outstanding, latency sum / (P × f_MBUS).

## Performance budgets

Access counts come from host models that run the compiled code against
counting register files. Times come from the architectural counter on the
board. RTF is recognition wall time divided by audio duration.

| Metric | Target | Measurement | Baseline |
| --- | --- | --- | --- |
| Report | 1 translation, 6 line operations, 1 DSB, 3 CCU reads, 0 NSI accesses, 0 writes, once per image | Host model over the bytes the cold-entry instruction check produces, plus stale and absent cases | No report |
| Hook | 2 CCU reads per call; per field 1 NSI access if at target, else 3; ≤ 9 per DE_SYS port, ≤ 45 per DE_SYS call, ≤ 3 per GPU call; 0 translations and 0 hook barriers per call | Host model per caller: native `io` model for DE_SYS, GPU platform model through its ops | 0 OS writes |
| Mapping checks | One per page per `io` instance, at initialization | Host model counts translation calls | Not applicable |
| Steady state | 0 tasks, timers, interrupts and accesses outside the load image | Linked-symbol check; host model | 0 |
| Window | Start: 18 NSI accesses, 1 DSB, 2 ISB. Collect: 17 NSI accesses, 1 DSB, 1 ISB. Pending check: 1 ISB, 1 counter read, 0 locks. ≤ 20 µs per phase; 20 windows/s | Host model per phase; counter around each phase | None |
| Counter headroom | No 32-bit wrap in P | Port data ≤ 2.4 × 10⁸ units at 76.8 GB/s (64-bit bus at 9,600 MT/s, above any LPDDR5 or LPDDR5X rate); period ≤ 3 × 10⁷ cycles at 600 MHz; latency sum below 2³² while mean DE reads outstanding < 143 at 600 MHz | Not applicable |
| Scan-out | HDMI stays `ACTIVE` with no `A733_HDMI_UNDERFLOW` in phase B; mean DE read latency over phase B, and in the worst window, lower with the table; mean port 32 + 33 total over phase B ≥ 70 % of the phase A maximum, else inconclusive | Load images with and without the table, in one board session | Not measured |
| Lowered GPU | Phase B GPU renders/s ≥ 0.95 × the no-table image | Same runs | Not measured |
| DDR trip | RTF ≤ 1.05 × without the trip; time with DDR ≥ 85 °C and CPUB < 90 °C ≤ 60 s of 600 s | OS image recognizing recorded input for 10 min, 25 °C ambient, fan off, with and without the trip; also total time at the 1014 MHz cap and RTF over capped intervals | Before the change: RTF, DDR − CPUB per sample, time each channel is ≥ 85 °C |
| Code size | ≤ 4 KiB text, ≤ 512 B data outside the load mode | `llvm-size` | 0 |

If the inherited GPU priority subfields are both 1 or below, the hook writes
nothing and the lowered-GPU bound holds trivially; the evidence shows this.
P stays 50 ms at run time. No pinned source gives DE's outstanding-read limit,
so a window whose latency sum exceeds a quarter of the wrap bound is counted
in the evidence block; a nonzero count leads to a reviewed change of the
constant. Each 50 ms window averages three frames, so the worst window
approximates per-frame peaks.

The DDR trip stays even if it exceeds its bound, because refresh behavior
above 85 °C is unknown. Exceeding the bound makes the next step blocking for
this component: decode boot0's derated-refresh setting from the boot0
write-sequence trace, or show that the DRAMC applies LPDDR5 MR4 refresh-rate
changes. The trip is raised only on that evidence.

## Left out, and the condition that adds each item

- **Training, frequency scaling and voltage.** Boot0's opaque routine owns
  them; replacing it is deferred by project policy.
- **Decoded memory fields on the board today.** Only an image entered through
  the stage-2 loader can pass the report's checks, and the OS image is the
  only such image; its 128-byte result record has no spare field. The fields
  are added to that record when it gains a version with room. FEL sessions
  keep the FEL DDR wrapper log as their memory evidence.
- **DRAMC fields, including temperature-based refresh state.** No pinned
  source names them. The boot0 write-sequence trace, owned by this component,
  is the prerequisite.
- **DRAM firewall check.** Boot0 requests a 120 MiB secure region whose
  registers no trace identifies. When the trace yields the range, one
  boot-time `a733_extents_overlap_or_wrap` test checks it against the OS
  memory map.
- **NPU port 18.** No image links both the NPU platform and a DE_SYS caller,
  so lowering it protects no scan-out. It is added when such an image exists.
- **VE 7, 8 and 9, ISP 16 and CSI 17.** No budget shows that their vendor
  values help scan-out. They are added when a scan-out run with those masters
  active shows an underflow or a DE latency regression.
- **USB_PCIE 13.** USB and PCIe share the port, and the board's USB link is
  live when PCIe powers on. No source maps the port to one controller.
- **GPU port 6 in the window.** The render task powers GPU_TOP off on its own
  schedule, and the vendor driver powers a master's domain before touching its
  port block. GPU load is measured by renders per second instead.
- **The DSI scan-out and deinterlace images.** The DSI image powers DE through
  the GPU resource manager, and the deinterlace image runs DI on port 4
  through an inherited domain-1 path in `apps/deinterlace/native.c`. Neither
  links a DE_SYS caller, so their ports stay inherited until a scan-out run
  there shows an underflow.
- **Other unwritten ports.** GMAC 0, MSI_LITE 1, 11 and 12, EINK 3 and GIC 10
  are neither written nor read. No OS driver uses GMAC or EINK, and no pinned
  source gives the power domain of GMAC, GIC or MSI_LITE.
- **Limiters, CPU direct channels, runtime priority changes and an adaptive
  window period.** No board use needs them; a limiter waits for a measured
  underflow.

## Changes outside this component, and host checks

- `a733_display_framebuffer.c` (domain 1 instance) and `apps/g2d/native.c`
  call the hook once, guarded by the done flag. `a733_de_sys_power.c` is not
  changed, so the domain 9 and 10 instances never call it.
- `a733_gpu_platform.c` gains `bus_priority` and the ON-entry call;
  `a733_gpu_power.h` gains the `A733_GPU_NSI` bank;
  `a733_gpu_platform_native.c` gains the allowlist entries and its one
  mapping check. `apps/gpu-display/render.c` sets `bus_priority`.
- `apps/gpu-display/observation.h` replaces `terminal.reserved[136]` with the
  evidence block and the reply version becomes 2. `observation.c` calls the
  report; `display.c` calls the window step from the output loop in the load
  build only.
- `checks/thermal-policy.c` sets DDR to code 1000 (108 °C) at line 113 and
  keeps it there. The commit that changes `trips[]` moves that reading below
  85 °C. The new `checks/a733-thermal-ddr-trip.c` expects THROTTLE at code
  1394 (85.02 °C), holds it at 1428 (83.01 °C) and releases it at 1429
  (82.95 °C).
- New host-model cases in `checks/dram-*` files:
  - the report decodes the cold-entry check's bytes, and reads "unavailable"
    for a wrong magic, version, size or status, a missing stage-2 entry and a
    stage-2 epoch or generation that differs from the session;
  - a not-admitted hook makes zero NSI accesses, and a repeated DE_SYS poll
    makes no second hook call;
  - a mismatch at each port and field position leaves later fields untouched;
  - non-zero unrelated bits survive every write, and a difference outside the
    field mask is not a mismatch;
  - unequal priority subfields, such as 3 and 1, or 2 and 0, give the
    specified result for vendor and lowered ports;
  - the GPU platform model runs the hook through its ops and the adapter
    refuses any NSI address outside its allowlist;
  - a start before the report admitted the fabric makes zero NSI accesses;
  - skewed enable writes, t_end ≥ t0 + 2P, differing port 32 reads and a port
    32 count below the floor each refuse or accept as specified, and every
    collect ends with three disable writes.

## Board evidence that would confirm the design

- A report from a stage-2-entered image whose type, frequency and size match
  the wrapper log, and "unavailable" from a FEL-loaded image.
- Inherited and final fields from every hook.
- The scan-out and lowered-GPU rows from both load images, and whether the
  counters hold, latch or run past the period.
- The DDR trip row with its baseline and capped-interval figures.
- The offset between the THS DDR channel and the DRAM die: a thermocouple on
  the LPDDR5 package read alongside the channel across the DDR trip run, and an
  MR4 reading once the boot0 trace identifies the mode-register read.
