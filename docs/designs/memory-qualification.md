# Design: returning memory qualification image

Status: **design**; no code exists. No figure has been measured on the board,
and figures marked *estimated* come from the instruction sequence. Readers are
assumed to know AArch64 caches and translation, the returning platform in
[a733-kernel.md](../a733-kernel.md) and
[performance-architecture.md](../performance-architecture.md).

The board has 12,288 MiB of LPDDR5 at physical `0x4000_0000`–`0x3_4000_0000`.
Board evidence covers transfers to two 4 KiB pages
([hardware.md](../hardware.md), DDR row), and the returning platform maps the
first 128 MiB at EL1. This image runs on the boot Cortex-A55 and tests all DRAM
outside the 64 MiB it occupies. It checks address decoding across the full map,
cache maintenance and DMA reachability above 4 GiB, and measures bandwidth and
latency. It then returns to the ROM's FEL interface, where the host reads a
4 KiB result record over USB0.

## Data flow and ownership

- **Run contract.** After the FEL upload, the host writes a 64-byte contract
  to a linker-placed address in the image: slice start and end, test mask,
  time budget in counter ticks, data seed, and the digest of a device
  admission record (zero disables the DMA cases). The image digest excludes
  the contract; the run record stores it. Admission checks the slice once
  with `a733_extent_within`.
- **Protected window and tested map.** The window `0x4000_0000`–`0x4400_0000`
  holds boot0 and launcher memory, the image at `0x4100_0000`, its stacks and
  the result block at `0x4200_0000`. Only the image's own code and data write
  it. The other 12,224 MiB form the tested map, which the image owns and
  overwrites. FEL cannot save and restore 12 GiB, so host admission must
  confirm that no DRAM content outside the window is needed afterwards.
- **Remote writers.** Admission calls `a733_remote_writers_excluded()` once
  and records its bitmask, so the host can attribute mismatches that another
  bus master causes.
- **Result record.** 4 KiB at `0x4200_4000`, after the platform's 16 KiB
  result block and in the same Normal non-cacheable 2 MiB block. Only core 0
  writes it. It holds the contract, the bitmask, each test's status (pass,
  fail, insensitive or not run), words checked and counter ticks, and the
  resume address. It also holds the first 32 mismatches (address, expected,
  observed, test, element), a count of later ones and the harness table.
  `platform_exit` issues `DSB SY` before the exit `SMC`.

## Tests, cheapest first

1. **Data lines.** Walking ones and zeros over one word and one 64-byte line,
   with `DC CIVAC` and `DSB SY` before each read so that it reaches DRAM.
2. **Address lines.** For each address bit 6 to 33, a pair of tested-map
   addresses that differ only in that bit receives a pattern and its
   complement. All pairs are written, cleaned and invalidated before any is
   read, so a stuck or open bit leaves an overwritten pair. Shorts between
   bits show in the page signature and the march. The memory
   configuration is not decoded, so this covers the bank, row and column bits
   wherever they lie. It runs over the full map in every run.
3. **Page signature.** One word per 4 KiB page of the full map holds its own
   address and is read back. This finds page-granular aliasing between slices
   of different runs, which the march cannot see, non-linear decoding, and
   Non-secure writes that the boot0 firewall drops.
4. **March C-** over the slice in 256 MiB sub-slices, with word data
   `P(a) = a XOR seed`: write P up; read P and write ¬P up; read ¬P and write
   P up; read P and write ¬P down; read ¬P and write P down; read P. A
   sub-slice is far larger than the caches, so only the lines an element
   wrote last can satisfy the next element's reads. Each element therefore
   ends with `DC CIVAC` over its last bytes, as many as the cache size read
   from `CCSIDR_EL1`. A sub-slice starts only if the previous one's time
   still fits the budget; the image then records the resume address.
5. **Cache maintenance and DMA reachability,** after the CPU tests and only
   with an admission digest for DMA0 and the crypto engine. In the clean case
   the CPU writes and cleans a source, DMA0 copies it, and the CPU invalidates
   and compares the destination. In the invalidate case the CPU caches the
   destination, DMA0 overwrites it, and the CPU invalidates and compares.
   Ranges start and end inside cache lines. Each case also runs without its
   maintenance and must then see stale data; otherwise it is reported
   insensitive. Reachability follows the guard matrix in
   [dma-address-width.md](../dma-address-width.md): bit 32 alone, bit 33
   alone, then both, with guards at the same low 32 bits. The both-bits case
   runs only after the others pass, with the master reading high memory.
   DMA0 covers both directions; a crypto engine hash covers its reads.
6. **Bandwidth and latency.** Sequential read, write and copy over 256 MiB,
   three samples each. A pointer chase of 2^20 loads over 16 KiB to 256 MiB
   in steps of four, through a full-period linear congruential permutation of
   the lines, inside one 1 GiB block so that TLB misses stay out. The CPU-L,
   CPU-B and DSU PLL words are recorded with `a733_cpu_clock_report`; no
   clock changes.

## Interrupts, barriers, cache policy and cores

