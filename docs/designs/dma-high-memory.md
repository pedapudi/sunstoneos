# Design: camera and NVMe rules for memory above 4 GiB

Status: **host-tested**. No board run has exercised these rules, and no step is qualified on hardware. The per-master rules are in [docs/dma-address-width.md](../dma-address-width.md). Section numbers refer to [docs/performance-architecture.md](../performance-architecture.md).

Three consumers check their address range once, when they admit their memory.

- **Native camera** ([a733_camera_native.c](../../drivers/camera/a733_camera_native.c)). The camera reaches memory through the system IOMMU, so the device sees an I/O virtual address (IOVA) that the IOMMU translates to a physical address. The camera uses fixed IOVAs: parameters at `A733_CAMERA_NATIVE_IOVA` (`0x1000_0000`, in `include/a733_camera_native.h`), the Y plane at that IOVA plus 8,192, and the UV plane at that IOVA plus 319,488. The 663,552-byte allocation may lie anywhere below 2^34 and its physical layout is unchanged. Span `physical` fields, `table_physical` and the admission compares in `mmu_admit`, `route_admit` and `dma_admit` use the lease's physical address or the fixed IOVA; CPU accesses use the lease's CPU address. The range check is `a733_extent_within(0, A733_IOMMU_PHYSICAL_LIMIT, physical, bytes)` and runs before the plane fill. A static assertion keeps the three spans (630,784 bytes) inside one 4 MiB IOMMU window and below `A733_IOMMU_IOVA_LIMIT`.
- **NVMe** ([a733_pcie_nvme.c](../../drivers/pci/a733_pcie_nvme.c)). `a733_pcie_nvme_init` tests `a733_extent_within(0, DMA_ADDRESS_32BIT_LIMIT, physical, A733_PCIE_NVME_POOL_BYTES)` (six 4 KiB pages, `include/a733_pcie_nvme.h`) after the identity test that follows `device_memory_acquire`. On refusal it releases the lease.
- **Literals.** The capture driver's IOVA compare uses `A733_IOMMU_IOVA_LIMIT` and the xHCI compare uses `DMA_ADDRESS_32BIT_LIMIT`; both are textual, so the object code is unchanged.

**Data flow and ownership.** The camera allocation moves from CPU-owned to device-owned before preparation and stays reserved. The retained readout copies each image once, by `memcpy`, into the application's lease. NVMe queue and data pages move to device-owned at controller enable and stay retained after stop. Copies per byte are one for both drivers; no bounce buffer exists.

**Hot paths.** The checks run only at admission. Camera rearm and stream slot (up to 30 frames per second) write the same register sequence; only the values of the DMA address registers (`0x220`, `0x228`) and the route's parameter address are IOVAs. NVMe commands (one per 4 KiB read) are unchanged, and after admission the high halves of the queue base registers and PRP entry 1 are zero. NVMe completes by polling the phase-tagged queue entry, because the image has no qualified PCIe interrupt delivery ([pcie-nvme.md](../pcie-nvme.md)), the condition in section 1.3 that permits polling. The camera keeps its bounded status interval of half a frame period. No shared state is added: the IOVA is a compile-time constant, each driver's admission runs on its owning task, and the camera's IOMMU tables are immutable after publication. The camera allocation stays Normal write-back with the existing maintenance (table arena and parameters cleaned at publication, planes cleaned and invalidated before DMA and invalidated after); NVMe pages stay in the non-cacheable pool. No MMIO sequence is added or reordered (section 1.2).

**Failure classes.** Both refusals are class 1 in section 1.8: they come before any device access, the owner retains nothing and the session stays usable. The NVMe refusal releases its lease. The camera refusal leaves the extent device-owned and fails the constructor before the fill. DRAM ends at `0x3_4000_0000`, below 2^34, and the IOMMU builder refuses a larger address before writing the table arena.

## Register encodings this design relies on

