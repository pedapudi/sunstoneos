# Sustained DRAM read bandwidth

Status: implemented as a returning measurement application. The board
results are recorded in `docs/evidence/memory-dma.jsonl`. The first measurement is
superseded for the Cortex-A76 sets by the fixed-window study at the end of this
document (`apps/dram-scaling/`).

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

### Board measurements of the scaling study

Conditions: port 3-1, development DRAM mode, CPU-L 1794 MHz, CPU-B 2002 MHz, DSU 1274 MHz,
MBUS 600 MHz, buffer base `0x44000000`, DDR 40 to 45 degrees Celsius at the start and 45
degrees at the end. Each figure is the median of three 12 ms windows of the sum of the
per-core rates. Three images (the first, plus one with a software-prefetch pattern and one with
the shared-stream patterns) repeated the base variants; the three runs agree within 2 percent.
The records are in `docs/evidence/memory-dma.jsonl`.

| Cores reading, sequential 64 KiB chunks | Run 1 GB/s | Run 2 GB/s | Run 3 GB/s | Share of 16.32 GB/s |
| --- | --- | --- | --- | --- |
| One A55 | 5.50 | 5.46 | 5.49 | 34 percent |
| Two A55 | 10.05 | 10.02 | 10.01 | 61 percent |
| Four A55 | 10.77 | 10.76 | 10.82 | 66 percent |
| Six A55 | 9.06 | 9.07 | 9.07 | 56 percent |
| One A76 | 14.83 | 14.81 | 14.86 | 91 percent |
| Two A76 | 14.02 | 14.15 | 14.08 | 86 percent |
| Four A55 and two A76 | 9.36 | 9.34 | 9.36 | 57 percent |
| All eight | 8.24 | 8.27 | 8.25 | 51 percent |

Per-core rates at eight cores: 0.86 to 1.04 GB/s for each A55 core and 1.29 and 1.46 GB/s for the A76
cores. When four A55 cores join the two A76 cores, the A76 cores fall from 7.0 to 2.0 GB/s each.

Other variants (run 1 unless stated; sums in GB/s for six A55, two A76, eight cores):

| Variant | Six A55 | Two A76 | Eight |
| --- | --- | --- | --- |
| Sequential, stagger 0 (reference) | 9.06 | 14.02 | 8.24 |
| Stagger 4 KiB, 64 KiB, 256 KiB between cores | 9.06, 9.01, 9.06 | 13.82, 13.36, 12.87 | 8.24, 8.23, 8.25 |
| Sequential, 4 KiB chunks | 9.05 | 13.28 | 8.22 |
| Two streams per core | 8.64 | 13.44 | 7.91 |
| Page-sized strides | 5.76 | 8.53 | 5.74 |
| Software prefetch into the L2 cache (run 2) | 8.36 | 13.75 | 7.94 |
| One shared stream, barrier per 16 KiB round (run 3) | 7.22 | 11.75 | 7.45 |
| Own regions, same barrier (run 3) | 5.93 | 11.30 | 5.84 |

### What the measurements show

1. **The earlier aggregate was partly an artifact for the A76 sets.** The sum of the per-core
   rates equals the bytes over the slowest core's time within 0.1 percent for every set, so the tail
   of fast cores is not the cause for the all-eight set. The earlier pass-based figures of
   12.69 GB/s for one A76 core and 11.37 GB/s for two were 14 and 19 percent below the
   fixed-window rates (14.83 and 14.08 GB/s): a pass of 5 MiB lasts 0.4 ms for an A76 core, and
   its start-up costs a visible share. The A55 sets agree (9.06 against 9.03, 8.25 against 8.44).
   One A76 core reaches 91 percent of the pin rate, so the interface, the clocks and the
   controller timing deliver at least 14.8 GB/s.
2. **The total depends on the number of cores reading and not on what each core does.** Two A76
   cores read 14.1 GB/s. Two A55 cores read 10.0, four 10.8, six 9.1 and all eight 8.25 GB/s. Four A55
   cores added to two A76 cores lower the total from 14.1 to 9.4 GB/s. Staggering the start offsets, reading 4 KiB chunks,
   issuing two streams per core and prefetching explicitly into the L2 cache change the six-core
   and eight-core totals by at most 8 percent; the stagger lowers the two-A76 total by up to 8
   percent, the opposite of what a conflict between aligned streams would cause.
   The L3 fill path of the A55 prefetchers is therefore not the limit: explicit L2 prefetches
   lower the totals. The hardware prefetchers matter for the shape of the stream: page-sized
   strides cut one A76 core from 14.7 to 6.7 GB/s.
