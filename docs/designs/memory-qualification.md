# Design: returning memory qualification image

Status: **design**; no code exists. No figure has been measured on the board,
and figures marked *estimated* come from the cost model stated beside them.
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

- **Run contract.** After the FEL upload, the host writes 64 bytes to a
  linker-placed address: slice start and end, test mask, time budget in
  counter ticks and data seed. The image digest excludes the contract. The
  host run log links each run to its DMA admission record, and host admission
  refuses a mask with a DMA bit unless that record exists.
- **Admission,** before any write outside the window, refuses unknown mask
  bits, a slice that is empty or not 2 MiB aligned, a slice outside the tested
  map (`a733_extent_within`), any fixed write extent inside an excluded range
  (`a733_extents_intersect`), and a `DCZID_EL0` other than a permitted 64-byte
  block. The fixed extents are the data-line and address-line words, the
  harness buffers (192 MiB read and write, 64 MiB copy), the chase table (up
  to 256 MiB) and the DMA buffers and guards. It clamps the time budget to
  20 s, inside the host's 30 s return window.
- **Remote writers.** In the memtest build only, the monitor samples
  `a733_remote_writers_excluded()` at EL3 before EL1 entry, stores the mask,
  and refuses entry on any bit other than OR100 and the crypto engine (CE).
  The board ROM leaves both able to write on every boot
  ([remote-writers.md](../remote-writers.md)). This departs from that
  document's caller rule: every exclusion holds only at the sampling instant,
  and a write by OR100 or CE during the run would look like a DRAM fault. The
  host profile records the user's acceptance of both consequences.
- **Protected window and tested map.** The window, offsets `0`–`0x400_0000`,
  holds boot0 and launcher memory, the image at `0x4100_0000` and the result
  block at `0x4200_0000`. The other 12,224 MiB, less excluded ranges, form the
  tested map, which the image owns and overwrites. Host admission confirms
  that no DRAM content outside the window is needed afterwards.
