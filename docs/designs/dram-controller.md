# DRAM controller and memory-bus priority

Status: the DDR trip is implemented and host-modeled in the thermal policy.
The priority hook, the latency window and the copy load are implemented and
host-modeled but not merged. Their code merges together with the framebuffer,
gpu-display and build edits that call them and with the integration host
cases listed under the changes outside this component. Physical behavior
stays unqualified until the board evidence in the last section exists.

The component has three parts:

- a fixed priority table for the memory-bus ports that serve display scan-out,
  written once per framebuffer power-on with read-back, in gpu-display images
  only;
- a latency window on the display port, in two load images;
- a passive trip on the DDR temperature channel.

DRAM training, frequency scaling, voltage and the boot0 DRAM firewall stay
inside boot0. The section on left-out items lists what else waits for evidence.

## Sources

- **Fabric clocks.** `NSI_CLK_REG` is CCU `+0x580` (gate bit 31, reset bit
  30), the NSI configuration word is `+0x584` (gate bit 0, reset bit 16) and
  `MBUS_CLK_REG` is `+0x588` (manual section 4.1.6). A set bit means the gate
  is open or the reset is deasserted. `a733_media_clock` compares `+0x580` and
  `+0x588` with the board record under mask `0x8700001f`, which requires gate
  bit 31 and leaves out reset bit 30. The compare runs in `stable()` on every
  media-clock poll from the baseline step on. In the framebuffer that is
  prepare stage 4, after DE_SYS power-on in stage 3. No driver checks `+0x584`.
- **NSI port fields.** The pinned `sunxi-nsi.c` and `sunxi-nsi.h` place NSI
  port n at `0x02020000 + 0x200 × n`. Vendor field writes are
  read-modify-writes: mode is bits [1:0] of `+0x10`; priority is two 2-bit
  fields at bits [1:0] and [3:2] of `+0x14`, both set to p ≤ 3; select is bit 0
  of `+0x18`. The counters are enable bit 0 of `+0xc0`, clear bit 0 of `+0xc4`,
  the period in MBUS cycles at `+0xc8`, read request, write request, read data
  and write data counts at `+0xcc..+0xd8`, and the read latency sum at `+0xdc`.
  Port 2 counts data in 16-byte units. The pinned `sunxi-nsi-bw.c` reads the
  latency sum and request count while the counters stay enabled, and reports 0
  when the request count is 0.
- **Port owners.** The `nsi0` node of the pinned `sun60iw2p1.dtsi` assigns DE
  to port 2 and the IOMMU0 and IOMMU1 page-table walkers, which serve DE's
  translated reads, to ports 14 and 15. The CPU clusters reach the DRAM
  controller through direct channels that no NSI port counts.

## Data flow and ownership

- **Port table.** A const table sets DE port 2 to priority 2 and IOMMU ports
  14 and 15 to priority 3, each with mode 0 and select 1, the vendor values.
  The priority mask is `0xf` and its target is `(p << 2) | p`, so unequal
  subfields are rewritten.
- **Priority hook.** `a733_dram_priority(io, evidence)` in `drivers/dram/`
  makes every access through an `io` pair, `read(context, address, &value)`
  and `write(context, address, value)`, the callback form of
  `a733_cpu_clock_report`. Host models run the compiled hook against a
  counting register file. The hook reads `+0x580` and `+0x584` and requires
  both resets deasserted and both gates open. Otherwise it records the two
  words, makes zero NSI accesses and returns failure. For each field it then
  reads the word. Only a field that differs from the target is written, with
  the other bits kept, and read back under the field mask. The first mismatch
  stops the hook with failure. The evidence holds the two CCU words, each
  port's inherited and final fields and the stopping field (0, or 1 + 3 ×
  port index + field index). Built with `SUNSTONE_DRAM_PRIORITY=1`, the hook
  makes the same reads and writes nothing. The hook does not check its
  arguments for NULL, because its one caller passes the framebuffer's own
  pair and evidence member; the caller keeps the return value for the
  evidence block.
- **Register path.** The hook has no register path of its own. The
  framebuffer passes an `io` over its existing `raw_read` and `raw_write`,
  which check ownership, address, mapping (`a733_mapping_check`,
  Device-nGnRnE) and timeliness per access. One `enter()` covers the hook's
  sequence and one `leave()` ends it. A new `dram_address()` admits a fixed
  set, in the style of `domain_address()`: CCU `+0x580` and `+0x584`, and the
  `+0x10`, `+0x14` and `+0x18` words of ports 2, 14 and 15 (`0x02020400`,
  `0x02021C00`, `0x02021E00`). `address()` admits that set as well.
