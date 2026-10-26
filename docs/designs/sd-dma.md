# Design: SD reads with descriptor DMA

Status: **designed, not built.** The performance and simplicity panel
(three lenses, two rounds) accepted the design with no open blocking finding.
An unbuilt first pass of the code is on `work/sd-dma-1025` (`29bc8a2c`);
"Next steps" in `docs/board-runbooks/sd-uhs.md` lists what remains. The design changes how the SD0 driver reads: one CMD18 per
request of up to 128 KiB, moved by the controller's internal DMA engine
(IDMAC) in the board kernel's order, with no CMD13 after a read, and a
returning image that measures each read without scheduler gaps. The card is
never written.

## What the first board measurement showed

Port 3-3 read 256 requests in SD profile 2 (four-bit high speed, 50 MHz) on
2026-10-25 with the returning image (`board-session-20261025-30/port-3-3/sd-ret-a`):

- **Programmed I/O nearly kept up with the bus.** Seven more blocks added
  (610 − 438) / 7 ≈ 25 µs each, against 20.8 µs per 512-byte block on the
  bus (1,042 clocks with CRC and start and end bits). The difference fits 128
  FIFO reads of about 190 ns each.
- **Each request cost about 417 µs** beyond its bus time. The driver part is
  about 120–170 µs: every platform poll re-checks about 25–30 clock, GIC and
  pin registers (`drivers/storage/a733_mmc_platform.c:102-150`, `:439-458`),
  every register access checks the task context and reads the clock twice,
  and every read is followed by CMD13 (`drivers/storage/sunxi_mmc.c:841-842`).
  The card's access time is the rest, about 250 µs; random reads add about
  300 µs.
- **113 of 256 reads were rounded to 2 ms steps.** The read task polls a
  transfer for at most 1 ms per wake and then sleeps a tick
  (`apps/storage-read-returning/main.c:218`, `:374`), so a longer read is
  seen only at the next wake.

## The vendor read path

The board kernel's SD0 driver (`allwinner,sunxi-mmc-v5p3x`) reads every data
command through IDMAC. The full sequence, with every register and citation,
is in `~/.cache/sunstoneos/evidence/sd-dma-20261025/vendor-idmac-sequence.md`;
"K" below is the board kernel's `bsp/drivers/mmc/sunxi-mmc.c`, "V" its
`sunxi-mmc-v5p3x.c` and "H" its `sunxi-mmc.h`, at `2ac08e8c`.

- **FIFO path:** `GCTRL` bit 31 (FIFO accessed by the CPU) stays clear, so
  the FIFO feeds IDMAC (H:156-157, K:729-735).
- **Once:** FIFO thresholds `FTRGL` = `0x200700F8` (8-word bursts, receive
  threshold 7; K:719, V:52) and the descriptor list base `DLBA` = physical
  address >> 2 (K:727, V:914-916). The shift of 2 applies because the
  controller version, `0x050530`, is at least `0x50300`.
- **Per read:** `BLKSZ` 512 and `BYTCNT` (K:2737-2738); the read threshold
  `THLD` at SDR104, `(512 << 16) | 1` with bit 2 clear (V:669-701);
  descriptors in non-cacheable memory, then a barrier (K:787-813); FIFO,
  DMA-interface and IDMAC resets, each waited (K:932-934); `GCTRL` DMA enable
  (bit 5); `IDIE` = receive (bit 1); `DMAC` = fixed burst and IDMAC on
  (`0x82`; K:951-958); then `CARG` and `CMD` = `0x80003352` for CMD18 with
  automatic CMD12, or `0x80002351` for CMD17 (K:2643, 2766-2769).
- **Descriptor:** 16 bytes, {config, size, buffer >> 2, next >> 2}, chained;
  every one OWN, chain and no-interrupt, the first also first-descriptor, the
  last also last-descriptor with the no-interrupt bit cleared, its next
  pointer at the following, unused slot; at most 4,096 bytes each
  (K:787-806, V:55).
- **Completion:** both the automatic-command-done bit (CMD18; data-over for
  CMD17) in `RINT` and the receive bit in `IDST`, or an error bit
  (K:1286-1338). No CMD13 follows a successful read.
- **Teardown:** `IDST` = `0x337`, `IDIE` = 0, `DMAC` = IDMAC reset, then DMA
  reset, DMA disable and FIFO reset in `GCTRL` (K:1171-1180); each descriptor's
  OWN bit is polled clear (K:826-847). The buffer is cleaned before the
  transfer and invalidated after it (inferred: the board kernel's arm64 DMA
  mapping source is not in the tree).

## The design

### Driver core (`drivers/storage/sunxi_mmc.c`)

