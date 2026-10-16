# GPU firmware memory and boot configuration

The native MIPS preparation component is **implemented and host-tested**.
It prepares firmware memory and wrapper register values for the A733 GPU.
It does not access the device. Firmware startup and physical qualification
remain outstanding.

This document assumes familiarity with page tables and device drivers. The
GPU's internal MIPS processor executes the [pinned firmware](gpu.md).
Its address mappings are separate from application GPU memory contexts,
which still need implementation.

## Preparation validates ownership before changing memory

[pvr_mips_prepare()](../include/pvr_mips.h) accepts the firmware image, code/data
buffers, a page-table buffer, and optional firmware objects such as command
queues. Each buffer has a CPU address and an explicit list of physical pages.
The CPU mapping can be contiguous while physical pages are discontiguous.
Preparation requires no allocator or filesystem.

The [implementation](../drivers/gpu/pvr_mips.c) revalidates the image, decodes
the ordered feature-parameter stream, and requires a 36-bit physical bus.
It checks for the firmware flag identifying hardware erratum 63553, which
requires a remap for accesses to physical address zero. It also checks the
architectural addresses and sizes of boot, exception, and stack sections.
Other feature values remain available in the image for later initialization.

The builder checks exact allocation sizes, CPU overlap, virtual overlap,
physical-page uniqueness, alignment, and address width. It excludes page zero
and the sensitive physical range `0x1fc00000..0x1fc03fff`, where wrapper remaps
would redirect accesses. Invalid input leaves all output buffers unchanged.
Inputs must remain stable and outputs exclusively owned until return.

The platform remains responsible for proving RAM ownership, device
addressability, and correspondence between CPU mappings and physical pages.
It must enforce reserved firmware heap regions and exclude physical aliases
with input storage. The offline builder cannot infer those facts from pointers.

## The page table covers complete firmware allocations

The MIPS heap occupies 16 MiB beginning at `0xc0000000`. Its 4,096 entries
describe 4 KiB pages, requiring four page-table pages. Entries are written
explicitly in little-endian order. Every unmapped entry is zero.

The complete code allocation starts at the heap origin. The complete data
allocation starts at the private-data address from the firmware layout.
These mappings include allocation pages that also have special boot or stack
aliases. Mapping only the main executable and private-data sections would
omit part of the reference driver's firmware address space.

Code and data use uncached mappings. Up to 64 additional objects can request
uncached or write-through mappings. All entries permit reads, writes, and
execution, matching the reference firmware object policy. This policy does
not establish isolation for application workloads. The caller must finish
allocating firmware objects before preparation; live table updates need a
separate cache and translation-invalidation protocol.

## Boot data and wrapper writes use the same physical allocations

Preparation stages the ELF code/data, fills the page table, and writes a
64-byte boot configuration into the boot-data page. That configuration contains
the stack page, GPU register base, four page-table addresses, page size, page
count, and zeroed reserved fields. Other staged bytes remain intact.

The returned plan contains eleven ordered register writes. They configure
the microMIPS boot instruction mode, register window, boot-code/data/exception
remaps, address-zero workaround, idle control, and debug control. The plan
records each register's access width. The builder does not execute it.

Allocation, CPU cache publication, GPU memory contexts, platform power and
clocks, reset sequencing, shared firmware structures, interrupts, and bounded
startup remain necessary before this prepared image can run. The
[command transport](gpu-queues.md) has host evidence; the FreeRTOS GPU service
has not been integrated.

## Focused evidence compares complete outputs with pinned source functions

One [sanitizer check](../checks/pvr-mips.c) passed discontiguous physical pages
above 4 GiB, the highest 36-bit page, both cache policies, unaligned CPU byte
buffers, allocation guards, and removal of optional mappings on reconstruction.
It also passed 61 refusal cases with every output unchanged.

The [reference comparison](../checks/pvr-mips-reference.py) extracts and compiles
the original page-mapping, boot-data, and wrapper-initialization functions from
the pinned board driver. It uses independent ELF staging and supplies physical
addresses through host substitutes for kernel memory objects. Cache flushes
are stubbed, and register writes are captured without device access.

All code/data bytes, all 16 KiB of page-table entries, and all eleven ordered
register writes match. An independent table walk checks the highest physical
address and both cache policies. The source manifest and firmware hash are
verified before comparison. The fixture maps 33 pages; it does not prove
physical translation, cache coherence, or firmware execution.

The reference harness initially failed to compile because a check header was
missing and its warning macro returned no value. Both fixes affected only
the harness. Native sanitizer and AArch64 compilation evidence remained valid.
Clang 21 compiled the native component with strict alignment and freestanding
options: 4,607 bytes of code/read-only data, no writable static data, and a
1,488-byte preparation stack frame. Calls into firmware decoding and staging
require additional stack space.

Commands, outputs, input copies, hashes, reference source, and objects are in
`~/.cache/sunstoneos/evidence/gpu-mips-20261016/`. Existing firmware staging,
OS, mimus, NPU, and recovery evidence was reused. No hardware access or
performance measurement occurred.
