# NPU implementation and model format

Physical NPU execution remains **unproven**. The first supervised native launch
produced no USB frames, and its sole refusal-report read timed out. That result
does not establish whether NPU initialization began. The
[native application record](npu-application.md) preserves the attempt, recovery
and exact image identities.

The host model inspector passes a reference comparison.
[Page-table construction](npu-mmu.md) and the
[controller core](npu-controller.md) pass host models and compile for AArch64.
The [memory policy](npu-memory.md) passes host integration for private mappings,
table validation, cache publication, and retained faults. The
[native CPU adapter](npu-native.md) passes a FreeRTOS emulator check.
The [FreeRTOS service](npu-service.md) passes a two-core check with EL0 clients,
a preemptible EL1 worker, task ownership, and retained faults. The checked
[address relocation component](nbg-relocation.md) passes host bounds checks
and comparison with SDK instructions. A [complete host loader trace](nbg-loader-trace.md)
reconstructs all active relocations and matches SDK command buffers at two
allocation bases. The [native container decoder](nbg-model.md) passes bounded
view and malformed-input checks and supplies the same reconstruction from C
records using saved SDK evidence. The [native assembler](nbg-assembly.md)
now plans allocations and constructs the observed neural-operation profile,
matching saved SDK bytes after address and completion-event adjustments.
The [checksum verifier](nbg-checksum.md) matches two published models and is
required before assembly.
The assembler also validates [encoded circular-buffer ranges](nbg-rings.md),
with host field-packing and complete-submission comparisons.
The [pinned host compiler](npu-compiler.md) now produces a small numerical fixture
whose simulator output matches the quantized CPU reference. Its `0x20000` format
now has native decoding support. The [directory investigation](nbg-directory.md)
records its changed layouts and checksum coverage. Native checksum verification
passes host checks. The [convolution capture](nbg-conv-loader.md) supplies allocation
and command evidence now matched by native assembly with no working-pool allocation.
The [native convolution application](npu-application.md) implements platform
resource ownership, private memory, trusted compiled-model registration and
repeated-job inference through the FreeRTOS service. Three pinned convolution
artifacts are selectable before scheduling; their native modeled-output checks
and host numerical references do not establish physical inference. An independent
boot-CPU profile is implemented alongside the cached all-core profile. All-core
hardware support remains required.

The [session runtime](npu-session.md) runs up to eight catalogue models on
the NPU at once, one client task per model, with round-robin job starts
through the service's shared mode. The general assembler now accepts graphs
with several inputs and outputs, UINT8, INT8 and INT16 tensors, and the
transpose steps that the compiler emits around fully connected layers; its
output matches the pinned SDK loader for every catalogue graph. Sessions take
their memory from the platform pool by model size, which in the extended
cached profile is the DDR reservation. Emulator checks with modeled device
output pass; no board image links the runtime yet. The
[mimus layer study](npu-mimus-linear.md) shows that the simulator computes a
mimus int8 linear layer's sums exactly but rounds the output to INT16.

The single-core terminal-USB correction integrated as `ef00a07` allows an NPU
failure frame only while USB retains its independent authenticated lifetime,
clock, lock, task and provider checks. Ten affected native cases and independent
review pass under `npu-usb-terminal-20261018/`. The corrected image has not
established physical USB reporting or NPU execution; the reporting defect does
not identify the earlier physical failure's cause.

Recovery depends on the selected profile. The watchdog profile requires qualified
watchdog recovery and ROM return. The supervised RAM-only profile instead uses
reviewed manual whole-board power removal, storage preservation and one bounded
attempt. Independent watchdog qualification is not a blanket prerequisite.
Every resource remains reserved until the selected recovery condition is met.

This document assumes familiarity with drivers, virtual memory, and neural
network inference. A Network Binary Graph (NBG) contains compiled operations,
weights, tensor descriptions, and relocation records. A relocation describes
how the loader must patch an address after allocating device memory.

## Hardware and available driver source

The pinned [board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f)
contains the VIPLite 2 driver under `bsp/drivers/npu/aw_nna_vip/vip2`.
Its core hardware, memory, and scheduling files offer a dual MIT/GPL license.
An adaptation can select the MIT terms while preserving the required notices.
Downloaded reference files retain their original notices outside the repository.

The SoC device tree supplies the following resources. These are reference
values; no native NPU identity or execution result has been captured.

| Resource | Reference value |
| --- | --- |
| Registers | Physical `0x03600000`, length `0x1000` |
| Interrupt | SPI 65, GIC interrupt ID 97, level high |
| Clocks | NPU core, parent PLL, bus, MBUS gate, AHB gate |
| Resets | NPU core, AXI, AHB |
| Power domain | `SUN60IW2_PCK_NPU` |
| Expected chip product ID | `0x1000003b`; revision `0x9202` in the feature database |
| Feature database entry | One NPU engine instance (VIP core), eight neural processing units, 512 KiB internal SRAM, 32-bit device virtual addresses |

