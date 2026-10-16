# Native assembly of the observed NPU model format

The native C assembler is **implemented and host-tested** for the neural-operation
profile in the pinned A733 ShuffleNetV2 model. It calculates buffer requirements,
resolves model addresses, copies weights, and constructs a command image entirely
in caller-owned RAM. Its output matches saved SDK bytes after allocation-address
and completion-event adjustments. It does not authorize hardware submission.

This document assumes familiarity with drivers, binary formats, and virtual
addresses. The interface is [nbg_assembly.h](../include/nbg_assembly.h), implemented
in [nbg_assembly.c](../drivers/npu/nbg_assembly.c). It consumes an immutable view
from the [container decoder](nbg-model.md) and uses the
[address encoder](nbg-relocation.md). The [SDK capture](nbg-loader-trace.md)
provides the allocation and command reference.

## Restrict assembly to the observed command format

The complete file must pass [checksum verification](nbg-checksum.md).
The supported profile requires target `0x1000003b`, format `0x1001e`, feature
word `0x02d00027`, one core, and one asymmetrically quantized UINT8 input and
output. One initialization operation precedes 1–1,024 sequential neural
operations with 192-byte descriptors. Working-pool alignment is 64 bytes;
internal SRAM use must fit 512 KiB. Fixed pool/SRAM bases and external SRAM
requirements are refused.

The assembler recognizes the initialization state and its relocation before
omitting them from the model command image, matching the SDK capture.
Ordinary and terminal neural operations must match the observed load-state
templates. Operation, descriptor, state-table, and relocation ordering must
agree. Unused relocations, repeated descriptor destinations, unknown sources,
split high addresses, unsupported transformations, and additional nonempty
execution sections are refused.
Descriptor patches are limited to the five observed pointer fields.
[Circular-buffer validation](nbg-rings.md) checks each complete encoded ring and
requires its tensor address to fall within that range in the same source allocation.

The first auxiliary operation word must be zero. The second remains opaque
and does not affect assembly; its semantics remain unresolved. Recognition
of a load-state template does not validate every field in its neural descriptor.
Other operation classes, tensor layouts, and format versions remain required
implementation work.

## Reserve buffers before modifying caller memory

`nbg_assembly_requirements` validates the supported profile and all relocations
using temporary address bases. It returns payload sizes, minimum reservations,
and allocation sizes rounded to 4 KiB. The total reservation is limited to
256 MiB. It allocates no memory and leaves the result unchanged on refusal.

`nbg_assemble` accepts six buffers with the calculated sizes. CPU and device
bases must be 4 KiB aligned. CPU ranges and device ranges must be disjoint;
device addresses cannot overlap the internal-SRAM window. Output allocations
cannot overlap the file, view, buffer metadata, or result. The result also
cannot overlap the file, view, or buffer metadata.

The caller must supply exclusively owned offline RAM and retain immutable
input metadata throughout the call. The assembler cannot detect physical
aliases, establish CPU permissions, or acquire ownership from another device.
Those responsibilities remain with the caller and the
[NPU memory policy](npu-memory.md).

Every operation is assembled in bounded stack scratch before any output changes.
After all operations pass, the assembler repeats construction into the buffers.
Stable input makes the second pass deterministic. Refusal preserves all caller
buffers and the result, including a failure in the final operation.

The fixture requires these reservations:

| Allocation | Payload bytes | Minimum bytes | Allocated bytes |
| --- | ---: | ---: | ---: |
| Neural descriptors | 24,000 | 24,000 | 24,576 |
| Coefficients | 892,608 | 892,736 | 892,928 |
| Working pool | 1,092,352 | 1,092,352 | 1,093,632 |
| Commands | 3,112 | 524,280 | 524,288 |
| Input | 150,528 | 150,592 | 151,552 |
| Output | 2 | 128 | 4,096 |

