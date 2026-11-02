# Sustained DRAM read bandwidth

Status: implemented as a returning measurement application. The board
result is recorded in `docs/evidence/memory-dma.jsonl` once the run completes.

The application in `apps/dram-bandwidth/` measures how many bytes per second a
set of cores reads from DRAM when the data does not fit in any cache. Speech
and language models stream their weights from DRAM once per chunk of audio or
text, so this rate bounds how fast a model with weights larger than the caches
can run when it computes little per weight byte.

## Method

Eight worker tasks, one pinned to each core, fill a 40 MiB work region with a
fixed 64-bit pattern, each core writing its own 5 MiB slice. They then run
timed read passes for five core sets in turn: one Cortex-A55 core (core 0),
one Cortex-A76 core (core 6), both A76 cores (6 and 7), the six A55 cores (0 to
5) and all eight cores.

- In a pass each participating core reads one slice with 128-byte vector loads
  and no software prefetch. The slice a core reads advances by one on every
  pass, so a core never reads the slice its own caches received in the pass
  before. Each A55 core has a 128 KiB private L2 cache and each A76 core a 256 KiB
  one (read from the cache ID registers on the board); all cores share a 1 MiB L3.
  Against 5 MiB read per core, at most a few percent of a pass can hit in cache.
- The cores start together at a barrier. A pass lasts from the first start to the
  last finish. The counter runs at 24 MHz, so a pass of a few milliseconds has an
  error below 0.01 percent.
- Two warm-up passes precede eight timed passes for each core set. The result
  records every timed pass, the XOR digest of the data each pass read, the
  clock records before and after the run and the five thermal sensor readings
  before and after.
- The host decoder recomputes every slice digest from the fill pattern and
  refuses a pass whose digest differs. It converts ticks to gigabytes per second
  (10^9 bytes per second).

## Memory configuration

The DRAM training records of both boards and the boot0 analysis give the
following configuration. Each item is a record or a source fact, not a
reading from the controller on the board.

- Type: LPDDR5 (boot0 parameter `dram_type` 8), 12,288 MiB, reported by boot0
  and stored in both training records.
- Channels: two (`dram_tpr13` bit 6). An LPDDR5 channel is 16 bits wide, so the
  data bus is 32 bits wide.
- Operating point: boot0 trains P-state 0 at 2040 MHz and the OS runs on it
  ([DRAM fast boot](dram-fast-boot.md)).
- Pin rate: with double data rate on the 2040 MHz clock the interface moves
  4080 megatransfers per second on 32 bits, which is 16.32 GB/s. The
  relation between the stated frequency and the data rate is an assumption.

## Predicted sustained read bandwidth

Sequential reads of an idle LPDDR5 interface reach 70 to 85 percent of the pin
rate when enough requests are outstanding, because refresh, bank turnaround and
the command bus consume the rest. That puts the ceiling at 11.4 to 13.9 GB/s.
How many requests a core keeps outstanding limits what one core reaches: with
about 12 line fills in flight at a loaded latency of 120 to 150 ns, a core
reaches at most 5 to 6.4 GB/s. The A76 core issues deeper prefetch streams and
has a larger window than the A55 core.

| Core set | Predicted GB/s (range) | Limit assumed |
| --- | --- | --- |
| One A55 core | 5 (3 to 6.5) | outstanding line fills |
| One A76 core | 9 (6 to 12) | outstanding line fills and prefetch depth |
| Two A76 cores | 12 (9 to 13.5) | approaches the interface ceiling |
| Six A55 cores | 12 (9 to 13.5) | interface ceiling |
| All eight cores | 12 (9.5 to 13.5) | interface ceiling |

A result of all eight cores below 9 GB/s would show a limit below the DRAM pins,
for example the 600 MHz memory-bus clock, and would change the plan for any
workload that streams more than that rate.

## Predicted rate of matrix products that stream their weights

A matrix product with R activation rows reads each int8 weight byte once and
uses it for R multiply-accumulates, so with R below about 8 the weight bytes
stream at close to one byte per useful multiply-accumulate. The packed 8-bit
weight by 16-bit activation kernel of the speech engine (a four-row, eight-channel
tile of dot-product instructions) computes four rows per tile whatever R is, so a
tile with R below 4 does the work of four rows. The engine's own measurement of the
eight-core kernel is 30.4 giga multiply-accumulates per second on full tiles; the
engine's worker weights (1024 for an A76 core, about 305 for an A55 core) place one
A76 core at 8.0 giga multiply-accumulates per second.

