# DRAM controller and memory-bus priority

Status: design for review. No code exists. Physical behavior stays
unqualified until the board evidence in the last section exists.

The component has three parts:

- a fixed priority table for the memory-bus ports that serve display scan-out,
  written once per display power-on with read-back;
- a bandwidth and latency window that runs in one load image;
- a passive trip on the DDR temperature channel.

DRAM training, frequency scaling, voltage and the boot0 DRAM firewall stay
inside boot0. Decoding the boot0 memory parameters is left out until an image
can carry the decoded values; the last sections give the conditions.

## Sources

- **Fabric clocks.** `NSI_CLK_REG` (CCU `+0x580`: gate bit 31, reset bit 30),
  the NSI configuration word (`+0x584`: gate bit 0, reset bit 16) and
  `MBUS_CLK_REG` (`+0x588`), per manual section 4.1.6. Each image's sealed
  board session carries the expected `+0x580` and `+0x588` words and their
  rates as `fabric[1..2]` and `fabric_hz[1..2]`. Drivers compare the live
  words with these under mask `0x8700001f`.
- **NSI port fields.** The pinned `sunxi-nsi.c` and `sunxi-nsi.h` place the
  register block of NSI port n at `0x02020000 + 0x200 × n`. Every vendor field
  write is a read-modify-write that keeps the other bits: mode, bits [1:0] of
  `+0x10`; priority, two 2-bit fields at bits [1:0] and [3:2] of `+0x14`, both
  set to the same value, maximum 3; select, bit 0 of `+0x18`. The counters are
  enable bit 0 of `+0xc0`, clear bit 0 of `+0xc4`, the period in cycles at
  `+0xc8`, read request, write request, read data and write data counts at
  `+0xcc..+0xd8`, and the read latency sum at `+0xdc`. Ports 32 and 33 count
  the total DRAM traffic of one channel each, in 64-byte units and NSI cycles.
  Port 2 counts in 16-byte units and MBUS cycles.
- **Port owners.** The `nsi0` node of the pinned `sun60iw2p1.dtsi` assigns DE
  to port 2 in the DE_SYS power domain and the IOMMU0 and IOMMU1 page-table
  walkers, which serve DE's translated reads, to ports 14 and 15.

## Data flow and ownership

- **Port table.** A const table holds DE 2 at priority 2 and IOMMU0 14 and
  IOMMU1 15 at priority 3, each with mode 0 and select 1, the vendor values.
- **Priority hook.** `a733_dram_priority(io, evidence)` makes every access
  through an `io` pair, `read(context, address, &value)` and
  `write(context, address, value)`. This is the callback form that
  `a733_cpu_clock_report` already uses, and it lets the host models run the
  compiled hook against a counting register file. The hook reads `+0x580` and
  `+0x584` and requires both NSI gates set and both resets deasserted. If they
  are not, it makes zero NSI accesses and returns not-admitted. Otherwise, for
  each field it reads the word. Only if the field differs from the target does
  it write the read value with the field replaced and read the word back,
  comparing under the field mask. The evidence holds each port's inherited and
  final fields.
- **Caller.** The domain 1 instance of `a733_display_framebuffer.c`, linked by
  the gpu-display, HDMI and DisplayPort images, runs the hook on the step where
  `a733_de_sys_power_poll` first returns ready, before `a733_iommu_open`. A
  done flag stops a second call, because the poll returns ready on every later
  step. At that point the IOMMU admission states every real IOMMU master
  excluded and drained (`excluded_masters = A733_IOMMU_REAL_MASTERS`) and DE
  has started no DMA, so no master on ports 2, 14 or 15 is active. In
  gpu-display this step runs in the output task, which also fills the
  observation reply.
- **Mapping checks.** The native `io` in `drivers/dram/` checks its pages once
  at initialization. The CCU page `0x02002000` and every other page it touches
  must translate to themselves as Device-nGnRnE (MAIR attribute `0x00`). Its
  address allowlist holds the table's words and, in the load image, the
  window's words and the reads that `a733_cpu_clock_address` permits. The page
  tables are sealed before device admission and no later code changes them.
  A page-table change would make the checks stale; none exists today. A failed
  check marks the `io` unusable: the hook returns not-admitted with zero NSI
  accesses, the caller records that and scans out with the inherited
  priorities, and the window refuses every start.
