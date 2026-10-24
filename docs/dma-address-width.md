# DMA address widths and the path above 4 GiB

Status: **implemented** for DMA0, the system IOMMU, the native camera, NVMe and
xHCI, with host checks. No transfer above 4 GiB has run on hardware, so every
rule below is unqualified until the board evidence at the end exists.

DRAM occupies physical `0x4000_0000`–`0x3_4000_0000`: 3 GiB below 4 GiB and
9 GiB above it, so a DRAM address uses physical bits 32 and 33. Every device
memory pool in the tree lies below 4 GiB
([device-memory-capacity.md](device-memory-capacity.md)). The rules on this
page say which masters can reach DRAM above 4 GiB once a memory class maps it,
and where each master's limit is enforced.

## Each master refuses addresses it cannot reach

Each owner checks its buffers when it admits them, before any device access
and before it writes any table or descriptor byte. No runtime routing table or
bounce buffer exists. In the table, **direct** means the device addresses DRAM
physically, **IOMMU** means the system IOMMU translates the master's address,
and **refuse** means the owner rejects the buffer.

Widths come from the board kernel source (`2ac08e8c`), its device tree
`sun60iw2p1.dtsi` and the A733 User Manual V1.00.

| Master | Width and rule | Enforced at |
| --- | --- | --- |
| DMA0 data (I2S, SPI, UART, memory copy) | Direct below 2^35. Descriptor parameter bits 11–13 and 15–17 carry source and destination bits 34:32 | `A733_DMA0_DATA_LIMIT` in `a733_dma_mapping_shape`, `dma_memory_init`, the controller's FIFO check and the descriptor builder |
| DMA0 descriptors | Refuse at or above `0xfffff800`, the end-of-chain link value. The board kernel puts bits 33:32 into the low two bits of the link word (`SET_DESC_HIGH_ADDR`, `sunxi-dma.c:142`); that encoding is unverified on hardware and unused here | `A733_DMA0_DESCRIPTOR_LIMIT` in `a733_dma_build` and `dma_memory_init` |
| DMA1 | Not used | — |
| Crypto engine | Direct below 2^40 | `drivers/crypto/a733_ce.c:19` |
| GPU | Direct through the GPU MMU; physical below 2^36 | `platform/a733/gpu_vm_memory.c:7` |
| NPU | Direct through the NPU MMU; physical below 2^40. The MMU image must not cross a 4 GiB boundary. Command addresses are 32-bit device addresses that the NPU MMU translates | `drivers/npu/a733_npu_memory.c:7,51`; `a733_npu_controller.c:70,130` |
| Display engine 0, G2D, deinterlacer, video decoders and encoder, CSI, ISP | IOMMU. Master IOVA below 4 GiB; physical below 2^34, because leaf and directory bits 9:8 hold physical bits 33:32 | `A733_IOMMU_IOVA_LIMIT` in `a733_iommu_init`; `A733_IOMMU_PHYSICAL_LIMIT` in the table builder and validator |
| xHCI (USB3) | Direct below 2^34 when `HCCPARAMS1.AC64` is 1 (IOMMU bank 0 bypass, which reaches all DRAM); below 4 GiB otherwise | `DMA_ADDRESS_32BIT_LIMIT` in `capabilities`, `drivers/usb/a733_xhci.c` |
| PCIe endpoint DMA (NVMe, endpoint test) | Refuse above 4 GiB; the root port's inbound address width is unpublished | `DMA_ADDRESS_32BIT_LIMIT` in `a733_pcie_nvme_init`; endpoint test `drivers/pci/a733_pcie_epf_test.c:45` |
| USB0 (MUSB), SD/MMC, SDIO | Programmed I/O; no DMA | — |
| EHCI/OHCI | Disabled | — |

