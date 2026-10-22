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
latency. It then returns to the ROM's FEL interface, where the host reads a
4 KiB result record over USB0. In this document, *offset* means physical
address minus `0x4000_0000`.

## Data flow and ownership

- **Run contract.** After the FEL upload, the host writes a 64-byte contract
  to a linker-placed address in the image. It holds slice start and end,
  test mask, time budget in counter ticks, data seed, and the digest of a DMA
  admission record. The test mask alone selects tests. The image copies the digest into
  the record as provenance and never interprets it; host admission refuses a
  mask with a DMA bit unless that admission record exists. The image digest
  excludes the contract.
- **Image admission,** before any write outside the image: refuse unknown
  mask bits, a slice that is empty or not 2 MiB aligned at both ends, a slice
  outside the tested map (`a733_extent_within`) and a slice that intersects
  an excluded firewall range (`a733_extents_intersect`). Clamp the time budget
  to 20 s whatever the contract says, so the run stays inside the host's 30 s
  return window.
- **Remote writers.** The monitor calls `a733_remote_writers_excluded()` at
  EL3 before EL1 entry and stores the mask in the result block. It refuses
  EL1 entry if any bit is set other than OR100 and the crypto engine (CE).
  Those two bits are set on every board: OR100 has no documented control, and
  the board ROM de-asserts both CE resets on every boot, while neither the ROM
  nor boot0 holds a CE register address ([remote-writers.md](../remote-writers.md)).
  Accepting them is the user's decision, recorded in the host profile; no
  board run starts before it is recorded. Every other master, including the
  E902, DMA0 and the SMHC IDMACs, must read excluded, so DMA0 is quiet before
  the image starts it.
- **Protected window and tested map.** The window, offsets `0`–`0x400_0000`,
  holds boot0 and launcher memory, the image at `0x4100_0000`, its stacks, the
  monitor and the result block at `0x4200_0000`. Only the image's own code and
  data write it. The other 12,224 MiB, less any excluded firewall range, form
  the tested map, which the image owns and overwrites. FEL cannot save and
  restore 12 GiB, so host admission must confirm that no DRAM content outside
  the window is needed afterwards.
- **Secure-memory controller region.** boot0 holds the function name
  `sunxi_smc_config` and the text `smc_region1_mb >= dram_size_mb`. The
  manual places controllers SMC0 and SMC1 at `0x0A00_0000` and `0x0A01_0000`
  but documents no registers for them. The 120 MiB secure region named in
  [dram-controller.md](dram-controller.md) comes from boot0's own boot path,
  while returning images run only the DDR wrapper's board and DRAM
  initialization calls (`0x48fad`, `0x49bdd`; [usb-platform.md](../usb-platform.md)).
  A host check, `checks/memtest-firewall.py`, disassembles the pinned boot0
  and decides whether either call reaches `sunxi_smc_config`. If one does,
  the check derives the region's placement and the build leaves its entries
  invalid, with a 4 KiB level-2 table under each 1 GiB block the region
  touches (tables at most 16 KiB). The coverage target is then 12,224 MiB
  minus the region. If neither call reaches it, the design assumes that every
  Non-secure EL1 access to the tested map completes and is not dropped, and
  the first run's page signature (test 3) checks that assumption.
- **Result record.** 4 KiB at `0x4200_4000`, after the platform's 16 KiB
  result block and in the same Normal non-cacheable 2 MiB block. Only core 0
  writes it. It holds the contract, the remote-writer mask, the alias-probe
  result, each test's status (pass, fail, insensitive or not run), words
  checked and counter ticks, and the resume address. It also holds the first
  32 mismatches (address, expected, observed, test, element), a count of later
  ones, the DMA0 poll count per transfer and the harness table.
  `platform_exit` issues `DSB SY` before the exit `SMC`.

## Tests, in run order

0. **Alias probe,** in every run before any other write outside the window.
   A 4 KiB canary page in the image's data holds one 64-byte canary line per
   offset bit 26 to 33; bit 26 is the lowest bit above the 64 MiB window, and
   bits 32 and 33 are the wraps at 4 GiB and 8 GiB. For each bit b, the image
   writes a unique tag at canary_b + 2^b in the tested map, cleans and
   invalidates both lines, issues one `DSB SY`, then reads every canary and
   every tag. A changed canary or a missing tag stops the run: the image
   records the bit and returns through `platform_exit` with no further write
   outside the window. The probe cannot see a non-linear decode that maps a
   multi-bit offset into the window; the page signature rereads the canaries
   after each GiB and stops on a change, which bounds such damage but does
   not prevent it.
