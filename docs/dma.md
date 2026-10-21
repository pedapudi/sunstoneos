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
The [DMA pool session](dma-pool-session.md) registers board RAM, and the native
policy admits the UART2, I2S0 and HDMI-audio FIFO routes their images use. The
[DMA service](dma-service.md) serializes production access, and the
[DMA platform startup](dma-platform.md) prepares DMA0 clocks, reset and CPU
routing. The service can wait for the DMA0 interrupt between DMA events when
the platform binds it; GIC routing of that interrupt, DMA1, descriptor addresses
at or above `0xfffff800` and physical qualification remain open.
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
section 3.9.4, lists two 16-channel controllers. The pinned A733 User Manual
V1.00 confirms DMA1's address and register layout. Its domain-specific request
routing and current ownership remain unresolved; executable DMA1 support is
unimplemented.

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

## DMA1 request routing remains unresolved

The A733 User Manual V1.00, section 13.1.6, lists both controllers under the
same register layout. Its interrupt table and CCU descriptions establish these
DMA1 resources:

| DMA1 resource | Source-defined value |
| --- | --- |
| Register base | `0x04024000`; 16 channel blocks at `+0x100 + channel * 0x40` |
| CPU interrupts | Nonsecure interrupt ID 145; secure interrupt ID 146 |
| Bus gate and reset | CCU `0x0200270c`, gate bit 0 and reset-release bit 16 |
| Memory-interface gate | CCU `0x020025e4`, bit 3; preserve every unrelated field |
| Channel security | Controller `+0x20`; one means nonsecure for each channel |
| Interrupt destinations | Controller `+0x34` CPU and `+0x38` CPUS channel masks |

The pinned Linux CCU source independently defines the DMA1 gate/reset and
memory-interface gate. Its source label `dcxo` does not establish the transfer
clock rate. The manual identifies AHB as the working clock and describes the
DMAC's AHB/MBUS interfaces. Existing normalized fabric/REF ownership must remain
held; this audit authorizes no parent retuning.

The request table cannot yet authorize a DMA1 DRAM copy. Section 13.1.1 says
one controller is in the CPUX domain and the other is in CPUS. Table 13-2 in
section 13.1.4.1 explicitly describes CPUX source and destination requests,
with SRAM at request 0 and DRAM at request 1. The common register list does not
extend that table to the other domain. The pinned Linux device tree exposes
only DMA0; its effective driver uses `DRQ_SDRAM=0` for memory copies. Neither
source establishes the DMA1/CPUS DRAM request value. A bounded search of
published A733/sun60iw2 DMA1 and RTOS HAL definitions found no resolving primary
source. This is an unresolved applicability conflict, rather than authority to
choose either value.

The smallest proposed operation is the existing DMA-copy application's finite
1,024-byte channel-0 copy, using its retained descriptor/source/destination pool,
service result/reap protocol and immutable copied-result observer. The core
controller and native memory adapter already accept a base address. Platform
startup, the service singleton and result admission still restrict it to DMA0.
An eventual extension needs two fixed controller profiles, selected once from
the immutable handoff and bound to the platform addresses, exclusive clock
token, native allowlist and service base. A union of both register allowlists
would permit access under the wrong controller's token.

Semantic RAM endpoints should remain RAM throughout mapping validation. Once
the request value is established, the owned controller profile can select its
hardware encoding inside submission, where descriptors are rebuilt. DMA1 FIFO
and cyclic modes need no admission in this first operation. Existing DMA0
descriptor bytes and its single-controller-per-image service remain supported.

Physical admission must independently exclude and drain every prior CPU,
remote-processor, channel, route and peripheral producer. It must also establish
nonsecure controller access, selected-channel permissions and descriptor/data
firewall access through the admitted lifecycle. The security register's reset
default cannot supply those facts. Section 13.1.4.4 describes secure-only DMAC
access with TrustZone enabled and secure channel defaults, conflicting with the
nonsecure reset bits in section 13.1.7.1. Neither passage authorizes a nonsecure
CPU read of `+0x20`. The zero readback described in section 13.1.4.4 concerns
DMA reads of secure memory, not CPU reads of protected registers.

