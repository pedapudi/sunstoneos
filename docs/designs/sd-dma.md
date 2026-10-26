# Design note: larger SD reads with descriptor DMA

Status: **proposed for the performance panel**; nothing here is implemented.
The first board measurement of SD reads, at high speed, shows that the fixed
cost of each request and the read task's tick yield set the rate, while
programmed I/O (PIO) keeps up with the 50 MHz bus. Larger requests through the
controller's descriptor DMA (IDMAC) are needed to approach the bus rate.

## What lifetime A measured

Port 3-3 ran the returning SD image in SD profile 2 (four-bit high speed,
50 MHz) on 2026-10-25 (`board-session-20261025-30/port-3-3/sd-ret-a`). Each of
the 256 reads records the time from the first poll after submission to
completion (`apps/storage-read-returning/main.c:205-220`). The supervised
checker's summary of those times gives 1.40 MB/s for 4 KiB sequential reads,
1.66 MB/s for 4 KiB random reads and 1.16 MB/s for single sectors, which it
splits into 355 µs per block and 85 µs per request.

That split comes from means that two populations dominate:

- **Reads that completed within one poll budget.** The 32 single-sector reads
  took 438–444 µs. The fastest 4 KiB reads took 610–1000 µs (74 of 128
  sequential, 36 of 96 random). From the minima, seven more blocks cost
  (610 − 438) / 7 ≈ 25 µs each, against 20.5 µs for 512 bytes on the bus,
  so PIO keeps up with the bus. The fixed cost of a request is about 413 µs.
- **Reads that outlasted it.** The other 113 reads took 1.25–13.9 ms, and most
  of them exactly 1,890, 3,890, 5,890 or 7,890 µs. The read task polls a
  transfer for at most 1 ms per wake (`main.c:218`) and then sleeps one
  scheduler tick (`main.c:374`), so each further wake adds 2 ms, and a
  transfer that completes during a sleep is seen only at the next wake. These
  times bound when the card finished, not how long it took. In the sequential
  pattern they come in runs of eight consecutive reads (reads 16–31, 48–55,
  72–79, 96–103 and later), that is in whole 32 KiB ranges, which points to
  the card's internal reads rather than the host.

The 413 µs fixed cost of a request has three parts:

- **Platform polls.** In the model of `checks/storage-read-returning.py`, a
  single-sector read takes 17 platform polls and 828 checked register
  accesses. Every poll checks card presence, takes the register lock and
  re-reads the clock, fabric and pin state (`drivers/storage/a733_mmc_platform.c:102-150`,
  `controller()` at `:439-458`), and every access checks the task context
  (`a733_mmc_platform_native.c:10-33`). On the board this is likely 150–250 µs.
- **Two commands per read.** After each CMD17 or CMD18 the driver confirms
  the card's state with CMD13 (`drivers/storage/sunxi_mmc.c:841-842`).
- **The card's access time** before the first data block, typically
  100–300 µs for a read.

## What the vendor driver does

The board kernel's driver moves data with IDMAC and chained descriptors
(`bsp/drivers/mmc/sunxi-mmc.c:746-806`, started at `:924-960` with `DMAC`
fixed burst and IDMAC on). It sends one CMD18 with automatic CMD12 for each
multi-block request, as large as the block layer's request, and no CMD13
after a read. It programs the FIFO thresholds in `FTRGL` to `0x20070008`
(`sunxi-mmc.c:719`): bursts of 8 words, receive threshold 7, transmit 8.
SunstoneOS today reads by PIO at most eight sectors per request
(`SUNXI_MMC_MAX_BLOCKS`), leaves `FTRGL` at its reset value and confirms every
read with CMD13.

## Proposed change

1. **Measure without the yield.** In the returning image, poll a transfer
   until it completes or its deadline passes, instead of for 1 ms per wake;
   the PMIC task has no work during reads. This changes only the image's
   read loop and gives the card's true per-read times. It is an image change,
   so it needs a new set; the frozen set 28 measures lifetime B with the same
   2 ms quantization as lifetime A.
2. **IDMAC with large CMD18 reads**, as below: descriptors from the
   device-memory pool, `FTRGL` at the vendor's `0x20070008`, `DLBA`, `DMAC`
   and `IDIE` admitted only in a DMA profile, completion by polling `IDST`.
3. **No CMD13 after a successful read**, which the vendor driver does not
   send; a read's own R1 response and data CRC report its errors.

| Request size | High speed, 50 MHz | SDR104, 150 MHz |
| --- | --- | --- |
| 4 KiB, today (from the fast reads) | 6.7 MB/s | not measured |
| 32 KiB, IDMAC | 1.7 ms, 19 MB/s | 0.86 ms, 38 MB/s |
| 128 KiB, IDMAC | 5.7 ms, 23 MB/s | 2.2 ms, 60 MB/s |

Each figure is 413 µs of fixed cost plus the bus time, 20.5 µs per block at
50 MHz and 6.9 µs at 150 MHz; removing CMD13 and the per-poll checks lowers
the fixed cost further. The card's own rate may be lower, and it decides the
sequential runs that took more than 1 ms in lifetime A. PIO at 150 MHz would
need each 512-byte block drained in 6.9 µs, 128 device reads, which the
present 25 µs per block at 50 MHz does not suggest; IDMAC is therefore needed
for SDR104 to gain over high speed.

The sections below give the eight-block limit change and the IDMAC
interface, written before the measurement.

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