1. **Data lines.** Walking ones and zeros over one word and one 64-byte line
   at `0x4400_0000`, with `DC CIVAC` and `DSB SY` before each read so that it
   reaches DRAM.
2. **Address lines.** A base word at offset `0xC00_0000` (physical
   `0x4C00_0000`) receives a pattern, and for each offset bit 6 to 33 the
   word at base XOR 2^b receives a unique tag. All 29 words are written,
   cleaned and invalidated before any is read. If bit b is stuck or open, its
   tag lands on the base, which then reads wrong; a missing tag names the
   bit. The base has offset bits 26 and 27 set, so no partner falls in the
   window. It has physical bit 30 set and bits 31 to 33 clear, so each offset
   pair is also a physical single-bit pair, except for bit 30, whose physical
   partner lies in the MMIO window. Shorts between bits show in the page
   signature and the march. This runs in every run.
3. **Page signature,** in the first run of a board session, and again in the
   last run with a new seed as a stability check. Each 4 KiB page of the
   tested map receives one word, physical address XOR the run seed, at byte
   offset (page index × 64) mod 4096. The offset spreads the 3,129,344
   accesses across cache sets and probably across DRAM banks. Each store is
   followed at once by `DC CIVAC` on its line, and one `DSB SY` ends the write
   pass, so no alias partner can stay dirty in the cache while its page is
   read back. The read pass then compares every word. This finds
   page-granular aliasing anywhere in the map, dropped writes and accesses
   that abort.
4. **March C-** over the slice in 256 MiB sub-slices, with 64-bit words
   P(a) = a + seed mod 2^64, where a is the word's physical address. The six
   elements are: up (write P); up (read P, write ¬P); up (read ¬P, write P);
   down (read P, write ¬P); down (read ¬P, write P); down (read P). Each element streams 256 MiB, far more than the caches, so an element
   that runs in the previous element's direction first reads lines evicted
   long before. The TRM documents no replacement policy; under LRU,
   pseudo-LRU or random replacement the chance that a line survives 256 MiB of
   streaming through a cache of at most 8 MiB is below e^-32. At the one turn,
   up to down, the next reads start on the lines just written, so the image
   cleans and invalidates the whole sub-slice there with
   `aarch64_device_cache`, one call per 2 MiB block. Inner loops use NEON:
   one `LD1` or `ST1` of four 128-bit registers per line, and expected values
   held in four registers that advance by a constant vector of 64 per line.
   The read-and-write elements write the complement of the data they read and
   accumulate differences in one vector. Once per 2 MiB block, `UMAXV` tests
   the accumulator. On a hit, the block is rescanned with scalar loads. Each
   word that differs from the value the element should have left is recorded
   (after a write, the observed value is the complement of what is there) and
   rewritten with that value. A read-and-write line costs (up direction; down uses a
   register post-index of -64):

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
   read element drops `MVN` and `ST1` (14). A sub-slice starts only if the
   time of the last one still fits the budget; the image then records the
   resume address.
5. **Bandwidth and latency,** in the first run of a session. Read uses `LD1`
   of four 128-bit registers per line; write uses two `STP Q` per line over
   whole lines, so the A55 enters write streaming mode after three full lines
   (Cortex-A55 TRM r2p0, A6.2.6); copy uses two `LDP Q` and two `STP Q` per
   line. The runtime `memcpy` is not used. Each runs over 256 MiB, three
   samples. A pointer chase of 2^20 dependent loads over 16 KiB to 256 MiB in
   steps of four follows a full-period linear congruential permutation of the
   lines, inside one 512 MiB-aligned half of a 1 GiB block. The A55 L2 TLB
   splits a 1 GiB block into 512 MiB entries, so no point walks the tables
   after its first load. The 16-entry L1 data TLB holds 4 KiB pages only (TRM
   A5.2), so points above 64 KiB include L1 TLB misses that hit the L2 TLB.
   Each table row records the kernel identifier and stride; the CPU-L, CPU-B
   and DSU PLL words come from `a733_cpu_clock_report`. No clock changes.
