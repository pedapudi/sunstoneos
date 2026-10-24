# Design: camera and NVMe rules for memory above 4 GiB

Status: **implemented**, with host checks; the
[measurement record](dma-high-memory-measurements.md) compares the change with
its parent against the budgets below. The per-master rules are recorded in
[docs/dma-address-width.md](../dma-address-width.md). Section
numbers refer to [docs/performance-architecture.md](../performance-architecture.md).
No board was used, and no step below is qualified on hardware.

## The change

Three consumers change, and each one checks its address range once, when it
admits its memory:

- **Native camera** ([a733_camera_native.c](../../drivers/camera/a733_camera_native.c)).
  The camera reaches memory through the system IOMMU, so the device sees an
  I/O virtual address (IOVA) that the IOMMU translates to a physical address.
  Today each IOVA equals the physical address, which forces the 663,552-byte
  camera allocation below 4 GiB. The camera will use fixed IOVAs:
  - parameters at `A733_CAMERA_NATIVE_IOVA` (`0x1000_0000`, a new constant in
    `include/a733_camera_native.h`);
  - the Y plane at that IOVA plus 8,192;
  - the UV plane at that IOVA plus 319,488.

  The physical layout inside the allocation is unchanged.
  Span `physical` fields, `table_physical` and the admission compares in
  `mmu_admit`, `route_admit` and `dma_admit` use the lease's physical address
  or the fixed IOVA. CPU accesses use the lease's CPU address. The
  constructor then needs no identity mapping (CPU address equal to physical
  address), and its range check becomes
  `a733_extent_within(0, A733_IOMMU_PHYSICAL_LIMIT, physical, bytes)` (below
  2^34). That check runs before the plane fill, as the 4 GiB check does today.
  A static assertion keeps the three spans (630,784 bytes) inside one 4 MiB
  IOMMU window and below `A733_IOMMU_IOVA_LIMIT`.
- **NVMe** ([a733_pcie_nvme.c](../../drivers/pci/a733_pcie_nvme.c)).
  `a733_pcie_nvme_init` adds
  `a733_extent_within(0, DMA_ADDRESS_32BIT_LIMIT, physical, A733_PCIE_NVME_POOL_BYTES)` to the
  existing identity test that follows `device_memory_acquire`. On refusal it
  releases the lease, as it already does when the identity test fails. A new
  `A733_PCIE_NVME_POOL_BYTES` (six 4 KiB pages) in
  `include/a733_pcie_nvme.h` replaces the local copy in
  [checks/dma-address-width.c](../../checks/dma-address-width.c).
- **Literals.** The capture driver's 4 GiB compare
  ([a733_camera_capture.c:155](../../drivers/camera/a733_camera_capture.c))
  uses `A733_IOMMU_IOVA_LIMIT`, and the xHCI compare
  ([a733_xhci.c:636](../../drivers/usb/a733_xhci.c)) uses
  `DMA_ADDRESS_32BIT_LIMIT`. Both are textual replacements, so the object
  code is unchanged.

**Data flow and ownership.** No data path changes. The camera allocation moves
from CPU-owned to device-owned before preparation and stays reserved, as
today. The retained readout copies each image once, by `memcpy`, into the
application's lease. NVMe queue and data pages move to device-owned at
controller enable and stay retained after stop. Copies per byte stay at one
for both drivers, and no bounce buffer is added.

## Hot paths and their cost

The new checks run only at admission. The hot paths and their rates:

- **Camera rearm and stream slot**, once per frame at up to 30 frames per
  second. The driver writes the same register sequence. Only the values of
  the DMA address registers (`0x220`, `0x228`) and the route's parameter
  address change, from physical to IOVA. `dma_admit` compares against a
  constant instead of a sum computed from the lease, so it adds no
  instruction.
- **NVMe command**, once per 4 KiB read. `build`, `submit`, `complete` and the
  doorbell write rule are unchanged. After admission the high halves of the
  queue base registers and of PRP entry 1 are always zero.

**Interrupts and polling.** The completion mechanisms stay as they are. NVMe
completes by polling the phase-tagged queue entry, because the image has no
qualified PCIe interrupt delivery ([pcie-nvme.md](../pcie-nvme.md)). That is
the condition in section 1.3 that permits polling. The camera keeps its
bounded status interval of half a frame period.

**Synchronization across cores.** No new shared state is added. The IOVA is a
compile-time constant. Each driver's admission runs on its single owning task.
The camera's IOMMU tables are immutable after publication, as today.

**Memory placement and cache policy.** The camera allocation stays in
Normal write-back memory, with the existing maintenance: the table arena and
parameters are cleaned at publication, the planes are cleaned and
invalidated before DMA and invalidated after it. The NVMe pages stay in the
non-cacheable pool and need no maintenance. A buffer above 4 GiB uses the
same attributes and the same maintenance as one below 4 GiB.

**Barriers.** No MMIO sequence is added or reordered, and the barriers stay
as they are (section 1.2).

## Register encodings this design relies on

