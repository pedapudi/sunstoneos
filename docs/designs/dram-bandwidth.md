# Sustained DRAM read bandwidth

Status: qualified on the board for sustained read bandwidth by core set, for the scaling with core count and for the NSI interconnect words. Implemented as four returning measurement applications. Open: CPU memory latency under NPU and DMA load.

Speech and language models stream their weights from DRAM once per chunk of audio or text, so the sustained read rate bounds how fast a model with weights larger than the caches runs when it computes little per weight byte. Four applications measure that rate and the factors that limit it. Every board run is a record in [memory-dma.jsonl](../evidence/memory-dma.jsonl) and [dram-bandwidth.jsonl](../evidence/dram-bandwidth.jsonl), each with its prediction, conditions and conclusion.

## Applications

| Application | Measures |
| --- | --- |
| `apps/dram-bandwidth/` | Median of eight timed passes for five core sets (one A55 core, one A76 core, both A76 cores, the six A55 cores, all eight) over a 40 MiB buffer filled with a fixed pattern. Each core reads its own 5 MiB slice with 128-byte vector loads and no software prefetch; the slice advances by one every pass. Cores start at a barrier and a pass lasts from the first start to the last finish on the 24 MHz counter. The host decoder recomputes every slice digest and refuses a pass whose digest differs. |
| `apps/dram-scaling/` | Fixed 12 ms windows per variant (one warm-up, three timed). Every core streams its own 3 MiB region until the window ends and reports the chunks read and the time to its last chunk, so the host computes a rate per core and the sum of the rates of cores that read together, without a tail of idle fast cores. Variants cover core sets, start staggers, chunk sizes, one or two streams per core, page-sized strides, software prefetch and shared-stream barrier rounds. The result records the buffer base address. |
| `apps/dram-nsi/` | The same variants with worker tasks at Non-secure EL1. A job on core 6 reads the nine NSI words of boot0 routine `0x48f38` and further CCU and NSI port registers; the write image runs the variants, writes boot0's values in boot0's order with a `dmb sy` after each store, reads them back and runs the variants again. Per-core Arm performance counters (L2D_CACHE_REFILL, L3D_CACHE_REFILL, BUS_ACCESS, BUS_ACCESS_RD, STALL_BACKEND, MEM_ACCESS) cover the last timed window of each variant. Counters of NSI target-agent ports 32 and 33 and initiator ports 19 and 20 are sampled. |
| `apps/dram-qos/` | CPU memory latency with the priority of the CPU initiator ports as the variable. Probes: a pointer chase on an A76 core (7) and an A55 core (0) through 16 MiB, and a 10 kHz control-loop proxy of 16 dependent loads on core 7. Settings: ports as found, CPU ports 19 and 20 at priority 3, and priority 3 plus the high-priority read bit. Loads: none, streaming reads on the other cores, an NPU job stream, DMA0 copies. Histograms have 4-cycle resolution below 256 cycles and 16 sub-bins per octave above; the maximum is exact. |

## Memory configuration