3. **Neighbouring streams read faster than distant ones.** With the same barrier per 16 KiB
   round, six A55 cores that read neighbouring chunks of one stream reach 7.22 GB/s, and the same
   cores on separate regions 5.93 GB/s: 22 percent more. For eight cores the figures are
   7.45 and 5.84 GB/s, 28 percent more. The barrier itself costs a third of the rate of the free-running
   cores (5.93 against 9.07 GB/s), so the shared-stream total does not exceed the free-running one.
   The direction shows that the rate depends on how far apart the concurrent streams are in
   the address space, which a cluster-side limit would not.

### Cause

The evidence supports the first hypothesis of the ranking (loss of row-buffer and bank
locality when several independent streams reach the controller), with these limits:

- The controller is configured by boot0's own code and parameter set, so its scheduling, page
  policy and address mapping equal the vendor's. A vendor kernel on the same board meets the
  same loss with eight independent streams.
- The Cortex-A76 and Cortex-A55 prefetchers, the DSU clock and the MBUS clock equal the vendor's or
  its top point, and the loss does not respond to the software prefetch variant.
- The one difference from the vendor that remains is the nine NSI port words that boot0 writes
  after DRAM initialization and that the FEL wrapper skips. Their meaning is not documented, and
  no measurement so far tests them. The shared-stream result does not exclude an arbitration
  effect, because arbitration between the 21 initiators and the CPU direct channel sits in the
  same path.

The loss is therefore a property of the memory system under many independent streams, not a
SunstoneOS configuration error that the data identifies. The test of the remaining difference
needs an interconnect register write and waits for approval (next subsection).

### Plan awaiting approval (superseded)

The coordinator approved items 1 and 2 on 2026-11-02; their results are in "Results of the interconnect study" below.

1. **Read the nine NSI words** (`0x02023a14`, `0x02023c14`, `0x02023e14`, `0x02024014`,
   `0x02024214`, `0x0202446c`, `0x0202466c`, `0x0202200c`, `0x0202220c`) at EL1 in an image built on
   the existing memory-bus priority hook (`drivers/dram/a733_dram.c`). A read changes nothing, but
   Non-secure access to the NSI is unproven and an abort ends the image. Prediction: the words read
   reset values, not the vendor values `0x3f`, `0x7f`, `0xf`, `0xf1`, `0xf1`, `0x40a05`, `0x40a05`, `1`, `1`.
2. **Write the vendor values** with boot0's own order and barriers (`dmb sy` between writes,
   routine `0x48f38`), read them back, then run the scaling image. Prediction: if these words
   affect CPU traffic, the eight-core total rises above 8.25 GB/s; if the loss is the locality loss
   of the controller, the totals stay within 3 percent of the table above. Either result is
   informative, and the second ends the search for a configuration difference.
3. **Application-side remedy that needs no register write**: weight-streaming kernels read
   neighbouring blocks of one weight matrix on all cores in the same period, rather than one
   contiguous slice per core. The shared-stream result shows 22 to 28 percent against separate
   regions at equal synchronization cost; a cheaper synchronization than a barrier per 16 KiB is
   needed to turn that into a gain over the free-running 8.25 GB/s. This belongs to the
   application (mimus), not to the OS.
4. **Not proposed**: a change of the DRAM clock to the 2400 MHz parameter set (19.2 GB/s pin
   rate) needs a clock and voltage decision, and a change of the A55 prefetch registers
   (`CPUECTLR`, L1 and L3 prefetch limits) would test over-fetching but is not a vendor setting.

## Interconnect words

The coordinator approved a read of the nine NSI words of boot0 routine `0x48f38` from Non-secure EL1, and,
if the read succeeds, a write of boot0's values in boot0's order followed by a repeat of the scaling study.
`apps/dram-nsi/` implements both. Its worker tasks run at EL1 (`os_application_task_privileged` returns true
for `dram_nsi_worker`). A job on core 6 reads the nine words and 13 further registers (the CCU words
`0x02002580`, `0x02002584` and `0x02002588`, and the words `+0x10`, `+0x14`, `+0x18`, `+0xc0` and `+0xc8` of NSI
ports 32 and 33). The write image then runs the variants once with the words as found, writes the words,
reads them back and runs the variants again.

### Earlier access to the NSI

