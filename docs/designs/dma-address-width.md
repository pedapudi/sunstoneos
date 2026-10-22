# Design: DMA address-width rules and the path above 4 GiB

Status: **design**; no code exists yet. Physical behavior is unqualified until
the board evidence listed at the end exists. Readers are assumed to know
non-coherent DMA, AArch64 cache maintenance and the A733 system IOMMU described
in [docs/iommu.md](../iommu.md).

DRAM occupies physical `0x4000_0000`–`0x3_4000_0000`: 3 GiB below 4 GiB and
9 GiB above it, so a DRAM address uses physical bits 32 and 33. The device
memory pools in the tree lie at `0x4800_0000`–`0x7900_0000`
([docs/device-memory-capacity.md](../device-memory-capacity.md)). The classes
planned in the memory-architecture design (`dma32` at
`0x4800_0000`–`0x7800_0000`, `npu` at `0xC000_0000`–`0x1_0000_0000`) also lie
below 4 GiB. That design is not merged yet. This design depends on its class
table keeping every class below 4 GiB until the consumer changes listed below
are made.

Most masters already refuse addresses they cannot reach, each in its own
driver. Two gaps remain. Each DMA0 and IOMMU limit is written as a literal in
several files, and the NVMe driver has no limit. NVMe writes queue and
physical-region-page (PRP) addresses as two 32-bit halves through a PCIe root
port whose inbound address width is unpublished. For a buffer above 4 GiB the
outcome is unknown: the access could reach a different address without an
error. This design records one rule per master in documentation, names the
limits that code enforces, and closes the NVMe gap.

## Per-master rules

The rules live as a table in a new document, `docs/dma-address-width.md`. It
gives the pinned source for each width (board kernel `2ac08e8c`, device tree
`sun60iw2p1.dtsi`, A733 manual V1.00) and the line that enforces it.
**Direct** means the device addresses DRAM physically. **IOMMU** means the
system IOMMU translates the master's address. **Refuse** means the owner
rejects the buffer before any device access. No runtime table or routing
function exists: every rule already has an owner check, and those checks are
what run.

| Master | Width and rule | Enforced at |
| --- | --- | --- |
| DMA0 data (I2S, SPI, UART, memory copy) | Direct below 2^35. The driver packs bits 34:32 into descriptor parameter bits 11–13 and 15–17 | `dma_memory.c:18,29`; `a733_dma.c:25`; `a733_dma_controller.c:122`; `a733_dma_controller.h:41` |
| DMA0 descriptors | Refuse at or above `0xfffff800`, the end-of-chain marker. The pinned kernel puts bits 33:32 into the low two bits of the link word (`SET_DESC_HIGH_ADDR`, `sunxi-dma.c:142`); this encoding is unverified on hardware | `dma_memory.c:22`; `a733_dma.c:62` |
| DMA1 | Not used | — |
| Crypto engine | Direct below 2^40 | `drivers/crypto/a733_ce.c:19` |
| GPU | Direct through the GPU MMU; physical below 2^36 | `platform/a733/gpu_vm_memory.c:7` |
| NPU | Direct through the NPU MMU; physical below 2^40. The MMU image must not cross a 4 GiB boundary. Command addresses are 32-bit device addresses that the NPU MMU translates | `a733_npu_memory.c:7,51`; `a733_npu_controller.c:70,130` |
| Display engine 0, G2D, deinterlacer, video decoders and encoder, CSI, ISP | IOMMU: master IOVA below 4 GiB; physical below 2^34 (leaf bits 9:8 hold physical bits 33:32) | `a733_iommu.c:32,161` |
| xHCI (USB3) | Direct below 2^34 when `HCCPARAMS1.AC64` is 1 (the IOMMU bank-0 bypass address space, which contains all DRAM); below 4 GiB otherwise | `drivers/usb/a733_xhci.c:636` |
| PCIe endpoint DMA (NVMe, endpoint test) | Refuse at or above 4 GiB; root-port inbound width unpublished | Endpoint test `a733_pcie_epf_test.c:45`; NVMe none (consumer change below) |
| USB0 (MUSB), SD/MMC, SDIO | Programmed I/O; not DMA masters | — |
| EHCI/OHCI | Disabled | — |

The camera is an IOMMU master like the others. `struct a733_capture_span`
carries separate IOVA and physical fields, and `a733_camera_capture.c:155`
limits only the IOVA. The native camera configuration
(`a733_camera_native.c:401`) chooses IOVA equal to physical and therefore
refuses physical addresses at or above 4 GiB. That limit belongs to the
configuration; the master has none below 2^34.

## Named limits and which checks run

A new header, `include/dma_address_width.h`, defines the limits that code
uses. Every owned check reads them:

- `A733_DMA0_DATA_LIMIT` (2^35) replaces the literals in `a733_dma.c`,
  `dma_memory.c`, `a733_dma_controller.c` and `a733_dma_controller.h`.
- `A733_DMA0_DESCRIPTOR_LIMIT` (`0xfffff800`) replaces the literals in
  `a733_dma.c`, `dma_memory.c` and `a733_dma_controller.c`.
- `A733_IOMMU_PHYSICAL_LIMIT` (2^34) bounds span and table physical addresses
  at `a733_iommu.c:32`.
- `A733_IOMMU_IOVA_LIMIT` (4 GiB) is the master-side IOVA policy at
  `a733_iommu.c:161`.
- `DMA_ADDRESS_32BIT_LIMIT` (4 GiB) serves 32-bit masters outside this
  package.

The IOMMU's 34-bit IOVA geometry (the directory index comes from IOVA bits
33:22) stays a private constant in `a733_iommu.c`. `a733_iommu_translate`
(line 112) keeps that geometry bound, because it walks tables that have not
necessarily passed `a733_iommu_init`. For admitted tables, the 4 GiB policy has
already refused every higher IOVA.

DMA0 width checks run three times at registration, in `dma_memory_init`, and
again on every submit. Per submit on the native path, counted from the code:

- **Each mapping, descriptors included:** twice. `a733_dma_mapping_shape`
  runs in the controller's `valid_mapping` and again in `dma_memory_accept`.
  Both stay. The controller's call is the no-wrap precondition for its own
  overlap arithmetic, which runs before the caller-supplied `validate_mapping`.
  In `dma_memory_accept`, window containment already implies the bus bound, but
  the same call checks alignment and nonzero length, which containment does
  not.
- **Descriptor chain:** twice, in `validate_transfer` (lines 104–105) and in
  `a733_dma_build` (lines 62–63). The `validate_transfer` compare is
  **removed**. `a733_dma_build`, which the cyclic builder also calls, checks
  the same limit over the descriptors actually linked. It runs in the same
  submit, before it writes any descriptor byte and before any register write.
  On the native path, `dma_memory_init` has also refused every descriptor
  window that crosses the limit. One result changes: a bad chain on a channel
  that is not quiet now returns `A733_DMA_RECOVERY` instead of
  `A733_DMA_INVALID`, after status-register reads that have no side effects.
- **Each FIFO endpoint:** twice, in `validate_transfer` (lines 122–123) and in
  `validate_endpoint` inside `a733_dma_build`. Both stay. The first is the
  no-wrap precondition for `fifo_memory_conflict`, because `validate_fifo` is
  caller-supplied and need not bound the address. The second guards the bit
  packing: `(address >> 32) << 11` would carry bit 35 into the next field, and
  host checks call `a733_dma_build` directly.
- **Each memory endpoint:** once, in `a733_dma_build`, to guard the bit
  packing.

Each check is two integer compares on addresses supplied with the request.
Section 2, item 4 of the performance architecture permits such checks on the
submit path.

No bounce buffer is provided. Every narrow master takes its buffers from pools
below 4 GiB, so no copy is needed. A bounce path would add one copy per byte
and a second owned buffer, and no consumer needs one.

## Data flow, interrupts, placement and synchronization

Each owner applies its rule at admission, inside the call that already admits
the buffer, and keeps its current lease calls and DMA ownership states. A
refusal happens before any device access and before any table or descriptor
byte is written. No rule adds a runtime event: DMA0 completion keeps its
interrupt when bound, and IOMMU installation stays one bounded poll at
initialization. Buffers above 4 GiB, once a class maps them, use the same
Normal write-back inner-shareable attributes as buffers below 4 GiB, and the
same cache maintenance by virtual address over the bytes the other agent reads
or wrote. IOMMU tables can lie anywhere in DRAM, because the directory base
register holds `physical >> 14`. The IOMMU v2 format has only 4 KiB leaf pages,
so large IOMMU buffers keep 4 KiB device pages. No barrier is added or changed.

The limits are compile-time constants, so this design adds no shared mutable
state, lock or barrier. Admission results stay valid until the page-table owner
publishes a new mapping generation, and IOMMU tables are never remapped while
live.

