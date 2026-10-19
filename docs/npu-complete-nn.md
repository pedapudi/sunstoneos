# Complete weight-bearing NN graph

The retained native NPU application supports one complete published
ShuffleNetV2 compiled graph through the existing IRQ service and copied-input
owner. The graph has 87 layers and 125 sequential NN operations, coefficients
and external intermediates. It supplies functional whole-graph coverage; physical
inference and complete NPU or mimus support remain unqualified.

## Admit the original published artifact

The model is the pinned SDK's
`examples/vpm_run/operator/v3/network_binary.nb`, 964224 bytes, SHA256
`fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f`.
Its recorded target is `0x1000003b`, format `0x1001e`, and embedded name is
`ShuffleNetV2_uint8_NCHW`. Input is raw UINT8 `[224,224,3,1]`, 150528 bytes;
output is raw UINT8 `[2,1]`, two logical bytes. Class meanings are undocumented.
The network name does not establish standard ImageNet classification semantics.

The fixed `nn-pool` catalogue entry authenticates the complete model, including
all coefficients, descriptors, states and relocations. The image selects it at
build time and retains that selection through CLOSE. No executable model upload,
hot switching or arbitrary descriptor admission is implemented.

The existing generic NN assembler constructs the graph without broader opcode
or patch interpretation. Its accepted SDK comparison covers 125 descriptors,
534 descriptor relocations and 125 state-pointer relocations. The command has
a 3056-byte body and 56-byte footer. One completion event follows all 125 NN
operations; the existing bootstrap is a separate command. Generic TP sections,
CPU software operations, dynamic I/O and unsupported patch classes remain refused.

Primary source records are the pinned TIM-VX
`include/tim/utils/nbg_parser/gc_vip_nbg_format.h` and the SDK's `vip_lite.h`.
The former describes operation, tensor, pool and patch records; the latter binds
prepare, input/output, execution and finish lifetimes. Exact pins and hashes are
in `~/.cache/sunstoneos/evidence/npu-complete-graph-proposal-20261019/source-anchors.json`.
The [accepted assembly evidence](nbg-assembly.md) and
[SDK preparation trace](nbg-loader-trace.md) establish the exact model's native
allocation bytes and relocation encodings.

## Use the fixed retained build

Configure `apps/npu-convolution-single-core` with the accepted AArch64 toolchain,
FreeRTOS and TinyUSB pins and these options:

```
-DA733_NPU_INTERRUPT=ON
-DA733_NPU_RUNTIME_INPUT=ON
-DA733_NPU_LARGE_GRAPH=ON
-DA733_NPU_NN_POOL_GRAPH=ON
```

Set `A733_NPU_MODEL`, `A733_NPU_INPUT` and `A733_NPU_REFERENCE` to the exact
registered NBG, published input and recorded two-byte SDK output. Build
`a733-npu-convolution-single-core`. This profile requires retained single-core
supervision and rejects incompatible large-profile selection. It does not enable
extended memory, finite ROM return or a new interrupt route.