- **Caller.** Stage 4 of `a733_display_framebuffer_prepare` calls the hook
  when `a733_media_clock_poll` returns `A733_MEDIA_PREPARED`, before the stage
  advances. Stage 4 advances once and no stage moves back, so the hook runs
  once per framebuffer instance. The poll that returns `PREPARED` has just run
  `stable()`, so the NSI and MBUS clock gates and the DE MBUS gate (`+0x5e0`
  bit 11) are verified. Stage 5 DE top clock writes, stage 6 IOMMU open and DE
  DMA all come later. The IOMMU admission states every real master excluded and
  drained, so no master on ports 2, 14 or 15 is active. Any hook failure takes
  the stage's existing `goto fail` path, which retains the framebuffer.
- **Images.** The call and the evidence member of the framebuffer structure
  compile only when `SUNSTONE_DRAM_PRIORITY` is defined. The gpu-display build
  defines it as 2 (table) in its normal image and the table load image, and as
  1 (reads only) in the no-table load image. The HDMI and DisplayPort builds
  leave it undefined, so nothing new runs there.
- **Latency window.** Port 2 over a period P of 50 ms, in the load images
  only. Each phase runs under one `enter()` and `leave()` through a load-only
  accessor of the framebuffer, compiled under `SUNSTONE_GPU_DISPLAY_DRAM_LOAD`.
  The accessor reuses `raw_read` and `raw_write`, and in that build
  `dram_address()` also admits port 2's words `+0xc0..+0xdc` and the reads that
  `a733_cpu_clock_address` permits. Initialization records the CPU clocks once
  through `a733_cpu_clock_report` and takes the MBUS rate from
  `fabric_hz[2]`, which the media clock verified. The framebuffer's
  `A733_CLOCK_FABRIC` exclusion holds that rate for the session. Times come
  from an ISB followed by `MRS CNTPCT_EL0`.
  - *Start* runs only while the display stage is 3, the application state is
    1, three presentations are done and the current phase has not ended. It
    reads the counter (t0) and makes 6 accesses in the vendor order: write the
    period P × `fabric_hz[2]`, clear enable bit 0, set clear bit 0 and write the
    saved enable word with bit 0 set. Then a counter read gives t1.
  - *Collect* returns pending until the counter passes t1 + P. It then makes 6
    reads with the counters enabled, as the vendor driver does: the four
    counts, the latency sum and the read data count again. A counter read
    gives t_end.
  - *Refusal.* Collect refuses a window in three cases. If t_end ≥ t0 + 2P,
    the reads came after the second period ended. If the two read data counts
    differ, the counter is free-running. If read requests are 0 or read data
    is below 1,024 units (16 KiB), DE fetched almost nothing; scan-out alone
    moves about 5.5 MB per window.
  - *Free-running fallback.* A counter that holds at the end of the period
    cannot change between the two read data reads, so one difference proves
    that the counters run past the period. After the first window refused as
    free-running, every later collect first writes the saved enable word with
    bit 0 clear and then makes its 6 reads. A result bit records the order.
    The two reads are about 1 µs apart, and DE fetches each 2,560-byte line
    in one burst per 22.2 µs line time and nothing during vertical blanking,
    so a free-running counter often reads equal; windows before the first
    detection are accepted. The statistic is a ratio, so neither those
    windows nor the longer counting interval after the fallback bias it.
  - *Stop and abandon.* No window starts after phase B ends, at stage 4 or in
    a state other than 1. A window in flight at `stop_at` or at `output_fault`
    is abandoned in software and counted. The next start disables the counters
    first, and a counter left enabled affects no other master. An abandon
    during phase B reads the counter once to record the end of phase B and
    sets the result bit for phases ended early, so the phase B length in the
    block is the time the copy load ran.
- **DDR temperature.** The DDR entry of `trips[]` in `a733_thermal.c` is
  {85000, 2000, 110000}; the vendor zone has only the 110 °C critical trip.
  JEDEC specifies LPDDR5 standard refresh up to 85 °C die temperature, and no
  decoded field shows derated refresh. The input is the SoC THS "DDR" channel,
  a proxy with an unknown offset from the DRAM die. The trip raises the
  existing global THROTTLE level. THROTTLE has no consumer in the OS image
  today: its thermal service acts only on CRITICAL. Connecting THROTTLE to the
  CPU clock driver, which caps both clusters at 1014 MHz, waits for the DDR
  trip row of the performance budgets, measured in the board session that
  qualifies the throttle.

