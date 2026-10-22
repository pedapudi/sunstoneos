# Design: returning memory qualification image

Status: **design**; no code exists. No figure has been measured on the board,
and figures marked *estimated* come from the instruction sequence. Readers are
assumed to know AArch64 caches, translation and Advanced SIMD (NEON), the
returning platform in [a733-kernel.md](../a733-kernel.md) and
[performance-architecture.md](../performance-architecture.md).

The board has 12,288 MiB of LPDDR5 at physical `0x4000_0000`–`0x3_4000_0000`.
Board evidence covers transfers to two 4 KiB pages
([hardware.md](../hardware.md), DDR row), and the returning platform maps the
first 128 MiB at EL1. This image runs on the boot Cortex-A55 and tests all DRAM
outside the 64 MiB it occupies. It checks address decoding, data and cache
maintenance, and DMA reachability above 4 GiB, and measures bandwidth and
latency. It then returns to the ROM's FEL interface, where the host reads the
result block over USB0. In this document, *offset* means physical address
minus `0x4000_0000`, and a *board session* is the sequence of runs between one
DRAM initialization and the next.

## Data flow and ownership

- **Run contract.** After the FEL upload, the host writes a 64-byte contract
  to a linker-placed address in the image. It holds slice start and end, test
  mask, time budget in counter ticks, data seed, and the digest of a DMA
  admission record. The test mask alone selects tests. The image copies the
  digest into the record as provenance and never interprets it; host
  admission refuses a mask with a DMA bit unless that admission record exists.
  The image digest excludes the contract.
- **Admission,** before any write outside the window: refuse unknown mask
  bits, a slice that is empty or not 2 MiB aligned at both ends, a slice
  outside the tested map (`a733_extent_within`), and any write extent, fixed
  ones included, that intersects an excluded range (`a733_extents_intersect`).
  The fixed extents are the probe tags, the data-line words, the address-line
  words and the DMA buffers and guards. Clamp the time budget to 20 s, so the
  run stays inside the host's 30 s return window.
- **Remote writers.** The monitor calls `a733_remote_writers_excluded()` at
  EL3 before EL1 entry and stores the mask in the result block. It refuses EL1
  entry if any bit is set other than OR100 and the crypto engine (CE). Those
  two are set on every board: OR100 has no documented control, and the board
  ROM de-asserts both CE resets on every boot
  ([remote-writers.md](../remote-writers.md)). Every other master, including
  the E902, DMA0 and the SMHC IDMACs, must read excluded, so DMA0 is quiet
  before the image starts it. This departs from the caller rule in
  remote-writers.md, which refuses on any set bit because a programmable agent
  such as OR100 could restart a stopped master. As a result, every exclusion
  holds only at the sampling instant, and a write by OR100 or CE during the
  run would appear as a mismatch that the record cannot tell apart from a DRAM
  fault. The host profile records the user's acceptance of both consequences
  before any board run.
- **Protected window and tested map.** The window, offsets `0`–`0x400_0000`,
  holds boot0 and launcher memory, the image at `0x4100_0000` and the result
  block at `0x4200_0000`. Only the image's own code and data write it. The other
  12,224 MiB, less any excluded range, form the tested map, which the image
  owns and overwrites. FEL cannot save and restore 12 GiB, so host admission
  must confirm that no DRAM content outside the window is needed afterwards.
- **Recovery region.** The image's linker script places the EL3 return path
  in four pages at the start of the image: monitor entry and vectors, the
  monitor's code and the cache routines it calls with their read-only data,
  the monitor state, and a 4,032-byte monitor stack. The secure timer and the
  exit `SMC` need only these pages, because the monitor runs with its MMU off.
  The last 64 bytes of each page hold a *beacon*: the beacon's physical
  address XOR a constant K whose bits 0–33 are zero. The region is a loaded
  section, so the FEL upload writes every beacon. The build planner checks
  from the linked image that every branch target reachable from the monitor
  vectors lies in the region and that the monitor's stack depth fits.