- **Mismatch.** A read-back that differs under the mask leaves the port in an
  undocumented mode. The hook stops at the first mismatch, and the caller takes
  its existing failure path for a failed DE_SYS power-on, before IOMMU open and
  before any DE DMA.
- **Bandwidth window.** Ports 2, 32 and 33 over a period P of 50 ms, in the
  load image only. Its initialization reads `+0x580`, `+0x584` and `+0x588`
  once and admits windows only if both fabric words match the board record and
  the NSI gates and resets are set. No OS code writes those clocks, and
  `a733_npu_pll_scopes` refuses an NPU PLL change that would affect the PLL
  MBUS or NSI selects, so the single compare holds for the session.
  Initialization also records the configured CPU clocks once through
  `a733_cpu_clock_report`. Every time value comes from an ISB followed by
  `MRS CNTPCT_EL0`.
  - *Start* runs only while the display stage is 3, the application state is
    1, three presentations are done, the current phase has not ended and
    `a733_de_sys_power_ready()` holds. It reads the counter (t0). For each
    port, in the vendor order, it makes 6 accesses, 18 in total: it writes the
    period, clears enable bit 0 by read and write, sets clear bit 0 by read
    and write, and writes the saved enable word with bit 0 set. The period is P × `fabric_hz[2]` (MBUS) for port 2 and
    P × `fabric_hz[1]` (NSI) for ports 32 and 33. Then `DSB SY` and a counter
    read give t1.
  - *Collect* returns pending until the counter passes t1 + P. If
    `a733_de_sys_power_ready()` no longer holds, it abandons the window: it
    disables ports 32 and 33, makes no port 2 access and counts the window as
    abandoned. Otherwise it reads port 32's four counts, port 33's four
    counts, port 2's four counts and latency sum, and port 32's read data count
    again: 14 reads. It disables each port with one write of the saved enable
    word with bit 0 clear, 3 writes, then issues `DSB SY` and reads the counter
    (t_end).
  - *Refusal.* Collect refuses a window if t_end ≥ t0 + 2P, which would place
    the reads after the end of a port's second period, or if the two port 32
    reads differ, which shows free-running counters. That second test needs
    channel 0 traffic, so collect also refuses a window whose port 32 read data
    count is below 1,024 units (64 KiB). Scan-out alone moves about 5.5 MB per
    window, about 2.8 MB per channel if the two channels share it evenly.
  - *Stop.* The output step aborts an in-flight window, with the abandon rule
    above for port 2, before it calls `a733_hdmi_stop` at `stop_at` and at the
    start of `output_fault`. No window starts after phase B ends, once the
    stage reaches 4 or once the state is 3. The PMIC park request comes after
    the stop, so it never meets a window in flight.
- **DDR temperature.** The DDR entry of `trips[]` in `a733_thermal.c` becomes
  {85000, 2000, 110000}. The vendor zone has only the 110 °C critical trip.
  JEDEC specifies LPDDR5 standard refresh up to 85 °C die temperature, and no
  decoded field shows derated refresh. The input is the SoC THS "DDR" channel,
  a proxy with an unknown offset from the DRAM die. A DDR passive trip raises
  the existing global THROTTLE level, which caps both CPU clusters at
  1014 MHz.

## Interrupts, memory, barriers and cores

The component installs no interrupt, timer or task. The NSI interrupt
(SPI 43) has no qualified delivery and stays masked. The window adds no
wake-up: its step runs in the output task's existing `vTaskDelay(1)` loop in
`apps/gpu-display/display.c`, a tick wait that performance architecture
section 1.3 permits for a device without qualified interrupt delivery. Its
pending check is one ISB, one counter read and one compare per pass, about
1,000 per second. It never blocks or busy-waits.

Records and window state live in normal write-back memory that no device
reads, so they need no cache maintenance. The hook issues no barrier.
Device-nGnRnE accesses to one peripheral complete in program order, so a
read-back returns after its write reached the port, and DE's DMA enable
depends on that comparison. The window issues one `DSB SY` per phase because
the next action, a counter read, is not a memory access (section 1.2).

