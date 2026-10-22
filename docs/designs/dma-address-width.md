# Design: DMA address-width rules and the path above 4 GiB

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence listed at the end exists. Readers are assumed to know
non-coherent DMA, AArch64 cache maintenance and the A733 system IOMMU described
in [docs/iommu.md](../iommu.md).

DRAM occupies physical `0x4000_0000`–`0x3_4000_0000`: 3 GiB below 4 GiB and
9 GiB above it. Every device memory class today, including `dma32`, lies below
2 GiB, so no DMA buffer is above 4 GiB yet. Width limits exist, but each driver
writes its own literal, and one path has no limit at all: the NVMe driver
writes queue and physical-region-page (PRP) addresses as two 32-bit halves through a PCIe root port
whose inbound address width is unknown. A buffer above 4 GiB on that path would
be truncated without an error. This design gives every DMA master one rule:
**direct** when the device reaches the physical range, **IOMMU** when the
master is translated by the system IOMMU, otherwise **refuse**.

## One constant table and one admission function

A new header, `include/dma_address.h`, holds a constant table indexed by
master, with three fields per row: the exclusive physical limit for direct
access, the IOMMU master ID or none, and the limit for descriptor or control
structures where it differs. `dma_address_route(master, physical, bytes)`
returns DIRECT, IOMMU or REFUSE. It rejects zero length and overflow, and it
compares `physical + bytes` against the row limit. It never narrows an
address, copies data or touches hardware. The rows below come from pinned
sources: the board kernel at `2ac08e8c`, its device tree `sun60iw2p1.dtsi`
and the A733 manual V1.00, page 406. The implementation document,
`docs/dma-address-width.md`, records the line anchors for each row.

| Master | Driver mode | Width source | Rule for DRAM above 4 GiB |
| --- | --- | --- | --- |
| DMA0 data (I2S, SPI, UART, memory copy) | descriptors | `dma-v106`: 35-bit data | Direct |
| DMA0 descriptors | descriptors | Descriptor-high register at `0x3c` not programmed; end marker `0xfffff800` | Refuse; descriptors stay in `dma32` |
| DMA1 | not used | same controller | Same as DMA0 once used |
| Crypto engine | descriptors | 40-bit task and data pointers | Direct |
| GPU | own MMU | 36-bit physical, feature database | Direct through the GPU MMU |
| NPU | own MMU | 40-bit physical, 32-bit device VA | Direct through the NPU MMU |
| Display engine 0, G2D, deinterlacer | IOMMU masters 8, 10, 9 | IOMMU leaf holds physical bits 33:32 | IOMMU, fixed IOVAs below 4 GiB |
| Video decoders, encoder | IOMMU masters 6, 7, 3 | same | IOMMU |
| CSI/VIPP camera, ISP | IOMMU masters 1, 2 | same; driver maps IOVA equal to physical | Refuse until the camera uses separate IOVAs |
| xHCI (USB3) | descriptors, IOMMU bypass | 64-bit capability bit `HCCPARAMS1.AC64`; IOMMU bank address space 34-bit | Direct if AC64 is 1, else refuse |
| USB0 (MUSB) | programmed I/O | no DMA | Not a DMA master |
| EHCI/OHCI | disabled | 32-bit mask in the board kernel | Refuse |
| SD/MMC, SDIO Wi-Fi | FIFO programmed I/O | no DMA | Not a DMA master |
| PCIe endpoint DMA (NVMe) | root port, no IOMMU master | inbound width unpublished; MSI target 32-bit | Refuse |

## Data flow and ownership

The rule runs once per buffer, at admission, inside the owner that already
admits the buffer. Owners keep their current lease calls and their DMA ownership
states. For DMA0, `dma_memory_init` registers each window with its role, and
`dma_memory_accept` already requires every mapping to lie inside one
registered window. The width check therefore moves into window registration and
is not repeated per submit. For IOMMU masters, `a733_iommu` span validation
takes its physical limit from the table. The NVMe and endpoint-test pools call
`dma_address_route` at initialization. The crypto, GPU and NPU drivers keep
their validators, which already enforce these widths; their rows serve the host
check. Refusal happens before any device access and before any table or
descriptor byte is written.

No bounce buffer is provided. Each narrow master takes its buffers from
`dma32`, so no copy is needed. A bounce path would add one copy per byte and a
second owned buffer, and no consumer needs one: no current buffer is above
4 GiB, and the memory-architecture design does not map that range.

## Interrupts, polling, memory placement and cache policy

