# Design: returning memory qualification image

Status: **implemented** and host-modeled, except the DMA0 cases;
[memory-qualification.md](../memory-qualification.md) describes the image and
[the measurements](memory-qualification-measurements.md) compare it with the
budgets below. No figure has been measured on the board, and figures marked
*estimated* come from the cost model stated beside them.
Readers are assumed to know AArch64 caches, translation and Advanced SIMD
(NEON), the returning platform in [a733-kernel.md](../a733-kernel.md) and
[performance-architecture.md](../performance-architecture.md).

The board has 12,288 MiB of LPDDR5 at physical `0x4000_0000`–`0x3_4000_0000`.
Board evidence covers transfers to two 4 KiB pages
([hardware.md](../hardware.md), DDR row), and the returning platform maps the
first 128 MiB at EL1. This image runs on the boot Cortex-A55, tests all DRAM
outside the 64 MiB it occupies, and returns to the ROM's FEL interface, where
the host reads the result block over USB0. *Offset* means physical address
minus `0x4000_0000`. A *board session* is the sequence of runs between one
boot0 upload, which initializes DRAM, and the next. The host counts boot0
uploads in its run log and treats any run after an unexplained disconnect or
reset as the first of a new session.

## Data flow and ownership

- **Run contract.** After the FEL upload, the host writes 128 bytes to a
  linker-placed address: a sequence number, slice start and end, test mask,
  time budget in counter ticks, data seed, and the excluded-range list. The
  list holds a count and up to ten ranges, each a 32-bit start and end 2 MiB
  block index; it is empty today. The image digest excludes the contract.
  The host run log links each run to its DMA admission record, and host
  admission refuses a mask with a DMA bit unless that record exists.
- **Admission** runs before any write outside the window. It refuses unknown
  mask bits; a slice that is empty or not 2 MiB aligned; an excluded range
  that is empty, unaligned, unsorted or outside the tested map; and a slice or
  fixed write extent outside the tested map (`a733_extent_within`) or inside
  an excluded range (`a733_extents_intersect`). It also refuses a `DCZID_EL0`
  other than a permitted 64-byte block, and a total data-cache size above
  8 MiB, summed from `CLIDR_EL1` and `CCSIDR_EL1`. A memory-side cache, if the
  SoC has one, stays unknown. The fixed extents are the data-line and
  address-line words, the harness buffers (192 MiB read and write, 64 MiB
  copy), the chase table (up to 256 MiB) and the DMA buffers and guards.
  `checks/memtest-plan.py` runs the same function on the host. Admission
  clamps the time budget to 20 s, inside the host's 30 s return window.
- **Remote writers.** In the memtest build only, the monitor samples
  `a733_remote_writers_excluded()` at EL3 before EL1 entry, stores the mask
  in the record, and refuses entry on any bit other than OR100, the crypto
  engine (CE) and USB0. The board ROM leaves OR100 and CE able to write on
  every boot ([remote-writers.md](../remote-writers.md)). USB0 is out of
  reset because the ROM's FEL stack uploaded the image; that stack uses PIO,
  as the physical ROM capture shows ([hardware.md](../hardware.md), USB
  row), and does not run while the image does. This departs from that
  document's caller rule: each exclusion holds only at the sampling instant,
  and a write by OR100, CE or USB0 during the run would look like a DRAM
  fault. `apps/memtest/check.py` prepares no contract until the run log
  holds the user's acceptance of this residual and the two below.
- **Protected window and tested map.** The window, offsets `0`–`0x400_0000`,
  holds boot0 and launcher memory, the image at `0x4100_0000` and the result
  block at `0x4200_0000`. The other 12,224 MiB, less excluded ranges, form the
  tested map, which the image owns and overwrites. Host admission confirms
  that no DRAM content outside the window is needed afterwards.