Each port has one writer. The hook writes ports 2, 14 and 15 and the window
writes only the counter registers of ports 2, 32 and 33. Both run in the
output task, which also reads the evidence. The load image's copy tasks share
one phase word with the output task. The output task wakes them with a task
notification at the start of phase B and stores the end phase with release,
and each task loads the word with acquire once per 2 MiB pass. The component
adds no lock.

## Failure classes

- Not admitted, or mapping check failed: class 1, refused before any NSI
  access. Scan-out continues with the inherited priorities; nothing is
  retained.
- Read-back mismatch: the caller's existing DE_SYS power-on failure path, with
  no DE DMA started; it retains what that path retains.
- Window refused or abandoned: class 1. The window is counted and its ports
  disabled; nothing is retained.

## Reuse

The component reuses the board fabric record and its masked compare, the
`a733_npu_pll_scopes` refusal and `a733_cpu_clock_report`. It also reuses the
shared MMIO and mapping helpers, the framebuffer's power-on and failure paths,
the IOMMU exclusion record, the output task's loop and observation reply, the
HDMI underflow check, the device-memory allocator and the thermal trip table.
It removes nothing, adds no boot record and needs no change from the primary
agent.

## Measurement

A load mode of gpu-display, `apps/gpu-display/dram-controller-load.c`,
compiled only with `SUNSTONE_GPU_DISPLAY_DRAM_LOAD=ON`, produces the windows.
Two images are built from it, one with the port table and one with the hook
compiled out, and run in one board session.

- **Session.** After the third presentation, slot 0 stays on scan-out at
  1280×720 RGB565, 60 Hz: 110.6 MB/s. Phase A lasts 2 s (40 windows) with
  scan-out only and gives the uncontended DE latency. Phase B runs scan-out and
  the copy load until `stop_at` − 1 s and needs at least 8 s (160 windows).
  `stop_at` lies 18 s into the 20 s session, so phase B is long enough when the
  third presentation comes at most 7 s into the session. In the gpu-display
  host model it comes at 0.27 s, which leaves 14.7 s. On the board the
  presentation record's `observed_us` gives the time. A later presentation
  makes the run inconclusive, and the next load image then needs a 30 s
  diagnostic window, a change to the monitor's session arming that the
  primary agent owns.
- **Copy load.** Eight tasks at priority 1, below every existing task, pinned
  one per core with `vTaskCoreAffinitySet` (A55 cores 0–5, A76 cores 6 and 7).
  A 32 MiB CPU-only lease from the write-back device pool gives each task a
  2 MiB source and a 2 MiB destination, larger than the caches; no device is
  granted the lease. The kernel is an assembly loop of four `LDP` and four
  `STP` of X registers, 64 bytes per iteration. The tasks save no FP state,
  so the kernel uses no SIMD registers; the project `memcpy` moves 8 bytes per
  iteration and is not used. The CPUs run at the boot0 clocks, which the
  evidence records.
- **DRAM bound.** Boot0 reports 2040 MHz. The design takes the data rate as
  4,080 MT/s and the bus width W as 64 bits unless the board schematic shows
  32, so B = 4,080 MT/s × W / 8 = 32.64 GB/s (16.32 GB/s at 32 bits). Both are
  the larger candidate reading, and overstating B can only make a run
  inconclusive. A run whose phase B mean of ports 32 + 33 is below 0.6 B is
  inconclusive; the next load image then raises the clusters through the
  existing CPU clock driver.
- **Latency statistic.** For each accepted window, x is port 2's latency sum
  divided by its read requests, in MBUS cycles per request, in Q8 fixed point.
  A window with no DE read request counts as idle. A window with x ≥ 2²⁴
  counts as out of range, so Σx² over a session's at most 360 windows stays
  below 2⁵⁷. Per phase the block holds Σx and Σx²; the host tool derives the mean m,
  the standard deviation and the standard error SE = s / √n.
