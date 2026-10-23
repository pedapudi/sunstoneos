# Memory qualification image

Status: **implemented** and host-modeled; no run has taken place on a board.
In Unicorn emulation the image's admission accepts the 12,288 MiB map that
its own `platform_init()` writes and the remote-writer sample that its
monitor stores. The DMA0 cache-maintenance and reachability cases are not
built, and admission refuses their contract bits.

The [design](designs/memory-qualification.md) gives the reasoning and the
budgets; [the measurements](designs/memory-qualification-measurements.md)
compare this implementation with the budgets. This page describes the image
as built.

## Purpose and run

The board has 12,288 MiB of LPDDR5 at physical `0x4000_0000`–`0x3_4000_0000`.
The image runs one FreeRTOS task on the boot Cortex-A55, tests the DRAM outside
the 64 MiB it occupies and returns to the ROM's FEL interface. The host then
reads a checksummed record over USB0. One run lasts at most 20 s; the March C-
test over the whole map takes three to five runs (estimated). An *offset* is a
physical address minus `0x4000_0000`, and a *block* is 2 MiB.

Each run follows these steps:

1. The host uploads the image to `0x4100_0000`, writes the 128-byte run
   contract at `0x4100_5000` and a zero record page at `0x4200_3000`, and
   starts the monitor entry at `0x4100_0000` with `reset64`.
2. The task admits the contract before any write outside the protected window.
3. The task runs the window scan, then the tests the contract selects.
4. The task writes the record checksum and returns through `platform_exit`.

## Memory layout

| Physical range | Contents |
| --- | --- |
| `0x4000_0000`–`0x43ff_ffff` | Protected window: boot0 and launcher memory, the image and the result block. Never tested |
| `0x4100_0000` | Monitor entry and EL1 start code |
| `0x4100_1000`–`0x4100_4fff` | Recovery region: monitor vector table, recovery code, monitor state and monitor stack, one page each |
| `0x4100_5000` | Run contract, 128 bytes, zero in the image |
| `0x4200_3000`–`0x4200_3fff` | Result record, the last page of the platform's 16 KiB non-cacheable result block |
| `0x4400_0000`–`0x3_3fff_ffff` | Tested map, 12,224 MiB, less the contract's excluded ranges |

The secure timer and the exit SMC run the monitor at EL3 with its MMU off,
so recovery needs only the four recovery pages. The recovery code is what the
timer, exceptions, renewal and exit run: `monitor_sync`, `monitor_reset`,
`monitor_exception`, `finish` and the set/way cache routines, with their
literal pools. `platform/a733/monitor.S` puts `monitor_sync` and
`monitor_reset` in the section `.text.monitor_recovery`.
`apps/memtest/link.ld` places this code, the monitor state and
a `0x7c0`-byte monitor stack in bytes `0x000`–`0x7bf` of their pages, and the
vector table in `0x000`–`0x7ff`. Linker assertions bound each part. Every
64-bit word from offset `0x7c0` to `0xfff` of the four pages is a *beacon*:
the word's own physical address XOR the constant `K = 0xa5c3_5a3c_0000_0000`,
whose bits 0–33 are zero. `apps/memtest/beacons.py` writes the beacons into
the linked ELF, refusing any byte that is not padding, before the build
converts the ELF to the uploaded binary.

The tests write these fixed extents of the tested map:

| Extent | Address | Bytes | Written by |
| --- | --- | --- | --- |
| Data line | `0x4400_0000` | 64 | Data-line test |
| Address lines | `0x4c00_0000` | 64 MiB span, 21 words | Address-line test |
| Harness buffer | `0x1_0000_0000` | 192 MiB | Bandwidth samples |
| Chase table | `0x1_2000_0000` | 256 MiB | Latency chase |

## Run contract and admission