6. **DMA0 cache maintenance and reachability,** last in the run and only when
   the mask selects them. Every buffer is whole 64-byte lines that DMA0 owns
   for the transfer; transfers start and end at unaligned offsets inside
   them, and the bytes outside the transfer must keep their fill pattern.
   Before the start, the CPU cleans the source lines and makes the
   destination lines clean. In the clean case the CPU writes pattern A,
   cleans and invalidates, then writes pattern B and cleans; DMA0 copies, and
   the CPU invalidates the destination and compares. In the invalidate case
   the CPU fills the destination, cleans it and reads it so its lines are
   resident and clean; DMA0 overwrites it, and the CPU invalidates and
   compares. The CPU touches no byte of those lines while DMA0 owns them.
   Each control omits only the maintenance under test, must see stale data
   (otherwise the case is reported insensitive), and ends with a clean and
   invalidate of its lines. Reachability follows the guard matrix in
   [dma-address-width.md](../dma-address-width.md) for bit 32 alone
   (`0x1_8000_0000`) and bit 33 alone (`0x2_8000_0000`), in both directions,
   with guards at every DRAM address sharing the low 32 bits.

Per-run split: the first run spends at most 1.5 s on the page signature and
2 s on the harness, and later runs give that time to the march. The alias
probe, data lines and address lines take under 10 ms *estimated*.

## Interrupts, barriers, cache policy and cores

The only interrupt is the platform's 1 kHz tick, and the image runs one task.
The monitor's secure timer expires after 2 s without renewal. The test loops
read the counter once per 2 MiB block (`ISB`, `MRS CNTPCT_EL0`), renew with
the existing `SMC` 0x734 every 200 ms and check the time budget with the same
read. DMA0 completion is a busy poll with a 10 ms counter deadline, which
section 1.3 of the performance rules permits: the returning platform does not
bind the DMA0 interrupt (SPI 111, INTID 143), and its delivery is not
board-qualified. The poll is replaced by binding INTID 143 in a returning
image once the I2S image qualifies its delivery. With at most 8 transfers and
no other work on the core, the poll costs nothing that matters.

The tested map is Normal write-back inner-shareable. Inner loops issue plain
loads, stores and, in the page signature, `DC CIVAC`, with no barrier.
`DSB SY` completes maintenance before a read-back (inside
`aarch64_device_cache`, and once after the signature write pass), precedes the
closing counter read of a write sample and precedes the exit `SMC`. Ownership
of a DMA0 buffer moves between CPU and device at the start write and at the
completion read only. NEON state is saved per task, since
`configUSE_TASK_FPU_SUPPORT` is 2.

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
driver and host admission record; and `a733_cpu_clock_report`. Admission
checks each descriptor once at its own granule: `a733_mapping_check_stride`
with a 2 MiB step over the 480 level-2 blocks of `0x4400_0000`–`0x8000_0000`,
and with a 1 GiB step over level-1 entries 2 to 12, 491 write translations
in all. The host comparison of the table words proves that each entry is a
block descriptor, so one translation covers the block. The tests make none.
Nothing is removed.

Changes needed from the primary agent:

