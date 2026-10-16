# DMA controller implementation and qualification requirements

The native [descriptor encoder](../drivers/dma/a733_dma.c) builds finite DMA
chains for incrementing memory and fixed peripheral FIFO ports. Its
[host execution model](../checks/dma-descriptors.c) passes memory copies,
FIFO streams, address-boundary checks, and refusal without output mutation.
The encoder compiles for AArch64. It has not executed on physical hardware.

The [polled controller core](../drivers/dma/a733_dma_controller.c) implements
channel claims, mapped-buffer ownership, cache-operation callbacks, completion,
and bounded cancellation. Its host model passes noncoherent memory/FIFO transfers
and fault handling checks. The [RAM admission policy and native cache/MMIO
adapter](dma-memory.md) now pass host and FreeRTOS emulator checks.
Board RAM/FIFO registration, production service serialization, interrupt delivery,
and clock/reset setup remain unimplemented.
The controller has not executed on physical hardware.

Calling the encoder alone performs no hardware access and does not transfer
buffer ownership to a device. Before physical DMA experiments, qualify the independent
watchdog and a reset path that returns to ROM without executing storage code.
See [recovery requirements](watchdog.md).

## Hardware facts and unresolved addressing

The pinned [board kernel DMA driver](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/dma/sunxi-dma.c)
defines the A733 controller as `dma-v106`, with per-channel interrupt registers
and 35-bit data addressing. The downloaded source has SHA-256
`d60e6598835ddf16b1f2374f746abd82da507dfefeb16edade9a0f7021b3d3bb`.
Its hardware definitions inform the native encoder; the Linux driver is not a
runtime dependency.