`struct memtest_contract` in `apps/memtest/memtest.h` holds a magic word, a
sequence number, the march slice as block indexes `[start, end)` from the DRAM
base, the test mask, the time budget in 24 MHz counter ticks, the data seed
and up to ten excluded ranges, also as block indexes. Mask bits: data lines
(1), address lines (2), page signature (4), March C- (8), harness (16), DMA0
cache maintenance (32) and DMA0 reachability (64). The window scan runs on
every run and has no bit.

Admission (`memtest_admit` in `apps/memtest/plan.c`) records the first refusal
it finds and returns before any write outside the window. It refuses:

- a contract with the wrong magic word, an unknown mask bit or a DMA0 bit;
- more than ten excluded ranges, or a range that is empty, unsorted,
  overlapping or outside the tested map;
- a march slice that is empty, outside the tested map or intersects an
  excluded range (`a733_extent_within`, `a733_extents_intersect`);
- a fixed extent outside the tested map or inside an excluded range;
- a slice that cannot form march groups;
- a seed whose bits 34–63, or those of its complement, lie within one of K's,
  and a zero budget; a budget above 20 s is clamped to 20 s;
- `DCZID_EL0` other than a permitted 64-byte block, and a data-cache total of
  zero or above 8 MiB, summed over the levels `CLIDR_EL1` lists;
- an absent remote-writer sample, and a sample naming a writer other than
  OR100, the crypto engine (CE) and USB0;
- a write translation of `0x4400_0000`, `0x8000_0000` or `0x3_3fff_f000`
  (one per descriptor kind) that is not Normal write-back inner-shareable at
  its own physical address.

The march planner cuts the slice into *members* of at most 256 MiB from its
start and joins consecutive members into groups of two or three whose members
other than any one total at least 256 MiB. A dynamic program over the members
picks the smaller group whenever the rest of the slice can still be grouped,
so every group boundary is a valid start for a later run. A slice shorter than
512 MiB cannot be grouped and is refused. The full map forms 48 members (47 of
256 MiB and one of 192 MiB) in 21 pairs and two triples.

## Tests in run order

Each walk kernel in `apps/memtest/kernels.S` processes whole blocks. Before
each block it stores the block address in the record's `progress` word; after
each block it reads the counter once, renews the secure timer when 200 ms have
passed and stops at the run's deadline.

1. **Window scan.** For every page of the tested map outside the excluded
   ranges, the kernel loads the first words of lines 31 and 63 (`0x7c0` and
   `0xfc0`), sixteen loads of eight pages before the first compare, and stops
   when a value minus `K | 0x4100_1000` is below the 16 KiB region size. A hit
   stops the run before any write outside the window. The record keeps the
   tested word, the value and the faulty offset bits: the offset of the tested
   word XOR the offset of the beacon it read. A completed scan marks offset
   bits 26–33 tested.
2. **Data lines** (bit 1). Walking ones and walking zeros over one word and
   over the 512 bits of one line, each pattern written, cleaned and
   invalidated with `DC CIVAC` and `DSB SY`, then read.
3. **Address lines, offset bits 6–25** (bit 2). A pattern at the base and the
   tag `2^32 + b` at each partner base XOR `2^b`; all 21 words are written,
   cleaned and invalidated before any is read. A tag that appears at the base
   or a partner that lost its tag names the bit.
4. **Page signature** (bit 4). The write pass zeroes line 0 of each page with
   `DC ZVA`, stores the page address XOR the seed, cleans the line with
   `DC CIVAC` and issues `DSB SY` once per block. The recovery digest is then
   checked. The read pass compares the signature of eight pages per batch. A
   page that holds another page's signature proves an address decode fault and
   stops the run before the march.