The native allowlist therefore excludes `+0x20`. A future security observation
needs separate source-backed access authority or a qualified trusted-firmware
record; startup must not probe unknown register visibility. No security write,
remote-core shutdown, inherited drain or recovery can be inferred from an idle
controller. Failure must retain the pool, controller and parent leases.
Remote ownership remains a physical blocker without reopening the separate
management-core investigation.

The audit used the existing cached primary sources:

| Source | Pin and local cache |
| --- | --- |
| A733 User Manual V1.00, sections 13.1.1, 13.1.4.1, 13.1.6–7, 4.1.6.84 and 4.1.6.86 | `sources.lock.json:a733-user-manual-1.00`, SHA256 `cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`; `sources/camera-research/A733_User_Manual_V1.00.pdf` and extracted text |
| Linux DMA driver | Revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, SHA256 `d60e6598835ddf16b1f2374f746abd82da507dfefeb16edade9a0f7021b3d3bb`; `sources/dma-platform/sunxi-dma.c` |
| Linux device tree | Same revision, SHA256 `1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa`; `sources/dma-platform/sun60iw2p1.dtsi` |
| Linux CCU driver | Same revision, SHA256 `1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77`; `sources/dma-platform/ccu-sun60iw2.c` |

Cache paths are relative to `~/.cache/sunstoneos/`. Source URLs and hashes are
also preserved in `sources/dma-platform/manifest.json` and the existing source
lock. No driver, application, startup, register access, build or runtime check
was changed or executed for this audit.

## The implemented encoder preserves DMA0 address fields

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
The controller never accesses the GIC. Each submission enables the channel's
queue-end (finite) or package-end (cyclic) interrupt bit and the timeout bit,
and poll acknowledges their pending events. The DMA0 interrupt therefore only
wakes the caller, which still polls; the [service](dma-service.md) defines that
wake path. Poll counts every acknowledgement that includes an interrupt-enabled
bit in the controller's wrapping `events` counter. `a733_dma_event_wait()`
reports whether a channel is active with its deadline more than a given margin
away, so the caller can wait for the interrupt instead of polling each tick.

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
the same FIFO port in the same direction concurrently. A read FIFO and a write
FIFO may share one register address, as the UART receive and transmit registers
do; each direction still needs its own native port admission. CPU writes to
leased allocations are forbidden.

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

A FIFO producer must disable its DMA requests before poll stops the channel.
`a733_dma_finite_state()` lets it observe queue end, error events and timeout
for its active finite transfer without acknowledging them. The query applies
the same routing, mode and descriptor-high checks as poll and retains the channel
in recovery when they fail.

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

The I2S application now uses [fixed cyclic rings](i2s-stream.md#hardware-linked-repeated-streaming)
with immutable TX and checked RX staging. RX device ordering and physical
visibility remain unqualified.

Pause, hardware timeout modes, burst mode, IOspeed mode,
request handshakes, DMA1, physical interrupt delivery and GIC routing, IOMMU
integration, and concurrent peripheral workloads remain required.
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
deadline consumed by cache preparation. It checks the interrupt-wait margin
across counter wrap and that a pending half-package event, which is not
interrupt-enabled, leaves the event counter unchanged. It rejects writes to
global registers or channels assigned to the always-on processor.

The AArch64 object compiles with warnings treated as errors. With the pinned
Clang 21 toolchain its code occupies 7,216 bytes, with no static data or BSS. The object references the descriptor
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
clang -std=c11 -O2 -g -Wall -Wextra -Werror -Iinclude -Iruntime/include \
  --target=aarch64-none-elf -march=armv8-a -mstrict-align \
  -ffreestanding -fno-builtin -c drivers/dma/a733_dma_controller.c \
  -o ~/.cache/sunstoneos/build/dma-controller-a733.o
```