## Interrupts, memory, barriers and cores

The component installs no interrupt or timer. The NSI interrupt (SPI 43) has
no qualified delivery and stays masked. The window step runs in the output
task's existing `vTaskDelay(1)` loop in `apps/gpu-display/display.c`, a tick
wait that performance architecture section 1.3 permits for a device without
qualified interrupt delivery. The pending check is one ISB, one counter read
and one compare, about 1,000 per second. Nothing blocks or busy-waits.

Records and window state live in normal write-back memory that no device reads,
so they need no cache maintenance. The hook adds no barrier. The framebuffer's
`raw_read` and `raw_write` keep their existing `DSB SY` pair per access, at
most 29 pairs once per framebuffer instance. The window adds no barrier
either. Performance architecture section 1.2 requires `DSB SY` before an
action that is not a memory access, here the counter read that ends a phase.
The accessor's `raw_write` and `raw_read` already end every access with
`DSB SY`, so the last NSI access of a phase has completed before the counter
read.

Each port has one writer. The hook writes ports 2, 14 and 15; the window writes
only port 2's counter words. Both run in the output task, which also fills the
evidence. The copy tasks share one phase word with the output task. The output
task wakes them with a task notification at the start of phase B and stores the
end phase with release. Each task loads the phase word and the application
state with acquire once per pass. After the end phase or a state other than 1
it calls `vTaskSuspend(NULL)`. A transition to state 2 or 3 therefore stops the
load within one pass, about 2 ms at 2 GB/s per core. Each task counts passes
in its own 64-byte-aligned slot, which the output task reads with acquire after
storing the end phase. The component adds no lock.

## Failure classes

- NSI reset asserted or gate closed, a failed register access, or a read-back
  mismatch: the framebuffer's stage 4 failure path, before DE top clock
  writes, IOMMU open or DE DMA. That path retains the register lock if held,
  the clock tokens and the framebuffer lease. The hook fills the evidence
  before it returns.
- Window refused or abandoned: class 1, counted; nothing is retained.
- A failed access in the load accessor: the framebuffer's existing retain path.

## Reuse

The component reuses the framebuffer's register path, failure path, fabric
compare and fabric exclusion, the board fabric record, `a733_cpu_clock_report`,
the IOMMU exclusion record, the output task's loop and observation reply, the
device-memory allocator and the thermal trip table. It adds no register path,
mapping check, boot record or failure class. It needs no change from the
primary agent.

## Measurement

`apps/gpu-display/dram-controller-load.c`, compiled only with
`SUNSTONE_GPU_DISPLAY_DRAM_LOAD=ON`, produces the windows. The table and
no-table load images run in one board session.

- **Session.** After the third presentation, slot 0 scans out 1280×720 RGB565
  at 60 Hz (110.6 MB/s). Phase A lasts 2 s (40 windows) with scan-out only.
  Phase B adds the copy load until `stop_at` − 1 s and needs at least 8 s (160
  windows). `stop_at` is 18 s into the 20 s session, so the third presentation
  must come by 7 s. The host model places it at 0.27 s, and the presentation
  record's `observed_us` gives the board time. A later presentation makes the
  run inconclusive.
- **Copy load.** `a733_smp_diagnostic_start` creates eight tasks with
  `xTaskCreateStaticAffinitySet`, priority 1, a NULL context and affinity mask
  `1 << core` for cores 0–7. `gpu_display_task_privileged` admits their entry
  with a NULL context in the load build only, so they run at EL1. Each task
  takes its slot from `portGET_CORE_ID()`. Before creating them,
  `a733_smp_diagnostic_start` acquires one 32 MiB lease at 2 MiB alignment
  from the write-back pool (`a733_memory_reservations_init`, 768 MiB) under
  owner `GPU_DISPLAY_DRAM_LOAD_OWNER`, a value no other gpu-display owner uses.
  The lease stays in `DEVICE_MEMORY_CPU` until power-off and no device is
  granted it. Each task copies between a 2 MiB source and a 2 MiB destination
  with a loop of two `LDP` and two `STP` of Q registers. Every task already
  saves Q0–Q31 (`configUSE_TASK_FPU_SUPPORT` is 2).
