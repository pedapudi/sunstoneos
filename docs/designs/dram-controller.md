# DRAM controller and memory-bus priority

Status: design for review. No code exists. Physical behavior stays
unqualified until the board evidence in the last section exists.

The component has three parts:

- a fixed priority table for the memory-bus ports that serve display scan-out,
  written once per display power-on with read-back;
- a latency window on the display port that runs in one load image;
- a passive trip on the DDR temperature channel.

DRAM training, frequency scaling, voltage and the boot0 DRAM firewall stay
inside boot0. Decoding the boot0 memory parameters is left out until an image
can carry the decoded values; the last sections give the conditions.

## Sources

- **Fabric clocks.** `NSI_CLK_REG` (CCU `+0x580`: gate bit 31, reset bit 30),
  the NSI configuration word (`+0x584`: gate bit 0, reset bit 16) and
  `MBUS_CLK_REG` (`+0x588`), per manual section 4.1.6. Each image's sealed
  board session carries the expected `+0x580` and `+0x588` words and their
  rates as `fabric[1..2]` and `fabric_hz[1..2]`. Before the framebuffer
  powers DE_SYS on, `a733_media_clock` compares both live words with the
  record under mask `0x8700001f`, and the framebuffer holds
  `A733_CLOCK_FABRIC` under exclusion for the rest of the session. That mask
  leaves out the NSI reset (bit 30), and no driver checks `+0x584`.
- **NSI port fields.** The pinned `sunxi-nsi.c` and `sunxi-nsi.h` place the
  register block of NSI port n at `0x02020000 + 0x200 × n`. Every vendor field
  write is a read-modify-write that keeps the other bits: mode, bits [1:0] of
  `+0x10`; priority, two 2-bit fields at bits [1:0] and [3:2] of `+0x14`, both
  set to the same value p, maximum 3; select, bit 0 of `+0x18`. The counters
  are enable bit 0 of `+0xc0`, clear bit 0 of `+0xc4`, the period in cycles at
  `+0xc8`, read request, write request, read data and write data counts at
  `+0xcc..+0xd8`, and the read latency sum at `+0xdc`. Port 2 counts in
  16-byte units and MBUS cycles.
- **Port owners.** The `nsi0` node of the pinned `sun60iw2p1.dtsi` assigns DE
  to port 2 in the DE_SYS power domain and the IOMMU0 and IOMMU1 page-table
  walkers, which serve DE's translated reads, to ports 14 and 15. The same node
  gives the CPU clusters direct channels to the DRAM controller
  (`cpu_direct_reg` at `0x0A030000` and `0x0A040000`), and `sunxi-nsi-bw.c`
  adds their counts to those of the TA ports 32 and 33 to obtain total DRAM
  traffic. Ports 32 and 33 therefore do not count CPU traffic on the direct
  channels.

## Data flow and ownership

- **Port table.** A const table holds DE 2 at priority 2 and IOMMU0 14 and
  IOMMU1 15 at priority 3, each with mode 0 and select 1, the vendor values.
  The priority field mask is `0xf` and its target is `(p << 2) | p`, so
  unequal subfields count as different and are rewritten.
- **Priority hook.** `a733_dram_priority(io, evidence)` makes every access
  through an `io` pair, `read(context, address, &value)` and
  `write(context, address, value)`. This is the callback form that
  `a733_cpu_clock_report` already uses, and it lets the host models run the
  compiled hook against a counting register file. The hook reads `+0x580` for
  the NSI reset bit, which the media-clock mask leaves out, and `+0x584` for
  the configuration gate and reset. It requires both resets deasserted and the
  gate set. If they are not, it makes zero NSI accesses and returns
  not-admitted. Otherwise, for each field it reads the word. Only if the field
  differs from the target does it write the read value with the field
  replaced and read the word back, comparing under the field mask. The
  evidence holds each port's inherited and final fields. The no-table load
  image compiles the write step out: its hook makes the same reads and writes
  nothing.
