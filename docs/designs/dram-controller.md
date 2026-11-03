# DRAM controller and memory-bus priority

Status: the DDR passive trip is **implemented and host-tested** in the thermal policy that the OS and thermal images link. The priority hook, the latency window and the copy pass are **implemented and host-tested** in `drivers/dram/` (see [memory-bus priority](dram-bus-priority.md)); no image links them. The framebuffer calls, the load application and the board measurement are **open**. Host measurements and the board plan are records in [dram-controller.jsonl](../evidence/dram-controller.jsonl) and [dram-bus-priority.jsonl](../evidence/dram-bus-priority.jsonl).

The component has three parts:

- a fixed priority table for the memory-bus ports that serve display scan-out, written once per framebuffer power-on with read-back, in gpu-display images only;
- a latency window on the display port, in two load images;
- a passive trip on the DDR temperature channel.

DRAM training, frequency scaling, voltage and the boot0 DRAM firewall stay inside boot0.

## Sources

- **Fabric clocks.** `NSI_CLK_REG` is CCU `+0x580` (gate bit 31, reset bit 30), the NSI configuration word is `+0x584` (gate bit 0, reset bit 16) and `MBUS_CLK_REG` is `+0x588` (manual section 4.1.6). A set bit means the gate is open or the reset is deasserted. Bit 28 of `+0x588`, `MBUS_DFS_EN`, enables dynamic frequency scaling of the MBUS clock. `a733_media_clock` compares `+0x580` and `+0x588` with the board record under mask `0x8700001f`, which requires gate bit 31 and leaves out reset bit 30 and `MBUS_DFS_EN`. The compare runs in `stable()` on every media-clock poll from the baseline step on; in the framebuffer that is prepare stage 4, after DE_SYS power-on in stage 3. No driver checks `+0x584`.
- **NSI port fields.** The pinned `sunxi-nsi.c` and `sunxi-nsi.h` place NSI port n at `0x02020000 + 0x200 × n`. Vendor field writes are read-modify-writes: mode is bits [1:0] of `+0x10`; priority is two 2-bit fields at bits [1:0] and [3:2] of `+0x14`, both set to p ≤ 3; select is bit 0 of `+0x18`. The counters are enable bit 0 of `+0xc0`, clear bit 0 of `+0xc4`, the period in MBUS cycles at `+0xc8`, read request, write request, read data and write data counts at `+0xcc..+0xd8`, and the read latency sum at `+0xdc`. Port 2 counts data in 16-byte units. The pinned `sunxi-nsi-bw.c` reads the latency sum and request count while the counters stay enabled, and reports 0 when the request count is 0.
- **Port owners.** The `nsi0` node of the pinned `sun60iw2p1.dtsi` assigns DE to port 2 and the IOMMU0 and IOMMU1 page-table walkers, which serve DE's translated reads, to ports 14 and 15. The CPU clusters reach the DRAM controller through direct channels that no NSI port counts (board result: the counters of ports 19 and 20 and of target agents 32 and 33 read zero under CPU traffic, `1793601581524930927-ca5ca9c7` in [memory-dma.jsonl](../evidence/memory-dma.jsonl)).

## DE uses NSI port 2 and its IOMMU walkers use ports 14 and 15

The vendor driver reaches a port by two routes, one for the field writes and one for the counter reads. Both give the same numbers, and the U-Boot driver uses the same addresses. `checks/dram-controller.py` checks each fact below in the pinned sources.

