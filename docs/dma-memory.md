# DMA memory admission and native AArch64 operations

The [memory policy](../drivers/dma/dma_memory.c) admits DMA buffers only within
platform-reserved RAM windows. The [native EL1 adapter](../platform/a733/dma_native.c)
validates CPU translations, maintains data caches, reads the physical counter,
and performs ordered 32-bit register accesses. These components implement the
callbacks used by the [DMA controller core](dma.md).

The policy passes a host check. The adapter also runs from two serialized
FreeRTOS callers in QEMU. No physical DMA controller was accessed.
Board RAM reservations, actual peripheral request tables, clock/reset setup,
production service integration, and physical qualification remain outstanding.
Independent watchdog recovery remains a prerequisite for physical execution.

## RAM windows define admissible mappings

The [policy interface](../include/dma_memory.h) accepts up to 16 RAM windows and
64 FIFO routes. The limits bound registry storage; they are software policy.
Initialization copies the tables and performs no allocation. An invalid table
leaves the policy unchanged.

Each RAM window supplies a CPU base, physical base, byte size, and permitted
roles: DMA source, DMA destination, or descriptor storage. Windows must contain
only dedicated DMA allocations. The platform must exclude code, page tables,
firmware, general-heap objects, and driver state before registering a window.
This policy does not discover RAM capacity or reserve memory by itself.

CPU and physical window ranges must be disjoint from other registered windows.
The policy rejects physical aliases, overlapping CPU ranges, FIFO/RAM overlap,
and a window covering the policy object. Descriptor-capable windows must remain
below the controller's unresolved wider-descriptor address boundary.

A submitted mapping must fit entirely inside one window, carry a permitted
role, and have the physical address implied by its CPU offset in that window.
Both bases and its size must align to the 64-byte ownership unit.
The policy checks every covered 4 KiB granule, including partial endpoint pages.
Checking intermediate pages prevents a valid first and last page from concealing
a hole, different physical backing, or incompatible permissions.

CPU translations must resolve to the expected physical address and describe
inner-shareable, write-back RAM with read/write allocation. DMA destinations and
descriptor storage additionally require CPU write permission.
Read-only source pages are admissible. Device memory and non-shareable mappings
are rejected as DMA RAM.

Device bus addresses equal physical addresses in this adapter. I/O memory
management unit (IOMMU) remapping and hypervisor-controlled stage-2 translation
are unsupported. The platform must keep mappings pinned
for the entire channel lease and prevent unregistered CPU aliases from writing
the same storage. The registry checks its own windows; it does not audit every
page table in the system.

FIFO admission matches a registered physical address, request ID, transfer
direction, access width, and burst length. No physical-board FIFO routes are
registered automatically. They must come from verified controller wiring and
request assignments. Shared-address full-duplex FIFO leases remain an extension
required by the controller core.

## Native operations require privileged, stable mappings

The [native interface](../include/a733_dma_native.h) runs at EL1 with the MMU and
data cache enabled. Initialization verifies that both pages of its 8 KiB register
window translate to the supplied physical range with Device-nGnRnE or
Device-nGnRE attributes. Initialization does not read peripheral registers.
Register callbacks reject accesses outside that window or without 32-bit alignment.
Transfer and stop durations use physical-counter ticks at the frequency reported
by `CNTFRQ_EL0`.

Address-translation instructions check reads or writes through stage 1.
The adapter masks asynchronous exceptions around each translation and preserves
both `PAR_EL1` and the caller's interrupt masks. It restores that state before
returning success or a translation fault. The
[Cortex-A55 reference manual](https://documentation-service.arm.com/static/5e7e1405b471823cb9de57ae),
sections B2.32 and B2.84, describes the cache geometry and translation-result fields.

Cache preparation reads `CTR_EL0` on the executing CPU. The maintenance stride
comes from its smallest data-cache line size. The writeback granule bounds how
much adjacent memory an eviction can overwrite. Unknown granules or values
larger than 64 bytes are rejected. Every CPU sharing these allocations needs
compatible geometry and qualified cache coherence before multicore use.

Source and descriptor buffers use `DC CVAC`; destination preparation uses
`DC CIVAC`; destination completion uses `DC IVAC`. System-wide data barriers
surround maintenance. The implementation touches only aligned, wholly owned
allocations and performs no whole-cache set/way operations.
The pinned [Trusted Firmware-A cache helpers](https://github.com/ARM-software/arm-trusted-firmware/blob/4ec2948fe3f65dba2f19e691e702f7de2949179c/lib/aarch64/cache_helpers.S)
provide a reference for maintenance to the point of coherency and completion barriers.

The controller must validate mappings before invoking cache callbacks and retain
their ownership until quiescence. Invalid callback use invokes a required fatal
handler; returning from that handler traps. MMIO and cache instructions can still
stall or fault on physical hardware, so this adapter does not replace the
independent watchdog.

The production service must serialize all controller calls. The emulator fixture
uses a FreeRTOS mutex with two EL1 tasks. The separate SMP runtime runs application
tasks at EL0 and still needs a privileged service interface. This check does not
establish that interface or physical multicore coherence.

## Focused validation and its limits

The [host check](../checks/dma-memory.c) passed with AddressSanitizer and
UndefinedBehaviorSanitizer. It covers a mapping spanning three pages, a failing
middle page, write-permission refusal, physical-address mismatch, Device memory,
non-shareable RAM, aliases, address limits, FIFO admission, and cache geometry.
The fixture uses bus addresses above 4 GiB without truncation.

The [FreeRTOS emulator check](../checks/dma-native.c) passed one bounded session.
Two tasks perform eight rounds each, serialized through a mutex. They execute
translation and cache instructions, compare buffer contents and neighboring
guards, and verify preservation of interrupt masks and `PAR_EL1`.
A read-only RAM block rejects write and descriptor admission; an unmapped
address returns a translation fault. Register operations access the emulated
PL011 UART, which supplies Device memory for this check.

QEMU has no A733 DMA controller. It also does not establish physical cache
visibility, bus drain, cross-core coherence, or watchdog recovery. The earlier
controller model remains the evidence for descriptor execution and channel
ownership. Its unchanged sources and results were reused.

The emulator image contains 22,864 bytes of code/read-only data, 8 bytes of
initialized data, and 97,536 bytes of BSS/reserved stacks. Its SHA-256 is
`5fe6dd3f5d4eaae14f3d82bf7c3dce76c9c70f10b1691e5f2bdfb47e397560e5`.
Commands, results, the image, and 131 source/dependency/tool/artifact identities
are retained under `~/.cache/sunstoneos/evidence/dma-native-20261016/`.

Reproduce after relevant inputs change, using the configured development build:

```sh
mkdir -p ~/.cache/sunstoneos/build
clang -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  checks/dma-memory.c drivers/dma/dma_memory.c \
  -o ~/.cache/sunstoneos/build/dma-memory-check
~/.cache/sunstoneos/build/dma-memory-check
cmake -S . -B ~/.cache/sunstoneos/build/qemu-virt
cmake --build ~/.cache/sunstoneos/build/qemu-virt --target dma-native-model -j4
timeout 10s ~/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64 \
  -machine virt,gic-version=3,secure=off,virtualization=off \
  -cpu cortex-a55 -smp 1 -m 128M -nographic -monitor none \
  -semihosting-config enable=on,target=native \
  -kernel ~/.cache/sunstoneos/build/qemu-virt/dma-native-model.elf
```

The emulator must exit with status zero and print `PASS DMA native:`.
Fresh build configuration follows [build.md](build.md).