The camera is an IOMMU master like the others. `struct a733_capture_span`
carries separate IOVA and physical fields. The capture driver limits each IOVA
with `A733_IOMMU_IOVA_LIMIT`, and the IOMMU builder limits each physical span
with `A733_IOMMU_PHYSICAL_LIMIT`. The native camera gives the device fixed
IOVAs from `A733_CAMERA_NATIVE_IOVA` (`0x1000_0000`), so its 663,552-byte
allocation may lie anywhere in DRAM below 2^34. Its constructor refuses an
allocation that ends above 2^34 before it writes any byte
([camera-native.md](camera-native.md)).

The NVMe driver writes queue bases and PRP entries as two 32-bit halves. The
inbound width of the root port that forwards them is unknown, so
`a733_pcie_nvme_init` refuses queue memory that ends above 4 GiB, before any
configuration or BAR access ([pcie.md](pcie.md)).

## Named limits

[`include/dma_address_width.h`](../include/dma_address_width.h) defines the
limits as exclusive ends: an accepted range satisfies
`address + bytes <= limit`.

| Name | Value | Meaning |
| --- | --- | --- |
| `A733_DMA0_DATA_LIMIT` | 2^35 | DMA0 data and FIFO addresses |
| `A733_DMA0_DESCRIPTOR_LIMIT` | `0xfffff800` | DMA0 linked descriptors; also the end-of-chain link value |
| `A733_IOMMU_PHYSICAL_LIMIT` | 2^34 | Physical addresses of IOMMU spans and tables |
| `A733_IOMMU_IOVA_LIMIT` | 4 GiB | Device addresses given to IOMMU masters |
| `DMA_ADDRESS_32BIT_LIMIT` | 4 GiB | Masters that write 32-bit bus addresses, outside the DMA and IOMMU drivers |

The IOMMU's 34-bit IOVA space stays private to `drivers/iommu/a733_iommu.c`,
because the table geometry takes the directory index from IOVA bits 33:22.
`a733_iommu_translate` keeps that bound, since it walks tables that need not
have passed `a733_iommu_init`. A static assertion keeps the IOVA policy inside
that space.

## Width checks on a DMA0 submit

Each width check is two integer compares on addresses supplied with the
request. On the native path, `a733_dma_submit` runs these:

- **Each mapping, descriptors included:** twice. `a733_dma_mapping_shape` runs
  in the controller's mapping check and again in `dma_memory_accept`. The
  controller's check guarantees that its overlap arithmetic cannot wrap, and it
  runs before the caller's `validate_mapping`. In `dma_memory_accept`, window
  containment already implies the bus bound, but the same call also checks
  alignment and nonzero length.
- **Descriptor chain:** once, in `a733_dma_build`, over the descriptors that
  the chain links. The cyclic builder calls the same function. The check runs
  before any descriptor byte, cache operation or register write. On the native
  path, `dma_memory_init` has also refused every descriptor window that crosses
  the limit.
- **Each FIFO endpoint:** twice. The controller's check keeps
  `fifo_memory_conflict` from wrapping, because the caller's `validate_fifo`
  need not bound the address. The builder's check guards the bit packing:
  `(address >> 32) << 11` would carry bit 35 into the next field.
- **Each memory endpoint:** once, in the builder, to guard the bit packing.

Because the descriptor check runs in the builder, the controller first
confirms that the channel is quiet. A chain that crosses the limit on a channel
that is not quiet therefore returns `A733_DMA_RECOVERY`, after status-register
reads with no side effects. On a quiet channel it returns `A733_DMA_INVALID`
with no descriptor, cache or register change. A descriptor mapping larger than
the chain may extend past the limit; the controller never links or reads its
unused tail.

No GPU, NPU, IOMMU or crypto job path gains a check.

## Buffers above 4 GiB use the same data path

Buffers above 4 GiB, once a memory class maps them, use the same Normal
write-back inner-shareable attributes as buffers below 4 GiB. They need the
same cache maintenance by virtual address over the bytes that the other agent
reads or writes. IOMMU tables can lie anywhere in DRAM below 2^34, because the
directory base register holds `physical >> 14`. The IOMMU v2 format has only
4 KiB leaf pages, so a large IOMMU buffer costs one leaf word per 4 KiB at any
physical address.