- **A DMA read path, chosen by configuration.** `struct sunxi_mmc_config`
  gains one field, the descriptor area (an identity-mapped pointer), and the
  operations gain `dma_sync`. With both, every read uses IDMAC and may be up
  to 256 blocks (128 KiB, `SUNXI_MMC_MAX_DMA_BLOCKS`); the area holds
  `SUNXI_MMC_DMA_DESCRIPTORS` = 33 entries, 32 of 4 KiB and the spare slot the
  last one points at, which the core zeroes on every read. Without them, reads
  keep programmed I/O and the 8-block limit, which also sizes the write path.
- **Per read, in the vendor order:**
  1. In the existing reset write, set `GCTRL` FIFO and DMA reset (bits 1 and
     2) with bit 31 clear, and `DMAC` = 1; the next step waits for all three
     bits to clear. Programmed I/O keeps writing bit 31 set in its own reset
     write, so its data commands (CMD6, ACMD51, CMD19) keep the FIFO on the
     CPU path even after a DMA read. Setup refuses a descriptor area on a
     controller older than version `0x50300`.
  2. Write `FTRGL`, `DLBA`, `BLKSZ`, `BYTCNT` and `THLD`; write `IDST` =
     `0x337` and require it to read 0; build the descriptors; barrier.
     `FTRGL` and `DLBA` are written per read, two writes, because tuning
     recovery resets the controller (`sunxi_mmc.c:672`) and the vendor
     rewrites both after every reset (K:4547).
  3. `GCTRL` |= DMA enable; `IDIE` = 0; `DMAC` = `0x82`; `CARG`; `CMD`.
  4. Complete when `RINT` shows the response, data over and, for CMD18, the
     automatic CMD12, and then every descriptor's OWN bit reads clear (read
     only after the `RINT` bits, not on every poll); record
     whether `IDST` showed receive. Any `RINT` error bit, or `IDST`
     fatal-bus-error, descriptor-unavailable, card-error or abnormal-summary
     bit, faults the read; the read deadline bounds every wait.
  5. Teardown as the vendor's; then `dma_sync` invalidates the buffer and the
     read is DONE.
- **Differences from the vendor, with reasons.** `IDIE` stays 0: SD0's
  interrupt stays disabled, and a pending receive interrupt would fail the
  platform's per-poll interrupt check (`a733_mmc_platform.c:166`). Completion
  rests on the OWN bits, which IDMAC clears as it closes each descriptor,
  because the receive bit's latching with `IDIE` = 0 is inferred, not
  documented. The `IDST` error bits are checked, which the vendor does not.
  The vendor's retry path is not reproduced: a fault retains every lease.
- **`dma_sync(context, buffer, bytes, phase)`:** `SUNXI_MMC_DMA_BEFORE`
  cleans the buffer to the point of coherency, `SUNXI_MMC_DMA_AFTER`
  invalidates it. The image supplies it,
  because several images link the SD adapter without the cache maintenance
  code (`arch/aarch64/device_memory.c`).
- **No CMD13 after a read**, for programmed I/O and DMA alike, except after an
  open-ended CMD18 that ends at the card's last sector (`read_tail`), whose
  OUT_OF_RANGE bit would otherwise appear in the next read's response.
- **`THLD`** uses the vendor's mask (`(t & ~0x0fff0004) | (512 << 16) | 1`)
  for programmed I/O as well.
- **Timestamps:** the core records, from its clock callback, when it wrote
  the command, when it first saw a nonzero card byte count (`CBCR`, 0x48),
  and when it first saw completion, with the number of platform polls between
  them. Each is a first observation, late by up to one poll.

### Platform (`drivers/storage/a733_mmc_platform*.c`)

- The configuration gains the descriptor area. `command()` admits up to
  `SUNXI_MMC_MAX_DMA_BLOCKS` per read when an area is bound and keeps
  `SUNXI_MMC_MAX_BLOCKS` otherwise (`a733_mmc_platform.c:504`). The platform
  refuses an area
  while a storage update is bound, an area that overlaps the 16 KiB result
  page at `0x42000000`, and a DMA read whose buffer is not 64-byte aligned,
  below 16 GiB and outside every provider's state.
- On the core's `CMD` write, when `GCTRL` DMA enable reads set, the platform
  refuses a command with the write bit and walks the descriptor chain from
  `DLBA` (at most 33 uncached reads): every buffer must lie in the read's
  borrowed buffer, the sizes must sum to `BYTCNT`, and the chain must end at
  the zeroed spare slot. The walk adds about 10–20 µs to the fixed cost.