- **Excluded ranges** come only from aborts on the board (see failure
  classes). The code that returning images run configures no secure-memory
  region. In the pinned boot0 (SHA-256 `829ea8e3…07fabe3`), no aligned or
  unaligned 32-bit literal, Thumb or A32 `MOVW`, or `ADR` forms the address of
  the strings `sunxi_smc_config`, `ERROR: SMC is not enabled!`, `smc-v2.c`,
  `smc_region1_mb >= dram_size_mb` or `sec_mem_map[%d]: …`. The neighboring
  function names `mmc_get_mclk` and `mmc_set_mclk` are loaded by literals at
  `0x5cedc` and `0x5c440`, so a linked SMC routine would show such a
  reference. The call graph from the DDR wrapper's calls at `0x48fad` and
  `0x49bdd` ([usb-platform.md](../usb-platform.md)) reaches 217 Thumb
  functions, the A32 `memset` and `memcpy` through two literal tail calls, and
  the formatter's two character sinks through its only register calls. None
  loads a literal in SMC0 or SMC1 (`0x0A00_0000`–`0x0A01_FFFF`). Returning
  images already run from DRAM at Non-secure EL1, so the controller's reset
  state does not deny all Non-secure access.
- **Result record.** A 4 KiB slot at `0x4200_3000`, the last page of the
  platform's 16 KiB result block, in its Normal non-cacheable 2 MiB block. The
  platform log keeps offsets `0x100`–`0x2FFF`. The returning host profile reads
  the 16 KiB block as for other images. Only core 0 writes the record. It holds
  the contract, the remote-writer mask, the DAIF value, one address-line result
  for bits 6–33, each test's status (pass, fail, insensitive or not run), words
  checked and counter ticks, and the resume address. It also holds the first
  32 mismatches (address, expected, observed, test, element), a count of later
  ones, the count of unconfirmed march hits, the DMA0 poll count per transfer
  and the harness table. `platform_exit` issues `DSB SY` before the exit `SMC`.

## Tests, in run order

1. **Window scan,** in the first run of a board session, before any write
   outside the window. The image loads the word at in-page offset `0xFC0` of
   every 4 KiB page of the tested map, 3,129,344 loads, and stops through
   `platform_exit` if any value XOR K is a beacon's address. DRAM
   initialization, which runs once per session, fixes the address decode, so
   one scan covers the session. A tested line at `0xFC0` that shares cells with
   a beacon line, through any single-bit, multi-bit or non-linear decode fault,
   reads that beacon. For a stuck, open or shorted offset bit b from 26 to 33,
   the line at beacon + 2^b does so, and the hit address minus the beacon
   address names the bits; in this run the scan gives the address-line result
   for bits 26–33. A false match only stops the run. The host refuses a seed
   whose bits 34–63, or their complement, are within one of K's, and the build
   checks the fixed patterns the same way. As the first read of each page, the
   scan also takes any firewall's read abort with a precise FAR before a write.
2. **Alias probe,** in each later run, before any other write outside the
   window: the address-line test for bits 26–33 against a fault that appears
   after the first run. A 4 KiB canary page in the image's data holds one
   64-byte canary line per bit. The image first cleans and invalidates the
   page and issues `DSB SY`, so no canary write-back can land later. For each
   bit b, it stores a unique tag at canary_b + 2^b, cleans and invalidates the
   tag line, and issues `DSB SY`. It then cleans and invalidates the eight
   canary lines and the tag lines written so far, issues `DSB SY`, and loads
   them all. The first barrier completes the tag's write-back before the
   canaries are invalidated, so any later refill, speculative or prefetched,
   reads DRAM that holds the tag. A changed canary or a missing tag stops the
   run before any further write outside the window. Each pair differs in one
   bit both as offsets and as physical addresses, except offset bit 30, whose
   physical partner differs in bits 30 and 31.
3. **Data lines.** Walking ones and zeros over one word and one 64-byte line
   at `0x4400_0000`, with `DC CIVAC` and `DSB SY` before each read so that it
   reaches DRAM.
4. **Address lines** for offset bits 6–25. A base word at offset `0xC00_0000`
   (physical `0x4C00_0000`) receives a pattern, and for each bit b the word at
   base XOR 2^b receives a unique tag. All 21 words are written, cleaned and
   invalidated before any is read. If bit b is stuck or open, its tag lands on
   the base, which then reads wrong; a missing tag names the bit. Every partner
   lies outside the window and is also a physical single-bit partner, since
   for offsets below 2^30 the physical address is `0x4000_0000` OR offset.
   This runs in every run.