5. **March C-** (bit 3): up(w P); up(r P, w ~P); up(r ~P, w P); down(r P,
   w ~P); down(r ~P, w P); down(r P), with P(a) = a + seed at each 64-bit word
   address a. Each element runs over every member of a group, in member
   order, before the next element starts; a down element walks each member
   from its top block to its bottom. Between a write to a line and its next
   read, the other members of the group, at least 256 MiB, stream through the
   caches, so the march needs no cache maintenance. `checks/memtest-plan.py`
   walks this order over every group of the board plan and confirms the
   distance. Because the member order is the same in both directions, the
   down elements visit two cells in different members in the same order as
   the up elements. Between members the march therefore detects only the
   coupling faults that its up elements detect: some faults whose aggressor
   lies in a later member than its victim go undetected.
   The kernels load each line one line before its first use, prefetch with
   `PRFM PLDL1STRM` 16 lines ahead in the direction of travel, OR the
   differences into one accumulator and test it with `UMAXV` once per block. In
   a member's last block the last 16 lines prefetch only the next line, so no
   prefetch reaches past the member.
   A nonzero accumulator triggers a scalar rescan of the block. A
   read-and-write element left the complement of each word it read, so the
   rescan recovers the value read. A value equal to P(b) or ~P(b) for another
   8-byte-aligned DRAM address b proves a decode fault and stops the run before
   its next write. Other differing words are recorded and rewritten with the
   value the element should have left. The rescan counts mismatches in
   cacheable memory and writes the record's count once per block; only the
   first 32 mismatches reach the record's non-cacheable entries. A rescan that
   finds nothing counts as an unconfirmed hit. A mismatch or an unconfirmed
   hit fails the run, and the current group finishes unless the deadline
   passes first. A group starts only if the slowest rate seen
   so far, scaled to the group's size, fits before the deadline; the first
   group uses the cost model's slow rate (1.45 s per 256 MiB). After each
   group the recovery digest is checked and `resume` moves to the next group.
   A single-bit cell fault in bits 3–33 of a word can read as the pattern of
   another DRAM address and then stops the run as a decode fault.
6. **Harness** (bit 16). Three samples each of an ascending read, a
   descending read and a write over 192 MiB, and three of a 64 MiB copy. A
   dependent-load chase follows, over 16 KiB to 256 MiB in steps of four with
   2^18 loads per point, through a full-period linear congruential permutation
   of the lines.
   Write and copy samples end with `DSB SY` before the closing counter read.
   The record also carries the oscillator, SID and CPU-L, CPU-B and DSU PLL and
   mux words that `a733_cpu_clock_report` reads.

The *recovery digest* cleans and invalidates the vector and code lines and the
beacon lines of all four recovery pages, 192 lines, issues one `DSB SY` and
hashes the 12 KiB as DRAM holds them. A digest that differs from the one taken
at admission stops the run.

## Time budget and the secure timer

The deadline is the task's start plus the admitted budget. When the contract
selects the harness, the march stops 2.5 s before the deadline. The monitor's
secure timer fires 2 s after the last renewal; every block, harness sample and
chase point is followed by a counter read that renews with `SMC 0x734` once
200 ms have passed. The scan, signature and march stop at the deadline; a
march group stopped by the deadline is repeated by the next run. A block that
needs a rescan returns from its kernel before the kernel's deadline check, so
the task checks the deadline after every rescan. A fault in every block
therefore ends the run, with its mismatches recorded, within one block and
one rescan of the deadline.

## Result record and failures

`struct memtest_record` opens with a magic word and a version word that holds
the layout size, and closes with an FNV-1a checksum over every earlier word.
The host trusts nothing in a record whose magic, version or checksum differs.
It also checks the contract's sequence number and digest. Overall status is
pass, fail, refused (with the refusal) or stopped (with the reason: scan hit,
decode fault, digest mismatch or deadline). Per test the record holds a status
(not run, pass, fail, stopped) with words checked and counter ticks. It also
holds the first 32 mismatches with a total count, the unconfirmed-hit count,
the remote-writer sample, DAIF, `DCZID_EL0`, the data-cache size, the tested
and failed address bits, the scan hit, both digests, groups completed, the
slowest march rate, renewals, the resume address and the harness table.