- **Transport.** The observation reply's `terminal.reserved[136]`, bytes
  888..1023, becomes the evidence block and the reply version becomes 2; the
  1,024-byte size and its static assert stay. Every scan-out image fills the
  hook fields; the load image fills all of them.

  | Offset | Fields |
  | --- | --- |
  | 0 | `u16` ×10: windows started, accepted in phase A, accepted in phase B, refused late, refused idle, refused free-running, abandoned, out of range, above a quarter of the wrap bound; result bits (mapping, NSI admitted, fabric match, hook result) |
  | 20 | `u16` ×6: inherited and final fields of ports 2, 14 and 15 (mode bits 1–0, priority bits 5–2, select bit 6) |
  | 32 | `u16` ×4: configured CPU-L, CPU-B and DSU MHz (0 when undecoded), zero |
  | 40 | `u32` ×3: CCU `+0x580`, `+0x584`, `+0x588` |
  | 52 | `u32`: phase B length in ms |
  | 56 | `u64` ×8: per phase, Σx, Σx², ports 32 + 33 data, port 2 read data |
  | 120 | 16 bytes, zero |

  Data means read plus write data counts.

## Performance budgets

Access counts come from host models that run the compiled code against
counting register files. Times come from the architectural counter on the
board.

| Metric | Target | Measurement | Baseline |
| --- | --- | --- | --- |
| Hook | 2 CCU reads; per field 1 NSI access if at target, else 3; ≤ 27 NSI accesses per call; 0 barriers; once per display power-on | Host model of the native `io` | 0 OS writes |
| Mapping checks | One per page per `io` instance, at initialization | Host model counts translation calls | Not applicable |
| Steady state | 0 tasks, timers, interrupts and accesses outside the load image | Linked-symbol check; host model | 0 |
| Window | Init: 3 CCU and 8 clock reads, once. Start: 18 NSI accesses, 1 DSB, 2 ISB. Collect: 17 NSI accesses, 1 DSB, 1 ISB. Abandon: 2 writes. Pending: 1 ISB, 1 counter read. ≤ 20 µs per phase | Host model per phase; counter around each phase | None |
| Counter headroom | No 32-bit wrap in P | Arithmetic at 76.8 GB/s, above any LPDDR5 rate: ports 32 and 33 ≤ 6.0 × 10⁷ units, port 2 ≤ 2.4 × 10⁸; period ≤ 3 × 10⁷ cycles at 600 MHz; latency sum below 2³² while DE averages under 143 reads outstanding | Not applicable |
| Scan-out | HDMI stays `ACTIVE` with no `A733_HDMI_UNDERFLOW`; m(no table) − m(table) in phase B > 2 √(SE₁² + SE₂²) | Both load images; conclusive only if each reaches 0.6 B in phase B, and the no-table image's phase B m exceeds its phase A m by more than 2 √(SE_A² + SE_B²) | Not measured |
| DDR trip | RTF ≤ 1.05 × without the trip; time with DDR ≥ 85 °C and CPUB < 90 °C ≤ 60 s of 600 s | OS image recognizing recorded input for 10 min, 25 °C ambient, fan off, with and without the trip; also time at the 1014 MHz cap | Before the change: RTF, DDR − CPUB per sample, time each channel is ≥ 85 °C |
| Code size | ≤ 3 KiB text, ≤ 256 B data outside the load mode | `llvm-size` | 0 |

RTF is recognition wall time divided by audio duration. CPU traffic reaches
the DRAM controller through its own channels, and no pinned source states
whether DE's port priority affects arbitration against it. If a conclusive run
fails the scan-out row, the table and the hook are removed. A window whose
latency sum exceeds a quarter of the wrap bound is counted; a nonzero count
leads to a reviewed change of P.

The DDR trip stays even if it exceeds its bound, because refresh behavior
above 85 °C is unknown. Exceeding the bound blocks further work on this
component until either boot0's derated-refresh setting is decoded from the
boot0 write-sequence trace or the DRAMC is shown to apply LPDDR5 MR4
refresh-rate changes. The trip is raised only on that evidence.

## Left out, and the condition that adds each item

- **Training, frequency scaling and voltage.** Boot0's opaque routine owns
  them; replacing it is deferred by project policy.