5. **Page signature,** in the first run only. Each 4 KiB page of the tested
   map receives one word, its physical address XOR the seed, at in-page offset
   0. Each store is followed at once by `DC CIVAC` on its line, and the write
   pass issues `DSB SY` after each 2 MiB block, so no alias partner stays dirty
   and an asynchronous error is taken inside the block that caused it. The
   read pass then compares every word. Two pages whose addresses differ in any
   combination of offset bits 12–33 share the word's cells when they alias, so
   the pass finds every page-granular alias, every dropped page write and
   every abort. An alias that also involves offset bits 6–11, between pages
   that no march group (test 6) holds together, stays uncovered.
6. **March C-** over the slice, with 64-bit words P(a) = a + seed mod 2^64,
   where a is the word's physical address. The six elements are: up (write P);
   up (read P, write ¬P); up (read ¬P, write P); down (read P, write ¬P); down
   (read ¬P, write P); down (read P). The planner divides the slice into
   groups of two or three sub-slices of at most 256 MiB, chosen so that the
   members other than any one total at least 256 MiB. Each element runs on
   every member of its group before the next element starts. Between an
   element's last write to a sub-slice and the next element's first read of
   it, at least 256 MiB streams through caches of at most 8 MiB. The TRM
   documents no replacement policy; under LRU, pseudo-LRU or random
   replacement a line survives with probability below e^-32. The march needs
   no cache maintenance or barrier, even at the up-to-down turn, and a write
   to one member that lands in another member's cells shows at that member's
   next read.
   Inner loops use NEON: one `LD1` or `ST1` of four 128-bit registers per
   line, and expected values held in four registers that advance by a constant
   vector of 64 per line. The read-and-write elements write the complement of
   the data they read and accumulate differences in one vector. Once per 2 MiB
   block, `UMAXV` tests the accumulator. On a hit, the block is rescanned with
   scalar loads. Each word that differs from the value the element should have
   left is recorded and rewritten with that value (after a write, the expected
   observation is the complement of the value written). A hit whose rescan
   finds no word counts as an unconfirmed hit, which fails the run. A
   read-and-write line costs (up direction; down uses a register post-index of
   -64):

   ```
   ld1  {v0.2d-v3.2d}, [x0]            // 1
   eor  v4..v7, v0..v3, v16..v19       // 4: difference from expected
   orr  v24, v24, v4..v7               // 4: accumulate
   mvn  v0..v3, v0..v3                 // 4: complement of data read
   st1  {v0.2d-v3.2d}, [x0], #64       // 1
   add  v16..v19, v16..v19, v20        // 4: next line's expected values
   ```

   That is 18 instructions, plus one per line for the compare and branch with
   two lines unrolled: 19. The write element is `ST1` and four `ADD` (6); the
   read element drops `MVN` and `ST1` (14). A group starts only if the time of
   the slowest group so far still fits the budget; the image then records the
   resume address. After each group and after the signature's write pass,
   the image cleans and invalidates the recovery region's first three pages
   and the stack's beacon line, issues `DSB SY`, and compares their digest
   with the one taken at admission; a mismatch stops the run. The digest
   detects damage that the scan cannot prevent, such as an alias involving
   in-page bits.
7. **Bandwidth and latency,** in the first run. Read uses `LD1` of four
   128-bit registers per line; write uses two `STP Q` per line over whole
   lines, so the A55 enters write streaming mode after three full lines
   (Cortex-A55 TRM r2p0, A6.2.6); copy uses two `LDP Q` and two `STP Q` per
   line. The runtime `memcpy` is not used. Read and write each take three
   samples of 192 MiB, and copy three samples of 64 MiB. A pointer chase of
   dependent loads over 16 KiB to 256 MiB in steps of four follows a
   full-period linear congruential permutation of the lines, with 2^20 loads
   up to 4 MiB and 2^19 above, inside one 512 MiB-aligned half of a 1 GiB
   block. The L2 TLB stores a 1 GiB block as 512 MiB entries (TRM A5.2.2), so
   a point walks the tables only on its first load. The L1 data TLB holds
   4 KiB entries only (TRM A5.2.1), so points above 64 KiB include L1 TLB
   misses that hit the L2 TLB. Each table row records the kernel identifier and stride; the CPU-L,
   CPU-B and DSU PLL words come from `a733_cpu_clock_report`. No clock changes.
