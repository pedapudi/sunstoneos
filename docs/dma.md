# DMA descriptor encoding and remaining controller work

The native [descriptor encoder](../drivers/dma/a733_dma.c) builds finite DMA
chains for incrementing memory and fixed peripheral FIFO ports. Its
[host execution model](../checks/dma-descriptors.c) passes memory copies,
FIFO streams, address-boundary checks, and refusal without output mutation.
The encoder compiles for AArch64. It has not executed on physical hardware.

The controller service, channel allocation, cache synchronization, interrupt
delivery, cancellation, and peripheral integration remain unimplemented.
Calling the encoder performs no hardware access and does not transfer buffer
ownership to a device. Before physical DMA experiments, qualify the independent
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

## Required controller and ownership behavior

The controller service must serialize channel allocation and establish an
exclusive lease before changing channel registers. CPU interrupt routing alone
is insufficient ownership evidence. Initialization must preserve channels used
by the always-on processor and coordinate shared clocks, resets, and autogating.

Submission must retain the descriptor chain and every data mapping until DMA
has stopped. Before enabling a channel, clean source data and descriptors,
prepare destination cache lines, and issue the required memory barriers.
Cache maintenance must operate on privately owned lines without damaging
concurrent CPU writes. Integration with the SMP runtime requires a privileged
service because its application tasks run at the unprivileged exception level,
EL0.

Completion must distinguish queue end from package completion and timeout.
The service must acknowledge only its channel's pending bits. Queue completion
or a stop-register write alone does not prove that outstanding writes drained.
Require the channel to be disabled and its busy bit to clear before invalidating
destination cache lines and returning buffers to the CPU.

Cancellation must use bounded polling. If a channel cannot stop, retain its
buffers and require independent recovery; do not report reusable memory.
The watchdog must bound a stalled CPU or bus even when normal polling cannot
continue. Channel restoration and recovery require physical checks.

Pause, cyclic transfers, hardware timeout modes, burst mode, IOspeed mode,
request handshakes, DMA1, interrupt delivery, IOMMU integration, and concurrent
peripheral workloads remain required.
Data addresses crossing 4 GiB boundaries and transfers near the upper address
limit need physical tests; software representation does not establish bus behavior.

## Focused evidence and reproduction

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