- The adapter admits `FTRGL`, `DLBA` (only the area's address >> 2), `IDST`,
  `IDIE` (only 0) and `DMAC` (only 0, 1 and `0x82`) writes, `CBCR` reads and
  `GCTRL` DMA enable only on a platform initialized with an area.
- The per-poll re-checks of clocks, pins and presence stay as they are; moving
  them to once per request is a separate, separately measured change.

### Returning image (`apps/storage-read-returning/`)

- **Times each read from submission**, with the 24 MHz counter, and polls it
  to completion with no per-wake budget and no tick sleep. The SD task runs
  at a higher priority than the PMIC task, so the PMIC task does not run
  during a read; identification still yields to it.
- **Reads through IDMAC** into a 128 KiB write-back buffer aligned to 4 KiB,
  with the 33 descriptors in the non-cacheable result block at `0x42100000`.
  The image's mapping check covers both once, at admission: write-back
  inner-shareable for the buffer, non-cacheable outer-shareable for the area.
- **The plan** has seven classes, 4.5 MiB in all: 64 sequential and 64 random
  4 KiB reads, 32 sequential and 32 random 32 KiB reads, 8 sequential and
  8 random 128 KiB reads, and 32 single sectors. Sequential classes start at
  sector 0, so the first 64 4 KiB reads repeat lifetime A's sectors and their
  CRCs must match. Random reads start at request-size boundaries below sector
  4,194,304.
- **CRC-32 with the ARMv8 CRC instructions**, about 2 ms for the plan instead
  of about 80 ms with the table, outside the measured time.
- **Each read line** gives the total time, the core's three first-observed
  times, the cache clean and invalidate times, and the poll count. The host
  tool fits t = F + b × blocks across the sequential classes of each mode,
  which gives the fixed cost F and the per-block time b, once with the
  recorded clean and invalidate times and once without them, and reports per class
  the rate, the mean command-to-first-byte time (the card's access time plus
  up to one poll) and the data time against the bus time.
- **SDR104 by default.** The default input selects profile 3; a card without
  UHS-I, or a failed switch before CMD11, falls back to high speed at 3.3 V
  (existing rule). A set runs one lifetime per mode: profile 2, then profile 3.
- **The clock stop stops IDMAC first.** When `DMAC` bit 7 or `GCTRL` bit 5
  reads set, it runs the vendor teardown and waits for `DMAC` bit 0 to clear
  and the `IDST` state field (bits 16:13) to read idle.
  If the stop does not complete, it asserts the SD0 bus reset (CCU `0xd0c`
  bit 16) as well as gating the module clock, because the module gate alone
  does not stop the bus-clocked IDMAC. After the monitor's 2 s exit or an
  exception, which run no image code, IDMAC may still write into the buffer
  or the area, so in both profiles the next step is power removal.

## Predictions

The time for one sequential read of n blocks is
t = F + S(n) + n × max(bus, card stream), with F the fixed cost per request
(240–400 µs with the per-poll re-checks kept), S(n) the cache maintenance
(about 5, 40–100 and 170–415 µs for 4, 32 and 128 KiB at the 494 MHz boot
clock), and a bus time of 20.8 µs per block at 50 MHz and 6.95 µs at
150 MHz. The SDR104 column assumes the card streams at 75 MB/s or faster.

| Request | High speed, 50 MHz | SDR104, 150 MHz |
| --- | --- | --- |
| 4 KiB | 7.1–9.9 MB/s | 8.7–13.6 MB/s |
| 32 KiB | 17.4–20.3 MB/s | 34–45 MB/s |
| 128 KiB | 20.7–22.8 MB/s | 50–60 MB/s |
| Single sector | 1.2–2.0 MB/s | 1.25–2.1 MB/s |

Random reads add about 300 µs per request, as lifetime A measured.

## Budgets

- Static memory in the returning image: 128 KiB buffer and 528 bytes of
  descriptors; the 4 KiB PIO buffer goes away.
- Code: the DMA path in the core, about 120 lines; the platform checks and
  filter, about 60; the image, about 80 changed.
- Images without a descriptor area change only in the removed CMD13 after a
  read and the `THLD` mask.

## Checks

The emulation check gains an IDMAC model: on a data command with DMA enabled
it reads the descriptor chain from memory, checks every field against the
vendor format, writes the card model's data through it, clears the OWN bits
and sets the `RINT` and `IDST` bits; it records the register order and
requires the sequence above. Cases cover every request class in both
profiles, an `IDST` fatal bus error, a chain that does not end at the spare
slot, a write-bit command with DMA enabled, a stalled transfer stopped at the
clock stop, an IDMAC that does not stop (bus reset), and the `read_tail` CMD13.
The host check covers the plan, the log format, the fit and the CRC
comparison with lifetime A.

## Deferred

- **Per-request re-checks** in the platform, instead of per poll, about 60 µs
  less per request.
- **The supervised read service** moves to DMA after this image has run on
  the board; programmed I/O then stays only for identification reads and
  writes.
