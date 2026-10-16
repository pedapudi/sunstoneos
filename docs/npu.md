# NPU implementation and model format

Native NPU execution remains **physically untested**. The host model inspector
passes a reference comparison. [Page-table construction](npu-mmu.md) and the
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
Board-resource integration, model admission, and numerical inference remain outstanding.
Physical access remains conditional on qualified independent
watchdog recovery and protected ROM return.

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
values; the attached NPU has not been identified or accessed.

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
still include Linux-specific interface headers. Removing Linux requires a
native platform interface and allocator, as well as FreeRTOS implementations
of those services.

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
and passes a host simulator comparison. Its emitted `0x20000` format differs from
the existing decoder profile. Host-side Linux tools are compatible with the
FreeRTOS requirement; the board runtime remains native.

[Issue 12](https://github.com/pedapudi/sunstoneos/issues/12) tracks an
in-repository compiler and development toolkit with native FreeRTOS execution.
This replacement of external tools is deferred until complete hardware support.
Published compiler dependencies remain acceptable during bring-up. CPU, GPU,
and NPU optimization follows the [validation schedule](validation.md).

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

The [isolated HAL comparison](nbg-rings.md) confirms circular-size/end packing
and identifies a transpose selector in the 48 descriptors without coefficient
relocations. Native assembly now checks complete encoded rings and requires the
tensor address to lie within the ring. These checks do not establish actual
access extents or prefetch behavior.

The initial saved inventory is `descriptor-research.json` under
`~/.cache/sunstoneos/evidence/nbg-checksum-20261016/`. It includes every decoded
candidate field, relocation correspondence, and descriptor tail. The ring record
adds a complete range inventory. Resolve instruction-family semantics, stride
units, the extra word, linear access bounds, and prefetch extents before
authorizing device execution.

## Native implementation sequence

1. **Load the numerical fixture natively.** The pinned compiler now produces a
   small convolution with exact float and matched quantized simulator references.
   Native decoding and checksum verification now support its `0x20000` layout.
   Use the [SDK convolution capture](nbg-conv-loader.md) to extend assembly and
   allocation planning for its zero working pool. Keep the existing SDK network
   as the `0x1001e` format reference.
2. **Complete driver integration.** The native controller, memory policy, CPU
   adapter, and FreeRTOS service have software evidence. Add interrupt delivery,
   controlled model/command registration, and recovery. Retain required source
   license notices and keep Linux file and ioctl interfaces out of the runtime.
3. **Integrate platform resources.** Establish the NPU power domain, regulators,
   clocks, resets, and interrupt route. Confirm identity after watchdog recovery
   is qualified. Derive safe operating points from board-specific power data.
4. **Complete device-memory integration.** Ordinary RAM mappings and ownership
   have software evidence. Qualify internal-SRAM routing and complete fault
   reporting. Reserve model buffers and track CPU/device addresses separately.
   Qualify the complete mapping, cache, and translation-cache behavior physically.
   The [memory policy](npu-memory.md) records the reference driver's separate SRAM
   window and the native bootstrap's existing base-address command. Do not add
   ordinary RAM page-table entries for this window without contrary hardware evidence.
5. **Complete native model admission.** Container decoding, allocation planning,
   and assembly have host evidence for the observed neural-operation profile.
   Establish full descriptor access extents,
   command permissions, supported operators, and output
   padding. Inspect `libNBGlinker.so` where published source is insufficient.
   Extend to other format versions with representative compiler output.
6. **Execute and recover.** Submit a bounded job, synchronize completion, and
   compare numerical output. On timeout or fault, retain buffers until device
   quiescence is established. Qualify cancellation, reset, stale interrupts,
   repeated loads, concurrent CPU access, and power transitions.
7. **Expose application inference.** Provide model lifetime, tensor binding,
   asynchronous completion, cancellation, resource limits, and capability queries.
   Required hardware behavior must work independently of mimus.
8. **Evaluate mimus separately.** Map compatible operations and shapes while
   preserving the existing model. Measure transfer and scheduling overhead,
   numerical differences, recognition accuracy, latency, and resource use.
   CPU execution of unsupported operations must remain explicit and cannot
   establish full NPU support.

The next independent implementation work is decoding the numerical fixture,
complete descriptor access bounds, and command admission. Physical inference
remains untested, and
[issue 9](https://github.com/pedapudi/sunstoneos/issues/9) retains both NPU and
video-codec acceptance requirements.