An external abort or SError ends in the platform's exception capture with no
checksum. The host then excludes the block that `progress` names and the block
before it in the pass's travel order, which `abort_exclusion` in
`apps/memtest/check.py` computes from the `progress` and `phase` words. In the
march it excludes both neighbors of the block, because a march walk may run
either way through a member. At the first block of a member walk the block
before it in travel order lies in another member and is not excluded; an abort
that arrives that late costs one more run and one more exclusion.

## Residuals the user accepts

Three failure modes remain that the image cannot detect or survive. The host
prepares no contract until the run log holds the user's acceptance of all
three.

- **Remote writers.** Admission accepts OR100, the crypto engine (CE) and USB0
  as able to write DRAM, because the board ROM leaves them so. Each exclusion
  holds only when the monitor samples it, and a write by one of them during a
  run reads as a DRAM fault.
- **Bus stall.** A denied access is assumed to receive an error or an
  ignored-write response. If it stalled the bus instead, the core would hang
  with no watchdog, and only the secure timer could end the run.
- **Partial-page alias.** The window scan reads two words per page. A fault
  can send both to words of a recovery page that are not beacons while
  another line of the page reaches monitor code, and the scan then misses it.
  Examples are two stuck line-index bits, such as offset bits 6 and 11 both
  stuck at 0, and an alias that moves only some lines of a page. The page
  signature or the march would then overwrite the monitor, and the board,
  which cannot be reconnected, would not return.

## Host side

`apps/memtest/check.py` compiles `apps/memtest/plan.c` for the host and calls
the same admission function the image runs.

- `prepare --elf IMAGE.elf --image IMAGE.bin --run-log LOG.json --output DIR`
  chooses the next contract from the run log and writes `image.bin`,
  `contract.bin`, `record-zero.bin` and `prepared.json`. The log holds the
  boot0 upload count as `session`, the user's acceptance record, the earlier
  runs and the excluded ranges. Each run entry holds its session, sequence and
  status, and from a trusted record its stop reason, resume address and DMA0
  phase marker. Every run checks the data and address lines and marches from
  the session's last resume address to the next excluded range or the end of
  the map. The first run of a session adds the page signature and the harness.
- The acceptance record is `{"by": ..., "date": ..., "residuals":
  ["remote-writers-or100-ce-usb0", "bus-stall", "partial-page-alias"]}`.
  Without it, or with a residual missing, `prepare` refuses, and
  `prepared.json` carries a copy of it.
- `prepare` also refuses while a run that needs review lacks a
  `"reviewed": {"by": ..., "date": ...}` entry. In any session, these runs
  need review: a stop for a scan hit, a decode fault or a digest mismatch; a
  DMA0 phase marker left set; and a run with an untrusted or missing record.
  In the current session, any other run that did not pass also needs review.
  A run whose untrusted record was explained by an abort and handled with an
  exclusion (`"abort_excluded": true`) blocks only its own session; after the
  next boot0 upload the runs continue without review.
- `verify --result RESULT.bin [--contract CONTRACT.bin]` checks a returned
  result block or record page and prints its decoded fields, or, for an
  untrusted record, the blocks to exclude.
- `execute()` performs one run through the transaction and return callbacks
  of `tools/a733-returning-host.py`. The physical admission of a run, its
  supervision record and the root-owned claim belong to the coordinator.

## Build and checks

`cmake/a733-memtest.cmake`, which `CMakeLists.txt` includes after the
FreeRTOS port setup, defines the `a733-memtest` target: the returning
platform sources, `platform/a733/remote_writers.c`, `apps/memtest` and the
FreeRTOS kernel, linked with `apps/memtest/link.ld` and `--emit-relocs`. The
target is excluded from the default build, so
`cmake --build BUILD --target a733-memtest` builds it.
`SUNSTONE_USB_RUNTIME` builds the monitor's renewal handler, and
`SUNSTONE_MEMTEST` selects the platform and monitor definitions described in
the next section. After the link the build removes the debug relocations,
writes the beacons and produces `a733-memtest.bin`.

The host checks run without a board:

```sh
PY="env PYTHONDONTWRITEBYTECODE=1 $HOME/.cache/sunstoneos/venv/bin/python"
$PY checks/memtest-plan.py --out OUT/plan
$PY checks/memtest-image.py --elf BUILD/a733-memtest.elf --binary BUILD/a733-memtest.bin --out OUT/image
$PY checks/memtest-emulate.py --elf BUILD/a733-memtest.elf --binary BUILD/a733-memtest.bin --out OUT/emulate
```

- `memtest-plan.py` checks that the host mirrors match the C layouts and runs
  every contract and fact refusal, the board plan, resume chaining, excluded
  ranges, the seed rule, the budget clamp, the data-cache sum and the
  run-count model. It walks the march's element, member and block order over
  every group and confirms that at least 256 MiB separate each write from the
  next read, and it runs every refusal of the upload gate and of the
  acceptance record.
- `memtest-image.py` checks the recovery placement, the beacons in the ELF
  and the binary, the targets of every relocation into the recovery pages, the
  EL3 stack depth, the zero contract and the record layout. It also emulates
  `platform_init` and classifies every translation-table word: the
  tested-map entries must equal the descriptor word given in the next
  section, and every table write must come after a clean and invalidate of
  both table pages and before a second one. `--baseline` reports the same
  figures for another image and also accepts a 128 MiB map there.
- `memtest-emulate.py` runs the linked kernels and the image's task in
  Unicorn over a reduced 64 MiB tested map with a modeled counter and monitor
  sample, after the image's own `platform_init` has written its tables. One
  case clears the tested-map entries and confirms the mapping refusal, and
  another runs `monitor_init` against modeled reset registers and checks the
  stored sample and the refusal of EL1 entry. It counts instructions, barriers,
  maintenance and accesses against the budgets and injects the faults that
  the design lists, apart from the DMA0 cases. One case puts a fault in every
  march block with a counter that moves 0.1 s per read and confirms that the
  run ends no later than a clean run. `checks/memtest-machine.py`
  holds the shared emulation model.

## Platform map, log limit and remote-writer sample

`SUNSTONE_MEMTEST` changes three things in the shared returning platform:

- **Map.** `platform_init()` in `platform/a733/platform.c` writes level-2
  entries 64–511 and level-1 entries 2–12 with `A733_EL1_DRAM_BLOCK(pa)` from
  `include/a733_device_memory_layout.h`:
  `PA | UXN | PXN | AF | SH=0b11 | AttrIndx=1 | 1` with AP = 0b00. Index 1 of
  the platform's `MAIR_EL1` value `0x44ff00` is `0xff`, Normal write-back.
  The cached memory builder uses the same macro for its stage-2 entries.
  Entries 0–63 and the 8 KiB of tables are unchanged. The tables map
  12,286 MiB write-back and the 2 MiB result block non-cacheable.
- **Log limit.** `platform_puts()` stops the log at offset `0x3000`, so it
  cannot reach the record page.
- **Remote-writer sample.** At the end of `monitor_init()`, at EL3 before
  EL1 entry, the monitor calls `a733_remote_writers_excluded()` with one
  aligned 32-bit load per register and stores `(0x57525452 << 32) | mask` in
  the record word at `0x4200_3010`. It refuses EL1 entry through
  `finish(4)` on any bit other than OR100 (1), CE (4) and USB0 (12).
  `monitor_init()` and the sampling code stay outside the recovery pages.
  [remote-writers.md](remote-writers.md) records the acceptance of these
  three writers.

## Board evidence that would confirm the image

Every record of a session carries the magic word, a valid checksum and the
contract's sequence, and its remote-writer sample names only OR100, CE and
USB0. DAIF shows SError unmasked. The scan passes, the address result covers
offset bits 6–33 with no failure, and no mismatch, unconfirmed hit or digest
change appears. The first run of a session passes the signature with no abort
and records the harness table and clock words. The resume addresses chain
across the tested map, and the measured scan and march times replace the
estimates.