- LPDDR5 (boot0 parameter `dram_type` 8), 12,288 MiB, two channels of 16 bits (`dram_tpr13` bit 6), a 32-bit data bus.
- Parameter set 5 of boot0 trains P-state 0 at 2040 MHz and the OS runs on it ([DRAM fast boot](dram-fast-boot.md)). The datasheet states the clock as half the data rate, so the pin rate is 4080 megatransfers per second on 4 bytes, 16.32 GB/s. The ratio between the stated clock and the data rate rests on the datasheet convention; a read of the LPDDR5 mode registers would confirm it.
- Parameter set 1 runs at 2400 MHz (19.2 GB/s); the board does not select it.
- Controller scheduling, page policy, bank and rank interleave, address mapping and training come from boot0's own `init_DRAM` with the same parameter set the vendor uses. The CPU reaches the controller through direct channels that no NSI port counts ([DRAM controller](dram-controller.md)).
- The boot path runs boot0's routine `0x49050` after `init_DRAM` (`boot/a733/ddr-entry.S` calls `0x49051`, after the restore helper at `0x92008` in the restore variant), which stores the nine NSI words in order with a barrier before each store. The case no host check executes the routine from the pinned boot0 under Unicorn and requires the nine stores, their values, their order and the call order in both wrapper variants. The microSD boot path runs boot0 itself and includes the step.
- MBUS runs at 600 MHz after EL3 entry. The DSU and L3 clock is 1274 MHz at 1000 mV, the vendor table's top point. Prefetcher control registers hold their reset values. The A76 auxiliary control bits that [the CPU coherence code](../../platform/a733/cpu_coherence.c) sets are an addition of this project whose meaning is reserved in the TRM except for the speculative-store-bypass bit.
- The NSI initiator ports (21 of them) read mode 0, priority 0, input/output select 1 and no bandwidth limit in this boot path, so the CPU, NPU, GPU, camera and display masters arbitrate with equal priority. Vendor Linux writes DE priority 2 and IOMMU priority 3 ([bus priority](dram-bus-priority.md)).
- Non-secure EL1 reads and writes the NSI without an abort. A watchdog reset restores the registers' reset values.

Non-secure EL1 access to the Arm performance counters needs the `MDCR_EL2.HPMN` change that both continuation paths apply; EL0 tasks trap on the counters ([CPU PMU](../cpu-pmu.md)). No source documents controller row hits, row misses or bank conflicts, and no NSI counter observes CPU traffic.

## Qualified figures

Port 3-1, development DRAM mode, CPU-L 1794 MHz, CPU-B 2002 MHz, DSU 1274 MHz, MBUS 600 MHz, buffer base `0x44000000`, DDR at 40 to 45 degrees Celsius.

| Quantity | Value | Record |
| --- | --- | --- |
| Pin rate | 16.32 GB/s | [reference comparison](../evidence/dram-bandwidth.jsonl) `dram-bandwidth-reference-comparison` |
| One A55 core | 5.5 GB/s (34 percent of pin) | [memory-dma.jsonl](../evidence/memory-dma.jsonl) `1793600409082394041-436818e2` |
| One A76 core | 14.8 GB/s (91 percent) | same |
| Two A76 cores | 14.1 GB/s (86 percent) | same |
| Two, four, six A55 cores | 10.0, 10.8, 9.1 GB/s | same |
| Four A55 and two A76 cores | 9.4 GB/s | same |
| All eight cores | 8.25 GB/s (51 percent) | same |
| Pass-based first measurement (one A76 core, all eight) | 12.69, 8.44 GB/s | `1793597424798692951-6741863a` |
| Writing the nine NSI words | eight-core total changes by 0.3 percent | `1793601453701366920-e6b42159` |
| Refill bytes over consumed bytes (A55, one and eight cores) | 1.00 | `1793601713672636837-caad23bd` |
| A55 backend stall fraction, one and eight cores | 0.40, 0.89 | same |
| Idle A76 pointer chase: median, 99.99th percentile | 152, 639 ns | `1793604009006779744-6a685222` |
| A76 chase with cores streaming: median, 99.99th percentile | 416, 1,662 ns | `1793604009213366498-b525353f` |
| Control loop of 16 dependent loads, idle and streaming | 2.3 us, 7.4 us (worst 10.6 us), no deadline miss | same |
| Weight-streaming encoder layer (24.1 MB) split over eight cores, one row | 3.07 ms | `dram-bandwidth-weight-streaming` |

## What limits the aggregate rate