The benchmark times one pass over the twelve weight matrices of one encoder layer
of a 0.6-billion-parameter streaming recognizer (23 MiB, 24.1 million bytes; the
matrices are listed in the benchmark's source) and over one 0.5 MiB matrix called 64 times
(weights resident in the L2 and L3 caches). It uses two kernels: the engine's
linear routine as shipped, which runs on the calling core alone when R is below 16,
and the same dot-product tile with the output channels split over all eight cores and
each weight tile reused for all row groups. Predicted time per pass, from tile counts
of ceil(R/4) four-row groups, the rates above and, for the streaming stack, a floor of
24.1 MB divided by 8.4 to 12.7 GB/s (2.0 to 2.9 ms):

| R | As shipped, streaming (ms) | Split over eight cores, streaming (ms) | Split, resident (ms) |
| --- | --- | --- | --- |
| 1 | 12.0 | 3.2 to 4 | 4.4 |
| 2 | 12.0 | 3.2 to 4 | 4.4 |
| 7 | 24.0 | 6.4 to 7 | 8.8 |
| 14 | 48.0 | 12.7 to 14 | 17.5 |
| 64 | 51 | 51 to 56 | 70 |

At R = 1 and R = 2 the prediction for the split kernel is set by the padded tiles,
not by the memory. The rate in multiply-accumulates per second of the streaming pass
should therefore equal that of the resident pass within about 25 percent; a
streaming rate further below the resident rate shows a limit from the memory.

## Predicted dispatch cost and in-cache rate of the split kernel

The pass times of the split kernel at one and two rows lie close to the time that
the streamed bytes need at the read rate of all eight cores (24.1 MB at 8.4 GB/s is
2.9 ms), which suggests a memory limit. Two other costs hide inside those times:
each call dispatches seven worker tasks, and the padded tiles take compute time. The
in-cache control separates them. It dispatches once and has each worker repeat its
own 64 KiB share of a 512 x 1024 matrix 64 times, so the share stays in the first-level
and second-level caches of the core. The padded-tile rate of the eight cores at 64
rows is the best case, 74.5 giga multiply-accumulates per second (measured with
the split kernel on this board in an earlier run); a pass of 64 repeats of the 0.52
million-weight matrix then takes 64 x 0.524 M x 4 padded rows / 74.5 G = 1.8 ms at one or
two rows, 3.6 ms at seven rows, 7.2 ms at fourteen rows and 28.8 ms at 64 rows,
plus under 0.1 ms for the one dispatch. The streaming pass of twelve calls then
holds 1.3 ms of tile compute and about 0.6 ms of dispatch at one row, so a measured
streaming time near 3.1 ms leaves about 1.2 ms for the memory.

## Result

Conditions: port 3-1, development DRAM mode, CPU-L at 1794 MHz and 1000 mV, CPU-B at
2002 MHz and 1050 mV, the shared-cache clock at 1274 MHz, DDR temperature 40 to
45 degrees Celsius, no thermal throttle at the start or the end of either run. The records
are in `docs/evidence/memory-dma.jsonl` and `docs/evidence/mimus-cpu.jsonl`.

Sustained read bandwidth (median of eight passes; the range of the eight passes is
within 5 percent except for two A76 cores, 10.6 to 12.3 GB/s):

| Core set | Predicted GB/s | Measured GB/s | Share of 16.32 GB/s |
| --- | --- | --- | --- |
| One A55 core | 5 (3 to 6.5) | 5.36 | 33 percent |
| One A76 core | 9 (6 to 12) | 12.69 | 78 percent |
| Two A76 cores | 12 (9 to 13.5) | 11.37 | 70 percent |
| Six A55 cores | 12 (9 to 13.5) | 9.03 | 55 percent |
| All eight cores | 12 (9.5 to 13.5) | 8.44 | 52 percent |

One A76 core streams faster than the prediction, and every larger set streams
slower than the prediction: the aggregate rate falls as more cores read, to 8.4 GB/s
for all eight. A single stream reaches 78 percent of the pin rate, so the
interface can deliver at least 12.7 GB/s; concurrent streams from many cores lose
efficiency below that, as is usual when independent sequential streams compete for
banks and rows. A reader that needs the most bandwidth from DRAM should use the two
A76 cores, or one, and not all eight cores.

Weight-streaming matrix products, one encoder layer (twelve matrices, 24.1 MB of int8
weights), median of eight passes, in milliseconds:

| Rows R | Engine routine as shipped | Split over eight cores | Split, weights resident in cache |
| --- | --- | --- | --- |
| 1 | 3.72 | 3.07 | 1.57 |
| 2 | 3.93 | 3.09 | 1.59 |
| 7 | 7.99 | 3.45 | 3.16 |
| 14 | 15.38 | 5.33 | 6.50 |
| 64 | 28.17 | 20.73 | 27.97 |

The resident column repeats a 0.5 MiB matrix 64 times, which is 33.5 million
weights per pass against 24.1 million for the streaming pass; it is therefore 39
percent more work than the streaming pass, and it is a compute floor for the streaming
pass at R = 1, 2 and 7 only after scaling by 0.72. At R = 64 the resident figure is
slower than the streaming one because its eight workers share only 64 units of 16
channels. The measured rates differ from the prediction
in two ways. The engine routine as shipped runs below 16 rows on one A76 core and does
about 26 giga multiply-accumulates per second of padded tile work, three times the 8
assumed from the engine's worker weights; its single-core times are 3.7 and 3.9 ms at
one and two rows, not 12. The eight-core figure of 30.4 giga multiply-accumulates
per second quoted for the speech engine is the average of its whole linear stage,
including row quantization and dispatch; the split routine reaches 74 to 85 giga
multiply-accumulates per second of padded tile work.

The split routine at one and two rows takes 3.07 ms. Its tile compute is 1.1 ms
(the resident time scaled to the weight count) and its twelve dispatches cost 0.65 ms
(54 microseconds per call, from the difference between 64 separate calls and one
dispatch with 64 repeats). The remaining 1.3 ms is the memory limit: 24.1 MB in
about 2.4 ms of effective streaming is 10 GB/s, between the all-eight rate and
the single-A76 rate. At seven rows the pass is still within 10 percent of that
limit; from fourteen rows it is compute-bound.

## Reference comparison and ranked hypotheses

The aggregate read rate falls from 12.69 GB/s for one Cortex-A76 core to 8.44 GB/s
for all eight cores. This section compares every configuration that can limit
the rate with the vendor's, derives the pin rate, and ranks the explanations. The
board runs of the next section test its predictions.

### Pin rate

- The board's boot0 selects DRAM parameter set 5, whose clock is 2040 MHz and whose type is
  LPDDR5 (`docs/vendor-firmware/data/boot0-tables.json`, `dram_parameter_sets`). The chip
  datasheet gives LPDDR4 up to 2133 MHz and LPDDR5 up to 2400 MHz (section on external memory).
  LPDDR4 at 2133 MHz is the 4266 megatransfer per second part, so the datasheet states the
  clock as half the data rate, and set 5 therefore runs at 4080 megatransfers per second.
- `dram_tpr13` bit 6 selects two channels, and the datasheet gives a 32-bit interface. The pin
  rate is 4080 million transfers per second on 4 bytes, 16.32 GB/s. The training records store
  the P-state clocks (2040, 1165, 816 and 408 MHz) and not the mode registers, so the ratio
  between the stated clock and the data rate rests on the datasheet convention. A
  board read of the LPDDR5 mode registers or of the controller timing would confirm it.
- Set 1 of the same table runs at 2400 MHz (19.2 GB/s). The board does not select it.
  Reaching it needs a DRAM clock and voltage change and is not part of this study.

### Our configuration against the vendor's

Our boot path runs boot0's board setup and `init_DRAM` unchanged from the FEL-loaded wrapper
(`boot/a733/ddr-entry.S`), so the controller timing, scheduling, page policy, address mapping and
training come from the vendor's own code and parameter set. No source describes the fields of
the controller registers (`docs/vendor-firmware/boot-rom-and-boot0.md`, open questions), so
those items are equal by construction and not observable by reading.

| Setting | Our value | Vendor value | Source |
| --- | --- | --- | --- |
| Controller scheduling, page policy, bank and rank interleave, address mapping | boot0 `init_DRAM` with parameter set 5 | The same code and set | `ddr-entry.S` calls `0x48fad` and `0x49bdd`; `boot0-tables.json` |
| DRAM clock | 2040 MHz (P-state 0) | 2040 MHz for set 5 | `dram-fast-boot.md` |
| MBUS clock | 600 MHz after EL3 entry | 600 MHz | `docs/status/memory-dma.md`, board-qualified scope; `action-plan.md` item 12 |
| NSI port words `0x02023a14`, `0x02023c14`, `0x02023e14`, `0x02024014`, `0x02024214`, `0x0202446c`, `0x0202466c`, `0x0202200c`, `0x0202220c` | Not written | `0x3f`, `0x7f`, `0xf`, `0xf1`, `0xf1`, `0x40a05`, `0x40a05`, `1`, `1` | boot0 routine `0x48f38`, called from `0x49050` after `init_DRAM`; the FEL wrapper skips it (`boot-rom-and-boot0.md`); the SCP writes the same values on resume (`scp-firmware.md`, step 9) |
| NSI initiator ports for DE, IOMMU and the other masters (priority, mode, select) | Not written, so reset values | Linux `sunxi_set_nsi_qos_params` writes DE priority 2 and IOMMU priority 3 | `dram-bus-priority.md`; the CPU is not an NSI port master (`dram-controller.md`) |
| CPU to DRAM path | CPU clusters reach the controller through direct channels that no NSI port counts | The same | `dram-controller.md` |
| DSU and L3 clock | 1274 MHz at 1000 mV, the vendor's top point | 780 MHz left by boot0; the shipped kernel builds no dsufreq driver; the vendor table's top point is 1274 MHz | `cpu-top-opp.md`; `board-kernel-config`; `sunxi-dsufreq.c` |
| DSU auxiliary control (`CLUSTERACTLR_EL1`) | Erratum values for the DSU revision, bit 15 and bits 16:15 when selected by the revision | Not written by the vendor monitor (DSU errata reported, not compiled) | `platform/a733/cpu_coherence.c`; `tf-a-monitor.md` |
| `CLUSTERECTLR_EL1` | Reset value | Reset value (the vendor monitor never writes it) | `reference-backsolve-cpu.md`, errata paragraph |
| A55 `CPUECTLR_EL1`, which holds the L1 prefetch limit (`L1PCTL`, reset 5 outstanding) and the L3 prefetch distance (`L3PCTL`) | Reset value, except revision 0 with no L3 | Reset value | A55 TRM `CPUECTLR`; `cpu_coherence.c` line 27; `reference-backsolve-cpu.md` |
| A76 `CPUECTLR_EL1`, which holds `PF_DIS` (hardware prefetch, reset enabled), the single-stride L2 distance (reset 40 lines) and the DRAM prefetch hints `PFT_LS` (reset off) | Reset value for r4p1 | Reset value | A76 r4p1 TRM `CPUECTLR_EL1`; `cpu_coherence.c` lines 39 and 40 apply only up to r2p0 and r3p0 |
| A76 `CPUACTLR2_EL1` (the board's A76 cores are revision r4p1, MIDR `0x414fd0b1`) | Bits 0, 16 and 22 (errata 2356586 and 3888013 and the static speculative-store-bypass control); `CPUACTLR_EL1` bit 13 and `CPUACTLR2_EL1` bit 2 apply only up to r4p0 and are not set | Only erratum 1946160 (system registers, not these bits); the dynamic speculative-store-bypass control clears bit 16 | `cpu_coherence.c` lines 31 to 38; `reference-backsolve-cpu.md`, row "A76 reset errata"; `docs/hardware/a733-facts.md`; the bit meanings are reserved in the TRM |
| DRAM temperature derating | Not applied; DDR at 40 to 45 degrees Celsius | Not applied below the trip | `dram-controller-measurements.md` |

Two differences from the vendor remain after this comparison:

1. The vendor's NSI words are not written in our boot path. Their ports 29 to 35 lie beyond the
   21 initiator ports of `enum nsi_pmu` for this chip, and ports 32 and 33 are the target agents
   (`dram-controller.md`, the item on TA ports 32 and 33), which sit on the way to the DRAM
   controller. Boot0 and the SCP both write them, and the meaning of the fields is not documented.
   The action plan listed three of the nine words as not written by boot0; the routine's code at
   `0x48f38` writes all nine.
2. The A76 auxiliary control bits listed above are an addition of ours. Their meaning is
   reserved in the TRM, except that bit 16 is the speculative-store-bypass control (the code comment on `cpu_coherence.c` line 35 and `cortex_a76.S` name it). The streaming loop issues no stores, so the speculative-store-bypass
   setting (bit 16) has no work to slow; the other bits are unexplained.

Neither the prefetchers (reset values at both levels) nor the DSU clock differ.

### Benchmark check

`apps/dram-bandwidth` meets the three requirements of the brief in part:

- Alignment and size: each core reads its own 5 MiB slice of a 40 MiB buffer. The slices are
  contiguous and begin at multiples of 5 MiB from the buffer base. The base address is not
  recorded, so the run does not show the alignment to the page that the controller sees. The
  buffer exceeds the 1 MiB L3 by a factor of 40.
- Start: the cores start at a barrier, and a pass lasts from the first start to the last finish.
- Reporting: the result holds only the pass duration and not the time of each core. With a
  fixed amount of work per core, the faster cores finish first and the remaining cores read
  alone, so the aggregate rate of the all-eight pass is the bytes divided by the slowest core's
  time, which is lower than the rate while all cores read. The aggregate for two equal A76 cores
  or six equal A55 cores is not affected in the same way.

`apps/dram-scaling` replaces the pass with a fixed 12 ms window per variant. Every core streams
until the window ends and reports the chunks read and the time to its last chunk. The
decoder reports the rate per core, the sum of those rates, and the bytes over the slowest core's
time, which is the earlier figure. The result records the buffer base address.

### Ranked hypotheses for the falling total

| Rank | Hypothesis | Why | Prediction that separates it |
| --- | --- | --- | --- |
| 1 | Independent sequential streams lose row-buffer hits and bank parallelism in the controller | A single stream reaches 78 percent of the pin rate, two equal A76 streams 70 percent, six A55 streams 55 percent: the loss grows with the number of streams. The controller's scheduling and mapping equal the vendor's, so the loss is a property of the hardware. | The all-core sum changes when the streams are staggered by a bank or row amount (stagger variants), and falls further when a core issues two or more streams (mode 2) or page-sized strides (mode 3). A flat result across staggers excludes it. |
| 2 | The interconnect words that the vendor writes after DRAM initialization change arbitration towards the controller | They are absent in our boot path and present in the vendor's, and ports 32 and 33 are the target agents of the controller. | Not separable with the benchmark alone. A board read of the nine words shows whether the reset values differ; a write of the vendor values (a change that needs approval) then shows the effect on the all-core sum. |
| 3 | The all-eight and six-core figures understate the rate while all cores read | Equal work per core and different core speeds leave a tail with fewer readers. | The sum of the per-core rates in the fixed window exceeds the earlier figure by more than 10 percent for all eight cores and by less than 3 percent for six A55 cores. |
| 4 | Limit on outstanding transactions of the DSU or the cluster interface, with prefetch traffic that is fetched and evicted before use | Six A55 cores prefetch up to 5 lines each at L1 and deeper at L3 into a 128 KiB private L2 and a shared 1 MiB L3. | A plateau instead of a fall: the per-core rate falls as 1/n beyond a core count, and the total stays flat. The 4 KiB chunk variants (modes 1 to 3) reset the stride detector at every chunk and so reduce prefetch depth; a higher total there would point to prefetch waste. |
| 5 | Placement of the buffer across banks and ranks, through the base address alignment | Unknown base and unknown mapping. | The stagger variants and the recorded base address. |
| 6 | MBUS or DRAM clock below the assumed value, or refresh derating | MBUS reads 600 MHz after EL3 entry on both boards, DDR is at 40 to 45 degrees Celsius. | Excluded by the records; the single A76 core reaches 78 percent of the pin rate on the same clocks. |

The performance monitor counters and the controller counters are not available to this
application: its tasks run at EL0, where the counters trap (`docs/cpu-pmu.md`), and no source
documents the controller registers. The run therefore separates the hypotheses by the
variants only.

### Board runs

One image runs 32 variants on port 3-1 in development DRAM mode at the approved operating
points (CPU-L 1794 MHz, CPU-B 2002 MHz, DSU 1274 MHz): the core sets of one, two, four and six A55
cores, one and two A76 cores, four A55 with both A76 and all eight cores; the stagger offsets 4 KiB,
64 KiB and 256 KiB (core c starts c times the stagger into a 1 MiB window of its slot) for six
A55, two A76 and eight cores; and the three access patterns of modes 1 to 3 for one A55, one
A76, six A55, two A76 and eight cores. Each variant reads one warm-up window and three timed
windows of 12 ms.
