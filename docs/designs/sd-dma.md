# Design: SD reads with descriptor DMA

Status:
- **Qualified on the board:** one 4 KiB read and one 64 KiB read through sixteen descriptors, at 3.3 V, 50 MHz and four bits, with restored normal ROM return (port 3-3).
- **Implemented and host-tested:** the general DMA read path of the core (up to 128 KiB per request, 33 descriptors), the platform admission, and the returning image's read plan. Sustained throughput, larger extents, other cards and DMA fault recovery are unqualified.

The SD0 driver reads with one CMD18 per request of up to 128 KiB, moved by the controller's internal DMA engine (IDMAC) in the board kernel's order, with no CMD13 after a read. The returning image measures each read without scheduler gaps. The card is never written by this path (the write path is [CMD25 with descriptor DMA](mmc-multiblock-write.md)).

## The vendor read path

The board kernel's SD0 driver (`allwinner,sunxi-mmc-v5p3x`) reads every data command through IDMAC. The full sequence with every register and citation is in `~/.cache/sunstoneos/evidence/sd-dma-20261025/vendor-idmac-sequence.md`. Below, "K" is the board kernel's `bsp/drivers/mmc/sunxi-mmc.c`, "V" its `sunxi-mmc-v5p3x.c` and "H" its `sunxi-mmc.h`, at `2ac08e8c`.

- **FIFO path:** `GCTRL` bit 31 (FIFO accessed by the CPU) stays clear, so the FIFO feeds IDMAC (H:156-157, K:729-735).
- **Once:** FIFO thresholds `FTRGL` = `0x200700F8` (8-word bursts, receive threshold 7; K:719, V:52) and the descriptor list base `DLBA` = physical address >> 2 (K:727, V:914-916). The shift of 2 applies because the controller version, `0x050530`, is at least `0x50300`.
- **Per read:** `BLKSZ` 512 and `BYTCNT` (K:2737-2738); the read threshold `THLD` at SDR104, `(512 << 16) | 1` with bit 2 clear (V:669-701); descriptors in non-cacheable memory, then a barrier (K:787-813); FIFO, DMA-interface and IDMAC resets, each waited (K:932-934); `GCTRL` DMA enable (bit 5); `IDIE` = receive (bit 1); `DMAC` = fixed burst and IDMAC on (`0x82`; K:951-958); then `CARG` and `CMD` = `0x80003352` for CMD18 with automatic CMD12, or `0x80002351` for CMD17 (K:2643, 2766-2769).
- **Descriptor:** 16 bytes, {config, size, buffer >> 2, next >> 2}, chained; every one OWN, chain and no-interrupt, the first also first-descriptor, the last also last-descriptor with the no-interrupt bit cleared, its next pointer at the following, unused slot. Each is at most 4,096 bytes (K:787-806, V:55).
- **Completion:** both the automatic-command-done bit (CMD18; data-over for CMD17) in `RINT` and the receive bit in `IDST`, or an error bit (K:1286-1338). No CMD13 follows a successful read.
- **Teardown:** `IDST` = `0x337`, `IDIE` = 0, `DMAC` = IDMAC reset, then DMA reset, DMA disable and FIFO reset in `GCTRL` (K:1171-1180); each descriptor's OWN bit is polled clear (K:826-847). The buffer is cleaned before the transfer and invalidated after it (inferred: the board kernel's arm64 DMA mapping source is not in the tree).

## Driver core (`drivers/storage/sunxi_mmc.c`)