- **Memory configuration in evidence records.** Cold entry copies boot0's
  parameter words to `0x47008000`. Only the OS image, entered through the
  stage-2 loader, can tell that copy from one left by an earlier boot, and its
  128-byte result record has no spare field. No evidence shows that nonsecure
  EL1 can read `0x47008000..0x47008400`, which EL3 wrote with the MMU off.
  The decode is added with an OS result record version that carries type,
  frequency, ranks and size, once a board read or the boot0 firewall range
  shows the interval readable. Until then the FEL DDR wrapper log names the
  configuration: LPDDR5, 12,288 MiB, trained at 2040, 1165, 816 and 408 MHz.
- **DRAMC fields, including temperature-based refresh state.** No pinned
  source names them. The boot0 write-sequence trace, owned by this component,
  is the prerequisite.
- **DRAM firewall check.** Boot0 requests a 120 MiB secure region whose
  registers no trace identifies. When the trace gives the range, one
  boot-time `a733_extents_overlap_or_wrap` test checks it against the OS
  memory map.
- **GPU 6, NPU 18, DI 4 and G2D 5.** No image runs these masters while DE
  scans out; gpu-display closes the GPU and waits for STOPPED before output
  starts. Each port is added when an image runs its master during scan-out.
  For the GPU that also needs lease disposal in the GPU service, which the GPU
  package owns, because the service refuses a reopen after STOPPED; this
  component does not relax that refusal. The GPU cost would then be measured
  as kick-to-completion time from the GPU interrupt.
- **VE 7, 8 and 9, ISP 16, CSI 17, USB_PCIE 13, the other ports, and the
  DSI scan-out image, which does not link the domain 1 framebuffer.** Each is
  added when a scan-out run with that master or image active shows an
  underflow or a DE latency regression.
- **Limiters, CPU direct channels, runtime priority changes and an adaptive
  window period.** No board use needs them.

## Changes outside this component, and host checks

- `a733_display_framebuffer.c` (domain 1 instance) calls the hook once,
  guarded by the done flag. `a733_de_sys_power.c` is not changed.
- `apps/gpu-display/observation.h` replaces `terminal.reserved[136]` with the
  evidence block, and the reply version becomes 2. `display.c` calls the
  window step, start limits and abort in the load build only.
- `apps/gpu-display/capture.py` keeps request version 1, accepts reply
  version 1 with bytes 888..1023 zero and reply version 2 with the block
  decoded and its last 16 bytes zero. The gpu-display observation host cases
  that expect reply version 1 change with it.
- `checks/thermal-policy.c` sets DDR to code 1000 (108 °C) at line 113. The
  commit that changes `trips[]` moves that reading below 85 °C. The new
  `checks/a733-thermal-ddr-trip.c` expects THROTTLE at code 1394 (85.02 °C),
  holds it at 1428 (83.01 °C) and releases it at 1429 (82.95 °C).
- New host-model cases in `checks/dram-*` files:
  - a not-admitted hook and a failed mapping check each make zero NSI
    accesses, and a repeated DE_SYS poll makes no second hook call;
  - a mismatch at each port and field position leaves later fields
    untouched, unrelated bits survive every write, and a difference outside
    the field mask is not a mismatch;
  - unequal priority subfields, such as 3 and 1, give the specified result;
  - the value written to `+0xc8` is P × `fabric_hz[2]` for port 2 and
    P × `fabric_hz[1]` for ports 32 and 33;
  - a start before fabric admission, after phase B, at stage 4 or in state 3
    makes zero NSI accesses;
  - t_end ≥ t0 + 2P, differing port 32 reads, a port 32 count below the
    floor, zero DE read requests and x ≥ 2²⁴ each refuse as specified, every
    collect ends with three disable writes, and an abandon makes no port 2
    access.

## Board evidence that would confirm the design

- Inherited and final fields from each scan-out image's hook.
- The scan-out row from both load images, with the CPU clocks and the third
  presentation time, and whether the counters hold, latch or run past the
  period.
- The DDR trip row with its baseline and capped-interval figures.
- The offset between the THS DDR channel and the DRAM die: a thermocouple on
  the LPDDR5 package read alongside the channel across the DDR trip run, and
  an MR4 reading once the boot0 trace identifies the mode-register read.