The rule has no runtime events. DMA0 completion keeps its interrupt when bound.
IOMMU installation stays a single bounded poll at initialization, which is not a
hot path. Buffers above 4 GiB, once a class maps them, use the same Normal
write-back inner-shareable attributes as `dma32` and the same cache maintenance
by virtual address over the exact bytes. IOMMU tables can lie anywhere in DRAM:
the directory base register holds `physical >> 14`. The tables are cleaned to
the point of coherency once, before the first base write. The IOMMU v2 format
has only 4 KiB leaf pages, so large IOMMU buffers keep 4 KiB device pages. No
barrier is added or changed.

## Synchronization across cores

The table is `const` data in read-only memory and needs no lock or barrier.
Each owner already serializes its admission under its own service lock.
An admission result stays valid until the page-table owner publishes a new
mapping generation; IOMMU tables are never remapped while live. This design
adds no shared mutable state.

## Reuse and removal

- The 35-bit DMA data limit is written three times, in `a733_dma.c`,
  `dma_memory.c` and `a733_dma_controller.c`. The `0xfffff800` descriptor limit
  is written twice. Each becomes one table constant.
- The IOMMU `LIMIT` of 2^34 becomes the table's IOMMU row. The IOMMU path
  above 4 GiB already exists: `checks/a733-iommu.c` maps a camera plane at
  physical `0x1_8000_0000` and walks the published tables.
- The global refusal of any class ending above `0x1_0000_0000`, planned in the
  memory-architecture host check, can be replaced by per-master routing once a
  high class exists. Until then it stays, and both checks refuse the same
  buffers.

## Budgets and measurement

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Width checks per DMA submit, frame, inference or job | DMA0: two compares per endpoint in `a733_dma_build` (counted) | No added work; zero `dma_address_route` calls after admission | Call counter in `checks/dma-service.c` over 1,000 submits |
| Instructions in `dma_address_route` | none | At most 24 AArch64 instructions, no loop, no barrier | `llvm-objdump` of the freestanding AArch64 object |
| Copies per byte for a buffer above 4 GiB | no such buffers | Zero | Host check: no bounce path; refusal writes nothing |
| IOMMU leaf words per 307,200-byte plane | 75 for planes at physical `0xffff_f000` and `0x1_8000_0000` (`checks/a733-iommu.c`) | 75; the table limit changes no encoding | The same check, rerun unchanged |
| DRAM below 4 GiB that DMA masters require | about 140 KiB: xHCI 128 KiB, NVMe 4 KiB, camera parameter page 4 KiB, DMA0 chains of up to 4 KiB (counted) | At most 1 MiB, all from `dma32` | Sum over the image's admission list in the same check |
| Master paths that can truncate an address without an error | One: NVMe through the PCIe root port (counted) | Zero | Every row probed at `0xffff_f000`, `0x1_0000_0000`, `0x3_3fff_f000` and `0x4_0000_0000` |

## Failure classes

- **Route refuses:** refused before any device access. The owner returns the
  error, and the session stays usable. Nothing is retained.
- **IOMMU span refused:** the builder validates before it writes the arena,
  so this failure is also refused before any device access.
- **IOMMU fault on a high mapping:** the existing rule in
  [docs/iommu.md](../iommu.md) applies. The state is unknown, so the tables,
  clocks and global token are retained.

## Consumer changes outside this package

- `drivers/pci/a733_pcie_nvme.c` and `drivers/pci/a733_pcie_epf_test.c`: call
  `dma_address_route` for the pool extent at initialization.
- Memory-architecture owner: keep `dma32` below 4 GiB. When a class above
  4 GiB is added, route each consumer of that class through the table.
- Camera owner: separate IOVAs from physical addresses before camera buffers
  move above 4 GiB.

## Left out of this design

- **Descriptors above 4 GiB for DMA0.** The meaning of the descriptor-high
  register is unconfirmed, and a chain costs at most 4 KiB of `dma32`.
- **Bounce buffers.** No consumer needs one.
- **Mapping DRAM above 4 GiB.** The memory-architecture design owns the map,
  and no consumer needs that range yet.
- **Per-submit translations in `dma_memory_accept`.** This is a separate
  budget row with its own before-and-after count.
- **IOMMU coverage beyond four 4 MiB windows, and fault interrupts.** Neither
  depends on address width.

## Board evidence that would confirm the rules

The evidence needs a class mapped above 4 GiB. For each direct or IOMMU row,
the master writes a pattern to a buffer at or above `0x1_0000_0000` while a
guard pattern sits at `physical − 4 GiB`, the address that 32-bit truncation
would reach. Pass means the payload is correct and the guard is unchanged.
The xHCI row needs a read of `HCCPARAMS1.AC64`. Lifting the PCIe refusal needs
the same guarded test with one NVMe read into a buffer above 4 GiB.