- **A DMA read path, chosen by configuration.** `struct sunxi_mmc_config` has a descriptor area (an identity-mapped pointer) and the operations include `dma_sync`. With both, every read uses IDMAC and may be up to 256 blocks (128 KiB, `SUNXI_MMC_MAX_DMA_BLOCKS`); the area holds `SUNXI_MMC_DMA_DESCRIPTORS` = 33 entries, 32 of 4 KiB and the spare slot the last one points at, which the core zeroes on every read. Without them, reads use programmed I/O and the 8-block limit, which also sizes the PIO write path. Setup refuses a descriptor area on a controller older than version `0x50300`.
- **Per read, in the vendor order:**
  1. In the existing reset write, set `GCTRL` FIFO and DMA reset (bits 1 and 2) with bit 31 clear, and `DMAC` = 1; the next step waits for all three bits to clear. Programmed I/O writes bit 31 set in its own reset write, so its data commands (CMD6, ACMD51, CMD19) keep the FIFO on the CPU path even after a DMA read.
  2. Write `FTRGL`, `DLBA`, `BLKSZ`, `BYTCNT` and `THLD`; write `IDST` = `0x337` and require it to read 0; build the descriptors; barrier. `FTRGL` and `DLBA` are written per read because tuning recovery resets the controller and the vendor rewrites both after every reset (K:4547).
  3. `GCTRL` |= DMA enable; `IDIE` = 0; `DMAC` = `0x82`; `CARG`; `CMD`.
  4. Complete when `RINT` shows the response, data over and, for CMD18, the automatic CMD12, and then every descriptor's OWN bit reads clear (read only after the `RINT` bits, not on every poll); record whether `IDST` showed receive. Any `RINT` error bit, or `IDST` fatal-bus-error, descriptor-unavailable, card-error or abnormal-summary bit, faults the read; the read deadline bounds every wait.
  5. Re-read `IDST` for late errors and acknowledge only the sampled bits, preserving errors that arrive before write-one-to-clear. Reset IDMAC and wait for reset clear and idle; reset the DMA interface and wait; disable DMA, reset the FIFO and wait. Then `dma_sync` invalidates the buffer and the read is DONE.
