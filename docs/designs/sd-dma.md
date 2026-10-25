# Design note: larger SD reads, then descriptor DMA

Status: **proposed**; nothing here is implemented or built. It follows
[the UHS-I design](sd-uhs.md), whose cost model predicts that programmed I/O
(PIO) bounds SDR104 reads below the 150 MHz bus rate: 4 KiB requests reach
8.5–15.6 MB/s against a bus ceiling of 75 MB/s. The board run of that design
measures the per-request and per-block costs that this note's predictions
use; its result decides whether step 2 is needed.

## Step 1: raise the eight-block read limit

A read request is at most eight sectors (`SUNXI_MMC_MAX_BLOCKS` in
`include/sunxi_mmc.h`, `STORAGE_READ_MAX_BLOCKS` in
`apps/storage-read/storage_read_service.h`). Each request pays a fixed cost of
50–110 µs in commands and platform polls, plus the card's first-block latency
(assumed 100 µs sequential, 300 µs random), so at 4 KiB the fixed cost is
40–60% of the request time.

The change separates the read limit from the write limit. Writes keep eight
sectors, because the write snapshot (`write_payload`, 4,096 bytes in every
`struct sunxi_mmc`) and the one-second programming deadline size them. Reads
rise to 64 sectors (32 KiB). One CMD18 with automatic CMD12 covers the
request; `SMHC_BCNT` is 32 bits wide, so the controller imposes no lower limit.

Costs and interfaces:

- The read service's borrowed buffer, both client slots' copies and the USB
  response buffer grow from 4 KiB to 32 KiB each, about 112 KiB of static
  memory in that image.
- The request wire (`blocks` up to 8) and the observer's 4,096-byte maximum
  change, so records become version 5.
- A 32 KiB PIO read takes 0.9–2.2 ms, longer than the 1 ms poll budget, so the
  budget rises to 3 ms per wake for reads of more than eight sectors, or the
  measurement keeps the first-poll-to-completion time across wakes.

| Prediction, PIO, 32 KiB requests | High speed, 50 MHz | SDR104, 150 MHz |
| --- | --- | --- |
| Sequential | 13.7–22 MB/s | 13.7–31 MB/s |
| Random | 12.7–19.5 MB/s | 12.7–26 MB/s |

The lower bound is the slow PIO case, 34 µs per block, which equals the
high-speed figure; then step 2 is required.

## Step 2: the vendor's chained-descriptor DMA

The board kernel's SMHC moves data with its internal DMA controller (IDMAC)
through a chain of descriptors (`bsp/drivers/mmc/sunxi-mmc.c` at `2ac08e8c`):

- **Descriptors** (`sunxi-mmc.c:746–806`; layout `struct sunxi_idma_des`,
  `sunxi-mmc.h:335`). Each holds a configuration word with OWN (bit 31),
  chain (bit 4) and disable-interrupt-on-completion (bit 1); the first adds
  first-descriptor (bit 3), the last adds last-descriptor (bit 2) and clears
  bit 1. It also holds the buffer size, at most `1 << idma_des_size_bits`
  bytes (12, 4 KiB, for v5p3x: `sunxi-mmc-v5p3x.c:55`), the buffer address and
  the next descriptor's address. Controller versions 5.3 and later store
  addresses shifted right by two (`des_addr_shift`,
  `sunxi-mmc-v5p3x.c:914–915`), which reaches 16 GiB.
- **Start** (`sunxi-mmc.c:924–960`): reset FIFO, DMA interface and DMA
  controller; set `GCTRL` DMA enable; enable the receive interrupt in `IDIE`;
  write `DMAC` with fixed burst (bit 1) and IDMAC on (bit 7). The descriptor
  list base goes to `DLBA` once (`sunxi-mmc.c:727`), and `FTRGL` holds the
  FIFO thresholds `(2 << 28) | (7 << 16) | 248` (`sunxi-mmc-v5p3x.c:51`).

SunstoneOS would add these, each a reviewed change:

- **Buffers.** Descriptors and data come from the device-memory pool, with
  ownership handed to the controller for the transfer. A read writes straight
  into the client slot's buffer, which removes the present borrowed-buffer
  copy. Cache maintenance invalidates the exact destination bytes by virtual
  address before and after the transfer.
- **Admission.** The platform today requires DMA disabled and the native
  filter refuses `DLBA`, `DMAC`, `IDIE` and `FTRGL` writes. A DMA profile
  admits them only with a pool lease, an address check against the 16 GiB
  reach and the descriptor list inside the lease.
- **Completion.** IRQ 193 stays masked until its delivery is qualified;
  completion is observed by polling `IDST` within the read budget. Every
  IDMAC error bit (fatal bus error, destination invalid, card error summary,
  abnormal summary) is terminal and retains the buffer and leases.

| Prediction, DMA, SDR104 at 150 MHz | 32 KiB requests | 128 KiB requests |
| --- | --- | --- |
| Sequential | 50–55 MB/s | 66–68 MB/s |
| Random | 38–41 MB/s | 60–62 MB/s |

The bus moves a 512-byte block in 6.9 µs, so 64 blocks take 442 µs; the
fixed cost (50–110 µs) and the card latency make up the rest. High speed
stays at its bus limit, about 22 MB/s at 32 KiB. The card's own read rate may
be lower than these figures.

## Measurement

The supervised launcher's throughput plan gains 32 KiB and, with step 2,
128 KiB request classes, read from the same sectors, with digests compared
against the 4 KiB reads. Each step is accepted only if both patterns are at
least as fast as before.