| Encoding | Source | Status |
| --- | --- | --- |
| Camera DMA plane address registers hold the device address shifted right by 2 | Board kernel `2ac08e8c`, `bsp/drivers/vin/vin-video/dma140/dma140_reg.c` ([camera-dma.md](../camera-dma.md)) | Source-checked; no capture has run on the board |
| Camera route parameter address holds the device address shifted right by 2, below 4 GiB | Board kernel `2ac08e8c`, `bsp/drivers/vin` ([camera-route.md](../camera-route.md)) | Source-checked; unproven on hardware |
| IOMMU leaf and directory bits 9:8 hold physical bits 33:32; the directory base holds the physical address shifted right by 14 | Board kernel `2ac08e8c`, `sunxi-iommu-pgtable.c:25–68`, `sunxi-iommu-v2.c:634–769` ([iommu.md](../iommu.md)) | Unproven above 4 GiB |
| NVMe ASQ and ACQ at BAR0 `0x28`–`0x34` and PRP entry 1 in dwords 6 and 7, each written as two 32-bit halves | U-Boot `b791be84`, `drivers/nvme/nvme.c` and `nvme.h` ([source.json](../../drivers/pci/source.json)) | Host-modeled; no NVMe device has run |
| xHCI `HCCPARAMS1.AC64`, bit 0 of capability offset `0x10` | xHCI 1.2, section 5.3.6 | The A733 value is unread |
| PCIe root-port inbound address width | Unpublished: no pinned source or the A733 User Manual V1.00 states it ([dma-address-width.md](../dma-address-width.md)) | Unknown; hence the NVMe refusal |

## Qualified figures

Host models and freestanding object builds only (record `dma-high-memory-host-objects` in [the evidence file](../evidence/dma-high-memory.jsonl)).

| Metric | Figure |
| --- | --- |
| DRAM below 4 GiB that width rules require | 184,320 bytes plus one descriptor page per DMA0 job table (xHCI 131,072, NVMe 24,576, endpoint test 28,672) |
| Master paths that can reach a wrong address with no error | zero |
| IOMMU 4 MiB windows used by the camera; leaf words | one at any placement; 154 |
| Modeled register writes of one capture | 2,348, same addresses and order |
| `a733_pcie_nvme_init` growth | 12 bytes (804 to 816), run once per controller initialization |
| Native camera `initialize` growth | 32 bytes (2,020 to 2,052), run once per camera start |
| `.text` of the camera, NVMe and board images | 205,232; 548,688; 498,736 bytes |

Host checks that pass: `checks/a733-camera-native.py` (cases 0 to 20, including an allocation at physical `0x1_8000_0000` and one across 2^34), `checks/a733-pcie-trace.py`, `checks/dma-address-width.c`, `checks/a733-camera-capture.py`, `checks/a733-xhci-detach.py` and `checks/camera-supervised.py`.

## Limitations

- The NVMe refusal stays until the root port's inbound width is known.
- The PCIe endpoint test's compare refuses a buffer that ends at 4 GiB, one case stricter than the named limit; no consumer needs it changed.
- The NVMe byte-by-byte data copy (section 1.6) belongs to the NVMe data path, which is unchanged.
- Mapping DRAM above 4 GiB and placing a pool there is the memory-architecture work ([memory-architecture.md](memory-architecture.md)); every allocation still lands below 4 GiB.
- The other IOMMU consumers that pass a CPU pointer as a physical address (`apps/h264`, `apps/g2d`, `apps/deinterlace`, `apps/jpeg`, `apps/h264-encode`) belong to other packages.

## Open work: board evidence

1. One camera capture below 4 GiB at the fixed IOVA. A correct image with intact guard fill proves that the CSI master translates through the IOMMU; a camera module must be connected.
2. One camera capture above 4 GiB, after a memory class maps that range, following the guard matrix in [dma-address-width.md](../dma-address-width.md): bit 32 only, then bit 33 only, then both.
3. The PCIe inbound width from the same matrix with an NVMe read, before the refusal can be lifted.
4. A read of `HCCPARAMS1.AC64`, which decides whether xHCI still needs its 131,072 bytes below 4 GiB.

The predictions are the `dma-high-memory-board-*` records in [the evidence file](../evidence/dma-high-memory.jsonl).