- **Differences from the vendor, with reasons.** `IDIE` stays 0: a pending receive interrupt would fail the platform's per-poll interrupt check. The receive bit's latching with `IDIE` = 0 is inferred, not documented, so completion rests on the OWN bits that IDMAC clears as it closes each descriptor. (The write path of the OS image arms the SMHC0 interrupt mask; see [the write service](storage-write.md#real-time).) The `IDST` error bits are checked, which the vendor does not. The vendor's retry path is not reproduced: a fault retains every lease.
- **`dma_sync(context, buffer, bytes, phase)`:** `SUNXI_MMC_DMA_BEFORE` cleans the buffer to the point of coherency, `SUNXI_MMC_DMA_AFTER` invalidates it. The image supplies it, because several images link the SD adapter without the cache maintenance code (`arch/aarch64/device_memory.c`).
- **No CMD13 after a read**, for programmed I/O and DMA alike, except after an open-ended CMD18 that ends at the card's last sector (`read_tail`), whose OUT_OF_RANGE bit would otherwise appear in the next read's response.
- **`THLD`** uses the vendor's mask (`(t & ~0x0fff0004) | (512 << 16) | 1`) for programmed I/O as well.
- **Timestamps:** the core records, from its clock callback, when it wrote the command, when it first saw a nonzero card byte count (`CBCR`, `0x48`), and when it first saw completion, with the number of platform polls between them. Each is a first observation, late by up to one poll.

## Platform (`drivers/storage/a733_mmc_platform*.c`)

- `command()` admits up to `SUNXI_MMC_MAX_DMA_BLOCKS` per read when an area is bound and `SUNXI_MMC_MAX_BLOCKS` otherwise. The platform refuses an area while a storage update is bound, an area that overlaps the 16 KiB result page at `0x42000000`, and a DMA read whose buffer is not 64-byte aligned, below 16 GiB and outside every provider's state.
- On the core's `CMD` write, when `GCTRL` DMA enable reads set, the platform refuses a command with the write bit (the bounded writer has its own rule). It also walks the descriptor chain from `DLBA` (at most 33 descriptors, four uncached fields each). Every buffer must lie in the read's borrowed buffer, the sizes must sum to `BYTCNT`, and the chain must end at the zeroed spare slot. The walk adds about 10-20 us to the fixed cost.
- The adapter admits `FTRGL`, `DLBA` (only the area's address >> 2), `IDST`, `IDIE` (only 0) and `DMAC` (only 0, 1 and `0x82`) writes, `CBCR` reads and `GCTRL` DMA enable only on a platform initialized with an area.
- The per-poll re-checks of clocks, pins and presence run on every poll; moving them to once per request would save about 60 us per request and is a separate change.

## Returning image (`apps/storage-read-returning/`)

- **Times each read from submission** with the 24 MHz counter and polls it to completion with no per-wake budget and no tick sleep. The SD task runs at a higher priority than the PMIC task, so the PMIC task does not run during a read; identification yields to it.
- **Reads through IDMAC** into a 128 KiB write-back buffer aligned to 4 KiB, with the 33 descriptors in the non-cacheable result block at `0x42100000`. The image's mapping check covers both once, at admission: write-back inner-shareable for the buffer, non-cacheable outer-shareable for the area.
- **The plan** has seven classes, 4.5 MiB in all: 64 sequential and 64 random 4 KiB reads, 32 sequential and 32 random 32 KiB reads, 8 sequential and 8 random 128 KiB reads, and 32 single sectors. Sequential classes start at sector 0, so the first 64 4 KiB reads repeat the high-speed reference run's sectors and their CRCs must match. Random reads start at request-size boundaries below sector 4,194,304. The optional `chain-read` plan reads sectors 0 to 127 (64 KiB, sixteen descriptors, one CMD18 with automatic CMD12) and reports sixteen indexed CRCs, one per 4,096-byte chunk, after the buffer has returned to the CPU. A 64 KiB `0xa5` fill before the clean makes missing payload writes observable.
- **CRC-32 with the ARMv8 CRC instructions**, about 2 ms for the plan instead of about 80 ms with the table, outside the measured time.
- **Each read line** gives the total time, the core's three first-observed times, the cache clean and invalidate times, and the poll count. The host tool fits t = F + b x blocks across the sequential classes of each mode (fixed cost F, per-block time b), with and without the recorded clean and invalidate times. It reports per class the rate, the mean command-to-first-byte time and the data time against the bus time.
- **High speed by default.** The input selects profile 2, 3.3 V, four-bit high speed. Profile 3 is explicit and cannot authorize ROM return.
- **Normal close reuses the completed-read proof.** Only after every read is collected, with no borrowed buffer and an idle parked PMIC worker, does the provider stop the card clock and restore SD0 module, bus, reset and PF pin fields. It then releases its rail and clock leases. It never resets a faulted DMA engine and calls that drain. A private EL3 guard verifies the final baseline before delegating to the byte-identical qualified monitor. Armed timer, exception, incomplete or uncertain exits park with ownership retained; failed or uncertain operation retains ownership until whole-board power removal. The card stays powered; another SD operation needs separate authorization and provider admission. See [the return contract](../../apps/storage-read-returning/README.md). The `restored-3v3-chain-read` policy binds only the profile-2 chain-read plan and requires fresh admission for each physical run.

## Qualified figures

| Result | Value | Record |
| --- | --- | --- |
| 16-descriptor, 64 KiB read, 3.3 V, 50 MHz | 3,332 us for the transfer; sixteen chunk CRCs and identity match PIO references | `sd-dma.jsonl` `sd-dma-chain-read` |
| Programmed-I/O reads, 256 requests | 25 us per block against 20.8 us bus time; 417 us per request overhead | `sd-dma.jsonl` `sd-dma-lifetime-a` |
| 160 sequential 128 KiB requests with descriptor DMA | 21.9 MB/s per request, 19.7 MB/s sustained (CPU at 494 MHz) | `storage.jsonl` `1793602100319385552-a0551753` |
| Rate and size predictions, budgets | high speed 20.7-22.8 MB/s at 128 KiB; SDR104 50-60 MB/s | `sd-dma.jsonl` `sd-dma-rate-prediction` |

The host checks (IDMAC model, linked cases, normal-return admission) are in the record `sd-dma-host-checks`.

## Limitations and open work

1. Per-request instead of per-poll re-checks in the platform (about 60 us per request).
2. Sustained throughput on broader extents and cards, and DMA fault recovery, are unqualified.
