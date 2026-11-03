# Multiple-block writes with descriptor DMA in the SD core

Status: **qualified on the board** for bounded writes on the 62.5 GB card at 3.3 V, 50 MHz, four bits (profile 2), through the OS image ([write service](storage-write.md)). The core implements `sunxi_mmc_write_dma` (CMD25 with the automatic CMD12, descriptor DMA, the vendor write threshold, DAT0 busy and a CMD13 confirmation); `checks/sunxi-mmc-write-dma.c` drives it against a strict controller model (`checks/sunxi-mmc-write.py` runs it). The native register filter admits CMD25 only for a verified bounds object in a bounded-writer build. Writes at 1.8 V or 150 MHz are not qualified ([adoption design](sd-uhs-adoption.md)).

The reference is the board kernel `bsp/drivers/mmc/sunxi-mmc.c` and `sunxi-mmc-v5p3x.c` at `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` (cached under `sources/sd-uhs/board-kernel-2ac08e8c…/`), the pinned Linux `drivers/mmc/core/block.c` (`sources/mmc-write/linux-block.c`) and the SD Physical Layer Simplified Specification 9.10.

## Reference sequence for an SD multiple-block write

| Step | Reference behavior | Source |
| --- | --- | --- |
| Command choice | More than one block uses CMD25 (`MMC_WRITE_MULTIPLE_BLOCK`). With CMD23 support (SCR CMD23 bit and a host capability) the block layer adds CMD23; otherwise it terminates with CMD12 | `block.c:1674-1714`, `block.c:2540-2550` |
| Host, CMD25 without CMD23 | The command word carries `SDXC_WRITE` (bit 10), `SDXC_DATA_EXPECT`, `SDXC_WAIT_PRE_OVER` (bit 13) and `SDXC_SEND_AUTO_STOP` (bit 12) with the automatic-stop-complete interrupt; with CMD23 the data-over interrupt replaces it | `sunxi-mmc.c:2638-2700` (`sunxi_mmc_parse_cmd`) |
| Descriptors | The chain is the same for both directions: `CH \| OWN \| DIC` per descriptor, `FD` on the first, `LD` on the last with `DIC` cleared, each next pointer at the following descriptor, a write barrier before the DMA starts | `sunxi-mmc.c:746-822` |
| DMA start | FIFO, DMA interface and IDMAC reset; `DMA_ENABLE` set in `GCTRL`; the receive interrupt enabled only for reads (`IDIE` stays 0 for a write); `DMAC = FIX_BURST \| IDMA_ON` | `sunxi-mmc.c:924-955` |
| Completion | The error bits fail the request; otherwise the response is read, and for a write the host checks card busy (DAT0) and, while busy, holds the request until the card releases DAT0 | `sunxi-mmc.c:1045-1190` (`sunxi_mmc_finalize_request`, which sets `mrq_busy` when `card_busy` reports DAT0 low), `sunxi-mmc.c:1548-1584` (the bottom half that completes the request when the busy ends) |
| Teardown | `IDST` cleared, `IDIE` 0, IDMAC soft reset, `GCTRL` DMA reset, DMA disable, FIFO reset, then wait for the IDMAC to go idle | `sunxi-mmc.c:1170-1180` |
| Hardware busy wait | `sunxi_mmc_hw_wbusy_wait` is set by neither `sunxi-mmc.c` nor `sunxi-mmc-v5p3x.c`, so the A733 path uses the generic busy check on DAT0, not the `BUSY_CLEAR` interrupt | grep of both files |
| Card state after the write | The block layer polls CMD13 until `READY_FOR_DATA` with the card out of the programming state | `block.c:2003-2035` (`mmc_blk_card_busy`) |
| Block count | At most the descriptors times 8 KiB per request; the core's limit is 256 blocks (`SUNXI_MMC_MAX_DMA_BLOCKS`) | `include/sunxi_mmc.h` |

## The core's write path

The core implements the read half of the sequence from the same file (`dma_start`, `dma_closed`, the reset and teardown steps) and the CMD24 programmed-I/O write with its CMD13 confirmation. The DMA write adds:

| Element | Where | Note |
| --- | --- | --- |
| A DMA write protocol, `dma_command()` true for it | `sunxi_mmc.c` | descriptors identical to reads; `IDIE` stays 0; the command word has `WRITE` and `AUTO` for CMD25 with CMD12 as the automatic stop |
| Cache clean before the DMA, none after | adapter `dma_sync` | `SUNXI_MMC_DMA_BEFORE` cleans; the `AFTER` invalidate is harmless |
| Wait for DAT0 and CMD13 after the data | `sunxi_mmc.c` | reuses `P_WRITE_STATUS` polling of the CMD24 flow |
| No 4 KiB snapshot for DMA writes | `sunxi_mmc_write` | the immutable snapshot cannot hold 128 KiB; the DMA write takes an exclusive, cache-clean source for the whole operation and the same authority callback |
| CMD25 in the write-capable build only | `queue()` and the native command filter | the read-only command set is unchanged; ACMD13 needs nothing added (CMD13 and CMD55 are in it) |
| Write threshold | `THLD` at `0x100` | Derived from `sunxi_mmc_thld_ctl_for_sdmmc_v5p3x` (`sunxi-mmc-v5p3x.c:647-701`): the transmit trigger level is 248 words (`SUNXI_DMA_TL_SDMMC_V5P3X`, line 52), so `tdtl` is 992 bytes. A 512-byte write satisfies `blksz <= 0xfff && blksz <= tdtl` at every timing, so the vendor sets `THLD[27:16] = 512` and the write threshold enable (bit 2), then clears the read threshold enable (bit 0) because the request is not a read. The core does the same for every write profile |

The write-side register filter refuses everything the sequence does not use. The strict controller model (`checks/sunxi-mmc-write-dma.c`) is driven through the platform's own callbacks (`checks/a733-mmc-platform-write.py`), because a model driven only through its own callbacks proves the core against the protocol and nothing about the layer that admits the core's accesses. The host check results are in the record `mmc-multiblock-write-host-checks` in [the evidence file](../evidence/mmc-multiblock-write.jsonl).

## Register filter for the bounded writer

The native adapter (`drivers/storage/a733_mmc_platform_native.c`) admits a CMD25 command-register write, and the write threshold register, only in a write-capable build (`A733_MMC_BOUNDED_WRITER`) and only for an SD card, while a verified bounds object (`a733_storage_bounds.h`) is bound to it. The rule for the command word is exact: start, wait for previous data, automatic stop, write, data, CRC, response and index 25, with no other bit, and no clock-update bit; every word with index 25 passes this rule before any other. The block-size register holds 512; the byte count is a whole number of 1 to 256 blocks; and the argument register, read at the moment of the write, starts a range that lies wholly inside one admitted write range. The bounds object keeps a pointer to the frozen first input and walks its ranges in place, so the number of ranges is limited only by the input.

Binding is refused unless the platform that owns the adapter has identified an SD card addressed by sector, because the rule reads the argument as a sector number. The test is made inside `a733_mmc_platform_native_bind_bounds` from the platform's own card record, so no caller can skip it. A read-only build refuses CMD25 and the threshold write even when a verified object is forced into it. The platform's descriptor-chain check (`dma_chain`) admits write-direction chains inside the borrowed source.

## Limitations

- Qualified at 3.3 V and 50 MHz only; the data and CRC-status sampling for writes at SDR104 is not modelled.
- The busy-clear interrupt (`RINTSTS` bit 17, and bit 13 with `THLD.BCIG`) is not used; DAT0 busy is waited with a one-shot timer.