- The aggregate rate follows the number of cores reading. It falls from 14.8 GB/s for one A76 core to 8.25 GB/s for all eight cores. Per-core access changes (start staggers, 4 KiB chunks, two streams per core, software prefetch) change the six-core and eight-core totals by at most 8 percent.
- The prefetchers fetch no unused lines (refill traffic equals consumed bytes) except under page-sized strides, which add 25 percent. Page-sized strides cut one A76 core from 14.7 to 6.7 GB/s.
- Neighbouring streams read faster than distant ones: with the same barrier per 16 KiB round, cores on neighbouring chunks of one stream are 22 percent (six A55 cores) to 28 percent (eight cores) faster than cores on separate regions.
- The backend stall fraction and the loaded read latency rise with the core count. The nine NSI words, prefetch waste, the clocks and the benchmark are not causes. The remaining explanation is the loss of row-buffer and bank locality in the DRAM controller under several independent streams, supported by exclusion and by the address dependence. A controller counter or its register documentation is needed to prove it.
- A reader that needs the most bandwidth from DRAM should use one or two A76 cores. Weight-streaming kernels in an application can read neighbouring blocks of one weight matrix on all cores in the same period. A synchronization cheaper than a barrier per 16 KiB is needed to turn the 22 to 28 percent gain into a gain over the free-running rate. That kernel design belongs to the application, not to the OS.
- With fewer than about 8 activation rows, a matrix product streams its weights from DRAM at about one byte per useful multiply-accumulate. The split kernel at one and two rows is limited by about 10 GB/s of effective streaming; from fourteen rows it is compute-bound.

## Quality-of-service settings

The vendor driver (`sunxi-nsi.c`) exposes these settings per initiator port n at `0x02020000 + 0x200 * n`. The boot path leaves them at the inherited values.

| Setting | Register | Vendor use |
| --- | --- | --- |
| Priority of a port | `+0x14` bits 3:0, two 2-bit fields, 0 to 3 | DE 2, IOMMU 3 |
| High-priority read class | `+0xc` (bit 16 on this chip) | boot0 writes 1 at `+0xc` of the ISP and CSI ports |
| Absolute bandwidth limit | `+0x28` with `+0x2c`, enabled through `nsi_port_set_abs_bwlen` | none in the device tree |
| Port mode | `+0x10` bits 1:0 | 0 for DE and IOMMU; meaning undocumented |
| Input/output select | `+0x18` bit 0 | 1 for DE and IOMMU; meaning undocumented |
| Target-agent words | `+0x14` of ports 29 to 33, `+0x6c` of ports 34 and 35 | boot0 values; meaning undocumented |

With every competitor a CPU port, raising the CPU port priority changes the chase latency by less than 5 percent, so the setting has no effect against core-to-core contention. The effect against the NPU, DMA and camera masters is unmeasured; the prediction and the decision rule are in `dram-bandwidth-qos-knobs`. No change of the default priorities is made.

## Limitations

- The pass-based measurement understates the A76 sets by 14 to 19 percent because a 5 MiB pass lasts 0.4 ms on an A76 core; use the fixed-window figures.
- `apps/dram-scaling/result.h` defines 52 variants. The first board image ran 32 of them (core sets, staggers, modes 1 to 3); the software-prefetch and shared-stream modes were added in later images.
- The A55 pointer chase on core 0 has a 99.99th percentile of 2.6 us and a maximum of 9 us with no load, and 16.6 us and 32 us with the cores streaming, against 0.64 us for the A76 core. The OS tick and interrupt handlers on core 0 are the candidate cause and are not separated.
- Row-hit and bank-conflict counters of the controller are undocumented, so the locality explanation stays unproven. The 2400 MHz parameter set is not used.

## Open work

1. Measure CPU memory latency with the NPU job stream and DMA masters loading memory and the CPU port priorities raised. The NPU condition needs the `apps/dram-qos` probes linked into a finite NPU image (the NPU stream owns those files) or a GPU streaming kernel; the DMA condition needs a captured DMA session record. Predictions 3, 4 and 6 are in `1793603527003752888-c6eca39d`, `1793603527212274330-dcf070c1` and `dram-bandwidth-qos-knobs` (outcome not-run).
2. Separate the A55 core 0 latency tail from the OS tick and interrupt handlers.
3. Read the LPDDR5 mode registers or controller timing to confirm the pin rate.