No earlier work accessed the NSI from Non-secure EL1 on a board. The memory-bus priority hook
(`drivers/dram/a733_dram.c`) is host-modeled only and linked into no image
([dram-bus-priority.md](dram-bus-priority.md#register-sources), "Unproven on the board"). The GPU NSI counters
(`drivers/gpu/a733_gpu_nsi_pmu.c`) have no board record. The FEL read of the words that the action plan
proposes (item 4) has not run. The risk is therefore unknown: a Secure-only NSI answers with an external abort,
which ends the image at the watchdog, and an abort from a lower exception level (ESR class `0x24`) would instead
show that the worker was not privileged.

### Counters

The first two write runs sampled the target-agent ports 32 and 33 and the third the initiator ports 19 and 20
(`cpu0`, `cpu1` in the sun60iw2 port enum). All read zero after 1 MiB of traffic with the clear word at zero and the
period retained, so no NSI counter observes CPU traffic, as the controller design states for the direct channel.
The fourth run therefore reads the Arm performance counters at EL1 (the change of `MDCR_EL2.HPMN` has landed in both
continuation paths). The vendor driver reads the total DRAM traffic from the counters of the target-agent ports 32 and 33
(`sunxi-nsi-bw.c`, `MBUS_PMU_TAG`): read and write request counts (`+0xcc`, `+0xd0`), data counts (`+0xd4`,
`+0xd8`) and the read latency sum (`+0xdc`), enabled through `+0xc0` and cleared through `+0xc4`. These are the
only DRAM-side counters in the sources held. No source documents row hits, row misses or bank conflicts of the
controller, and the Arm performance counters are not enabled for EL1 in this boot path (`docs/cpu-pmu.md`,
prerequisite). The image clears and enables the two ports at every window and reads them after the window.
The data unit is assumed to be 16 bytes, as for port 2; the single-stream ratio of counted bytes to consumed bytes
checks the assumption.

### Predictions, written before the runs

1. **Read image.** If Non-secure EL1 can read the NSI, the words read reset or ROM-left values and not boot0's
   values, because the FEL wrapper skips routine `0x48f38`; the CCU words read gates open and `MBUS_CLK_REG` reads
   600 MHz. Otherwise the board returns by the watchdog and the breadcrumbs name an abort.
2. **Locality hypothesis.** If the loss comes from DRAM row and bank locality, the DRAM-side figures show
   (a) counted read bytes within 5 percent of the bytes the cores consumed for every core set (no prefetch waste),
   (b) a mean read latency per request at the target agents that rises with the number of streams (more than
   1.5 times the one-stream value at eight cores), and (c) the total DRAM data rate equal to the total consumed
   rate, so that the controller delivers 8.25 GB/s at eight cores while one stream shows 14.8 GB/s. If instead the
   limit is on the CPU side, the mean latency at the target agents stays within 20 percent of the one-stream
   value. If counted bytes exceed consumed bytes by more than 10 percent at eight cores, prefetch waste
   contributes.
3. **Write image.** If the nine words affect CPU traffic, the eight-core total changes by more than 3 percent
   after the write. If it stays within 3 percent, the words are not a cause. A readback that differs from
   boot0's values shows a write that the NSI does not accept from Non-secure EL1.

### Performance counters, prediction written before the run

Each core reads the cycle counter and six events over the last timed window of each variant: L2D_CACHE_REFILL (0x17),
L3D_CACHE_REFILL (0x2a), BUS_ACCESS (0x19), BUS_ACCESS_RD (0x60), STALL_BACKEND (0x24) and MEM_ACCESS (0x13).

- **No prefetch waste:** L2D_CACHE_REFILL times 64 bytes is within 10 percent of the bytes the core consumed in every
  set, including eight cores. A ratio above 1.1 at eight cores and near 1 for one core would mean that the prefetchers
  fetch lines that are evicted unused, which reduces the useful rate without a locality loss.
- **Loaded latency:** by Little's law the refill latency per core is the number of outstanding refills divided by the
  refill rate. The refill rate per A55 core falls from 5.5 GB/s (one core) to about 1.0 GB/s (eight cores), so a
  fixed number of outstanding refills implies a latency about five times larger. The stall fraction (STALL_BACKEND per
  cycle) of an A55 core rises from its one-core value towards 1 as cores are added. This shows that the cores wait on
  memory, but it does not separate the controller's locality loss from an interconnect throughput limit.
- **Limits:** no source documents controller row hits, so the locality hypothesis is supported only by the address
  dependence shown above and by the absence of waste; it is not proven by a controller counter.

### Results of the interconnect study

Conditions: port 3-1, development DRAM mode, CPU-L 1794 MHz, CPU-B 2002 MHz, DSU 1274 MHz, worker tasks at Non-secure EL1.
Six runs: the read image, three write images, the performance-counter image and the port-word image. The records are in
`docs/evidence/memory-dma.jsonl`.

1. **The read succeeded.** Non-secure EL1 reads the NSI without an abort, so the risk named above did not materialize and
   no watchdog return was needed. The nine words read `0x30a05` and `0x30a05` at `0x0202446c` and `0x0202466c` and zero at
   the other seven, against boot0's `0x40a05`, `0x40a05`, `0x3f`, `0x7f`, `0xf`, `0xf1`, `0xf1`, `1`, `1`. The prediction
   holds: the words are not boot0's, and the first two differ from boot0's in one bit field (`0x30a05` against `0x40a05`).
   `NSI_CLK_REG` reads `0xc3000000` (gate open, reset deasserted), `NSI_BGR_REG` reads `0x10001` and `MBUS_CLK_REG` reads
   `0x81000000`. NSI ports 32 and 33 read mode 2 and priority and select zero, with their counter enable word set.
   The words read the same values at the start of every run: a watchdog reset restores them.
2. **The write sticks and changes nothing.** Written in boot0's order with a `dmb sy` after each store, the nine words read
   back as boot0's values in all three write runs. The sums of the per-core rates before and after the write agree within
   the run-to-run spread:

   | Cores reading | Before, GB/s (three runs) | After, GB/s (three runs) |
   | --- | --- | --- |
   | One A76 | 14.84, 14.84, 14.84 | 14.82, 14.82, 14.82 |
   | Two A76 | 13.33, 14.16, 14.11 | 13.99, 14.14 |
   | Six A55 | 9.02, 9.07 | 9.06, 9.03 |
   | Eight | 8.24, 8.24, 8.23 | 8.27, 8.26, 8.25 |

   The eight-core total changes by 0.3 percent, under the 3 percent threshold. The page-stride and shared-stream
   variants also change by under 1 percent. The nine words are therefore not a cause of the falling total, and the
   effect on mimus recognition time is nil within the threshold; it was not measured.
3. **No NSI counter observes CPU traffic.** The counters of the target-agent ports 32 and 33 and of the initiator ports 19
   and 20 read zero after the CPU read 1 MiB with the counters enabled and the clear word at zero, and the period word
   retained its value. The controller design's statement that the CPU direct channel is uncounted holds.
4. **Performance counters at EL1** (run with the unmodified words). The refill traffic of every A55 core equals the bytes
   it consumed (L2D_CACHE_REFILL times 64 bytes over the consumed bytes: 1.00 for one core and for eight cores), and
   BUS_ACCESS_RD counts four 16-byte beats per line. The prefetchers fetch nothing that is not used, so prefetch waste does
   not explain the loss. The backend stall fraction of the A55 cores rises from 0.40 with one core to 0.89 with eight,
   and that of the A76 cores from 0.65 to 0.97. With the number of outstanding refills fixed, the refill rate per A55 core
   falling from 5.5 to about 1.0 GB/s implies a loaded read latency about five times that of one stream. Page-sized strides
   raise the refill traffic by 25 percent over the bytes consumed (the prefetchers fetch lines that are not used),
   which accounts for part of the loss in that variant only.
5. **Inherited port state.** Every one of the 21 initiator ports reads mode 0, priority 0, input/output select 1, no
   bandwidth limit (`+0x28` zero) and zero at `+0xc` and `+0x2c`. The CPU, the NPU, the GPU, the camera and the display
   masters therefore arbitrate with equal priority in this boot path.

### Conclusion on the cause

The write is applied only inside the measuring image. The operating system does not write the words, so its memory path is
unchanged and the memory qualification and the mimus recognition time were not rerun; both reruns belong to a decision to
adopt the write, which the bandwidth result does not call for.

The loss is not caused by the nine interconnect words, by prefetch waste, by the clocks or by the benchmark. What remains is
the memory system's throughput under several independent streams: the stall fraction rises, the loaded latency rises, the
delivered rate falls from 14.8 to 8.25 GB/s, and the address-distance dependence of the barrier runs points at the DRAM
controller. No counter in the sources held observes row hits or bank conflicts, so the hypothesis of lost row-buffer
locality is supported by exclusion and by the address dependence and is not proven. A proof needs either the controller
register documentation or a trace of the controller's own counters.

### Quality-of-service knobs and CPU latency under DMA load

Robot workloads run the NPU, the camera pipeline and DMA masters beside control code whose worst-case memory latency
matters. The boot path leaves every initiator port at the same priority (result 5), so a long NPU or camera burst queues
CPU reads behind it in the controller. The vendor driver (`sunxi-nsi.c`, `sunxi-nsi.h`) exposes these knobs per initiator
port n at `0x02020000 + 0x200 * n`. Their effect on CPU worst-case latency is predicted from the driver and the common
meaning of the terms, not measured; the measurement method is below. This section changes none of them beyond boot0's
values, which the write image applied for the measurements above.

| Knob | Register | Vendor use | Prediction for CPU worst-case latency under DMA load |
| --- | --- | --- | --- |
| Priority of a port | `+0x14` bits 3:0, two 2-bit fields, set to p from 0 to 3 | DE 2, IOMMU 3 | Raising the CPU ports (19, 20) above the NPU, camera and DMA ports lets CPU requests win arbitration at the NSI. Bounds the queueing delay behind a competing burst to roughly one request of the other master, per arbitration point, but does not shorten the queue already inside the DRAM controller. |
| High-priority read class | `+0xc` (bit 16 on this chip) through `nsi_port_sethpr` | boot0 writes 1 at `+0xc` of the ISP and CSI ports | Moves a port's reads to the controller's high-priority queue, if the bit has that meaning. The word is documented only by the function name; test on one port before relying on it. |
| Absolute bandwidth limit | `+0x28` with `+0x2c` (saturation), enabled through `nsi_port_set_abs_bwlen` | none in the device tree | Caps a streaming master (NPU, ISP, CSI, video) to a bandwidth share, which bounds how much of the controller's queue it can occupy. The most direct way to bound CPU latency against a master whose traffic is known, at the cost of that master's throughput. |
| Port mode | `+0x10` bits 1:0 | 0 for DE and IOMMU | Meaning not documented; leave at 0. |
| Input/output select | `+0x18` bit 0 | 1 for DE and IOMMU | Meaning not documented; leave at 1. |
| Target-agent words | `+0x14` of ports 29 to 33 and `+0x6c` of ports 34 and 35 | boot0 values `0x3f`, `0x7f`, `0xf`, `0xf1`, `0xf1`, `0x40a05`, `0x40a05` | Not documented. They sit on the way to the controller and may carry arbitration at the controller side. The write of boot0's values did not change the CPU-only bandwidth, so the effect, if any, appears only under contention. |
| DRAM controller queue and page policy | Controller registers set by boot0 | Same as ours | Fixed by boot0's parameter set. |

**Measurement method.** The loaded read latency of a core is its outstanding refills divided by its refill rate, which the
Arm counters at EL1 give (result 4). Run a fixed CPU read loop on one core while the NPU or a copy engine streams; record
the maximum and the 99th percentile of the time per refill with the ports as inherited, then with the CPU priorities
raised, then with a bandwidth limit on the streaming master. This needs a second master in the image and is not part of
this study. The NPU stage and the camera pipeline define the loads that matter; the scheduler stream owns the timing
budgets.

## Boot-path adoption of boot0's interconnect step

The coordinator decided to match the vendor where no measurement argues against it. boot0's `main` calls routine `0x49050`
after `init_DRAM`; it stores the nine NSI words in order with a `dmb sy` before each store (routine `0x48f38`). The
RAM-only wrapper `boot/a733/ddr-entry.S` stood in for `main` and skipped it. The wrapper now calls `0x49051` (the Thumb entry
of `0x49050`) after `init_DRAM` and, in the restore variant, after the restore helper at `0x92008`. The case
`checks/ddr-nsi-words.py` executes the routine from the pinned boot0 under Unicorn and requires the nine stores in boot0's order with a
barrier before each, the values and addresses that the study wrote, and the call order in both wrapper variants. It fails on the
wrapper before this change. The microSD boot path runs boot0 itself and already includes the step.

### Adoption predictions, written before the runs

1. **Readback.** With the new wrapper, the probe-only interconnect image reads the nine words as boot0's values at the start of
   the run (`0x40a05`, `0x40a05`, `0x3f`, `0x7f`, `0xf`, `0xf1`, `0xf1`, `1`, `1`) and not the inherited `0x30a05` and zeros, and
   returns normally.
2. **Memory qualification.** The returning memory probe over the 12,224 MiB tested map from `0x44000000` (data, address and
   signature passes, `apps/memtest`) returns status 0 with no address failure, no signature mismatch and no beacon, and its scan
   and signature times stay within 5 percent of the earlier record, because the words did not change CPU-only reads and the DRAM
   decode does not depend on them.