Every narrow master takes its buffers from pools below 4 GiB, so no copy is
needed. A bounce path would add one copy per byte and a second owned buffer,
and no consumer needs one.

## Measurements

Baseline and final values come from the parent of the implementing commits and
the last of them, built from the same configuration. Instruction counts come
from AArch64 emulation of one call per scenario, with the image's C flags.
Raw records are under
`~/.cache/sunstoneos/evidence/dma-address-width-20261022/` and
`~/.cache/sunstoneos/build/dma-address-width-measure/`; the
[measurement record](designs/dma-address-width-measurements.md) gives the
method and every figure.

| Metric | Before | After |
| --- | --- | --- |
| Width checks per DMA0 submit | Two per mapping, two per descriptor chain, two per FIFO endpoint, one per memory endpoint | One per descriptor chain; the others unchanged |
| AArch64 instructions per successful DMA0 submit: one-segment copy, four-segment copy, memory to FIFO | 1,148; 2,822; 1,017 | 1,121; 2,795; 990 |
| AArch64 instructions per DMA0 submit refused for a descriptor chain that crosses `0xfffff800`: quiet channel, channel not quiet | 136; 136 | 710; 667 |
| AArch64 code of `a733_dma_controller.c` | 7,248 bytes | 7,200 bytes |
| AArch64 code of `a733_dma.c`, `dma_memory.c` | 2,228 and 1,588 bytes | Identical instructions |
| Copies per byte for a buffer above 4 GiB | No such buffers | Zero |
| IOMMU leaf words per 307,200-byte plane at physical `0xffff_f000` and `0x1_8000_0000` | 75 and 75 | 75 and 75 |

The camera and NVMe rules have their own
[measurement record](designs/dma-high-memory-measurements.md), which compares
them with their parent commit:

| Metric | Before | After |
| --- | --- | --- |
| DRAM below 4 GiB that width rules require, summed over every driver | 847,872 bytes plus one 4 KiB descriptor page per DMA0 job table | 184,320 bytes plus one 4 KiB descriptor page per DMA0 job table |
| Master paths that can reach a wrong address without an error | One: NVMe | None |
| IOMMU 4 MiB windows and leaf words used by the native camera | One or two windows, depending on placement; 154 leaf words | One window at any placement; 154 leaf words |
| Modeled register writes of one native camera capture | 2,348 | 2,348 at the same addresses; only the parameter and two plane address values differ |
| AArch64 code of the native camera constructor, route and DMA admission | 2,020, 188 and 288 bytes | 2,052, 184 and 284 bytes |
| AArch64 code of `a733_pcie_nvme_init` | 804 bytes | 816 bytes; the command, poll, read and copy paths have identical instructions |
| AArch64 code of the capture constructor and xHCI capability check | 1,920 and 532 bytes | Identical instructions |

The 184,320 bytes are the xHCI pool when `AC64` is 0 (131,072), the NVMe pool
of six 4 KiB pages (24,576) and the PCIe endpoint test's two 12,288-byte
buffers and one 4,096-byte buffer (28,672). The figure is an upper bound for one
image, because no image links every driver.

The DMA0 conflict scan walks the 16 channels by pointer and executes six
instructions for each idle channel, one of them a stack load of the controller
pointer. A scan in a separate, non-inlined function keeps that pointer in a
register, but its call adds 15 instructions to each successful submit, so the
scan stays inside `submit`.

## Failure handling

- **An owner refuses a buffer:** the refusal happens before any device access.
  The owner returns the error, retains nothing, and the session stays usable.
- **The IOMMU builder refuses a span:** the builder validates every span
  before it writes the table arena, so the refusal also precedes device access.
- **An IOMMU fault on a high mapping:** the rule in [iommu.md](iommu.md)
  applies. The state is unknown, so the tables, clocks and global token stay
  retained.