The pinned [device tree](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi)
describes DMA0 with 16 channels and 64 request IDs. The
[A733 datasheet](https://dl.radxa.com/cubie/a7a/docs/hw/datasheet/A733_Datasheet_V0.93.pdf),
section 3.9.4, lists two 16-channel controllers. DMA1 remains in scope;
its address, ownership, and peripheral routing need confirmation.

| DMA0 resource | Reference value or behavior |
| --- | --- |
| Register window | `0x04601000`, size `0x2000` |
| Interrupt | SPI 111, GIC interrupt ID 143 |
| Channel block | Controller base plus `0x100 + channel * 0x40` |
| Channel enable, pause, descriptor | Channel offsets `0x00`, `0x04`, `0x08` |
| Channel interrupt enable and pending | Channel offsets `0x34`, `0x38`; pending bits are cleared by writing ones |
| Controller busy bitmap | Controller offset `0x30` |
| CPU and always-on processor interrupt routing | Controller offsets `0x34`, `0x38` |
| Reference processor partition | CPU channels 0–7; always-on processor channels 8–15 |
| Access widths and burst lengths | Widths 1, 2, 4 bytes; bursts 1, 4, 8, 16 accesses |

The routing values are the kernel's initialization policy. They do not establish
the attached board's current routing or exclusive ownership of an idle channel.
Controller-global interrupt banks used by older sunxi DMA revisions do not
describe this revision's per-channel registers.

Data address bits 32–34 occupy descriptor parameter bits 11–13 for the source
and 15–17 for the destination. The encoder retains the original 64-bit input
addresses until it writes both the low words and these high fields.

Descriptor addresses have a separate unresolved encoding. The kernel's
scatter/gather path puts two high address bits into the low descriptor word,
while its memory-copy path does not perform that transformation. The pinned
[bootloader header](https://github.com/orangepi-xunlong/u-boot-orangepi/blob/b791be842935b27268ae3d00e943a9075495f30a/arch/arm/include/asm/arch-sunxi/dma.h)
also defines a descriptor-high register at channel offset `0x3c`.
The inspected bootloader driver does not program that register.
Until this is resolved, the encoder accepts descriptor storage only below
`0xfffff800`, the end-of-chain marker. Wider data addresses remain representable.
Register state and physical address reachability still require qualification.

## Encoder contract

The [C interface](../include/a733_dma.h) accepts explicit DMA bus addresses.
It never converts a CPU pointer into a bus address. The caller supplies a
64-byte-aligned descriptor array and its corresponding aligned bus address.
Input segments must remain immutable during the call. The output allocation
must be exclusively accessible and contain at least the stated number of slots.

Each slot contains six little-endian words followed by zero padding:

| Byte offset | Field |
| --- | --- |
| 0 | Source and destination request, width, burst, and address mode |
| 4 | Low 32 source address bits |
| 8 | Low 32 destination address bits |
| 12 | Transfer length in bytes |
| 16 | Normal wait value 8 and upper data address fields |
| 20 | Next slot's bus address, or `0xfffff800` for the final slot |
| 24–63 | Zero padding; each descriptor occupies a separate 64-byte slot |

Request ID zero selects incrementing memory. IDs 1–63 select a fixed FIFO port;
the service integrating a peripheral must validate its actual request ID and
port address. The encoder accepts memory-to-memory, memory-to-FIFO, and
FIFO-to-memory segments. It rejects FIFO-to-FIFO segments.

The current software policy permits 1–64 segments, each containing 1 byte
through 1 MiB. These bounds limit validation and descriptor allocation; they
are not measured silicon limits. Lengths must be multiples of both endpoint
widths. Each address must align to its endpoint's width.
The complete addressed range must fit within 35 bits.
For a fixed port, that range is one bus access wide.

All segments are validated before any output byte changes. Invalid input leaves
the output allocation untouched. Validation rejects descriptor overlap with
input metadata or addressed data, overlapping memory destinations, and any
memory destination that overlaps a source anywhere in the chain.
Read-only source ranges may overlap. Repeated FIFO ports are allowed, but a
fixed port cannot overlap a range described as memory.

These checks compare supplied bus addresses. They cannot detect two bus
addresses mapped to the same physical memory or CPU aliases of data buffers.
The future mapping service must reject those conflicts and establish valid RAM,
peripheral, and descriptor mappings. The encoder does not validate the I/O memory
management unit (IOMMU), cache ownership, or conflicts with other active channels.

## Controller ownership and completion

The [controller interface](../include/a733_dma_controller.h) requires serialized
calls and an exclusive platform lease for its authorized channel mask.
CPU interrupt routing alone is insufficient ownership evidence.
The platform must prepare clocks, resets, and autogating before claiming channels.
The current polled implementation requires the DMA interrupt to remain masked
at the GIC. Per-channel queue-end and timeout interrupt bits are enabled so the
service can inspect and acknowledge their pending events.

Claims require CPU routing without always-on processor routing, a disabled idle
channel, no pending or enabled interrupts, and zero pause, mode, and descriptor-high
registers. Claiming a channel saves its descriptor pointer without writing hardware.
The implementation never changes controller-global registers or processor routing.

A transfer provides a descriptor allocation, segments, and RAM mappings with
explicit CPU addresses, bus addresses, sizes, and DMA read/write directions.
Each allocation owns complete 64-byte cache lines. The platform callback must
validate canonical RAM mappings and reject reserved or aliased memory.
A separate callback validates each FIFO's request ID, address, access width,
burst length, and direction against its peripheral.

The core rejects buffer overlap with descriptors, controller state, or transfer
metadata. Concurrent transfers can share read-only source allocations.
Any overlap involving a writable allocation is rejected, including disjoint
written byte ranges within the same owned cache lines. Two channels cannot use
the same FIFO port concurrently. CPU writes to leased allocations are forbidden.

Submission retains the immutable transfer metadata and its allocations before
cache preparation. It cleans source and descriptor lines, cleans and invalidates
destination lines, and invokes the platform barrier before enabling the channel.
Configuration readback and the transfer deadline must pass before enable.
Cache preparation that exhausts the deadline causes failure without starting DMA.

Polling distinguishes queue end from half/package events and timeout.
It acknowledges only the selected channel's pending bits. Queue completion
or a stop-register write alone does not prove that outstanding writes drained.
The core requires the channel to be disabled and its busy bit to clear.
It then verifies restored channel settings, invalidates destination lines, and
invokes a final barrier before releasing allocations. Timeout takes precedence
over queue completion when both events are observed.

Cancellation issues one pause/disable/resume sequence. Each poll performs a
bounded amount of work and never waits internally. The caller schedules polls
against a monotonic 64-bit tick source; elapsed-time arithmetic handles counter
wrap. Transfer and stop durations must be nonzero and below half the counter range.

| Returned state | Allocation ownership and next action |
| --- | --- |
| `ACTIVE` | Buffers remain leased; continue polling or request cancellation |
| `STOPPING` | Stop or failed submission is draining; continue polling |
| `COMPLETE` | Queue end, quiescence, and restoration observed; buffers released |
| `CANCELED` or `FAILED` | Quiescence and restoration observed; buffers released, contents may be partial |
| `RECOVERY` | Independent recovery required; retain every transfer for which `a733_dma_owns()` remains true |

Ignored stop writes, uncleared pending events, failed restoration, or unexpected
processor routing can leave a channel in recovery state. The core provides no
forced-release or live-context reinitialization operation. Failed transfers remain
in conflict checks so another channel cannot reuse their buffers.
The platform must invoke independent recovery when required. An independent
watchdog must also bound MMIO accesses that stall before a poll can return.

The [native adapter](dma-memory.md) provides cache instructions, memory barriers,
and page-translation validation. Platform integration must supply dedicated RAM
reservations, verified FIFO routes, and serialization through the FreeRTOS service.
The SMP runtime runs application tasks at the unprivileged exception level, EL0,
so controller access requires a privileged service. Physical tests must establish
that enable/busy observations and cache operations provide the assumed drain
and visibility guarantees.

Pause, cyclic transfers, hardware timeout modes, burst mode, IOspeed mode,
request handshakes, DMA1, interrupt delivery, IOMMU integration, and concurrent
peripheral workloads remain required.
Bidirectional peripheral ports that share one register address need a
peripheral-aware FIFO lease extension; the current core serializes those ports.
Data addresses crossing 4 GiB boundaries and transfers near the upper address
limit need physical tests; software representation does not establish bus behavior.

## Descriptor evidence and reproduction

One host session passed under AddressSanitizer and UndefinedBehaviorSanitizer.
The execution model follows descriptor links and moves bytes through simulated
RAM and FIFO ports. Memory cases cover all eight values of the upper address
bits, all supported widths and bursts, guarded destinations, and a full chain
sharing one read-only source. FIFO cases verify stream order in both directions.

Invalid later segments leave the entire descriptor pool unchanged. The cases
include unsupported formats, overflow, misalignment, descriptor/data conflicts,
overlapping writes, and copies that depend on earlier writes in the chain.
A fixed byte vector independently checks field packing. Boundary cases cover
the last permitted descriptor slot, the 35-bit data limit, and a range crossing
4 GiB. The AArch64 compilation passed with warnings treated as errors.

Commands, results, input hashes, compiler identity, and artifacts are in
`~/.cache/sunstoneos/evidence/dma-descriptors-20261016/`.
`build.json`, `result.json`, and `manifest.json` hold the reusable evidence.
No board requests were issued. Physical transfer, cache coherence, interrupt,
and recovery qualification remain outstanding.

Reproduce only when relevant inputs change:

```sh
mkdir -p ~/.cache/sunstoneos/build
clang -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  checks/dma-descriptors.c drivers/dma/a733_dma.c \
  -o ~/.cache/sunstoneos/build/dma-descriptors-check
~/.cache/sunstoneos/build/dma-descriptors-check
clang -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
  --target=aarch64-none-elf -march=armv8-a -mstrict-align \
  -ffreestanding -fno-builtin -c drivers/dma/a733_dma.c \
  -o ~/.cache/sunstoneos/build/dma-a733.o
```

## Controller evidence and reproduction

The [controller host model](../checks/dma-controller.c) maintains separate CPU and
device copies of RAM. Descriptor execution reads device-visible memory; CPU
results change only through cache invalidation. This exposes missing source or
descriptor cleaning and preserves destination bytes outside the requested ranges.

The final candidate passed one session under AddressSanitizer and
UndefinedBehaviorSanitizer. Cases cover memory and FIFO streams, two channels
sharing source data, conflicts between writable cache lines, and completion
before bus drain. Faults cover ignored start/stop writes, descriptor configuration
and restoration failures, uncleared events, and changed processor routing.
The model also checks cancellation, timeout precedence, counter wrap, and a
deadline consumed by cache preparation. It rejects writes to global registers
or channels assigned to the always-on processor.

The AArch64 object compiles with warnings treated as errors. Its code occupies
5,124 bytes, with no static data or BSS. The object references the descriptor
encoder and `memcpy`; final platform linkage remains outstanding.
Commands, results, input hashes, compiler identity, and artifacts are in
`~/.cache/sunstoneos/evidence/dma-controller-20261016/final/`.
The unchanged descriptor encoder retains its separate evidence above.
No physical device requests were issued.

```sh
mkdir -p ~/.cache/sunstoneos/build
clang -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  checks/dma-controller.c drivers/dma/a733_dma_controller.c \
  drivers/dma/a733_dma.c -o ~/.cache/sunstoneos/build/dma-controller-check
~/.cache/sunstoneos/build/dma-controller-check
clang -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude \
  --target=aarch64-none-elf -march=armv8-a -mstrict-align \
  -ffreestanding -fno-builtin -c drivers/dma/a733_dma_controller.c \
  -o ~/.cache/sunstoneos/build/dma-controller-a733.o
```
