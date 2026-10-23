# Native assembly of observed NPU model formats

The native C assembler is **implemented and host-tested** for the neural-operation
profiles in the pinned A733 ShuffleNetV2 model, the compiled numerical
convolutions, a compiled graph with two inputs and two outputs, and a compiled
mimus fully connected layer with transpose steps. It calculates buffer requirements,
resolves model addresses, copies weights, and constructs a command image entirely
in caller-owned RAM. Its output matches saved SDK bytes after allocation-address
and completion-event adjustments. It does not authorize hardware submission.

This document assumes familiarity with drivers, binary formats, and virtual
addresses. The interface is [nbg_assembly.h](../include/nbg_assembly.h), implemented
in [nbg_assembly.c](../drivers/npu/nbg_assembly.c). It consumes an immutable view
from the [container decoder](nbg-model.md) and uses the
[address encoder](nbg-relocation.md). The [ShuffleNetV2 capture](nbg-loader-trace.md)
and [convolution capture](nbg-conv-loader.md) provide allocation and command references.

## Restrict assembly to the observed command format

Each model must pass the applicable [checksum verification](nbg-checksum.md).
Every profile requires target `0x1000003b` and one core. The six-slot interface
below also requires one asymmetrically quantized UINT8 input and output; the
general interface described later accepts more tensors and types. The two
container formats use different wire values:

| Property | ShuffleNetV2 profile | Numerical convolution profile |
| --- | --- | --- |
| Format | `0x1001e` | `0x20000` |
| Feature word | `0x02d00027` | `0x0a` |
| Initialization layer | `0xffff` | `0xffffffff` |
| Operation state word | `0x0491e040` | `0x0493e040` |
| Relocation record bytes | 40 | 28 |

The state-word difference is bit 17; its hardware meaning remains unresolved.
These are accepted profiles, not a claim that every model in either format
uses those values. One initialization operation precedes 1–1,024 sequential
operations with 192-byte descriptors. Each operation is a neural-network
operation (kind 2) or, in the directory format only, a neural-transpose step
(kind 8). The compiler wraps a fully connected layer in two transpose steps
that move data through internal SRAM; a transpose step has no coefficients and
uses the same pointer and ring fields. Working-pool alignment is 64 bytes;
internal SRAM use must fit 512 KiB. Fixed pool/SRAM bases and external SRAM
requirements are refused.

The assembler recognizes the initialization state and its relocation before
omitting them from the model command image, matching the SDK capture.
Every operation state must match one of two observed load-state forms. The
24-byte form sets word 5 and is used for operations followed directly by
another. The 32-byte form clears word 5 and appends a second state pair; the
compiler emits it for the last operation and, in the directory format, for
every operation of a graph with transpose steps. The command body is the
48-byte prefix plus the actual state sizes. Operation, descriptor, state-table, and relocation ordering must
agree. Unused relocations, repeated descriptor destinations, unknown sources,
split high addresses, unsupported transformations, and additional nonempty
execution sections are refused.
Directory metadata sections for network information, memory requirements, and
checksums are accepted after decoding and checksum verification. Other nonempty
sections outside the execution profile remain refused.
Descriptor patches are limited to the five observed pointer fields.
[Circular-buffer validation](nbg-rings.md) checks each complete encoded ring and
requires its tensor address to fall within that range in the same source allocation.

The first auxiliary operation word must be zero. The second remains opaque
and does not affect assembly; its semantics remain unresolved. Recognition
of a load-state template does not validate every field in its neural descriptor.
Tensor-processor, shader and software operations, other tensor types and other
format versions remain unsupported by this assembler; the fixed shader and
transpose graph uses the separate [relative-shift assembler](npu-relative-shift.md).

## A general plan covers several tensors

`nbg_assembly_layout` and `nbg_assemble_layout` take graphs with one to eight
inputs and one to eight outputs. Each tensor must be affine-quantized UINT8,
INT8 or INT16. The plan lists the descriptor, coefficient, working-pool and
command allocations, then one allocation per input in model order, then one
per output. Relocation sources 1 and 2 select an input or output allocation
by their tensor index. With one input and one output this is the six-slot
order, and `nbg_assembly_requirements` and `nbg_assemble` are thin wrappers
that keep their single-UINT8 contract and produce identical bytes.

The container records one scale and zero point per input and output tensor.
Per-channel weight quantization, which the compiler applies with the
`perchannel_symmetric_affine` quantizer, exists only inside the compiled
coefficient stream and descriptors; the assembler copies those bytes
unchanged, so it needs no per-channel metadata. Input and output padding
follows the same rule for every tensor type: logical bytes rounded up to 64,
plus 64.

## A stepwise form assembles in bounded steps

`nbg_assembly_begin` and `nbg_assembly_next` let a loader assemble into memory
that no device or other task uses while it runs, a few operations per call.
The NPU runtime uses them to load a model into a slot without a long
non-yielding block ([design](designs/npu-breadth.md)). The file must be an
authenticated copy of one that `nbg_assembly_layout` accepted: `begin`
repeats the profile and placement checks without the CRC-32 pass, which the
digest supersedes, and writes nothing. Each `next` call validates and writes up
to the requested number of operations: the command prefix before the first,
each 192-byte descriptor in place and each command state after the previous
one. After the last operation it checks that every patch was consumed, writes
the footer and fills in the patch counts. The caller writes everything else:
coefficients, END pairs after the stream, padding, and the tensor and pool
allocations. A refusal can leave earlier operations written, so the caller
discards the assembly. The runtime check compares the bytes of every catalogue
model loaded this way with a one-call `nbg_assemble_layout` at the same device
addresses: descriptors, coefficients with padding and the complete command fetch
span match.