8. **DMA0 cache maintenance and reachability,** last in the run and only when
   the mask selects them. Every buffer is whole 64-byte lines that DMA0 owns
   for the transfer; transfers start and end at unaligned offsets inside them,
   and the bytes outside the transfer must keep their fill pattern. Before the
   start, the CPU cleans the source lines and makes the destination lines
   clean. In the clean case the CPU writes pattern A, cleans and invalidates,
   then writes pattern B and cleans; DMA0 copies, and the CPU invalidates the
   destination and compares. In the invalidate case the CPU fills the
   destination, cleans it and reads it so its lines are resident and clean;
   DMA0 overwrites it, and the CPU invalidates and compares. The CPU touches no
   byte of those lines while DMA0 owns them. Each control omits only the
   maintenance under test, must see stale data (otherwise the case is reported
   insensitive), and ends with a clean and invalidate of its lines.
   Reachability follows the guard matrix in
   [dma-address-width.md](../dma-address-width.md) for bit 32 alone
   (`0x1_8000_0000`) and bit 33 alone (`0x2_8000_0000`), in both directions,
   with guards at every DRAM address sharing the low 32 bits.

Per-run split: the first run spends at most 0.6 s on the scan, 1.5 s on the
signature, 1.5 s on the harness and 0.1 s on DMA0, which leaves 16 s for the
march. Later runs give the march 19.5 s. The probe, data lines and address
lines take under 10 ms *estimated*.

## Interrupts, barriers, cache policy and cores

The only EL1 interrupt is the platform's 1 kHz tick, and the image runs one
task. The monitor's secure timer expires after 2 s without renewal. The scan,
signature and march loops read the counter once per 2 MiB block (`ISB`,
`MRS CNTPCT_EL0`), renew with the existing `SMC` 0x734 every 200 ms and check
the time budget with the same read. DMA0 completion is a busy poll with a
10 ms counter deadline, which section 1.3 of the performance rules permits:
the returning platform does not bind the DMA0 interrupt (SPI 111, INTID 143),
and its delivery is not board-qualified. Binding INTID 143 replaces the poll
once the I2S image qualifies its delivery. With at most 8 transfers and no
other work on the core, the poll costs nothing that matters.

The tested map is Normal write-back inner-shareable. Inner loops issue plain
loads and stores, and in the signature write pass one `DC CIVAC` per store,
with no barrier. `DSB SY` appears where a read must observe DRAM after
maintenance: in the probe (17), the signature (one per 2 MiB block and one
after the write pass), the data-line test, the per-group digest and inside
`aarch64_device_cache` for DMA0. It also precedes the closing counter read of
a write sample and the exit `SMC`. Ownership of a DMA0 buffer moves between
CPU and device at the start write and at the completion read only.

The task clears PSTATE.A (`MSR DAIFClr, #4`) at start and records DAIF, so an
SError is taken at EL1, where the vector table's SError slot branches to the
platform's exception capture. The tick may interrupt a NEON loop: the port's
default `vApplicationIRQHandler` saves the floating-point registers before it
calls the platform's `vApplicationFPUSafeIRQHandler`, so v16–v24 survive.

Secondary cores stay off, as in every returning image. Physical eight-core
startup is unqualified, and the attached board cannot be reconnected if an
attempt fails. The image has no shared data, lock or cross-core barrier. At
the 494 MHz boot clock one A55 limits throughput, so the harness reports a
lower bound on DRAM bandwidth.

## Reuse and changes outside the package

Reused unchanged: the returning platform (monitor, exit and renewal `SMC`s,
exception capture, result block); `a733-returning-host.py`, with a profile in
`apps/memtest/check.py`; `aarch64_device_cache`; `a733_range.h`;
`a733_mapping_check.h`; `a733_remote_writers_excluded`; the DMA0 controller
driver and host admission record; and `a733_cpu_clock_report`. No existing
code is removed.

The host compares every table word with the shared golden descriptor, which
proves the tables as built. Admission adds one `a733_mapping_check` write
translation per descriptor kind: `0x4400_0000` (level-2 block),
`0x8000_0000` (first level-1 block) and `0x3_3FFF_F000` (last), each checking
physical address, write-back attribute and inner shareability. Only these see
live state that the host model cannot: a `TTBR0_EL1` naming other tables, a
MAIR index with another meaning, or a `TCR_EL1` whose input or physical size
cuts off the top of the map.