- **Recovery region.** The secure timer and the exit `SMC` run the monitor
  with its MMU off, so recovery needs only the monitor's pages. The linker
  places them first in the image: vector table; entry code, monitor code,
  cache routines and read-only data; monitor state; 2 KiB stack. Each page
  holds code or data in lines 0–31 (`0x000`–`0x7FF`, the vector table's size)
  and a *beacon* in every word of lines 32–63: the word's physical address XOR
  a constant K whose bits 0–33 are zero. The FEL upload writes the beacons.
  The region is four pages while the monitor's code fits in 2 KiB, as it does
  today. The build planner checks that every branch target reachable from the
  vectors lies in a code line and that the stack depth fits.
- **Excluded ranges** come only from aborts on the board. No returning image
  configures a secure-memory region, and the pinned boot0 (SHA-256
  `829ea8e3…07fabe3`) contains no call that configures one (appendix A).
- **Result record.** The last 4 KiB page of the platform's 16 KiB result
  block, Normal non-cacheable; the platform log keeps `0x100`–`0x2FFF`. Only
  core 0 writes it. It holds the contract, remote-writer mask, DAIF,
  `DCZID_EL0`, the address-line result for bits 6–33, the resume address, and
  each test's status (pass, fail, insensitive, not run) with words checked and
  counter ticks. It also holds the first 32 mismatches (address, expected,
  observed, test, element) and a count of later ones, the unconfirmed-hit
  count, the DMA0 poll counts and the harness table. `platform_exit` issues
  `DSB SY` before the exit `SMC`.

## Tests, in run order

1. **Window scan,** every run, before any write outside the window. For each
   4 KiB page of the tested map the image loads the first word of lines 31
   and 63 (`0x7C0`, `0xFC0`) and stops through `platform_exit` if a value XOR
   K lies in the recovery region. Each pass issues the 16 loads of eight pages
   before the first compare: the 4 KiB stride defeats the prefetcher, and the
   in-order A55 stalls only when an instruction uses an outstanding load. If
   page T aliases recovery page R with line i reaching line i XOR d, then for
   every d in 0–63 either 31 XOR d or 63 XOR d is at least 32, so one load
   reads a beacon. This covers single-bit, multi-bit and hashed decode faults
   in bits 6–33 that keep 64-byte bursts intact. For a fault in offset bit b
   from 26 to 33, page R + 2^b reads R's beacon, and the hit address minus the
   beacon address names the bits; the scan is the address-line test for bits
   26–33. The host refuses a seed whose bits 34–63, or their complement, are
   within one of K's, and the build checks the fixed patterns the same way.
   No value the image writes then matches a beacon, and a false match from
   power-on contents only stops the run.
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
   The read pass compares every word, eight pages per batch. It finds every
   alias in offset bits 12–33, every dropped page write and every abort. A
   value equal to another page's signature proves a decode fault and stops
   the run before the march. An alias that also involves bits 6–11, between
   pages that no march group holds together, stays uncovered.
5. **March C-** over the slice, with words P(a) = a + seed mod 2^64 at
   physical address a: up (write P); up (read P, write ¬P); up (read ¬P,
   write P); down (read P, write ¬P); down (read ¬P, write P); down (read P).
   The planner forms groups of two or three sub-slices of at most 256 MiB,
   such that the members other than any one total at least 256 MiB, and runs
   each element on every member before the next element. Between a write to a
   line and its next read, at least 256 MiB streams through at most 8 MiB of
   cache; under LRU, pseudo-LRU or random replacement a line survives with
   probability below e^-32. The march therefore needs no maintenance or
   barrier, and a write that lands in another member's cells shows at that
   member's next read.

   The loop handles two lines per iteration and loads line n+1 before it
   processes line n. Expected values sit in v16–v19 and advance by v20 (64 per
   lane). A read-and-write line (down uses a post-index of -64):

   ```
   ld1  {v0.2d-v3.2d}, [x0]            // next line's load issued here
   eor  v4..v7, v0..v3, v16..v19       // 4: difference from expected
   orr  v4, v4, v5                     // 4: accumulate as a tree
   orr  v6, v6, v7
   orr  v4, v4, v6
   orr  v24, v24, v4
   mvn  v0..v3, v0..v3                 // 4: complement of data read
   st1  {v0.2d-v3.2d}, [x0], #64
   add  v16..v19, v16..v19, v20        // 4: next line's expected values
   ```

   *Cost model:* eight cycles for each four-register Q-form `LD1` or `ST1`
   (64 bits per cycle), one per Q-form integer instruction, no dual issue. A
   read-and-write line costs 8 + 4 + 4 + 4 + 8 + 4 = 32 cycles plus half a
   cycle of branch: 0.97 GB/s at 494 MHz. With 128 bits per cycle and some
   pairing, the line costs about 20 cycles: 1.6 GB/s. The write element
   costs 8–12 cycles and is bounded by DRAM at 2–3 GB/s; the read element
   costs 16–20 cycles, 1.5–2.0 GB/s.

   Once per 2 MiB block `UMAXV` tests v24; a hit rescans the block with
   scalar loads. A word that reads P(b) or ¬P(b) for an address b other than
   its own proves a decode fault, and the image calls `platform_exit` before
   the next write. Other differing words are recorded and rewritten with the
   value the element should have left. A hit whose rescan finds nothing is an
   unconfirmed hit and fails the run. A group starts only if the slowest group
   so far still fits the budget, and the image records the resume address.
   After each group and after the signature write pass, the image cleans and
   invalidates the recovery region's vector, code and beacon lines, issues
   `DSB SY`, and compares their digest with the admission digest. The digest
   detects damage that has already happened; a damaged exit path may not
   return.
6. **Bandwidth and latency,** first run of a session. The kernels are:
   read `LD1` of four Q registers per line; write two `STP Q` per whole line,
   so the A55 enters write streaming after three lines (Cortex-A55 TRM r2p0,
   A6.2.6); copy two `LDP Q` and two `STP Q`. Read and write take three
   192 MiB samples, copy three 64 MiB samples. `DSB SY` before the closing
   counter read waits only for issued stores. If streaming does not engage,
   up to 8 MiB of dirty lines, about 4% of a sample, may remain; the record
   marks the write figure as an upper bound by that fraction. A dependent-load
   chase over 16 KiB to 256 MiB in steps of four follows a full-period linear
   congruential permutation of the lines inside one 512 MiB-aligned half of a
   1 GiB block. Only its first load walks the tables (TRM A5.2.2), and points
   above 64 KiB include L1 TLB misses that hit the L2 TLB. Rows carry the
   CPU-L, CPU-B and DSU PLL words from `a733_cpu_clock_report`.
7. **DMA0 cache maintenance and reachability,** last, only when the mask
   selects them. DMA0 owns whole-line buffers for each transfer; transfers
   start and end at unaligned offsets, and bytes outside them must keep their
   fill. The clean case writes pattern A, cleans and invalidates, writes B and
   cleans before the copy; the invalidate case leaves the destination resident
   and clean before DMA0 overwrites it. The CPU invalidates and compares after
   completion and touches no buffer line while DMA0 owns it. Each control
   omits only the maintenance under test and must see stale data, or the case
   is reported insensitive. Reachability follows the guard matrix of
   [dma-address-width.md](../dma-address-width.md) for bit 32 alone
   (`0x1_8000_0000`) and bit 33 alone (`0x2_8000_0000`), both directions,
   with guards at every DRAM address sharing the low 32 bits. The march may
   have left guard lines dirty, so the CPU cleans and invalidates every guard
   line with `DSB SY` before the start write, leaves them alone during the
   transfer and invalidates them before readback.

Per-run split: the scan takes at most 0.7 s in every run. The first run of a
session adds 1.5 s of signature, 1.5 s of harness and 0.1 s of DMA0, leaving
about 16 s for the march; later runs give the march about 19.3 s. Data and
address lines take under 10 ms *estimated*.

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

A DMA0 buffer
changes owner only at the start write and the completion read.

The task clears PSTATE.A at start and records DAIF, so an SError is taken at
EL1 into the platform's exception capture. The port's default
`vApplicationIRQHandler` saves the floating-point registers before it calls
`vApplicationFPUSafeIRQHandler`, so v16–v24 survive the tick.

Secondary cores stay off, as in every returning image: physical eight-core
startup is unqualified, and the board cannot be reconnected if an attempt
fails. The image has no shared data, lock or cross-core barrier. At the
494 MHz boot clock one A55 limits throughput, so the harness gives a lower
bound on DRAM bandwidth. At an approved 1794 MHz CPU-L operating point
([cpu-clock.md](../cpu-clock.md)) each line would cost about a quarter of the
time, and the march would take about two runs, limited by single-core DRAM
bandwidth that no board figure yet bounds. The image keeps the boot clock,
because each clock change needs the user's approval.

## Reuse and changes outside the package

Reused unchanged: the returning platform (monitor, exit and renewal `SMC`s,
exception capture, result block); `a733-returning-host.py` with a profile in
`apps/memtest/check.py`; `aarch64_device_cache`; `a733_range.h`;
`a733_mapping_check.h`; `a733_remote_writers_excluded`; the DMA0 driver and
admission record; `a733_cpu_clock_report`. No existing code is removed.

The host compares every table word with the shared golden descriptor. Admission
adds one `a733_mapping_check` write translation per descriptor kind
(`0x4400_0000`, `0x8000_0000`, `0x3_3FFF_F000`), which sees live state the
host cannot: `TTBR0_EL1`, the MAIR index and the `TCR_EL1` sizes.

Changes needed from the primary agent, all under the memtest build definition:

- `platform/a733/platform.c`: level-2 entries 64–511 and level-1 entries
  2–12 become `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP =
  0b00, the word from `docs/designs/memory-architecture.md`, taken by both
  builders from one shared macro. Entries 0–63 and the 8 KiB of tables stay.
  `platform_puts` stops the log at `0x3000`.
- The monitor samples the remote-writer mask as described above; other images
  keep their current admission until their own designs adopt the check.
- The monitor's linker placement follows the recovery region layout.
- `CMakeLists.txt` gains `include(cmake/a733-memtest.cmake)`, and
  `docs/remote-writers.md` names this image's acceptance of OR100 and CE.

## Budgets and measurement

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| DRAM tested per board | 8 KiB | 12,224 MiB less abort-discovered ranges | Planner model `checks/memtest-plan.py`: slices outside the window and excluded ranges; every fixed write extent, harness and chase included, outside excluded ranges |
| DRAM mapped at EL1 | 128 MiB; 8 KiB of tables | 12,288 MiB; 8 KiB of tables | Host comparison with the golden word |
| Translations | none | 3 at admission; none afterwards | `AT` count in Unicorn |
| March cycles per line, A55 model | none | write ≤ 12; read-verify ≤ 20; read-verify-write ≤ 32.5 | Unicorn trace over 1 MiB, priced by the cost model |
| March work per 2 MiB block outside the loop | none | one `ISB` and counter read, one `UMAXV` test, one progress store, ≤ one `SMC` per 200 ms; no call, cache maintenance, `DSB` or `DMB` | Unicorn counters |
| March rate at 494 MHz | none | write 2–3, read-verify 1.5–2.0, read-verify-write 0.97–1.6 GB/s *estimated*; 0.89–1.42 s per 256 MiB | Cost model; board figures replace it |
| Runs for March C- | none | 3–5 *estimated* (appendix B) | Planner |
| Window scan, every run | none | 6,258,688 loads; ≤ 9 instructions per page; ≥ 15 instructions from each load to its use; ≤ 0.7 s *estimated* | Unicorn counters and load-to-use trace |
| Address lines, bits 6–25 | none | 21 writes, 21 reads | Unicorn counters |
| Page signature, first run | none | 3,129,344 each of `DC ZVA`, store, `DC CIVAC` and load; 6,113 `DSB SY`; no linefill read; ≤ 7 instructions per page per pass; ≤ 1.5 s *estimated* | Unicorn counters and load-to-use trace |
| Recovery digest | none | 192 `DC CIVAC`, one `DSB SY`, 12 KiB of loads, per group and after the signature write pass | Unicorn counters |
| Harness, first run | none | read ≤ 2, write ≤ 3, copy ≤ 5 instructions per line; chase ≤ 1.25 per load; ≤ 1.5 s *estimated*; samples ≥ 50 ms | Unicorn count over 1 MiB |
| Wall time per run | none | ≤ 20 s; renewal gap ≤ 200 ms plus one block, sample or chase point | Budget logic in Unicorn with a modeled counter |
| DMA0 cases | none | ≤ 8 transfers of ≤ 1 MiB; maintenance on buffer and guard lines only | Case code with the `checks/dma-controller.c` model |

Unicorn memory hooks inject a stuck and an open bit in 6–25; a single-bit
wrap at each of bits 26–33; a multi-bit fold onto a recovery page; an alias
onto a recovery page at each line delta from 0 to 63; a two-bit alias with
one bit in 12–17 and one at 28 or above; a page alias at a distance that is
not a multiple of 64 pages; a dropped write; a coupled word; and a transient
read error, which must count as an unconfirmed hit. Each fault must be
reported at its address. A fault reaching the recovery region must stop the
run in the scan, and a march decode fault before the next write. Contract
cases cover each admission refusal.

## Failure classes

- **Admission refused,** including a remote-writer bit outside OR100 and CE:
  before any write outside the window. Nothing is retained.
- **Scan hit or proven decode fault:** the run stops before its next write
  outside the window. The host makes no further upload until the user
  reviews the record.
- **Digest mismatch:** recorded if the exit still works; a damaged monitor
  can hang the exit or the secure timer. No further upload until review.
- **Mismatch or unconfirmed hit:** recorded; the group finishes so the
  fault's extent is known. Nothing is retained.
- **External abort or SError:** the platform records ESR, ELR and FAR and
  returns to ROM; no device is active. A denied read may abort with a valid
  FAR, with ESR.FnV set, or as an SError. A denied write surfaces at a
  write-back. The host therefore relies on the progress word written per 2 MiB
  block and excludes that block and the one before it, at the cost of one
  run. The earlier block covers an SError arriving after the block's
  `DSB SY`; no A733 source bounds that delay. A denied access is assumed to
  get an error or ignored-write response. If it stalled the bus, the core
  would hang with no watchdog; the host profile records the user's acceptance
  of that residual before the first full-map scan.
- **Time budget reached:** not a failure; the resume address is recorded.
- **DMA0 deadline expired:** state unknown. The recovery path stops the
  channel and records whether it reads quiet; the image skips the remaining
  DMA0 cases and returns. The channel and buffers stay retained until a FEL
  read shows the channel quiet.

Residual: an alias that moves only some lines of a page, so that lines 31 and
63 of a tested page reach no recovery line while another line reaches monitor
code, passes the scan. The march would then overwrite the monitor and lose
the board. The host profile records the user's acceptance of this residual
alongside OR100 and CE.

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

Every record of a session shows a remote-writer mask of OR100 and CE only,
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

- **March.** A 256 MiB sub-slice takes one write element (0.09–0.13 s), four
  read-and-write elements (0.67–1.11 s) and one read element (0.13–0.18 s):
  0.89–1.42 s. The 47.75 sub-slices take 42.7–67.8 s.
- **Runs.** The slowest-group rule loses at most one group of three
  sub-slices, 2.7–4.3 s, per run. At the fast rate, three runs (16, 19.3 and
  19.3 s, less one group each) cover 42.7 s. At the slow rate, four runs
  suffice without loss and five in the worst case.
- **Scan.** 6,258,688 loads at 200 ns with two misses overlapped take 0.63 s,
  with four 0.31 s. With no overlap the scan takes 1.25 s, and the budget
  logic shortens the march.
- **Signature.** The read pass costs one line read per page, 0.31 s at the
  scan's rate. The write pass costs one 64-byte write per page and no read;
  at 100 cycles per page it takes 0.63 s. Barriers: one per 2 MiB block of
  12,224 MiB plus one, 6,113.
- **Harness.** Read 0.40 s, write 0.30 s, copy 0.40 s (1.0 GB/s over twice
  the copied bytes), chase 0.33 s (150 ns per load): 1.43 s.
- **Digest.** 32 vector and code lines in each of two pages plus 32 beacon
  lines in each of four pages: 192 lines.