- **Caller.** In the domain 1 instance of `a733_display_framebuffer.c`,
  `cold_step` case 4 calls the hook when `a733_de_sys_power_poll` returns 1,
  before the `break` that advances `f->index`. The poll is never repeated after
  that return, and `a733_display_framebuffer_prepare` never moves a stage
  backwards, so the hook runs once per framebuffer instance. At that point the
  IOMMU admission states every real IOMMU master excluded and drained
  (`excluded_masters = A733_IOMMU_REAL_MASTERS`) and DE has started no DMA, so
  no master on ports 2, 14 or 15 is active. The gpu-display, HDMI and
  DisplayPort images link this file and run the hook.
- **Mapping checks.** The native `io` in `drivers/dram/` checks its pages once
  at initialization. The CCU page `0x02002000` and every other page it touches
  must translate to themselves as Device-nGnRnE (MAIR attribute `0x00`). Its
  address allowlist holds the two CCU words, the table's words and, in the load
  image, port 2's counter words and the reads that `a733_cpu_clock_address`
  permits. The page tables are sealed before device admission and no later
  code changes them; a page-table change would make the checks stale. A failed
  check marks the `io` unusable: the hook returns not-admitted with zero NSI
  accesses, the caller records that and scans out with the inherited
  priorities, and the window refuses every start.
- **Mismatch.** A read-back that differs under the mask leaves the port in an
  undocumented mode. The hook stops at the first mismatch, and the caller takes
  its existing failure path for a failed DE_SYS power-on, before IOMMU open and
  before any DE DMA.
- **Latency window.** Port 2 over a period P of 50 ms, in the load image only.
  Initialization makes no CCU access. It takes the MBUS rate from
  `fabric_hz[2]`, which the media clock verified, and relies on the
  framebuffer's fabric exclusion to hold that rate for the session. It records
  the configured CPU clocks once through `a733_cpu_clock_report`. Every time
  value comes from an ISB followed by `MRS CNTPCT_EL0`.
  - *Start* runs only while the hook reported admitted, the display stage is
    3, the application state is 1, three presentations are done and the
    current phase has not ended. It reads the counter (t0) and makes 6
    accesses in the vendor order: it writes the period P × `fabric_hz[2]`,
    clears enable bit 0 by read and write, sets clear bit 0 by read and write,
    and writes the saved enable word with bit 0 set. Then `DSB SY` and a
    counter read give t1.
  - *Collect* returns pending until the counter passes t1 + P. It then reads
    port 2's four counts and latency sum and the read data count again, 6
    reads, writes the saved enable word with bit 0 clear, and issues `DSB SY`
    and a counter read (t_end).
  - *Refusal.* Collect refuses a window in three cases:
    t_end ≥ t0 + 2P, which places the reads after the end of the second
    period; two read data counts that differ, which show a free-running
    counter; and a read data count below 1,024 units (16 KiB). Scan-out alone
    moves about 5.5 MB per window, so the last rule refuses only a window in
    which DE fetched almost nothing.
  - *Stop.* Phase B ends at `stop_at` − 1 s, and no window starts after that,
    once the stage reaches 4 or once the state is not 1. On the `stop_at` path,
    before `a733_hdmi_stop`, a window still in flight is abandoned with the one
    disable write. `output_fault` abandons an in-flight window in software
    only, with zero NSI accesses, because device access after a declared fault
    breaks the retention rule and a counter left enabled is harmless.
- **DDR temperature.** The DDR entry of `trips[]` in `a733_thermal.c` becomes
  {85000, 2000, 110000}. The vendor zone has only the 110 °C critical trip.
  JEDEC specifies LPDDR5 standard refresh up to 85 °C die temperature, and no
  decoded field shows derated refresh. The input is the SoC THS "DDR" channel,
  a proxy with an unknown offset from the DRAM die. A DDR passive trip raises
  the existing global THROTTLE level, which caps both CPU clusters at
  1014 MHz.

## Interrupts, memory, barriers and cores