- `platform/a733/platform.c`, under a build definition set only by the
  memtest target: level-2 entries 64 to 511 and level-1 entries 2 to 12
  become `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP = 0b00, the
  stage 2 and 3 word of the memory architecture design
  (`docs/designs/memory-architecture.md`).
  Both builders take the word from one shared descriptor macro, so the host
  comparison checks them against one golden value. Entries 0 to 63 stay as
  they are. Table memory stays 8 KiB unless a firewall range is excluded.
- The monitor's admission calls `a733_remote_writers_excluded()` with an
  aligned 32-bit load, stores the mask and refuses EL1 entry as described
  above.
- `CMakeLists.txt` gains one `include(cmake/a733-memtest.cmake)` line.

## Budgets and measurement

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| DRAM tested per board | 8 KiB | 12,224 MiB less any firewall range | Planner host model `checks/memtest-plan.py`: slices contiguous, outside the window and excluded ranges |
| DRAM mapped at EL1 | 128 MiB; 8 KiB of tables | 12,288 MiB; 8 KiB of tables (16 KiB with a firewall range) | Host comparison of the table words with the shared golden word |
| Translations | none | 491 at admission; none afterwards | `AT` count in Unicorn emulation |
| March instructions per 64-byte line, two lines unrolled | none | write ≤ 6; read-verify ≤ 14; read-verify-write ≤ 19 | Unicorn count over 1 MiB, image flags |
| Work per 2 MiB block outside the loop | none | one counter read, one `UMAXV` test, one progress store, ≤ one `SMC` per 200 ms; no call or barrier | Unicorn counters |
| March maintenance | none | one clean and invalidate of the sub-slice (4,194,304 lines, 34–170 ms *estimated*) at the up-to-down turn; none elsewhere | Unicorn counters; board time in the record |
| March rate on one A55 at 494 MHz | none | write 2–3, read-verify 1.5–2.2, read-verify-write 1.0–1.6 GB/s *estimated*; 0.9–1.6 s per sub-slice | Instruction counts at about one 128-bit instruction per cycle; board figures replace the estimate |
| Runs for March C- over the map | none | 3–5 (44–74 s of march at 16 s in the first run and 19 s in later runs) *estimated* | Planner |
| Alias probe and address lines | none | 8 tags and 8 canaries; 29 writes and 29 reads; every run | Unicorn counters |
| Page signature | none | 2 × 3,129,344 accesses and 3,129,344 `DC CIVAC`; ≤ 6 instructions per page per pass; ≤ 1.5 s *estimated*, assuming at least four misses overlap; first and last run only | Unicorn counters and recorded offsets |
| Harness kernels | none | read ≤ 2, write ≤ 3, copy ≤ 5 instructions per line; chase ≤ 1.25 per load (eight loads unrolled); ≤ 2 s; samples ≥ 50 ms; first run only | Unicorn count over 1 MiB |
| Wall time per run | none | ≤ 20 s, clamped at admission | Budget logic in Unicorn with a modeled counter |
| DMA0 cases | none | ≤ 8 transfers of ≤ 1 MiB; maintenance on the enclosing lines only | Case code linked with the `checks/dma-controller.c` model |

Unicorn memory hooks also inject a stuck bit, an aliased address bit, an
offset wrap that lands in the window canaries, a dropped write, a page alias
and a coupled word; the intended test must report each one at its exact
address, and the wrap must stop the run before any further high write.
Contract cases cover each admission refusal and a time budget above 20 s.

## Failure classes

- **Contract or admission refused,** including a remote-writer bit outside
  OR100 and CE: before any write outside the window. Nothing is retained.
- **Alias probe change:** the run stops before further high writes and
  records the bit. Nothing is retained; the host makes no further upload to
  that board until the user reviews the record.
- **Mismatch:** recorded and counted; the run finishes the sub-slice so that
  the fault's extent is known. Nothing is retained.
- **External abort or SError,** such as a firewalled range: the platform
  records ESR, ELR and FAR and returns to ROM. The progress word names the
  2 MiB block. No device is active, so the state is known. The host adds the
  range to the exclusions with a rebuild, which costs one run.
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
- **The both-bits DMA case** (`0x3_2000_0000`): dropping both bits lands at
  `0x2000_0000`, the start of the PCIe outbound window in the A733 memory
  map, where a read is a PCIe access with no documented side-effect-free
  behavior. It needs that documentation first.
- **Crypto engine reachability:** the CE bit is set before the image could
  touch CE on every board, so the rule above refuses the case.
- **Interleaving sub-slices** to remove the turn's clean and invalidate,
  unless board timing shows that pass above 10% of the sub-slice time.
- **Clock changes and DRAM frequency or voltage margining:** each clock change
  needs the user's approval, and training, frequency and voltage stay in boot0.
- **Row-disturbance tests and timing-based bank-map discovery:** they need
  hours or a decoded configuration, and no consumer needs them.
- **GPU, NPU, IOMMU, xHCI and PCIe reachability:** each needs its own device
  bring-up, and those packages run the same guard matrix.
- **Native USB0 results:** FEL reads the 4 KiB record after return, so a
  native stack would add failure modes and no information.

## Board evidence that would confirm the design

Each run record of one board session shows a remote-writer mask of OR100 and
CE only, a passing alias probe, no mismatch and passing address-line tests.
The first and last runs pass the page signature with no abort, and the resume
addresses chain to cover the tested map. With a DMA admission record, both
cache cases pass and their controls see stale data, and the guard matrix
passes for bit 32 and bit 33. The first record carries the harness table and
clock words, and every record carries the turn's maintenance time.