The expected product ID also appears in the A733 SDK sample. It must be checked
against physical identity before accepting a compiled model. The device-tree
operating-point hints do not establish a safe voltage or clock rate on this board.

The driver separates register control, device page tables, memory allocation,
command construction, task scheduling, and power management. Its OS interface
covers register reads/writes, allocation, cache maintenance, interrupt waits,
mutexes, atomics, threads, signals, timers, and monotonic time. Some core files
still include Linux-specific interface headers. The native application supplies
its own checked register access, private allocations, cache publication, bounded
polling and FreeRTOS task/lock interfaces through repository source. It does not
link the Linux driver or its file and ioctl interfaces into FreeRTOS. The first
trusted convolution defaults to polling with interrupt ID 97 masked. The optional
[finite interrupt profile](../apps/npu-interrupt/README.md) implements deferred
completion on that interrupt. Physical interrupt delivery and broader concurrent
use still require qualification.

No external firmware request was found in the inspected VIPLite 2 subtree.
That observation does not establish that every compiler or execution mode is
firmware-independent. The driver itself is available as source; binary analysis
is currently needed for parts of model loading.

## Model compilation and loading remain separate dependencies

The [A733 SDK](https://github.com/ZIFENG278/ai-sdk/tree/fc90006d0f6569da2f6726c2d8395877686f5aca)
is pinned at `fc90006d0f6569da2f6726c2d8395877686f5aca`.
The inspected AArch64 VIPLite 2 package publishes public API headers and two
shared libraries. `libNBGlinker.so` implements network and buffer operations.
It imports hardware-abstraction functions from `libVIPhal.so`, plus C library,
math, and pthread functions. The hardware abstraction library imports Linux
file, ioctl, and pthread operations. These Linux libraries cannot be linked
directly into the present FreeRTOS image.

The SDK also publishes `nbinfo`, an x86-64 model inspection executable with
function symbols and DWARF debug types. Disassembly, debug types, and a reader
trace establish the inspected container layout. The native inspector uses
these format facts; it does not execute downloaded commands or load either
AArch64 shared library.

The open [TIM-VX parser](https://github.com/VeriSilicon/TIM-VX/tree/702715d714277376d7ff466b7b72833ff57564c1/include/tim/utils/nbg_parser)
provides an MIT-licensed reference for an older format. At revision
`702715d714277376d7ff466b7b72833ff57564c1`, its maximum format version is
`0x10014`. The inspected A733 sample uses `0x1001e`, which adds fields and
sections. Reusing that parser unchanged would reject this sample.

The [compiler setup guide](https://docs.radxa.com/en/cubie/a7s/app-dev/npu-dev/cubie-acuity-env)
identifies the A733 host package as `ubuntu-npu:v2.0.10.2`.
The [compilation example](https://docs.radxa.com/en/cubie/a7s/app-dev/npu-dev/cubie-acuity-usage)
selects hardware version `v3` and runtime version `v2.0` for A733.
The [compiler record](npu-compiler.md) pins the downloaded image, target,
dependencies, and original license information. A small convolution now compiles
and passes a host simulator comparison. Native decoding, checksum verification,
allocation planning and assembly now support its observed `0x20000` profile;
the Python inspection tool remains limited to `0x1001e`.

The runtime links repository NPU/NBG source, FreeRTOS and TinyUSB source. The
native C/AArch64 image is built with the pinned Clang/LLD toolchain. Model
compilation uses the external x86-64 Linux ACUITY 6.30.22 and Vivante IDE 5.11.0
host package; it is separate from building the native executable. The application
artifact generator checks the model/input/reference hashes in
[`tools/npu-models.json`](../tools/npu-models.json) and embeds the selected bytes
without invoking that compiler. Model and command artifacts remain external
compiler outputs, not repository compiler source.

`libNBGlinker.so`, `libVIPhal.so`, `nbinfo` and the compiler image's simulator
supply the documented format, loader and address-path reference evidence. Their
Linux shared libraries are not loaded by the FreeRTOS application. Existing
platform DDR preparation also retains its pinned boot firmware. These accepted
dependencies, their execution locations and original license records are
separate from a claim that every NPU mode is firmware-independent. Source pins,
compiler identities and artifact provenance are recorded in
[npu-compiler.md](npu-compiler.md), [sources.md](sources.md) and the component
records. No dependency replacement is needed to attempt the trusted workload.

[Issue 12](https://github.com/pedapudi/sunstoneos/issues/12) tracks an
in-repository compiler and development toolkit with native FreeRTOS execution.
This replacement of external tools is deferred until complete hardware support
and performance tuning. Documenting their provenance, execution role and license
status takes priority over replacement. Published compiler dependencies remain
acceptable during bring-up and optimization. CPU, GPU and NPU optimization
follows the [validation schedule](validation.md).

## Structural inspection is implemented

Run the host inspector with a downloaded model path:

```sh
python3 tools/inspect-nbg.py /path/to/network_binary.nb
```

It accepts format `0x1001e`, limits input to 256 MiB, and emits JSON. It reads
little-endian fields explicitly. The fixed header occupies 592 bytes and
contains 27 section descriptors after byte 376. Their serialized order differs
from the inspection executable's in-memory structure order.

The output includes tensor dimensions and quantization, logical tensor bytes,
working-pool requirements, SRAM use, command sizes, section locations,
operation counts, and relocation-source counts. Names use replacement characters
for invalid UTF-8. Other format versions fail explicitly.

The inspector checks section bounds and overlap, table strides, header counts,
relative payload ranges, tensor sizes, finite positive quantization scales,
selected operation references, relocation tensor indices, and pool alignment.
It accepts only disjoint nonempty sections. Other legitimate layouts may require
an extension supported by a compiled fixture.

Successful inspection does not authorize hardware submission. The host inspector
does not resolve relocations, validate command semantics or SRAM placement, check
device identity, or verify integrity. Unknown operation and relocation-source
values appear explicitly in its inventory. Its output reports
`checksum_verified: false`; the native assembler uses the separate
[checksum verifier](nbg-checksum.md) before constructing commands.

## Reference comparison and malformed-file checks

The checked fixture is the SDK's
`examples/vpm_run/operator/v3/network_binary.nb`. Its SHA-256 is
`fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f`.
The filename is generic; the embedded network name is `ShuffleNetV2_uint8_NCHW`.
This fixture establishes container interpretation, not model accuracy or device
execution.

| Property | Observed value |
| --- | --- |
| File length | 964,224 bytes |
| Target and format | `0x1000003b`, `0x1001e` |
| Layers and operations | 87 layers; 125 neural-network operations and one initialization operation |
| Neural command size | 192 bytes |
| Relocations | 660 records, each 40 bytes |
| Coefficient data | 892,608 bytes |
| Working pool | 1,092,352 bytes, 64-byte alignment |
| Internal SRAM use | 518,144 bytes |
| Input | UINT8, dimensions `[224, 224, 3, 1]`, 150,528 logical bytes |
| Output | UINT8, dimensions `[2, 1]`, two logical bytes |

The published inspection tool reports a 64-byte output size despite the
two-byte logical tensor. The [SDK loader trace](nbg-loader-trace.md) records a
128-byte allocation request. The native assembler reproduces that reservation
and rounds it to a 4 KiB allocation for the memory policy. Complete descriptor
access extents remain unresolved. The host inspector reports logical sizes.

One focused check compares network metadata and tensor descriptions against
recorded `nbinfo` reports. It also rejects 18 malformed variants covering
truncation, unknown layout, invalid ranges and aliases, count mismatch, tensor
size and quantization errors, invalid references, and allocation alignment.
No physical request or OS rebuild occurred.

The check is reproducible with the same fixture and recorded reports:

```sh
python3 checks/nbg-inspection.py /path/to/network_binary.nb /path/to/reference-reports
```

Local evidence is under `~/.cache/sunstoneos/evidence/npu-source-20261016/`.
It includes reference reports, the reader trace, disassembly, check results,
and an input hash manifest. Download manifests under
`~/.cache/sunstoneos/sources/npu/` record source URLs, revisions, sizes, and
SHA-256 values. Retrieved files were also checked against Git blob identities.

## Descriptor field compatibility is a research result

Mesa's [neural-command implementation](https://gitlab.freedesktop.org/mesa/mesa/-/blob/45b86c99242b3dbf8bdaec7d5f90084106338a63/src/gallium/drivers/etnaviv/etnaviv_ml_nn.c)
provides a candidate field layout at revision
`45b86c99242b3dbf8bdaec7d5f90084106338a63`. Its MIT-licensed source is cached
with the original notices, download URLs, Git blob identities, and SHA-256 values
under `~/.cache/sunstoneos/sources/npu/mesa-45b86c99242b3dbf8bdaec7d5f90084106338a63/`.

All 534 descriptor relocations in the pinned A733 fixture fall at candidate
weight, tensor-address, or circular-buffer-end fields:

| Byte offset | Candidate meaning | Relocations |
| ---: | --- | ---: |
| 20 | Weight address, shifted by six | 77 |
| 24 | Input tensor address | 125 |
| 28 | Output tensor address | 125 |
| 76 | Output circular-buffer end plus one, shifted by six | 102 |
| 84 | Input circular-buffer end plus one, shifted by six | 105 |

This correspondence is an inference. Mesa's structure occupies 136 bytes;
the A733 descriptors occupy 192 bytes. Word 35 is nonzero in 77 descriptors,
beyond that structure. Mesa assigns image heights to its named Y-stride fields,
while the A733 fixture contains values resembling per-channel image area.
The first descriptor reports an input width and height of 224 but a Y-stride
value of 50,176. These differences prevent direct adoption as an access validator.
The [simulator investigation](nbg-strides.md) now identifies direct channel-stride
packing in the target's revision-9.2 decoder. It explains this difference but
does not establish complete access extents or prefetch behavior.

The [isolated HAL comparison](nbg-rings.md) confirms circular-size/end packing
and identifies a transpose selector in the 48 descriptors without coefficient
relocations. Native assembly now checks complete encoded rings and requires the
tensor address to lie within the ring. These checks do not establish actual
access extents or prefetch behavior.

The initial saved inventory is `descriptor-research.json` under
`~/.cache/sunstoneos/evidence/nbg-checksum-20261016/`. It includes every decoded
candidate field, relocation correspondence, and descriptor tail. The ring record
adds a complete range inventory. General model admission still needs resolved
instruction-family semantics, stride units, the extra word, linear access bounds
and prefetch extents. These structural comparisons alone do not authorize a new
model. The fixed trusted convolution has a separate
[address-path review](npu-address-path.md) of translated external accesses and
owned internal SRAM; its physical confinement remains unqualified.

## Implemented path and remaining qualification

1. **Qualify the trusted convolution on hardware.** Native decoding, checksum
   verification, allocation planning, assembly and immutable registration are
   implemented for the selected compiler artifacts. The application submits two
   distinct jobs and checks both copied outputs. Saved CPU and host-simulator
   references are valid evidence for comparison; modeled device output is not
   numerical inference. Keep the SDK ShuffleNet artifact as the separate
   `0x1001e` format reference.
2. **Qualify platform and device-memory behavior.** Native providers implement
   PMIC sampling, PLL acquisition, clocks, resets and power-domain startup under
   shared ownership. The permanent reservation tracks CPU/device addresses,
   private tables, cache publication and retained faults. Actual board admission
   must establish independent DRAM/PHY sources, complete SRAM ownership, supported
   operating points and excluded writers. Qualify the effective mapping, SRAM
   routes, cache visibility and translation-cache behavior physically. The
   [memory policy](npu-memory.md) records the separate SRAM route; ordinary RAM
   page-table entries do not replace its ownership contract.
3. **Qualify completion, errors and recovery.** The default fixed workload
   polls completion once per worker tick. The single-core interrupt profile
   wakes the worker from interrupt 97 on CPU0, and its runtime client receives
   completion through a held WAIT reply. The corrected single-core USB path can report a terminal NPU
   failure only while the transport remains valid. Physical delivery remains
   untested. Keep buffers and hardware resources on timeout or fault until the
   reviewed recovery condition is met. Interrupt-driven completion, cancellation,
   reset, stale interrupts, repeated loads, concurrent CPU access and power
   transitions remain required hardware coverage.
4. **Extend admission beyond the trusted catalogue.** Concurrent models from
   the [session catalogue](npu-session.md) are implemented, including
   multi-tensor graphs and compiled fully connected layers, and are checked
   only in emulation. Uncatalogued model uploads, runtime model switching and
   tensor-processor, shader or software operations in general graphs are
   unsupported. Establish complete descriptor access extents, command
   permissions, operator semantics and output padding for each added profile.
   Reuse published source and representative compiler output; inspect binary
   loader references where source is insufficient.
5. **Broaden application inference after the fixed path works.** The current
   service already provides model/session lifetime, copied tensor input and
   output, asynchronous jobs, deadlines and explicit result reaping. Wider
   capability queries, limits and concurrent-client behavior need an explicit
   supported contract and hardware evidence. Required NPU behavior must work
   independently of mimus.
6. **Evaluate mimus separately.** Map compatible operations and shapes while
   preserving the existing model. The [linear-layer study](npu-mimus-linear.md)
   compiles mimus layers from their int8 codes and per-row scales; the
   simulator matches one rounding of the exact result to INT16, not the mimus
   float32 output, and the compiler refuses the INT32 output an exact CPU
   read-out would need. After hardware functionality, measure transfer
   and scheduling overhead, numerical differences, recognition accuracy, latency
   and resource use. Explicit CPU fallback for unsupported operations does not
   establish full NPU support. The native compiler/toolkit under issue 12 remains
   deferred as described above.

A first native launch has been attempted, but no captured result establishes NPU
execution. Physical completion with copied numerical output is still the gating
result for this application. [Issue 9](https://github.com/pedapudi/sunstoneos/issues/9)
retains both NPU and video-codec acceptance requirements.