Coefficients receive the observed 128-byte padding. Tensor reservations round
logical size to 64 bytes and add 64 bytes. These formulas reproduce the SDK
allocation requests; they do not prove the complete access extent of each
neural operation. Four-KiB rounding matches the current memory-policy interface.
The six allocations total 2,691,072 bytes, excluding model storage, page tables,
bootstrap commands, and service memory.

Assembly initializes all allocations, including the input, output, working
pool, and padding. Applications must supply input data afterward. The command
body occupies 3,056 bytes. Its footer uses processing-engine event 0, as required
by the native controller, and retains the END instruction. Remaining fetch
space contains END pairs. The reservation covers the controller's fixed
524,280-byte fetch span even though the command image is much smaller.

## Host evidence covers assembly and refusal

One final ASan/UBSan check passed the complete fixture, buffer guards, padding,
event adaptation, deterministic reassembly, and 120 refusal cases. Cases include
unsupported profiles, malformed relocations, a late-operation failure, CPU and
device aliases, metadata aliases, misalignment, overflow, undersized buffers,
checksum or payload corruption, invalid ring geometry, and unsupported pointer
fields. Malformed-profile cases receive repaired
checksums so that they continue to exercise the intended assembly validation.
A valid final allocation ending at the 32-bit address-space boundary also passes.
A separate valid case places a ring end at its source allocation's end.

One integration check starts from the hash-verified SDK submission capture at
high device addresses. It adjusts captured address words for the native page
allocations and changes the recorded event from 4 to 0. Native assembly matches
all 534 descriptor relocations, 125 command-pointer relocations, complete
descriptor and command images, coefficients, allocation padding, and guards.
The SDK is not executed again.

The current AArch64 object build passes. It contains 5,524 bytes of text and read-only
data, no writable static data, and references the checksum verifier, decoder, encoder, `memcpy`,
and `memset`. Its SHA-256 is
`11a0f93f4d339e5abd56501a7a323dc534fdcc2d4f8ef5d7a67434ac8966da2d`.
Disassembly shows a 480-byte assembly frame and a 528-byte per-operation frame;
decoder, encoder, and memory-helper frames are additional. The returned plan
occupies 88 bytes on the 64-bit host. The isolated build target is `nbg-assembly`.

The focused checks are [nbg-assembly.c](../checks/nbg-assembly.c) and
[nbg-assembly.py](../checks/nbg-assembly.py). Build the host shared library from
`nbg_assembly.c`, `nbg_model.c`, `nbg_relocation.c`, and `nbg_checksum.c` with
`-shared -fPIC -Iinclude`.
The Python check takes the model, shared library, saved capture directory, and
a new output directory. It uses [host dependencies](../checks/host-requirements.txt).
Current results, artifacts, commands, and hashes are under
`~/.cache/sunstoneos/evidence/nbg-rings-20261016/`. The initial implementation
evidence remains under `~/.cache/sunstoneos/evidence/nbg-assembly-20261016/`;
checksum-specific evidence is under `~/.cache/sunstoneos/evidence/nbg-checksum-20261016/`.

The initial evidence directory retains a fixture refusal caused by an incorrect
interpretation of the second auxiliary operation word. A separate build failure
identified the missing runtime-header include path in the isolated CMake target.
Both corrections are covered by the final checks relevant to those changes.
Unchanged kernel, controller, service, DMA, and mimus suites were not repeated.

## Admission and physical execution remain outstanding

Linear tensor and weight references currently check one addressed byte. Encoded
rings are checked as complete spans with tensor addresses inside them.
Command-pointer references check the complete 192-byte descriptor. Actual device
accesses, including prefetch, are not established by those ring checks. Full access extents,
internal-SRAM routing, auxiliary-field semantics, and cold
initialization equivalence remain unresolved. Arbitrary descriptor contents
can therefore pass offline assembly without being safe to execute.

The assembler returns no controller token and is not registered with the
FreeRTOS execution service. Complete command admission, controlled registration,
platform resources, numerical output, physical completion, and recovery remain
required. No physical device request occurred during this work.