- **Port enum.** For `CONFIG_ARCH_SUN60IW2`, `enum nsi_pmu` in the pinned `sunxi-nsi.h` gives DE port 2, IOMMU0 port 14 and IOMMU1 port 15. The `nsi0` node gives its `de`, `iommu0` and `iommu1` children the same `id` values.
- **Field writes.** `sunxi_set_nsi_qos_params` in `sunxi-nsi.c` compares each child's name with the sun60iw2 `pmu_name[]` table and passes the index of the match to `nsi_port_setmode`, `nsi_port_setpri` and `nsi_port_setio`. In that table `"de"` is entry 2, `"iommu0"` entry 14 and `"iommu1"` entry 15. The setters subtract one from the port only when `AW_NSI_CPU_CHANNEL` and `NSI_HARDCODED_PORT_MAPPING` are both defined, and `sunxi-nsi.h` defines `NSI_HARDCODED_PORT_MAPPING` only for sun55iw3.
- **Counter reads.** `sunxi-nsi-bw.c` addresses each counter word with the master's `ia_index`. The node sets `sub_node_id_mapping`, so `sunxi-nsi.c` sets `ia_index` to the child's `id`: again 2, 14 and 15.
- **U-Boot.** The pinned U-Boot `drivers/nsi/sunxi_nsi.c` addresses `SUNXI_NSI_BASE + 0x200 × id`, with `SUNXI_NSI_BASE` equal to `0x02020000` in `plat-sun60iw2p1/cpu_autogen.h`. Ports 2, 14 and 15 are at `0x02020400`, `0x02021C00` and `0x02021E00`.
- **Counter clock.** With `topology_type = <2>`, `__rate_for_master` takes each port's clock from `master_clks`. Ports 2, 14 and 15 are absent from that list and keep clock index 1, which the node names `bus` and points at `CLK_MBUS`. The window period is therefore written in MBUS cycles.

## Data flow and ownership