The component installs no interrupt or timer. The NSI interrupt (SPI 43) has
no qualified delivery and stays masked. The window adds no wake-up: its step
runs in the output task's existing `vTaskDelay(1)` loop in
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
writes only port 2's counter registers. Both run in the output task, which
also reads the evidence. The load image's copy tasks share one phase word with
the output task. The output task wakes them with a task notification at the
start of phase B and stores the end phase with release. Each task loads the
phase word and the application state with acquire once per pass. After it
sees the end phase or a state other than 1, it calls `vTaskSuspend(NULL)` and
never returns. A transition to state 2 or 3 from any task therefore stops the
load within one pass, about 2 ms at 2 GB/s per core. Each task counts its
completed passes in its own 64-byte-aligned slot; the output task reads the
eight slots with acquire when it stores the end phase. The component adds no
lock.

## Failure classes

- Not admitted, or mapping check failed: class 1, refused before any NSI
  access. Scan-out continues with the inherited priorities; nothing is
  retained.
- Read-back mismatch: the caller's existing DE_SYS power-on failure path, with
  no DE DMA started; it retains what that path retains.
- Window refused or abandoned: class 1. The window is counted; nothing is
  retained.

## Reuse

The component reuses the board fabric record, the media clock's fabric compare,
the framebuffer's fabric exclusion and `a733_cpu_clock_report`. It also reuses
the shared MMIO and mapping helpers, the framebuffer's power-on and failure
paths, the IOMMU exclusion record, the output task's loop and observation
reply, the HDMI underflow check, the device-memory allocator and the thermal
trip table. It removes nothing, adds no boot record and needs no change from
the primary agent.

## Measurement

A load mode of gpu-display, `apps/gpu-display/dram-controller-load.c`,
compiled only with `SUNSTONE_GPU_DISPLAY_DRAM_LOAD=ON`, produces the windows.
Two images are built from it, one with the port table and one with the write
step compiled out, and run in one board session.

- **Session.** After the third presentation, slot 0 stays on scan-out at
  1280×720 RGB565, 60 Hz: 110.6 MB/s. Phase A lasts 2 s (40 windows) with
  scan-out only and gives the uncontended DE latency. Phase B runs scan-out and
  the copy load until `stop_at` − 1 s and needs at least 8 s (160 windows).
  `stop_at` lies 18 s into the 20 s session, so phase B is long enough when the
  third presentation comes at most 7 s into the session. In the gpu-display
  host model it comes at 0.27 s. On the board the presentation record's
  `observed_us` gives the time. A later presentation makes the run
  inconclusive, and the next load image then needs a 30 s diagnostic window,
  a change to the monitor's session arming that the primary agent owns.
- **Copy load.** Eight tasks at priority 1, below every existing task, pinned
  one per core with `vTaskCoreAffinitySet` (A55 cores 0–5, A76 cores 6 and 7).
  `gpu_display_task_privileged` in `apps/gpu-display/main.c` lists their entry
  in the load build, so they run at EL1; at EL0 their FreeRTOS calls and pool
  accesses would trap. A 32 MiB CPU-only lease from the write-back device pool
  gives each task a 2 MiB source and a 2 MiB destination, larger than the
  caches. The cached image maps that pool with 2 MiB blocks
  (`platform/a733-smp-cached/memory.c`), as section 2.8 requires. No device
  is granted the lease, and it stays in `DEVICE_MEMORY_CPU` until power-off,
  so its dirty lines never meet a device. A pass copies 2 MiB with an assembly
  loop of two `LDP` and two `STP` of Q registers, 64 bytes per iteration.
  `configUSE_TASK_FPU_SUPPORT` is 2, so every task already saves Q0–Q31 on a
  switch and the kernel needs no port change. The CPUs run at the boot0
  clocks, which the evidence records.
- **Conclusiveness.** The no-table image must show contention: its phase B
  mean latency must exceed its phase A mean by more than
  2 √(SE_A² + SE_B²). If it does not, the next load image raises the clusters
  through the existing CPU clock driver. If the raised run still shows no
  contention, the scan-out row is recorded as not demonstrable under CPU load,
  the table stays at the vendor values, and the row is retested when an image
  runs the GPU or NPU during scan-out. CPU bandwidth is a recorded figure:
  passes × 4 MiB (read plus write) over the phase B time, per core.