| Encoding | Source | Status |
| --- | --- | --- |
| Camera DMA plane address registers hold the device address shifted right by 2 | Board kernel `2ac08e8c`, `bsp/drivers/vin/vin-video/dma140/dma140_reg.c` ([camera-dma.md](../camera-dma.md)) | Source-checked; no capture has run on the board |
| Camera route parameter address holds the device address shifted right by 2, below 4 GiB | Board kernel `2ac08e8c`, `bsp/drivers/vin` ([camera-route.md](../camera-route.md)) | Source-checked; unproven on hardware |
| IOMMU leaf and directory bits 9:8 hold physical bits 33:32; the directory base holds the physical address shifted right by 14 | Board kernel `2ac08e8c`, `sunxi-iommu-pgtable.c:25–68`, `sunxi-iommu-v2.c:634–769` ([iommu.md](../iommu.md)) | **Unproven** above 4 GiB |
| NVMe ASQ and ACQ at BAR0 `0x28`–`0x34` and PRP entry 1 in dwords 6 and 7, each written as two 32-bit halves | U-Boot `b791be84`, `drivers/nvme/nvme.c` and `nvme.h` ([source.json](../../drivers/pci/source.json)) | Host-modeled; no NVMe device has run |
| xHCI `HCCPARAMS1.AC64`, bit 0 of capability offset `0x10` | xHCI 1.2, section 5.3.6 | The A733 value is **unread** |
| PCIe root-port inbound address width | Unpublished: no pinned source or the A733 User Manual V1.00 states it ([dma-address-width.md](../dma-address-width.md)) | **Unknown**; hence the NVMe refusal |

## Budgets and measurement

Baselines come from commit `745a6ec`; later integrated commits with the same
sources give the same figures. Object sizes come from a freestanding
compile of each file with the image's C flags (`clang-21 --target=aarch64-none-elf
-O2 -march=armv8-a -mstrict-align -mgeneral-regs-only -ffreestanding
-fno-builtin`). The [measurement record](dma-high-memory-measurements.md)
names its build directories.

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| DRAM below 4 GiB that width rules require | 847,872 bytes (*measured*) | 184,320 bytes, plus one descriptor page per DMA0 job table | `checks/dma-address-width.c` sum, using the exported NVMe size |
| Master paths that can reach a wrong address with no error | One: NVMe (*counted*) | Zero | New NVMe model case: a pool that crosses 4 GiB is refused with zero configuration and BAR accesses and an unchanged pool; a pool that ends at 4 GiB is admitted with zero high halves |
| IOMMU windows (4 MiB second-level tables) used by the camera | One or two, depending on placement (*counted*) | One at any placement | Nonzero directory words in the arena, `checks/a733-camera-native.c` |
| IOMMU leaf words for the camera | 154 (*counted*) | 154 | Same check |
| Camera MMIO writes per capture and per rearm | Not counted | Same count and offsets as the baseline; only the three address values differ | Write trace added to the native camera model, compared before and after |
| Copies per byte (camera readout, NVMe read) | One and one | Unchanged | Code reading; `memcpy` and byte-loop calls counted in the models |
| Object size: capture `initialize`, xHCI `capabilities` | 1,920 and 532 bytes | Identical instructions | `llvm-objdump-21 -d` comparison |
| Object size: `a733_pcie_nvme_init` | 804 bytes | At most 16 bytes larger; `poll`, `read`, `copy`, `write_allowed` and `build` identical. Measured growth is 12 bytes: three instructions that compare the pool address with a constant and branch to the refusal, run once per controller initialization | Same |
| Object size: native camera `initialize`, `route_admit`, `dma_admit` | 2,020, 188 and 288 bytes | `initialize` at most 32 bytes larger; the two admission callbacks no larger. Measured growth is 32 bytes: eight instructions, run once per camera start, that build each span's IOVA and physical address separately, where one value served both before | Same, built with `SUNSTONE_CAMERA_MODEL` |
| `.text` of the camera, NVMe and board images | 205,208, 548,676 and 498,736 bytes | No more than the function growth in the two rows above: 24 bytes for the camera image (32 − 4 − 4), 12 for the NVMe image, none for the board image | `llvm-size-21 -A` of each image |

The camera model also gains a case with the allocation's physical address at
`0x1_8000_0000` and its CPU address at the host mapping. The case requires
the IOMMU to translate each IOVA to that physical address, and the DMA and
route registers to hold IOVAs below 4 GiB. The existing camera, IOMMU,
PCIe, xHCI and address-width checks run unchanged.

## Failure classes

Both new refusals are class 1 in section 1.8: they come before any device
access, the owner retains nothing, and the session stays usable. The NVMe
refusal releases its lease. The camera refusal keeps today's behavior: the
extent stays device-owned and the constructor fails before the fill. A buffer
above 2^34 cannot occur, because DRAM ends at `0x3_4000_0000`. The IOMMU
builder still refuses one before writing the table arena.

## Left out

- **Lifting the NVMe refusal.** Lifting it needs the root port's inbound width,
  which no pinned source states.
- **The PCIe endpoint test's limit.** Its compare refuses a buffer that ends
  at 4 GiB, one case stricter than the named limit. It is correct, and
  changing it would alter behavior that no consumer needs.
- **The NVMe byte-by-byte data copy** (section 1.6). It belongs to the NVMe
  data path, which this design does not change.
- **Mapping DRAM above 4 GiB and placing a pool there.** That is the
  memory-architecture work. Until it exists, every allocation still lands
  below 4 GiB, and this change removes only the requirement.
- **The other IOMMU consumers that pass a CPU pointer as a physical address**
  (`apps/h264`, `apps/g2d`, `apps/deinterlace`, `apps/jpeg`, `apps/h264-encode`).
  Those files belong to other packages.

## Board evidence still required

1. **One camera capture below 4 GiB at the fixed IOVA.** Today a capture
   cannot distinguish IOMMU translation from bypass, because each IOVA equals
   its physical address. With fixed IOVAs, a correct image with intact guard
   fill proves that the CSI master translates through the IOMMU. A camera
   module must be connected first.
2. **One camera capture above 4 GiB**, after a memory class maps that range.
   It follows the guard matrix in [dma-address-width.md](../dma-address-width.md):
   bit 32 only, then bit 33 only, then both bits.
3. **The PCIe inbound width**, from the same matrix with an NVMe read, before
   the refusal can be lifted.
4. **A read of `HCCPARAMS1.AC64`**, which decides whether xHCI still needs its
   131,072 bytes below 4 GiB.