## Changes that other drivers must make

- **IOMMU consumers that pass a CPU pointer as a span's physical address**
  (`apps/h264/native.c:305`, `apps/g2d/memory.c:50`,
  `apps/deinterlace/memory.c:70`, `apps/jpeg/native.c:168`,
  `apps/h264-encode/storage.c:30`): a pointer equals the physical address only
  under an identity CPU mapping. A consumer of a class whose virtual address
  differs from its physical address must pass the lease's physical address.
- **Memory architecture:** keep the memory classes that those consumers use
  identity mapped until they pass the lease's physical address. The native
  camera's allocation can move above 4 GiB once a memory class maps that
  range. The NVMe pool, and the xHCI pool while `AC64` is unread, must stay
  below 4 GiB.

DMA0 descriptors above 4 GiB, bounce buffers, the mapping of DRAM above 4 GiB,
IOMMU coverage beyond four 4 MiB windows and IOMMU fault interrupts are not
implemented.

## Host checks

The [width check](../checks/dma-address-width.c) asserts the limit values and
counts IOMMU leaf words for one plane below and above 4 GiB. It admits an IOMMU
span that ends at 4 GiB, refuses one that ends a page later, and sums the named
pool sizes. The [native camera fixture](../checks/a733-camera-native.c)
captures an image from an allocation at physical `0x1_8000_0000` and refuses
one across 2^34. The [NVMe model](../checks/a733-pcie-nvme.c) admits a pool
that ends at 4 GiB and refuses one that ends a page later. The [controller check](../checks/dma-controller.c) refuses a
chain whose second descriptor crosses `0xfffff800` with no descriptor, cache or
register change, enters recovery for the same chain on a channel that is not
quiet, and completes a chain that ends at the limit. Removing the builder's
descriptor compare, or moving the IOVA policy one page higher, fails these
cases.

```sh
F="-std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
clang-21 $F checks/dma-address-width.c drivers/iommu/a733_iommu.c \
  -o ~/.cache/sunstoneos/build/dma-address-width-check
~/.cache/sunstoneos/build/dma-address-width-check
clang-21 $F checks/dma-controller.c drivers/dma/a733_dma_controller.c \
  drivers/dma/a733_dma.c -o ~/.cache/sunstoneos/build/dma-controller-check
~/.cache/sunstoneos/build/dma-controller-check
```

The descriptor, cyclic, memory-policy, IOMMU and supervised-camera IOMMU host
checks, and the DMA0 service, startup and I2S emulator models, pass unchanged.

## Board evidence that would confirm the rules

The evidence needs a memory class mapped above 4 GiB. Each direct and IOMMU
row, and the NVMe test that would lift the PCIe refusal, needs one guarded
transfer per high-bit pattern: bit 32 only (`0x1_8000_0000`), bit 33 only
(`0x2_8000_0000`) and both bits (`0x3_2000_0000`). Guard patterns sit at every
other DRAM address with the same low 32 bits, so a master that drops or
misplaces either of bits 33:32 writes a guard. For `0x3_2000_0000` the guards
are at `0x2_2000_0000` and `0x1_2000_0000`. A transfer passes when the payload
and every guard read back correct. For DMA0 the same matrix distinguishes the
two high-address encodings in the board kernel's `sunxi-dma.c`: 3-bit fields at
bits 11 and 15 (lines 138–139), which this driver uses, and 2-bit fields at bits
16 and 18 (lines 140–141).

Dropping both bits of a both-bits address lands below `0x4000_0000`, in the
MMIO window, where no guard can sit. The both-bits transfer therefore runs only
after both single-bit transfers pass, and only in the direction in which the
master reads the high buffer and writes a guarded buffer below 4 GiB. Before
that run, the A733 memory map must show that a read of the alias at
`0x2000_0000` has no side effect. The xHCI row also needs a read of
`HCCPARAMS1.AC64`.