- **Inherited fields.** Nothing in the OS resets the NSI or restores the port
  fields, so a table written earlier in a board session persists. The session
  therefore runs the no-table load image before any image that writes the
  table. A no-table run counts only if its recorded inherited fields of ports
  2, 14 and 15 differ from the table. If boot0 already sets the table values,
  the scan-out row is recorded as not demonstrable.
  `capture.py` enforces the field rule when it moves to reply version 2.
- **Conclusiveness.** The no-table image must show contention: its phase B mean
  latency must exceed its phase A mean by more than 2 √(SE_A² + SE_B²). If it
  does not, the next load image raises the clusters through the existing CPU
  clock driver. If that run still shows no contention, the scan-out row is
  recorded as not demonstrable under CPU load and the table stays at the vendor
  values. CPU bandwidth per core is passes × 4 MiB over the phase B time. A
  run whose phase B was abandoned is inconclusive.
- **Latency statistic.** For each accepted window, x is port 2's latency sum
  divided by its read requests, in MBUS cycles per request, Q8 fixed point. A
  window with x ≥ 2²⁴ counts as out of range, so Σx² over at most 360 windows
  stays below 2⁵⁷. Per phase the block holds Σx and Σx². The host tool derives
  the mean m, the standard deviation and the standard error SE over the n
  accepted windows.
- **Transport.** The gpu-display observation reply's `terminal.reserved[136]`,
  bytes 888..1023, becomes the evidence block and the reply version becomes 2.
  The 1,024-byte size and its static assert stay. `capture.py` accepts only
  reply version 2 from gpu-display. `a733_dram_block()` in the hook object
  fills the hook fields; the bits after the hook result come from the build
  macro and the hook's return value. `a733_dram_window_block()` in the load
  object adds the window and copy-slot fields, so images without the load
  mode link no packing code for them.

  | Offset | Fields |
  | --- | --- |
  | 0 | `u16` ×8: windows started, accepted in phase A, accepted in phase B, refused late, refused idle, refused free-running, out of range, abandoned |
  | 16 | `u16`: result bits (hook built, table image, hook result, collect disables first, phases ended early); `u16`: stopping field of a failed hook (0, or 1 + 3 × port index + field index) |
  | 20 | `u16` ×6: inherited and final fields of ports 2, 14 and 15 (mode bits 1–0, priority bits 5–2, select bit 6) |
  | 32 | `u16` ×4: configured CPU-L, CPU-B and DSU MHz (0 when undecoded), 0 |
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
| Hook | 2 CCU reads; per field 1 NSI access at target, else 3; at most 27 NSI accesses; 1 lock pair; once per framebuffer instance; 0 NSI accesses when the NSI is not running | Host model of the framebuffer prepare sequence | 0 OS accesses |
| HDMI, DisplayPort and steady state | 0 added accesses, tasks, timers or interrupts | Linked-symbol check; host model | 0 |
| Window | Init: 8 clock reads, once. Start: 6 NSI accesses, 1 lock pair, 2 ISB, no added DSB. Collect: 6 NSI reads (7 accesses after the fallback), 1 lock pair, 1 ISB, no added DSB. Abandon: 0 register accesses; 1 counter read in phase B. Pending: 1 ISB, 1 counter read. At most 10 µs per phase | Host model per phase; counter around each phase on the board | None |
| Counter headroom | No 32-bit wrap in P | Arithmetic at 76.8 GB/s: port 2 ≤ 2.4 × 10⁸ units; period ≤ 3 × 10⁷ cycles at 600 MHz; latency sum below 2³² while DE averages under 143 reads outstanding | Not applicable |
| Copy-load stop | No pass starts after the end phase or a state change is stored | Host model of the copy task loop | Not applicable |
| Scan-out | HDMI stays `ACTIVE` without `A733_HDMI_UNDERFLOW`; m(no table) − m(table) in phase B > 2 √(SE₁² + SE₂²) | Both load images, under the conclusiveness rule | Not measured |
| DDR trip | RTF ≤ 1.05 × without the trip; DDR ≥ 85 °C while CPUB < 90 °C for at most 60 s of 600 s | OS image recognizing recorded input for 10 min, 25 °C ambient, fan off, with and without the trip; also time at the 1014 MHz cap | Not measured: RTF, DDR − CPUB per sample, time each channel is ≥ 85 °C. Both runs belong in the board session that qualifies the throttle, before THROTTLE reaches the CPU clock driver |
| Code size | ≤ 2 KiB text, ≤ 128 B data outside the load mode | `llvm-size` | 0 |

RTF is recognition wall time divided by audio duration. If a conclusive run
fails the scan-out row, the table and the hook are removed. The DDR trip stays
even past its bound, because refresh behavior above 85 °C is unknown. The trip
is raised only when boot0's derated-refresh setting or MR4-driven refresh is
shown.