## Budgets and measurement

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Width checks per DMA0 submit (two compares each) | Two per mapping, two per descriptor chain, two per FIFO endpoint, one per memory endpoint (counted) | One per descriptor chain, the others unchanged; zero added on any GPU, NPU, IOMMU or crypto job path | `checks/dma-controller.c`: a chain crossing `0xfffff800` is still refused with no register write |
| Copies per byte for a buffer above 4 GiB | No such buffers | Zero | No bounce path exists |
| IOMMU leaf words per 307,200-byte plane | 75 at physical `0xffff_f000` and `0x1_8000_0000` (`checks/a733-iommu.c`) | 75 | The same check, rerun unchanged |
| DRAM below 4 GiB that width rules require, summed over every driver in the tree (an upper bound for one image) | 847,872 bytes plus one 4 KiB descriptor page per DMA0 job table (counted): xHCI 131,072 (`include/a733_xhci.h:7`, only when AC64 is 0); NVMe 6 × 4,096 = 24,576 (`a733_pcie_nvme.c:14,179`); native camera 663,552 (`include/a733_camera_native.h:13`); endpoint test 2 × 12,288 + 4,096 = 28,672 (`include/a733_pcie_epf_test.h:8`, `a733_pcie_epf_test.c:43`) | 184,320 bytes plus descriptor pages, once the native camera uses fixed IOVAs | A new `checks/dma-address-width.c` sums the named size constants from their headers |
| Master paths that can reach a wrong address without an error | One: NVMe (counted) | Zero | `checks/a733-pcie-nvme.c`: a pool ending above 4 GiB is refused at initialization with no register write |

## Failure classes

- **Owner check refuses a buffer:** refused before any device access. The owner
  returns the error and the session stays usable. Nothing is retained.
- **IOMMU span refused:** the builder validates before it writes the arena, so
  this failure is also refused before any device access.
- **IOMMU fault on a high mapping:** the existing rule in
  [docs/iommu.md](../iommu.md) applies. The state is unknown, so the tables,
  clocks and global token are retained.

## Consumer changes outside this package

- **PCIe owner, `drivers/pci/a733_pcie_nvme.c`:** after
  `device_memory_acquire`, refuse the pool unless
  `a733_extent_within(0, DMA_ADDRESS_32BIT_LIMIT, physical, PAGES * 4096)`
  holds, and export the pool size as a header constant for the host sum. The
  endpoint test already refuses high memory at line 45; it may switch its
  literal to the named limit and needs no added check.
- **Camera owner:** in `a733_camera_native.c`, choose fixed IOVAs below
  `A733_IOMMU_IOVA_LIMIT` instead of identity IOVAs. The capture driver needs
  no rework; its IOVA check at line 155 reads the named limit.
- **IOMMU consumers that pass a CPU pointer as a span's physical address**
  (`apps/h264/native.c:305`, `apps/g2d/memory.c:50`,
  `apps/deinterlace/memory.c:70`, `apps/jpeg/native.c:168`,
  `apps/h264-encode/storage.c:30`): the pointer equals the physical address
  only under an identity CPU mapping. A consumer of a class mapped at a virtual
  address that differs from its physical address must pass the lease's
  physical address.
- **Memory-architecture owner:** keep every class below 4 GiB until the
  consumer changes above are made.

## Left out of this design

- **DMA0 descriptors above 4 GiB.** The link-word encoding is unverified on
  hardware, and a chain needs only one 4 KiB page below 4 GiB.
- **Bounce buffers.** No consumer needs one.
- **Mapping DRAM above 4 GiB.** The memory-architecture design owns the map.
- **Per-submit translations in `dma_memory_accept`.** A separate budget row in
  the performance architecture covers them.
- **IOMMU coverage beyond four 4 MiB windows, and fault interrupts.** Neither
  depends on address width.

## Board evidence that would confirm the rules

The evidence needs a class mapped above 4 GiB. Each Direct and IOMMU row, and
the NVMe test that would lift the PCIe refusal, needs one guarded transfer per
high-bit pattern: bit 32 only (`0x1_8000_0000`), bit 33 only (`0x2_8000_0000`)
and both bits (`0x3_2000_0000`). Guard patterns sit at every other DRAM address
with the same low 32 bits, so a master that drops or misplaces any of bits
33:32 writes a guard. For `0x3_2000_0000` the guards are at `0x2_2000_0000` and
`0x1_2000_0000`. A transfer passes when the payload and every guard read back
correct. For DMA0 the same matrix distinguishes the two high-address encodings
in the pinned `sunxi-dma.c`: 3-bit fields at bits 11 and 15 (lines 138–139),
which the driver uses, and 2-bit fields at bits 16 and 18 (lines 140–141).

Dropping both bits of a both-bits address lands below `0x4000_0000`, in the
MMIO window, where no guard can sit. The both-bits transfer therefore runs only
after both single-bit transfers pass, and only in the direction in which the
master reads the high buffer and writes a guarded buffer below 4 GiB. Before
that run, the A733 memory map must show that a read of the alias at
`0x2000_0000` has no side effect. The xHCI row also needs a read of
`HCCPARAMS1.AC64`.