- **Port table.** A const table sets DE port 2 to priority 2 and IOMMU ports 14 and 15 to priority 3, each with mode 0 and select 1, the vendor values. The priority mask is `0xf` and its target is `(p << 2) | p`, so unequal subfields are rewritten.
- **Priority hook.** `a733_dram_priority(io, evidence)` makes every access through an `io` pair, `read(context, address, &value)` and `write(context, address, value)`. The hook reads `+0x580` and `+0x584` and requires both resets deasserted and both gates open; otherwise it records the two words, makes zero NSI accesses and returns failure. For each field it reads the word, writes only a field that differs from the target (other bits kept) and reads it back under the field mask; the first mismatch stops the hook with failure. The hook zeroes the evidence block and fills its hook part: the result bits, the two CCU words, each port's inherited and final fields and the stopping field (0, 1 + 3 × port index + field index, or 10 for a failed CCU read). Built with `SUNSTONE_DRAM_PRIORITY=1`, the hook makes the same reads and writes nothing.
- **Register path.** The framebuffer passes an `io` over its `raw_read` and `raw_write`, which check ownership, address, mapping (`a733_mapping_check`, Device-nGnRnE) and timeliness per access. A `dram_address()` admits a fixed set: CCU `+0x580` and `+0x584`, and the `+0x10`, `+0x14` and `+0x18` words of ports 2, 14 and 15.
- **Caller.** Stage 4 of `a733_display_framebuffer_prepare` calls the hook when `a733_media_clock_poll` returns `A733_MEDIA_PREPARED`, before the stage advances, once per framebuffer instance. The IOMMU admission states every real master excluded and drained, so no master on ports 2, 14 or 15 is active. Any hook failure takes the stage's existing `goto fail` path, which retains the framebuffer.
- **Images.** The call and the evidence member compile only when `SUNSTONE_DRAM_PRIORITY` is defined: 1 (reads only) in the no-table load image and 2 (table) in the table load image. The normal gpu-display image defines it as 2 only after the load images show Non-secure reads and writes that read back. The HDMI and DisplayPort builds leave it undefined.
- **Latency window.** Port 2 over a period P of 50 ms, in the load images only, through a load-only accessor compiled under `SUNSTONE_GPU_DISPLAY_DRAM_LOAD` that reuses `raw_read` and `raw_write` (in that build `dram_address()` also admits port 2's words `+0xc0..+0xdc` and reads of CCU `+0x588`). Times come from an ISB followed by `MRS CNTPCT_EL0`.
  - *Start* runs only while the display stage is 3, the application state is 1, three presentations are done and the current phase has not ended. It reads the counter (t0), makes 6 accesses in the vendor order (write the period P × `fabric_hz[2]`, clear enable bit 0, set clear bit 0, write the saved enable word with bit 0 set) and reads the counter again (t1). The first start of a session first reads `+0x588`; with `MBUS_DFS_EN` set the MBUS rate can change during a window, so that start makes no port access, ends the phases early and sets a result bit.
  - *Collect* returns pending until the counter passes t1 + P, then makes 6 reads with the counters enabled (four counts, the latency sum and the read data count again) and reads the counter (t_end).
  - *Refusal.* Collect refuses a window if t_end ≥ t0 + 2P, if the two read data counts differ (the counter is free-running), or if read requests are 0 or read data is below 1,024 units (16 KiB; scan-out alone moves about 5.5 MB per window).
  - *Free-running fallback.* After the first window refused as free-running, every later collect first writes the saved enable word with bit 0 clear and then makes its 6 reads; a result bit records the order. A free-running counter often reads equal across the two reads (DE fetches each 2,560-byte line in one burst per 22.2 µs line time), so windows before the first detection are accepted; the statistic is a ratio and is not biased by them.
  - *Stop and abandon.* No window starts after phase B ends, at stage 4 or in a state other than 1. A window in flight at `stop_at` or at `output_fault` is abandoned in software and counted; the next start disables the counters first. An abandon during phase B reads the counter once to record the end of phase B and sets the result bit for phases ended early.
- **DDR temperature.** The DDR entry of `trips[]` in `a733_thermal.c` is {85000, 2000, 110000}; the vendor zone has only the 110 °C critical trip. JEDEC specifies LPDDR5 standard refresh up to 85 °C die temperature, and no decoded field shows derated refresh. The input is the SoC THS "DDR" channel, a proxy with an unknown offset from the DRAM die. The trip raises the global THROTTLE level, which has no consumer in the OS image: its thermal service acts only on CRITICAL. Connecting THROTTLE to the CPU clock driver, which caps both clusters at 1014 MHz, waits for the DDR trip row of the performance budgets, measured in the board session that qualifies the throttle. The policy check expects THROTTLE at code 1394 (85.02 °C), holds it at 1428 (83.01 °C), releases it at 1429 (82.95 °C) and reaches CRITICAL at 970 while throttled.

## Interrupts, memory, barriers and cores

The component installs no interrupt or timer. The NSI interrupt (SPI 43) has no qualified delivery and stays masked. The window step runs in the output task's existing `vTaskDelay(1)` loop in `apps/gpu-display/display.c`, a tick wait that [performance architecture](../performance-architecture.md) section 1.3 permits for a device without qualified interrupt delivery. The pending check is one ISB, one counter read and one compare, about 1,000 per second.

Records and window state live in normal write-back memory that no device reads, so they need no cache maintenance. The
hook adds no barrier. The framebuffer's `raw_read` and `raw_write` keep their `DSB SY` pair per access, at most 29 pairs once per framebuffer instance, and the last NSI access of a phase has completed before the counter read that ends the phase (section 1.2).

Each port has one writer: the hook writes ports 2, 14 and 15; the window writes only port 2's counter words. Both run in the output task, which also fills the evidence. The copy tasks share one phase word with
the output task. It wakes them with a task notification at the start of phase B and stores the end phase with
release. Each task loads the phase word and the application state with acquire once per pass and calls `vTaskSuspend(NULL)` after the end phase or a state other than 1. Each task counts passes in its own 64-byte-aligned slot, which the output task reads with acquire after storing the end phase. The component adds no lock.

## Failure classes

- NSI reset asserted or gate closed, a failed register access, or a read-back mismatch: the framebuffer's stage 4 failure path, before DE top clock writes, IOMMU open or DE DMA. That path retains the register lock if held, the clock tokens and the framebuffer lease.
- Window refused or abandoned: class 1, counted; nothing is retained.
- A failed access in the load accessor: the framebuffer's existing retain path.
- A synchronous external abort or SError from an NSI or `+0x584` access, as a Secure-only NSI may raise: the access never returns, the platform's exception capture records ESR, ELR and FAR and ends the image, and nothing retries the access.

The component reuses the framebuffer's register path, failure path, fabric compare and exclusion, the board fabric record, `a733_cpu_clock_report`, the IOMMU exclusion record, the output task's loop and observation reply, the device-memory allocator and the thermal trip table.

## Measurement design

`apps/gpu-display/dram-controller-load.c`, compiled only with `SUNSTONE_GPU_DISPLAY_DRAM_LOAD=ON`, produces the windows. The table and no-table load images run in one board session. The file is not in the tree yet.

- **Session.** After the third presentation, slot 0 scans out 1280×720 RGB565 at 60 Hz (110.6 MB/s). Phase A lasts 2 s (40 windows) with scan-out only. Phase B adds the copy load until `stop_at` − 1 s and needs at least 8 s (160 windows). `stop_at` is 18 s into the 20 s session, so the third presentation must come by 7 s; a later presentation makes the run inconclusive.
- **Copy load.** `a733_smp_diagnostic_start` creates eight tasks with `xTaskCreateStaticAffinitySet`, priority 1, a NULL context and affinity mask `1 << core`; `gpu_display_task_privileged` admits their entry in the load build only, so they run at EL1. Before creating them it acquires one 32 MiB lease at 2 MiB alignment from the write-back pool under owner `GPU_DISPLAY_DRAM_LOAD_OWNER`; the lease stays in `DEVICE_MEMORY_CPU` until power-off and no device is granted it. Each task copies between a 2 MiB source and a 2 MiB destination with a loop of two `LDP` and two `STP` of Q registers.
- **Inherited fields.** Nothing in the OS resets the NSI or restores the port fields, so a table written earlier in a board session persists. The session runs the no-table load image before any image that writes the table. A no-table run counts only if its recorded inherited fields of ports 2, 14 and 15 differ from the table; otherwise the scan-out row is not demonstrable. `capture.py` enforces the field rule when it moves to reply version 2.
- **Conclusiveness.** The no-table image must show contention: its phase B mean latency must exceed its phase A mean by more than 2 √(SE_A² + SE_B²). If it does not, the next load image raises the clusters through the CPU clock driver; if that run still shows none, the scan-out row is not demonstrable under CPU load and the table stays at the vendor values. A run whose phase B was abandoned is inconclusive.
- **Latency statistic.** For each accepted window, x is port 2's latency sum divided by its read requests, in MBUS cycles per request, Q8 fixed point. A window with x ≥ 2²⁴ counts as out of range, so Σx² over at most 360 windows stays below 2⁵⁷. Per phase the block holds Σx and Σx²; the host tool derives the mean, standard deviation and standard error over the accepted windows.
- **Transport.** The gpu-display observation reply's `terminal.reserved[136]`, bytes 888..1023, becomes the evidence block and the reply version becomes 2; `capture.py` accepts only version 2 from gpu-display. `a733_dram_window_block()` in the load object adds the window and copy-slot fields.

| Offset | Fields |
| --- | --- |
| 0 | `u16` ×8: windows started, accepted in phase A, accepted in phase B, refused late, refused idle, refused free-running, out of range, abandoned |
| 16 | `u16`: result bits (hook built, table image, hook result, collect disables first, phases ended early, `MBUS_DFS_EN` found set); `u16`: stopping field of a failed hook |
| 20 | `u16` ×6: inherited and final fields of ports 2, 14 and 15 (mode bits 1–0, priority bits 5–2, select bit 6) |
| 32 | `u16` ×4: configured CPU-L, CPU-B and DSU MHz (0 when undecoded); at offset 38, `phase_ticks_max`, the longest start or collect in 24 MHz ticks, saturating at 65,535 |
| 40 | `u32` ×2: CCU `+0x580`, `+0x584` |
| 48 | `u32` ×2: `fabric_hz[2]`; phase B length in ms |
| 56 | `u64` ×6: per phase, Σx, Σx², port 2 read data |
| 104 | `u32` ×8: copy passes per core in phase B |

## Performance budgets

Access counts come from host models that run the compiled code against counting register files. Times come from the architectural counter on the board. The measured host figures are in `dram-controller-ddr-trip` and `dram-bus-priority-host-measurement`.

| Metric | Target |
| --- | --- |
| Hook | 2 CCU reads; per field 1 NSI access at target, else 3; at most 27 NSI accesses; 1 lock pair; once per framebuffer instance; 0 NSI accesses when the NSI is not running |
| HDMI, DisplayPort and steady state | 0 added accesses, tasks, timers or interrupts |
| Window | Init: 8 clock reads, once. Start: 6 NSI accesses (the first start also reads `+0x588`), 1 lock pair, 2 ISB, no added DSB. Collect: 6 NSI reads (7 after the fallback), 1 lock pair, 1 ISB, no added DSB. Abandon: 0 register accesses. Pending: 1 ISB, 1 counter read. At most 20 µs per phase |
| Counter headroom | No 32-bit wrap in P: at 76.8 GB/s, port 2 ≤ 2.4 × 10⁸ units; period ≤ 3 × 10⁷ cycles at 600 MHz; latency sum below 2³² while DE averages under 143 reads outstanding |
| Copy-load stop | No pass starts after the end phase or a state change is stored |
| Scan-out | HDMI stays `ACTIVE` without `A733_HDMI_UNDERFLOW`; m(no table) − m(table) in phase B > 2 √(SE₁² + SE₂²) |
| DDR trip | Recognition RTF ≤ 1.05 × without the trip; DDR ≥ 85 °C while CPUB < 90 °C for at most 60 s of 600 s; OS image recognizing recorded input for 10 min, 25 °C ambient, fan off, with and without the trip; also time at the 1014 MHz cap |
| Code size | ≤ 2 KiB text, ≤ 128 B data outside the load mode |

RTF is recognition wall time divided by audio duration. If a conclusive run fails the scan-out row, the table and the hook are removed. The DDR trip stays even past its bound, because refresh behavior above 85 °C is unknown; it is raised only when boot0's derated-refresh setting or MR4-driven refresh is shown.

## Left out

- Training, frequency scaling and voltage: boot0's routine owns them.
- DRAMC fields, temperature-based refresh state and the DRAM firewall check: the boot0 write-sequence trace of the DRAMC, SMC0 and SMC1.
- Memory configuration in evidence records: a board read showing that EL1 can read boot0's parameter copy at `0x47008000`.
- TA ports 32 and 33 and the CPU direct-channel counters: board results show no NSI counter observes CPU traffic.
- Other masters (GPU 6, NPU 18, VE, ISP, CSI, DI, G2D, USB_PCIE): an image that runs one during scan-out and shows an underflow or a DE latency rise. Limiters, runtime priority changes and an adaptive period wait for a board use. The hook is not in HDMI and DisplayPort images until the scan-out row passes.

## Open work

1. Merge the integration edits and their host cases (owners: the display framebuffer and gpu-display packages; the thermal policy check changes with the DDR trip):
   - Framebuffer: `dram_address()`, the stage 4 call and the evidence member under `SUNSTONE_DRAM_PRIORITY`, and the load accessor under `SUNSTONE_GPU_DISPLAY_DRAM_LOAD`.
   - `apps/gpu-display`: the evidence block and reply version 2 in `observation.h`; in the load build only, the window step and abandon rules in `display.c`, the copy tasks, lease and privileged entry in `main.c`, and the owner constant in `application.h`; `capture.py` and the observation host cases for reply version 2.
   - `cmake/a733-gpu-display.cmake`: the two flags for the three images. `checks/device-memory-capacity.c`: a replay of the load image's write-back leases, including the 32 MiB lease at 2 MiB alignment.
   - Host cases: a full prepare sequence calls the hook once, at the step from stage 4 to 5, and makes zero NSI accesses when
     the NSI is off. Each port and field mismatch reaches the failure path and leaves later
     fields untouched. The reads-only build writes nothing and an HDMI build makes
     no NSI access. The period written is P × `fabric_hz[2]` and a start at stage 4, in state 2 or 3 or after phase B makes
     zero accesses. Each refusal rule and the free-running fallback behave as
     specified. A first start with `+0x588` bit 28 set ends the phases early.
     Abandon makes zero register accesses. No copy pass starts after the end phase.
2. Run the table and no-table load images in one board session, no-table image first: inherited fields and CCU words, then final fields, the scan-out row, the collect-order bit, `phase_ticks_max`, the CPU clocks, passes per core and the third presentation time. The prediction and conclusiveness rule are in `dram-controller-scan-out-board`.
3. Measure the DDR trip's recognition RTF and the offset between the THS DDR channel and the DRAM die (a thermocouple on the LPDDR5 package read alongside the channel) before THROTTLE reaches the CPU clock driver.