The only interrupt is the platform's 1 kHz tick, and the image runs one task.
The monitor's secure timer expires after 2 s without renewal. The test loops
read the counter once per 2 MiB block (`ISB`, `MRS CNTPCT_EL0`), renew with
the existing `SMC` 0x734 every 200 ms and check the time budget with the same
read. DMA0 completion is a busy poll with a 10 ms counter deadline. Section 1.3
of the performance rules permits it: the returning platform delivers no DMA0
interrupt, and the core has no other work to run.

The tested map is Normal write-back inner-shareable, as for every DRAM
consumer. Inner loops issue plain loads and stores and no barrier. `DSB SY`
completes maintenance before a read-back (inside `aarch64_device_cache`),
precedes the closing counter read of a write sample and precedes the exit
`SMC`. The CPU cleans the exact source bytes before a DMA0 start and
invalidates the exact destination bytes after the completion read. Ownership
moves between CPU and device at those two points only.

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
driver and host admission record; the crypto engine driver; and
`a733_cpu_clock_report`. Admission makes one write translation per 2 MiB
block, 6,112 in all, and the tests make none. Nothing is removed.

The primary agent adds a build definition to `platform/a733/platform.c`, set
only by the memtest target. It fills all 512 level-2 entries for
`0x4000_0000`–`0x8000_0000` as write-back blocks, except the non-cacheable
result block. It sets level-1 entries 2 to 12 as 1 GiB write-back
inner-shareable execute-never blocks, the stage 3 words of the memory
architecture design. Table memory stays 8 KiB. `CMakeLists.txt` gains one
`include(cmake/a733-memtest.cmake)` line.

## Budgets and measurement

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| DRAM tested per board | 8 KiB | 12,224 of 12,288 MiB | Planner host model `checks/memtest-plan.py`: slices contiguous and outside the window |
| DRAM mapped at EL1 | 128 MiB; 8 KiB of tables | 12,288 MiB; 8 KiB of tables | Host comparison of the table words with the stage 3 words |
| Translations | none | 6,112 at admission; none afterwards | `AT` count in Unicorn emulation |
| Instructions per 64-byte line | none | write ≤ 8; read-verify ≤ 12; read-verify-write ≤ 16 | Unicorn count over 1 MiB, image flags |
| Work per 2 MiB block outside the loop | none | one counter read, one progress store, ≤ one `SMC` per 200 ms; no call or barrier | Unicorn counters |
| Maintenance per march element | none | `DC CIVAC` over at most 8 MiB, one `DSB SY` | Unicorn counters |
| Address-line, page-signature accesses | none | 112; 2 × 3,129,344 (under 1 s *estimated*) | Unicorn counters |
| Wall time per run | none | ≤ 20 s, inside the host's 30 s return window | Budget logic in Unicorn with a modeled counter |
| Runs for March C- over the map | none | 4–7 at 1–2.5 GB/s *estimated* | Planner; board figures replace the estimate |
| DMA and crypto engine cases | none | ≤ 24 transfers of ≤ 1 MiB; one clean and one invalidate per transfer, exact bytes | Case code linked with the `checks/dma-controller.c` model |
| Harness | none | ≤ 2 s per run; samples ≥ 50 ms | Unicorn counters |

Unicorn memory hooks also inject a stuck bit, an aliased address bit, a
dropped write, a page alias and a coupled word. The intended test must report
each one at its exact address.

## Failure classes

- **Mismatch:** recorded and counted; the run finishes the sub-slice so that
  the fault's extent is known. Nothing is retained.
- **External abort or SError,** such as a firewalled region: the platform
  records ESR, ELR and FAR and returns to ROM. The progress word names the
  2 MiB block. No device is active, so the state is known.
- **Time budget reached:** not a failure; the resume address is recorded.
- **DMA0 deadline expired:** state unknown. The controller's recovery path
  stops the channel, and the record says whether it reads quiet. If not, the
  host makes no further upload until a FEL read shows the channel quiet. The
  channel and its buffers stay retained.

## Left out

- **Multicore runs,** until the primary agent qualifies a returning multicore
  base. Each core would then own a contiguous slice and a result slot on its
  own 64-byte line, and core 0 would wait for completion words with `WFE`.
- **The protected window,** which needs a second link address in high DRAM.
- **Clock changes and DRAM frequency or voltage margining:** each clock change
  needs the user's approval, and training, frequency and voltage stay in boot0.
- **Row-disturbance tests and timing-based bank-map discovery:** they need
  hours or a decoded configuration, and no consumer needs them.
- **GPU, NPU, IOMMU, xHCI and PCIe reachability:** each needs its own device
  bring-up, and those packages run the same guard matrix.
- **Native USB0 results:** FEL reads the 4 KiB record after return, so a
  native stack would add failure modes and no information.

## Board evidence that would confirm the design

Each run record of one board session reports no mismatch and passing
address-line and page-signature tests, and the resume addresses chain to
cover 12,224 MiB. With an admission record, both cache cases pass and their
controls see stale data, and the guard matrix passes for bit 32, bit 33 and
both bits. Each record carries its harness table and clock words.
