# Multiple-block writes with descriptor DMA in the SD core

Status: the vendor-sequence comparison is complete; the core change and its host model are not implemented. The read
side of the same core, and the ACMD13 SD Status read, are implemented and host-checked (`checks/sunxi-mmc.py`).

The reference is the board kernel `bsp/drivers/mmc/sunxi-mmc.c` and `sunxi-mmc-v5p3x.c` at
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` (cached under `sources/sd-uhs/board-kernel-2ac08e8c…/`), the pinned Linux
`drivers/mmc/core/block.c` (`sources/mmc-write/linux-block.c`) and the SD Physical Layer Simplified Specification 9.10.

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
| Block count | At most the descriptors times 8 KiB per request; the core's present limit is 256 blocks (`SUNXI_MMC_MAX_DMA_BLOCKS`) | `include/sunxi_mmc.h` |

## Delta from the present core

The present core already implements the read half of this sequence from the same file (`dma_start`, `dma_closed`,
the reset and teardown steps) and the CMD24 PIO write with its CMD13 confirmation.

| Change | Where | Note |
| --- | --- | --- |
| A DMA write protocol, `dma_command()` true for it | `sunxi_mmc.c` | descriptors identical; `IDIE` stays 0; the command word gains `WRITE`, `AUTO` for CMD25 with CMD12 as the automatic stop |
| Cache clean before the DMA, none after | adapter `dma_sync` | `SUNXI_MMC_DMA_BEFORE` already cleans; the `AFTER` invalidate is harmless |
| Wait for DAT0 and CMD13 after the data | `sunxi_mmc.c` | reuses `P_WRITE_STATUS` polling of the present CMD24 flow |
| No 4 KiB snapshot for DMA writes | `sunxi_mmc_write` | the immutable snapshot cannot hold 128 KiB; the DMA write takes an exclusive, cache-clean source for the whole operation and the same authority callback |
| CMD25 in the write-capable build only | `queue()` and the native command filter | the read-only command set is unchanged; ACMD13 needs nothing added (CMD13 and CMD55 are in it) |
| Write threshold | `0x100` | the core clears bit 2 (write threshold enable) for SDR104 reads; whether a write sets it and with what size is derived from the vendor `thld_ctl` before the change, not assumed here |

## Host checks required before any board run

- The strict controller model refuses any register access the sequence does not use, models the IDMAC consuming
  descriptors in order, `OWN` clearing, the data-end and automatic-stop interrupts, and DAT0 busy that outlasts the data.
- The real core is driven through 1, 2, 255 and 256 blocks, a DAT0 busy of several polls, a data CRC error, a
  card error in the CMD25 or CMD12 response, and a timeout; each ends in the terminal state the CMD24 path ends in.
- `checks/sd-read-only.py` passes with CMD24 and CMD25 absent from the read-only set; `--elf` passes on the
  linked large-region image.
- A boundary check proves no byte outside the requested range is written (the model compares the whole card).