## Left out, and the evidence that adds each item

- Training, frequency scaling and voltage: none; boot0's routine owns them by
  project policy.
- Memory configuration in evidence records: a board read showing that EL1 can
  read boot0's parameter copy at `0x47008000`.
- DRAMC fields and temperature-based refresh state: the boot0 write-sequence
  trace.
- DRAM firewall check: the boot0 trace naming the 120 MiB secure region.
- TA ports 32 and 33 and the CPU direct-channel counters: an image whose CPU
  traffic its own tasks cannot count.
- Other masters (GPU 6, NPU 18, VE, ISP, CSI, DI, G2D, USB_PCIE): an image
  that runs one during scan-out and shows an underflow or a DE latency rise.
- The hook in HDMI and DisplayPort images: a passing scan-out row.
- Limiters, runtime priority changes and an adaptive period: a board use.

## Changes outside this component

No package in this wave owns `drivers/display/a733_display_framebuffer.c` or
its header, or `cmake/a733-gpu-display.cmake`. The coordinator assigns those
edits, and the hook and window code merge with them. The gpu-graphics
package owns `apps/gpu-display/` and `checks/gpu-display*`, and the
memory-architecture package owns `checks/device-memory-*`. The
`checks/thermal-policy.c` edit merges with the DDR trip, because the trip
breaks its old DDR case.

- Framebuffer: `dram_address()`, the stage 4 call and the evidence member
  under `SUNSTONE_DRAM_PRIORITY`, and the load accessor under
  `SUNSTONE_GPU_DISPLAY_DRAM_LOAD`.
- `apps/gpu-display`: the evidence block and reply version 2 in
  `observation.h`. In the load build only, the window step and abandon rules in
  `display.c`, the copy tasks, lease and privileged entry in `main.c`, and the
  owner constant in `application.h`. `capture.py` and the observation host
  cases move to reply version 2.
- `cmake/a733-gpu-display.cmake`: the two flags for the three images.
- `checks/device-memory-capacity.c`: a replay that acquires the load image's
  write-back leases, including the 32 MiB lease at 2 MiB alignment.
- `checks/thermal-policy.c` changes with the DDR trip. Its DDR reading in the
  case without a passive trip is code 1395 (84.96 °C), and its
  single-channel critical case uses the NPU channel. New cases expect
  THROTTLE at code 1394 (85.02 °C), hold it at 1428 (83.01 °C), release it at
  1429 (82.95 °C) and reach CRITICAL at 970 while throttled.
- `checks/index.json`: `checks/dram-controller.py` enters the index in the
  same merge as the hook.

New host cases in `checks/dram-*`. The cases that need the framebuffer and
gpu-display edits are a merge condition for the change that first links
`a733_dram.c` or `a733_dram_load.c` into an image, and the hook is not
recorded as tested before they pass:

- a full prepare sequence with `+0x580` bit 31 clear makes zero NSI accesses;
  with only reset bit 30 or a `+0x584` bit wrong, it reaches the stage 4
  failure path with zero NSI accesses;
- a full prepare sequence calls the hook once, at the step from stage 4 to 5;
- a mismatch at each port and field reaches the failure path and leaves later
  fields untouched; unrelated bits survive every write; a difference outside
  the mask is not a mismatch; priority subfields 3 and 1 with p = 3 become
  `0xf`;
- the reads-only build writes nothing; an HDMI build makes no NSI access;
- the period written is P × `fabric_hz[2]`; a start at stage 4, in state 2 or
  3 or after phase B makes zero accesses;
- each refusal rule, including zero read requests, refuses as specified;
  collect makes no write before the fallback and one after the first
  free-running refusal, including one refusal among held windows;
- abandon at `stop_at` and at `output_fault` makes zero register accesses;
  an abandon in phase B records the phase B end and the early-end bit;
- no copy pass starts after the end phase or state 3 is stored.

## Board evidence that would confirm the design

- Inherited and final fields and the two CCU words from the gpu-display images.
- The scan-out row from both load images, no-table image first, with the
  inherited fields, the CPU clocks, the copy passes per core, the third
  presentation time and the collect order bit, which shows whether the
  counters hold or run past the period.
- The DDR trip row with its baseline and capped-interval figures.
- The offset between the THS DDR channel and the DRAM die: a thermocouple on
  the LPDDR5 package read alongside the channel during the DDR trip run.