## Reserve buffers before modifying caller memory

`nbg_assembly_requirements` validates the supported profile and all relocations
using temporary address bases. It returns payload sizes, minimum reservations,
and allocation sizes rounded to 4 KiB. The total reservation is limited to
256 MiB. It allocates no memory and leaves the result unchanged on refusal.

`nbg_assemble` accepts six buffer slots with the calculated sizes. An empty
working pool requires `{NULL, 0, 0}` and causes no allocation or memory access.
A model relocation cannot reference that absent pool. Nonempty CPU and device
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

The ShuffleNetV2 fixture requires these reservations:

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
Its six allocations total 2,691,072 bytes, excluding model storage, page tables,
bootstrap commands, and service memory.

The numerical convolution has five allocations:

| Allocation | Payload bytes | Minimum bytes | Allocated bytes |
| --- | ---: | ---: | ---: |
| Neural descriptor | 192 | 192 | 4,096 |
| Coefficients | 256 | 384 | 4,096 |
| Working pool | 0 | 0 | 0 |
| Commands | 136 | 524,280 | 524,288 |
| Input | 192 | 256 | 4,096 |
| Output | 72 | 192 | 4,096 |

These allocations total 540,672 bytes with the same exclusions. Its command
body occupies 80 bytes. The larger fetch reservation follows the current
controller contract; reducing it is separate work.

Assembly initializes all allocations, including the input, output, working
pool, and padding. Applications must supply input data afterward. The command
body for ShuffleNetV2 occupies 3,056 bytes. Each footer uses processing-engine event 0, as required
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

The directory extension passes one ASan/UBSan check for five allocations,
absent-pool handling, guards, padding, and fourteen atomic refusals. It covers
noncanonical empty slots, references to the absent pool, mismatched profile
values, unconsumed relocations, descriptor bounds, and corrupted coefficients.
Mutation cases repair primary and section checksums using an independent
bitwise CRC implementation, so policy checks cannot pass merely by rejecting
a bad checksum. A final allocation ending at the 32-bit address limit also passes.

One comparison with the saved convolution SDK buffers passes all three descriptor
relocations, its command pointer, coefficients, input/output initialization,
padding, and guards. The capture's empty comparison field remains unchanged;
the new comparison result resides in the assembly evidence. Both models reuse
their captures without executing SDK instructions. The shared assembler changed,
so its existing 120-case sanitizer check and ShuffleNetV2 buffer comparison
were run once and passed. Decoder and checksum implementations were unchanged;
their previous evidence was reused.

The current AArch64 object build passes. It contains 6,776 bytes of text and read-only
data, no writable static data, and references the checksum verifier, decoder, encoder, `memcpy`,
and `memset`. Its SHA-256 is
`8793b3c3c6daea0eb6d0e13de0c64adc7aeaaf79cb391916bf96df1fc9032387`.
The six-slot plan occupies 88 bytes and the general plan 264 bytes. The
isolated build target is `nbg-assembly`.

The general interface has its own comparison with the pinned SDK loader.
[nbg-layout-reference.py](../checks/nbg-layout-reference.py) runs the loader
in an instruction emulator with every input and output bound, stops at the
intercepted submission, and compares every native allocation, minimum size,
relocation and guard page with the SDK buffers after the address and
completion-event adjustments. It passes for the two-input, two-output UINT8
graph, its INT8 per-channel variant, the 8×8 convolution and the compiled
mimus query projection, whose three operations are a transpose step, a
neural-network operation and a second transpose step with INT16 tensors.
The check also refuses a buffer count that differs from the plan. Results are
under `~/.cache/sunstoneos/build/audit-npu-runtime/evidence/layout-*/`; the
existing 120-case and directory checks and both saved SDK comparisons were
rerun after the change and pass.

The focused checks are [nbg-assembly.c](../checks/nbg-assembly.c),
[nbg-directory-assembly.c](../checks/nbg-directory-assembly.c), and
[nbg-assembly.py](../checks/nbg-assembly.py). Build the host shared library from
`nbg_assembly.c`, `nbg_model.c`, `nbg_relocation.c`, and `nbg_checksum.c` with
`-shared -fPIC -Iinclude`.
The Python check takes the model, shared library, saved capture directory, and
a new output directory. It uses [host dependencies](../checks/host-requirements.txt).
Current results, artifacts, commands, and hashes are under
`~/.cache/sunstoneos/evidence/nbg-directory-assembly-20261016/`.
Earlier evidence after the decoder view grew resides under
`~/.cache/sunstoneos/evidence/nbg-directory-native-20261016/`.
Ring-specific evidence remains under `~/.cache/sunstoneos/evidence/nbg-rings-20261016/`.
The initial implementation
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