The selected workload marker is `runtime-large-nn-pool-irq`. The existing
version-2 large-tensor USB profile binds model hash and geometry 150528/2 in the
prepared image. Input and output are opaque UINT8 bytes with the catalogue's
recorded affine scales and zero points. The [chunked owner protocol](npu-relative-shift.md#caller-protocol)
retains BEGIN, UPLOAD, COMMIT, READ, full transmit callbacks, FINISH and healthy
CLOSE. Optional numerical oracles remain separate from ordinary copied output.

## Keep coefficients and intermediates under one owner

The private constructor acquires 3235840 bytes from the existing fixed 4 MiB
parent at `0x41600000..0x41a00000`. Seven payload buffers and five private NPU
table pages remain reserved throughout the session and on faults.

| Buffer | CPU/physical interval | NPU start | Allocated bytes |
| --- | --- | --- | ---: |
| NN descriptors | `0x41600000..0x41606000` | `0xe0000000` | 24576 |
| Coefficients | `0x41606000..0x416e0000` | `0xe0007000` | 892928 |
| Pool | `0x416e0000..0x417eb000` | `0xe00e2000` | 1093632 |
| Command | `0x417eb000..0x4186b000` | `0xe01ee000` | 524288 |
| Input | `0x4186b000..0x41890000` | `0xe026f000` | 151552 |
| Output | `0x41890000..0x41891000` | `0xe0295000` | 4096 |
| Bootstrap | `0x41891000..0x41911000` | `0xe0297000` | 524288 |
| NPU tables | `0x41911000..0x41916000` | not payload-mapped | 20480 |

The working pool's logical requirement is 1092352 bytes. Coefficient payload
is 892608 bytes with the SDK's 128-byte minimum tail. The command and bootstrap
retain their complete 524288-byte fetch reservations. Input minimum is 150592
bytes and output minimum is 128; callers receive only their logical extents.
There is no shader allocation for this family. Device guard pages and CPU
staging remain absent from payload mappings.

A large-profile 1 MiB model-copy bound accommodates the original NBG. The model
copy is separate from device resources and runtime staging. Root's linked review
places BSS through `0x41338ea0` and the exception stack through `0x41340ea0`, below
staging at `0x41400000`. Code, tables and device data retain the existing CPU
mapping profile; no shared reservation or startup change was needed.

The constructor's actual NPU table builder produces root entry `0x41912001` and
second-level storage at `0x41912000`, with 4 KiB alignment. The focused check walks
all 785 payload pages and guards. This is constructed-table evidence with modeled
CPU translations, not physical translation or device confinement evidence.

Coefficients, NN descriptors, command and bootstrap bytes are immutable and
hashed again at the wrapper's publication boundary. A changed coefficient refuses
before cache publication or device triggering. While memory policy is idle,
each submission clears the entire pool and copies input into its owned device
buffer. Existing cache policy cleans coefficients and other read buffers,
cleans/invalidates pool/output before execution, and invalidates writable buffers
after matched completion. Intermediate storage is not released by layer number.
The next caller input waits until copied output and FINISH are fully acknowledged.
CLOSE requires the existing healthy NPU stop receipt and leaves USB available.

## Numerical and native evidence have different limits

The pinned host SDK/simulator imports and runs the original NBG with the exact
published input. Its consumed input dump matches SHA256
`d38c22c2007db314572c9ddfeb513a9f3d11ec99e8deec0f65bc91481502f1b9`.
Raw output is `[0,255]`, SHA256
`06eb7d6a69ee19e5fbdf749018d3d2abfa04bcbd1365db312eb86dc7169389b8`.
The model's affine interpretation gives approximately
`[-0.2080624551,0.2064369671]`. Only host tensor bindings were adapted and rebuilt;
NBG compilation, calibration, coefficients and quantization were unchanged.

One prepared SDK network also consumes a bitwise-complement input after its
first job and replaces a poisoned output tensor. Both jobs produce the same
saturated output. This supports reused-pool SDK compatibility and actual input
consumption/output replacement, but not input-sensitive classification or
accuracy. No additional shape or input sweep was run.

`~/.cache/sunstoneos/evidence/npu-complete-nn-20261019/` contains the native image,
actual linked object/dependency closure, constructor/cache-policy checks, new
admission refusals, paced two-input/copied-result/CLOSE check, production host
capture, compatibility checks and Astra review. The linked checks model hardware
service replies, IRQ completion, numerical outputs, CPU translation/cache effects
and USB transport. SDK numerical output is injected only at the unchanged copied
completion boundary; it is not native numerical execution.

The combined native mimus application retains its existing 7501968-byte base and
6560592-byte overlay in `apps/bluetooth/recognition-models.json`, with hashes
`f73ccd02…` and`d877604c…`. These contain existing F32/I8 tensors and per-row
activation rules. Their historical recognition parity does not establish NPU
compatibility. The separate 437947416-byte FP32 host punctuation checkpoint is
not the current combined-native model. This SDK graph does not replace either
mimus model or change its quantization.

Physical NN computation, SRAM routes, cache/IRQ behavior, confinement and recovery
remain pending. Public records and exact model trust do not establish every
microcoded descriptor field, complete prefetch extent or firmware dependency.