- **Latency statistic.** For each accepted window, x is port 2's latency sum
  divided by its read requests, in MBUS cycles per request, in Q8 fixed point.
  A window with x ≥ 2²⁴ counts as out of range, so Σx² over a session's at
  most 360 windows stays below 2⁵⁷. Per phase the block holds Σx and Σx²; the
  host tool derives the mean m, the standard deviation s and the standard
  error SE = s / √n over the n accepted windows.
- **Transport.** The gpu-display observation reply's
  `terminal.reserved[136]`, bytes 888..1023, becomes the evidence block and
  the reply version becomes 2; the 1,024-byte size and its static assert stay.
  Every gpu-display image fills the hook fields; the load images fill all of
  them. The HDMI and DisplayPort images run the hook without reporting its
  fields: a mismatch fails their scan-out, which their existing result
  records.

  | Offset | Fields |
  | --- | --- |
  | 0 | `u16` ×8: windows started, accepted in phase A, accepted in phase B, refused late, refused idle, refused free-running, out of range, abandoned |
  | 16 | `u16`: result bits (mapping, NSI admitted, hook result); `u16`: copy kernel identifier |
  | 20 | `u16` ×6: inherited and final fields of ports 2, 14 and 15 (mode bits 1–0, priority bits 5–2, select bit 6) |
  | 32 | `u16` ×4: configured CPU-L, CPU-B and DSU MHz (0 when undecoded), zero |
  | 40 | `u32` ×2: CCU `+0x580`, `+0x584` |
  | 48 | `u32` ×2: `fabric_hz[2]`; phase B length in ms |
  | 56 | `u64` ×6: per phase, Σx, Σx², port 2 read data |
  | 104 | `u32` ×8: copy passes per core in phase B |

## Performance budgets

Access counts come from host models that run the compiled code against
counting register files. Times come from the architectural counter on the
board.

| Metric | Target | Measurement | Baseline |
| --- | --- | --- | --- |
| Hook | 2 CCU reads; per field 1 NSI access if at target, else 3; ≤ 27 NSI accesses per call; 0 barriers; once per framebuffer instance | Host model of the native `io` | 0 OS writes |
| Mapping checks | One per page per `io` instance, at initialization | Host model counts translation calls | Not applicable |
| Steady state | 0 tasks, timers, interrupts and accesses outside the load image | Linked-symbol check; host model | 0 |
| Window | Init: 0 CCU and 8 clock reads, once. Start: 6 NSI accesses, 1 DSB, 2 ISB. Collect: 7 NSI accesses, 1 DSB, 1 ISB. Abandon: 1 write at `stop_at`, 0 on fault. Pending: 1 ISB, 1 counter read. ≤ 10 µs per phase | Host model per phase; counter around each phase | None |
| Counter headroom | No 32-bit wrap in P | Arithmetic at 76.8 GB/s, above any LPDDR5 rate: port 2 ≤ 2.4 × 10⁸ units; period ≤ 3 × 10⁷ cycles at 600 MHz; latency sum below 2³² while DE averages under 143 reads outstanding | Not applicable |
| Copy-load stop | No pass starts after the end phase or a state change is stored | Host model of the copy task loop | Not applicable |
| Scan-out | HDMI stays `ACTIVE` with no `A733_HDMI_UNDERFLOW`; m(no table) − m(table) in phase B > 2 √(SE₁² + SE₂²) | Both load images, under the conclusiveness rule | Not measured |
| DDR trip | RTF ≤ 1.05 × without the trip; time with DDR ≥ 85 °C and CPUB < 90 °C ≤ 60 s of 600 s | OS image recognizing recorded input for 10 min, 25 °C ambient, fan off, with and without the trip; also time at the 1014 MHz cap | Before the change: RTF, DDR − CPUB per sample, time each channel is ≥ 85 °C |
| Code size | ≤ 3 KiB text, ≤ 256 B data outside the load mode | `llvm-size` | 0 |