Changes needed from the primary agent:

- `platform/a733/platform.c`, under build definitions set only by the memtest
  target: level-2 entries 64 to 511 and level-1 entries 2 to 12 become
  `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP = 0b00, the stage 2
  and 3 word of the memory architecture design
  (`docs/designs/memory-architecture.md`). Both builders take the word from
  one shared descriptor macro, so the host comparison checks them against one
  golden value. Entries 0 to 63 stay as they are, and table memory stays
  8 KiB. `platform_puts` stops the log at a build-defined end, `0x3000` for
  this image and the full block otherwise.
- The monitor's admission calls `a733_remote_writers_excluded()` with an
  aligned 32-bit load, stores the mask and refuses EL1 entry as described
  above.
- `CMakeLists.txt` gains one `include(cmake/a733-memtest.cmake)` line.
- `docs/remote-writers.md` names this image's acceptance of OR100 and CE as a
  documented departure from its caller rule, with the two consequences above.

## Budgets and measurement

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| DRAM tested per board | 8 KiB | 12,224 MiB less abort-discovered ranges | Planner host model `checks/memtest-plan.py`: slices contiguous, outside the window and excluded ranges; every fixed write extent outside excluded ranges |
| DRAM mapped at EL1 | 128 MiB; 8 KiB of tables | 12,288 MiB; 8 KiB of tables | Host comparison of the table words with the shared golden word |
| Translations | none | 3 at admission; none afterwards | `AT` count in Unicorn emulation |
| March instructions per 64-byte line, two lines unrolled | none | write ≤ 6; read-verify ≤ 14; read-verify-write ≤ 19 | Unicorn count over 1 MiB, image flags |
| March work per 2 MiB block outside the loop | none | one counter read, one `UMAXV` test, one progress store, ≤ one `SMC` per 200 ms; no call, maintenance or barrier | Unicorn counters |
| Recovery digest | none | per march group and after the signature write pass: 193 `DC CIVAC`, one `DSB SY`, ≤ 12.1 KiB of loads | Unicorn counters |
| March rate on one A55 at 494 MHz | none | write 2–3, read-verify 1.5–2.2, read-verify-write 1.0–1.6 GB/s *estimated*; 0.88–1.39 s per 256 MiB sub-slice | Instruction counts at about one 128-bit instruction per cycle; board figures replace the estimate |
| Runs for March C- over the map | none | 3–5 *estimated*: 47.75 sub-slices take 42–66 s, at 16 s in the first run and 19.5 s later, less up to one group (≤ 4.2 s) per run | Planner |
| Window scan | none | 3,129,344 loads; ≤ 6 instructions per page; ≤ 0.6 s *estimated*; first run only | Unicorn counters |
| Alias probe | none | 8 tags, 17 `DSB SY`, 172 `DC CIVAC`; every later run | Unicorn trace of the access and barrier order |
| Address lines, bits 6–25 | none | 21 writes, 21 reads; every run | Unicorn counters |
| Page signature | none | 2 × 3,129,344 accesses, 3,129,344 `DC CIVAC`, 6,113 `DSB SY`; ≤ 6 instructions per page per pass; ≤ 1.5 s *estimated*, assuming at least four misses overlap; first run only | Unicorn counters |
| Harness kernels | none | read ≤ 2, write ≤ 3, copy ≤ 5 instructions per line; chase ≤ 1.25 per load (eight loads unrolled); ≤ 1.5 s *estimated*; bandwidth samples ≥ 50 ms; first run only | Unicorn count over 1 MiB; time from the stated rates |
| Wall time per run | none | ≤ 20 s, clamped at admission | Budget logic in Unicorn with a modeled counter |
| DMA0 cases | none | ≤ 8 transfers of ≤ 1 MiB; maintenance on the enclosing lines only | Case code linked with the `checks/dma-controller.c` model |

Arithmetic behind the time rows. A sub-slice of 268 MB takes one write
element (0.09–0.13 s), four read-and-write elements (0.67–1.07 s) and one
read element (0.12–0.18 s). The harness at the low rates takes 0.40 s for
read, 0.30 s for write, 0.40 s for copy at 1.0 GB/s over twice the copied
bytes, and 0.33 s for the chase: 0.24 s at 150 ns per load for the three
largest points and 0.09 s for the five smaller ones. The total is 1.43 s.
The signature's barriers are one per 2 MiB block of 12,224 MiB plus one. The
probe's maintenance is 64 lines for the canary page, then for the i-th bit
one tag line, eight canary lines and i tag lines: 64 + 8 × 9 + 36 = 172.

Unicorn memory hooks inject a stuck bit and an open bit in 6–25; a single-bit
wrap at each of bits 26–33, which the scan must report in a first run and the
probe in a later run; a multi-bit fold onto a beacon line; a two-bit alias
with one bit in 12–17 and one at 28 or above; a page alias at a distance that
is not a multiple of 64 pages; a dropped write; a coupled word; and a
transient read error, which must raise the unconfirmed-hit count. The
intended test must report each fault at its exact address, and each fault
that reaches the window must stop the run before any further write outside
it. Contract cases cover each admission refusal, a time budget above 20 s and
a fixed write extent inside an excluded range.

## Failure classes

- **Contract or admission refused,** including a remote-writer bit outside
  OR100 and CE: before any write outside the window. Nothing is retained.
- **Scan hit, probe change or digest mismatch:** the run stops before further
  writes outside the window and records the addresses. Nothing is retained;
  the host makes no further upload to that board until the user reviews the
  record.
- **Mismatch or unconfirmed hit:** recorded and counted; the run finishes the
  group so that the fault's extent is known. Nothing is retained.
- **External abort or SError:** the platform records ESR, ELR and FAR and
  returns to ROM. A denied read aborts synchronously in the scan with a precise
  FAR. A denied write surfaces at a write-back; the signature's per-block
  barrier makes the progress word name the 2 MiB block. No device is active,
  so the state is known. The host excludes that block with a rebuild, which
  costs one run. This assumes that a denied access receives an error or
  ignored-write response and does not stall the bus. AXI requires a response
  to every transaction, but no A733 source states the controller's behavior.
  A stall would hang the core with no watchdog to recover it; this residual
  stays until a source or board evidence settles it.
- **Time budget reached:** not a failure; the resume address is recorded.
- **DMA0 deadline expired:** state unknown. The controller's recovery path
  stops the channel, and the record says whether it reads quiet. The image
  skips the remaining DMA0 cases and returns at once; no later test runs, so
  nothing touches the buffers. The channel and its buffers stay retained, and
  the host makes no further upload until a FEL read shows the channel quiet.

## Left out

- **Multicore runs,** until the primary agent qualifies a returning multicore
  base. Each core would then own a contiguous slice and a result slot on its
  own 64-byte line, and core 0 would wait for completion words with `WFE`.
- **The protected window,** which needs a second link address in high DRAM.
- **Reading the DRAM controller's address map** to prove a linear decode: the
  manual documents no such registers, so the window scan covers that risk.
- **The both-bits DMA case** (`0x3_2000_0000`): dropping both bits lands at
  `0x2000_0000`, the start of the PCIe outbound window in the A733 memory map,
  where a read is a PCIe access with no documented side-effect-free behavior.
  It needs that documentation first.
- **Crypto engine reachability:** the CE bit is set before the image could
  touch CE on every board, so the rule above refuses the case.
- **Clock changes and DRAM frequency or voltage margining:** each clock change
  needs the user's approval, and training, frequency and voltage stay in boot0.
- **Row-disturbance tests and timing-based bank-map discovery:** they need
  hours or a decoded configuration, and no consumer needs them.
- **GPU, NPU, IOMMU, xHCI and PCIe reachability:** each needs its own device
  bring-up, and those packages run the same guard matrix.
- **Native USB0 results:** FEL reads the result block after return, so a
  native stack would add failure modes and no information.

## Board evidence that would confirm the design

Each run record of one board session shows a remote-writer mask of OR100 and
CE only, SError unmasked, a passing address-line result for bits 6–33, no
mismatch, no unconfirmed hit and no digest change. The first run passes the
window scan and the page signature with no abort, and the resume addresses
chain to cover the tested map. With a DMA admission record, both cache cases
pass and their controls see stale data, and the guard matrix passes for bit 32
and bit 33. The first record carries the harness table and clock words.