- **Recovery region.** The secure timer and the exit `SMC` run the monitor
  with its MMU off, so recovery needs only the monitor's pages. The linker
  places them first in the image: vector table; the timer, exception, renewal
  (`monitor_sync`) and exit code with the cache routines and their literal
  pools; monitor state; 2 KiB stack. Entry code, `monitor_init` and the
  remote-writer sampling run once before any write outside the window and lie
  outside the region. Each page holds code or data in lines 0–31
  (`0x000`–`0x7FF`, the vector table's size) and a *beacon* in every word of
  lines 32–63 and in the word at `0x7C0`: the word's physical address XOR a
  constant K whose bits 0–33 are zero. In the vector page `0x7C0` lies in the
  padding of the last vector slot, which holds one branch; elsewhere the
  linker reserves it and the stack tops out below it. The FEL upload writes
  the beacons. Linker `ASSERT`s place each recovery input section inside
  lines 0–31 of its page and bound its size, and a host check refuses any
  relocation from those sections to a symbol outside the region and any stack
  depth above the bound.
- **Excluded ranges** come only from aborts on the board and reach the image
  only through the contract list; the scan, signature and march skip them,
  so a new range needs no rebuild. No returning image configures a
  secure-memory region, and the pinned boot0 (SHA-256 `829ea8e3…07fabe3`)
  contains no call that configures one (appendix A).
- **Result record.** The last 4 KiB page of the platform's 16 KiB result
  block, Normal non-cacheable; the platform log keeps `0x100`–`0x2FFF`. Only
  core 0 writes it. It opens with a magic word and closes with a checksum,
  which the host verifies before trusting a pass. The contract's sequence
  number and digest show that the host's FEL write landed. The record also
  holds the remote-writer mask, DAIF, the data-cache size, `DCZID_EL0`, the
  address-line result for bits 6–33, the resume address, and each test's
  status (pass, fail, insensitive, not run) with words checked and
  counter ticks. It also holds the first 32 mismatches (address, expected,
  observed, test, element) and a count of later ones, the unconfirmed-hit
  count, the DMA0 phase marker and poll counts, and the harness table.
  `platform_exit` issues `DSB SY` before the exit `SMC`.

## Tests, in run order

1. **Window scan,** every run, before any write outside the window. For each
   4 KiB page of the tested map the image loads the first word of lines 31 and
   63 (`0x7C0`, `0xFC0`) and stops through `platform_exit` if a value XOR K
   lies in the recovery region. Each pass issues the 16 loads of eight pages
   before the first compare: the 4 KiB stride defeats the prefetcher, and the
   in-order A55 stalls only when an instruction uses an outstanding load.
   Suppose page T aliases recovery page R and line i of T reaches line f(i) of
   R. If f(i) = i XOR d, then for every d in 0–63 either 31 XOR d or 63 XOR d
   is at least 32, so one load reads a beacon. Let line-index bit k (offset
   bit 6 + k) be stuck. Stuck at 1, it maps line 63 to itself. Stuck at 0, it
   maps line 63 to 63 − 2^k, at least 32, for k below 5; for k = 5 it maps
   lines 31 and 63 to line 31, whose word at `0x7C0` is a beacon. The scan
   therefore covers XOR (including hashed) faults in bits 6–33 and single
   stuck bits in 6–11, each combined with any page alias, provided 64-byte
   bursts stay intact. For a fault in offset bit b from 26 to 33, page R + 2^b
   reads R's beacon, and the hit address minus the beacon address names the
   bits; the scan is the address-line test for bits 26–33. The host refuses a
   seed whose bits 34–63, or their complement, are within one of K's, and the
   build checks the fixed patterns the same way. No value the image writes
   then matches a beacon, and a false match from power-on contents only stops
   the run.
2. **Data lines.** Walking ones and zeros over one word and one line at
   `0x4400_0000`, with `DC CIVAC` and `DSB SY` before each read.
3. **Address lines,** bits 6–25. A base word at physical `0x4C00_0000` gets a
   pattern and each partner base XOR 2^b a unique tag; all 21 words are
   written, cleaned and invalidated before any is read. A stuck or open bit
   puts its tag on the base; a missing tag names the bit. Below offset 2^30
   the physical address is `0x4000_0000` OR offset, so each partner differs
   in one physical bit.
4. **Page signature,** first run of a session. The write pass zeroes line 0
   of each page with `DC ZVA` (no linefill read), stores the page's address
   XOR the seed, and issues `DC CIVAC`, with `DSB SY` after each 2 MiB block.
   Eviction alone would write the lines back, but later, so a denied write
   would abort blocks after the progress word moved on; the per-page clean
   ties each abort to its block. The read pass compares each page's
   signature word, eight pages per batch. It finds every alias in offset
   bits 12–33, every dropped page write and every abort. A
   value equal to another page's signature proves a decode fault and stops
   the run before the march. An alias that also involves bits 6–11, between
   pages that no march group holds together, stays uncovered.
5. **March C-** over the slice, with words P(a) = a + seed mod 2^64 at
   physical address a: up (write P); up (read P, write ¬P); up (read ¬P,
   write P); down (read P, write ¬P); down (read ¬P, write P); down (read P).
   The planner forms groups of two or three sub-slices of at most 256 MiB,
   such that the members other than any one total at least 256 MiB, and runs
   each element on every member, in member order, before the next element. A
   down element walks each member from its top block to its bottom. Between a
   write to a line and its next read the line's other members stream through
   the caches, so at least 256 MiB streams through at most 8 MiB of cache;
   under LRU, pseudo-LRU or random replacement a line survives with
   probability below e^-32. `checks/memtest-plan.py` walks the element,
   member and block order over every group of the board plan and confirms
   that distance. The march therefore needs no maintenance or barrier, and a
   write that lands in another member's cells shows at that member's next
   read. Reversing the member order of the down elements would make them the
   exact reverse of the up elements, but the last lines that element 2 writes
   would then be the first that element 3 reads, from the cache. With one
   member order, the down elements visit cells in different members in the
   same order as the up elements, so between members the march detects only
   the coupling faults its up elements detect.

   The loop alternates two register sets, v0–v3 and v8–v11, so each line's
   `LD1` issues one line, 20–33 cycles, before its first use. One
   `PRFM PLDL1STRM` per line, 16 lines (1 KiB) ahead in the direction of
   travel, hides a DRAM miss in both directions without relying on the
   hardware prefetcher (appendix B). Expected values sit in v16–v19 and
   advance by v20 (±64 per lane). One read-and-write line, x1 = x0 ± 64:

   ```
   ld1  {v8.2d-v11.2d}, [x1]           // line n+1, used next half-iteration
   prfm pldl1strm, [x0, #±1024]        // line n+16 in the travel direction
   eor  v4..v7, v0..v3, v16..v19       // line n, loaded one line earlier
   orr  v4, v4, v5                     // accumulate as a tree
   orr  v6, v6, v7
   orr  v4, v4, v6
   orr  v24, v24, v4
   mvn  v0..v3, v0..v3                 // complement of data read
   st1  {v0.2d-v3.2d}, [x0]
   add  v16..v19, v16..v19, v20        // next line's expected values
   ```

   The second half repeats this with v0–v3 and v8–v11 exchanged. Appendix B
   derives the per-line cycle budgets from a cost model.

   Once per 2 MiB block `UMAXV` tests v24; a hit rescans the block with
   scalar loads. A word that reads P(b) or ¬P(b) for an address b other than
   its own proves a decode fault, and the image calls `platform_exit` before
   the next write. Other differing words are recorded and rewritten with the
   value the element should have left. The rescan counts mismatches in
   cacheable memory and writes the non-cacheable record's count once per
   block, with entries for the first 32 only. A hit whose rescan finds nothing
   is an unconfirmed hit and fails the run. A kernel returns a hit before its
   deadline check, so the image checks the deadline after each rescan; a fault
   in every block then ends the run within one block and one rescan of the
   deadline. A group starts only if the slowest group
   so far still fits the budget, and the image records the resume address.
   After each group and after the signature write pass, the image cleans and
   invalidates the recovery region's vector, code and beacon lines, issues
   `DSB SY`, and compares their digest with the admission digest. The digest
   detects damage that has already happened; a damaged exit path may not
   return.
6. **Bandwidth and latency,** first run of a session. The kernels are: read
   `LD1` of four Q registers per line; write two `STP Q` per whole line, so
   the A55 enters write streaming after three lines (Cortex-A55 TRM r2p0,
   A6.2.6); copy two `LDP Q` and two `STP Q`. The read kernel runs ascending
   and descending without `PRFM`, so the table shows what the hardware
   prefetcher does for the down elements. Each read direction and the write
   take three 192 MiB samples, copy three 64 MiB samples. `DSB SY` before the
   closing counter read waits only for issued stores. If streaming does not
   engage, up to 8 MiB of dirty lines, about 4% of a sample, may remain; the
   record marks the write figure as an upper bound by that fraction. A
   dependent-load chase over 16 KiB to 256 MiB in steps of four follows a
   full-period linear congruential permutation of the lines inside one
   512 MiB-aligned half of a 1 GiB block. Only its first load walks the tables
   (TRM A5.2.2), and points above 64 KiB include L1 TLB misses that hit the L2
   TLB. Rows carry the CPU-L, CPU-B and DSU PLL words from
   `a733_cpu_clock_report`.
7. **DMA0 cache maintenance and reachability,** last, only when the mask
   selects them. DMA0 owns whole-line buffers for each transfer; transfers
   start and end at unaligned offsets, and bytes outside them must keep their
   fill. The clean case writes pattern A, cleans and invalidates, writes B and
   cleans before the copy; the invalidate case leaves the destination resident
   and clean before DMA0 overwrites it. The CPU invalidates and compares after
   completion and touches no buffer line while DMA0 owns it. Before each start
   write the image sets a DMA0 phase marker in the record, followed by
   `DSB SY`, and clears it after the completion read. Each control omits only
   the maintenance under test and must see stale data, or the case is
   reported insensitive. Reachability follows the guard matrix of
   [dma-address-width.md](../dma-address-width.md) for bit 32 alone
   (`0x1_8000_0000`) and bit 33 alone (`0x2_8000_0000`), both directions,
   with guards at every DRAM address sharing the low 32 bits. The march may
   have left guard lines dirty, so the CPU cleans and invalidates every guard
   line with `DSB SY` before the start write, leaves them alone during the
   transfer and invalidates them before readback.

Per-run split: the scan takes 0.7 s *estimated* with two misses overlapped
and 1.25 s with none; the budget logic absorbs the difference by shortening
the march. The first run of a session adds 1.5 s of signature, 1.9 s of
harness and 0.1 s of DMA0, leaving about 15.8 s for the march; later runs
give the march about 19.3 s. Data and address lines take under 10 ms
*estimated*.

## Interrupts, barriers, cache policy and cores

The only EL1 interrupt is the platform's 1 kHz tick, and the image runs one
task. The monitor's secure timer fires after 2 s without renewal. The scan,
signature and march read the counter once per 2 MiB block (`ISB`,
`MRS CNTPCT_EL0`), the harness between samples and the chase between points;
each read renews with the existing `SMC` 0x734 when 200 ms have passed and
checks the budget. DMA0 completion is a busy poll with a 10 ms deadline, as
section 1.3 of the performance rules permits: the returning platform does not
bind INTID 143, and its delivery is unqualified. Binding it replaces the poll
once the I2S image qualifies delivery. At most 8 transfers poll for under
10 ms with no other work on the core.

The tested map is Normal write-back inner-shareable. Inner loops issue plain
loads and stores, plus one `DC ZVA` and one `DC CIVAC` per page in the
signature write pass. `DSB SY` appears only where a read must observe DRAM
after maintenance or a record must be complete:

- per block and at the end of the signature write pass;
- in the data-line and address-line tests;
- after the digest and guard maintenance, and inside `aarch64_device_cache`;
- before a write sample's closing counter read and before the exit `SMC`.

A DMA0 buffer changes owner only at the start write and the completion read.

The task clears PSTATE.A at start and records DAIF, so an SError is taken at
EL1 into the platform's exception capture. The port's default
`vApplicationIRQHandler` saves the floating-point registers before it calls
`vApplicationFPUSafeIRQHandler`, so v16–v24 survive the tick.

Secondary cores stay off, as in every returning image: physical eight-core
startup is unqualified, and the board cannot be reconnected if an attempt
fails. The image has no shared data, lock or cross-core barrier. At the
494 MHz boot clock one A55 limits throughput, so the harness gives a lower
bound on DRAM bandwidth. The image keeps the boot clock, because each clock
change needs the user's approval ([cpu-clock.md](../cpu-clock.md)); at
1794 MHz the march would take about two runs.

## Reuse and changes outside the package

Reused unchanged: the returning platform (monitor, exit and renewal `SMC`s,
exception capture, result block); `a733-returning-host.py` with a profile in
`apps/memtest/check.py`; `aarch64_device_cache`; `a733_range.h`;
`a733_mapping_check.h`; `a733_remote_writers_excluded`; the DMA0 driver and
admission record; `a733_cpu_clock_report`. No existing code is removed.

A host check compares every table word the builder emits with the shared
golden descriptor once per build, since the tables are fixed output of the
build. Admission adds one `a733_mapping_check` write translation per
descriptor kind (`0x4400_0000`, `0x8000_0000`, `0x3_3FFF_F000`), which sees
live state the host cannot: `TTBR0_EL1`, the MAIR index and the `TCR_EL1`
sizes.

Changes outside the package, all selected by the memtest build definition
`SUNSTONE_MEMTEST`:

- `platform/a733/platform.c` writes level-2 entries 64–511 and level-1
  entries 2–12 as `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP =
  0b00, the word from `docs/designs/memory-architecture.md`. Both builders
  take it from `A733_EL1_DRAM_BLOCK` in
  `include/a733_device_memory_layout.h`. Entries 0–63 and the 8 KiB of tables
  stay. `platform_puts` stops the log at `0x3000`.
- The monitor samples the remote-writer mask as described above; other images
  keep their current admission until their own designs adopt the check.
- The monitor's linker placement follows the recovery region layout:
  `monitor_sync` and `monitor_reset` sit in the section
  `.text.monitor_recovery`, which `apps/memtest/link.ld` places in the
  recovery code page. `cmake/a733-memtest.cmake` defines
  `SUNSTONE_USB_RUNTIME`, which compiles the 0x734 renewal in `monitor.S`; a
  Unicorn case checks that a renewal returns and moves `CNTPS_CVAL_EL1`
  forward.
- `CMakeLists.txt` includes `cmake/a733-memtest.cmake`, whose target is
  excluded from the default build, and `docs/remote-writers.md` names this
  image's acceptance of OR100, CE and USB0.

## Budgets and measurement

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| DRAM tested per board | 8 KiB | 12,224 MiB less the contract's excluded ranges | Planner model `checks/memtest-plan.py`: slices and every fixed write extent, harness and chase included, inside the tested map and outside excluded ranges |
| DRAM mapped at EL1 | 128 MiB; 8 KiB of tables | 12,288 MiB; 8 KiB of tables. Measured 12,286 MiB write-back and 2 MiB non-cacheable in 8 KiB, every tested-map entry equal to the golden word, from the tables the image's own `platform_init` writes. The image's mapping check accepts this map in `checks/memtest-emulate.py` and refuses it with the tested-map entries cleared | Host comparison with the golden word, once per build |
| Translations | none | 3 at admission; none afterwards | `AT` count in Unicorn |
| March cycles per line, A55 model | none | write ≤ 12; read-verify ≤ 21; read-verify-write ≤ 33.5; each `LD1` at least one line before its use; one `PRFM` per loaded line, 16 lines ahead in the travel direction. Measured 12.19, 21.19 and 33.19. The per-line work is 12, 21 and 33 cycles; each kernel's loop adds one `SUBS` and one `B.NE`, 1.5 cycles per eight lines, or 0.19 cycles per line. The write and read bounds omit that loop cost, which the read-and-write bound includes as half a cycle. A 2 MiB block holds 32,768 lines, so the kernels need a loop; a 16-line unroll would leave 12.09 and 21.09 cycles and a 32-line unroll 12.05 and 21.05, still above the bounds, for 1.2 KiB and 3.6 KiB more text across the three kernels | Unicorn trace and load-to-use trace over 1 MiB in each direction, priced by the cost model |
| March work per 2 MiB block outside the loop | none | one `ISB` and counter read, one `UMAXV` test, one progress store, ≤ one `SMC` per 200 ms; no call, cache maintenance, `DSB` or `DMB` | Unicorn counters |
| March rate at 494 MHz | none | write 2–3, read-verify 1.5–1.9, read-verify-write 0.94–1.6 GB/s *estimated*; 0.91–1.45 s per 256 MiB. Estimated from the measured cycles: 2.59, 1.49 and 0.95 GB/s; 1.41 s per 256 MiB. The read rate falls 0.01 GB/s below its lower bound by the same 0.19 cycles of loop cost per line as the row above. A 32-line unroll would reach 1.50 GB/s for 3.6 KiB more kernel text; the pass time, which sets the number of runs, stays inside its bound | Cost model; board figures replace it |
| Runs for March C- | none | 3–5 *estimated* (appendix B) | Planner |
| Window scan, every run | none | 6,258,688 loads; ≤ 9 instructions per page; ≥ 15 instructions from each load to its use; 0.7 s *estimated*, 1.25 s with no overlap | Unicorn counters and load-to-use trace |
| Address lines, bits 6–25 | none | 21 writes, 21 reads | Unicorn counters |
| Page signature, first run | none | 3,129,344 each of `DC ZVA`, store, `DC CIVAC` and load; 6,113 `DSB SY`; no linefill read; ≤ 7 instructions per page per pass; ≤ 1.5 s *estimated* | Unicorn counters and load-to-use trace |
| Recovery digest | none | 192 `DC CIVAC`, one `DSB SY`, 12 KiB of loads, per group and after the signature write pass | Unicorn counters |
| Harness, first run | none | read ≤ 2, write ≤ 3, copy ≤ 5 instructions per line; chase ≤ 1.25 per load; ≤ 1.9 s *estimated*; samples ≥ 50 ms; read ascending and descending | Unicorn count over 1 MiB |
| Wall time per run | none | ≤ 20 s, with a fault in every march block too; renewal gap ≤ 200 ms plus one block, sample or chase point | Budget logic in Unicorn with a modeled counter |
| March eviction distance | none | ≥ 256 MiB between each write and the next read of a line | Element, member and block walk over the board plan in `checks/memtest-plan.py` |
| DMA0 cases | none | ≤ 8 transfers of ≤ 1 MiB; maintenance on buffer and guard lines only; phase marker set across every transfer | Case code with the `checks/dma-controller.c` model |

Unicorn memory hooks inject a stuck and an open bit in 6–25; a single-bit
wrap at each of bits 26–33; a multi-bit fold onto a recovery page; an alias
onto a recovery page at each line delta from 0 to 63; a stuck-at-0 and a
stuck-at-1 bit at each of bits 6–11, each with a page alias onto each
recovery page; a two-bit alias with
one bit in 12–17 and one at 28 or above; a page alias at a distance that is
not a multiple of 64 pages; a dropped write; a coupled word; and a transient
read error, which must count as an unconfirmed hit. Each fault must be
reported at its address. A fault reaching the recovery region must stop the
run in the scan, and a march decode fault before the next write. Contract
cases cover each admission refusal and a non-empty excluded-range list. An
exception and a secure-timer exit during a DMA0 transfer must leave the phase
marker set.

## Failure classes

- **Admission refused,** including a remote-writer bit outside OR100, CE and
  USB0, which the record names: before any write outside the window. Nothing
  is retained.
- **Scan hit or proven decode fault:** the run stops before its next write
  outside the window. The host makes no further upload, in this session or a
  later one, until the run log holds a review entry (reviewer and date) for
  the run.
- **Digest mismatch:** recorded if the exit still works; a damaged monitor
  can hang the exit or the secure timer. No further upload until the review
  entry exists.
- **Alias onto the rest of the window:** the scan does not cover the EL1
  image, its translation tables or the result block, so the signature write
  pass or the march may corrupt them. Recovery then depends on the secure
  timer and the intact monitor. A record with a bad magic word or checksum,
  or none, counts as a failure and is never read as a pass.
- **Mismatch or unconfirmed hit:** recorded; the group finishes so the
  fault's extent is known. Nothing is retained.
- **External abort or SError:** the platform records ESR, ELR and FAR and
  returns to ROM. A denied read may abort with a valid
  FAR, with ESR.FnV set, or as an SError. A denied write surfaces at a
  write-back. The host therefore relies on the progress word written per 2 MiB
  block and excludes that block and the one before it, at the cost of one
  run; in the march, which walks members both ways, it excludes both
  neighbors. A run with an untrusted record blocks its own session; once its
  blocks are excluded, the next session continues without review. The earlier block covers an SError arriving after the block's
  `DSB SY`; no A733 source bounds that delay. A denied access is assumed to
  get an error or ignored-write response. If it stalled the bus, the core
  would hang with no watchdog; the run log holds the user's acceptance of
  that residual before any contract is prepared.
- **Time budget reached:** not a failure; the resume address is recorded.
- **DMA0 deadline expired:** state unknown. The recovery path stops the
  channel and records whether it reads quiet; the image skips the remaining
  DMA0 cases and returns. The channel and buffers stay retained until a FEL
  read shows the channel quiet.
- **Any other return with the DMA0 phase marker set,** by exception, by the
  secure timer or with no valid record: neither the exception path nor the
  monitor's exit stops DMA0. The host treats it as a deadline expiry and
  makes no upload until a FEL read shows DMA0 quiet and the run log holds a
  review entry for the run. Tests 1–6 never start DMA0.

Residual: a line mapping outside the two classes the scan covers can send
lines 31 and 63 of a tested page to non-beacon words of a recovery page while
another line reaches monitor code. Examples are two or more stuck line-index
bits, such as bits 6 and 11 both stuck at 0, and an alias that moves only some
lines of a page. The signature write pass, and later the march, would then
overwrite the monitor and lose the board. The run log's acceptance record
names this residual alongside OR100, CE and USB0 and the bus stall, and
`prepared.json` carries a copy.

## Left out

- **Scanning every line of every page in each run:** it would close the
  residual above but read 12 GiB per run, about 6 s.
- **Multicore runs,** until the primary agent qualifies a returning multicore
  base; each core would then own a slice and a result slot on its own line.
- **Testing the protected window,** which needs a second link address.
- **Reading the DRAM controller's address map:** the manual documents no such
  registers. The scan covers whole-page aliases onto the recovery region; the
  residual above remains.
- **The both-bits DMA case** (`0x3_2000_0000`): dropping both bits lands in
  the PCIe outbound window at `0x2000_0000`, where a read has no documented
  side-effect-free behavior.
- **Crypto engine reachability:** the CE bit is always set, so admission
  refuses the case.
- **Clock changes and DRAM margining:** each clock change needs the user's
  approval, and training, frequency and voltage stay in boot0.
- **Row-disturbance tests and bank-map discovery:** they need hours or a
  decoded configuration, and no consumer needs them.
- **GPU, NPU, IOMMU, xHCI and PCIe reachability:** those packages run the
  same guard matrix after their own bring-up.
- **Native USB0 results:** FEL reads the result block after return.

## Board evidence that would confirm the design

Every record of a session shows a remote-writer mask of OR100, CE and USB0
only,
SError unmasked, a passing scan and address-line result for bits 6–33, no
mismatch, no unconfirmed hit and no digest change. The first run passes the
signature with no abort, and the resume addresses chain across the tested map.
With a DMA admission record, both cache cases pass with stale controls, and the
guard matrix passes for bits 32 and 33. The first record carries the harness
table and clock words, and measured scan and march times replace the
estimates.

## Appendix A: boot0 configures no secure-memory region

In the pinned boot0, no aligned or unaligned 32-bit literal, Thumb or A32
`MOVW`, or `ADR` forms the address of the strings `sunxi_smc_config`,
`ERROR: SMC is not enabled!`, `smc-v2.c`, `smc_region1_mb >= dram_size_mb` or
`sec_mem_map[%d]: …`. The neighboring names `mmc_get_mclk` and `mmc_set_mclk`
are loaded by literals at `0x5cedc` and `0x5c440`, so a linked SMC routine
would show such a reference. The call graph from the DDR wrapper's calls at
`0x48fad` and `0x49bdd` ([usb-platform.md](../usb-platform.md)) reaches 217
Thumb functions, the A32 `memset` and `memcpy` through two literal tail calls,
and the formatter's two character sinks; none loads a literal in SMC0 or SMC1
(`0x0A00_0000`–`0x0A01_FFFF`). Returning images already run from DRAM at
Non-secure EL1, so the controller's reset state does not deny Non-secure
access.

## Appendix B: time arithmetic

- **March cost model.** Eight cycles for each four-register Q-form `LD1` or
  `ST1` (64 bits per cycle), one per Q-form integer instruction and per
  `PRFM`, no dual issue. A read-and-write line costs
  8 + 1 + 4 + 4 + 4 + 8 + 4 = 33 cycles plus half a cycle of branch: 0.94 GB/s
  at 494 MHz. With 128 bits per cycle and some pairing, the line costs about
  20 cycles: 1.6 GB/s. The write element costs 8–12 cycles and is bounded by
  DRAM at 2–3 GB/s; the read element costs 17–21 cycles, 1.5–1.9 GB/s. These
  two figures count the per-line work only. The kernels loop over eight lines
  per iteration, and the loop's `SUBS` and `B.NE` add 0.19 cycles per line:
  12.19 and 21.19 cycles, 2.59 and 1.49 GB/s at the slow end.
- **Latency term.** A miss costs 150–200 ns. The `PRFM` runs 16 lines, at
  least 320 cycles or 650 ns, ahead of the load, so the model's latency term,
  miss latency less lookahead, is zero. If prefetching failed, the one-line
  lookahead (40–68 ns) would leave each read line latency-bound at about
  0.43 GB/s, and the march would take about 155 s, nine runs or more. The
  harness's descending sample and the measured march times show which holds.
- **March.** A 256 MiB sub-slice takes one write element (0.09–0.13 s), four
  read-and-write elements (0.68–1.14 s) and one read element (0.14–0.18 s):
  0.91–1.45 s. The 47.75 sub-slices take 43.5–69.2 s.
- **Runs.** The slowest-group rule loses at most one group of three
  sub-slices, 2.7–4.35 s, per run. At the fast rate, three runs (15.8, 19.3
  and 19.3 s, less one group each) cover 43.5 s. At the slow rate, four runs
  suffice without loss and five in the worst case.
- **Scan.** 6,258,688 loads at 200 ns with two misses overlapped take 0.63 s,
  with four 0.31 s. With no overlap the scan takes 1.25 s, and the budget
  logic shortens the march.
- **Signature.** The read pass costs one line read per page, 0.31 s at the
  scan's rate. The write pass costs one 64-byte write per page and no read;
  at 100 cycles per page it takes 0.63 s. Barriers: one per 2 MiB block of
  12,224 MiB plus one, 6,113.
- **Harness.** Read 0.40 s in each direction, write 0.30 s, copy 0.40 s
  (1.0 GB/s over twice the copied bytes), chase 0.33 s (150 ns per load):
  1.83 s.
- **Digest.** 32 vector and code lines in each of two pages plus 32 beacon
  lines in each of four pages: 192 lines.