RTF is recognition wall time divided by audio duration. CPU traffic reaches
the DRAM controller through its direct channels, and no pinned source states
whether DE's port priority affects arbitration against it. If a conclusive run
fails the scan-out row, the table and the hook are removed.

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
- **TA ports 32 and 33, the CPU direct-channel counters and the
  `0x08000200` channel select.** In the load images DE is the only active NSI
  master, so port 2 already counts the NSI traffic, and the copy tasks count
  their own bytes. The direct-channel counters are added when an image needs
  CPU traffic that its tasks cannot count.
- **GPU 6, NPU 18, DI 4 and G2D 5.** No image runs these masters while DE
  scans out; gpu-display closes the GPU and waits for STOPPED before output
  starts. Each port is added when an image runs its master during scan-out.
  For the GPU that also needs lease disposal in the GPU service, which the GPU
  package owns, because the service refuses a reopen after STOPPED; this
  component does not relax that refusal.
- **VE 7, 8 and 9, ISP 16, CSI 17, USB_PCIE 13, the other ports, and the
  DSI scan-out image, which does not link the domain 1 framebuffer.** Each is
  added when a scan-out run with that master or image active shows an
  underflow or a DE latency regression.
- **Limiters, runtime priority changes and an adaptive window period.** No
  board use needs them.

## Changes outside this component, and host checks

- `a733_display_framebuffer.c` (domain 1 instance) calls the hook inline in
  `cold_step` case 4 when the poll returns 1. `a733_de_sys_power.c` is not
  changed.
- `apps/gpu-display/observation.h` replaces `terminal.reserved[136]` with the
  evidence block, and the reply version becomes 2. `display.c` calls the
  window step, start limits and abandon rules in the load build only.
  `main.c` creates the copy tasks and adds their entry to
  `gpu_display_task_privileged` in the load build only.
- `apps/gpu-display/capture.py` keeps request version 1, accepts reply
  version 1 with bytes 888..1023 zero and reply version 2 with the block
  decoded. The gpu-display observation host cases that expect reply version 1
  change with it.
- `checks/thermal-policy.c` sets DDR to code 1000 (108 °C) at line 114. The
  commit that changes `trips[]` moves that reading below 85 °C. The new
  `checks/a733-thermal-ddr-trip.c` expects THROTTLE at code 1394 (85.02 °C),
  holds it at 1428 (83.01 °C) and releases it at 1429 (82.95 °C).
- New host-model cases in `checks/dram-*` files:
  - a not-admitted hook, including one whose only fault is NSI reset bit 30,
    and a failed mapping check each make zero NSI accesses;
  - a full framebuffer prepare sequence calls the hook once;
  - a mismatch at each port and field position leaves later fields
    untouched, unrelated bits survive every write, and a difference outside
    the field mask is not a mismatch;
  - priority subfields 3 and 1 with p = 3 are rewritten to `0xf`, and the
    read-back is compared under `0xf`;
  - the no-table hook makes reads only;
  - the value written to `+0xc8` is P × `fabric_hz[2]`;
  - a start before admission, after phase B, at stage 4 or in state 2 or 3
    makes zero NSI accesses;
  - t_end ≥ t0 + 2P, differing read data counts, a count below the floor and
    x ≥ 2²⁴ each refuse as specified, and every collect ends with one disable
    write;
  - `output_fault` with a window in flight makes zero NSI accesses;
  - no copy pass starts after the end phase or state 3 is stored.

## Board evidence that would confirm the design

- Inherited and final fields from the gpu-display images' hook; pass or fail
  from the HDMI and DisplayPort images.
- The scan-out row from both load images, with the CPU clocks, the copy
  passes per core, the third presentation time, and whether the counters hold,
  latch or run past the period.
- The DDR trip row with its baseline and capped-interval figures.
- The offset between the THS DDR channel and the DRAM die: a thermocouple on
  the LPDDR5 package read alongside the channel across the DDR trip run, and
  an MR4 reading once the boot0 trace identifies the mode-register read.
