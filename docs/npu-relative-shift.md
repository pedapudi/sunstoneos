# Fixed FP32 relative-attention shift

This path is for developers extending the native NPU application. It executes a
fixed, weight-free subgraph from mimus through the existing copied-input client
and interrupt service. Host callers provide input during the retained session,
read copied output in chunks, and close the NPU service. USB remains available
after healthy CLOSE. Physical execution and confinement remain unqualified.

The model reproduces `mimus/train/conformer.py`'s `rel_shift` followed by the
attention caller's crop, at mimus revision
`46d8e64aa5eb841f19c4205113b030e501b72c7a`. With four heads and sequence length
128, input is FP32 `[4,128,255]` and output is FP32 `[4,128,128]`.
The exact relation is `out[h,i,j] = input[h,i,127-i+j]`. There are no weights,
rounding operations or precision conversions. The C++ encoder fuses relative
indexing into attention; this application establishes native operator coverage
and does not change or accelerate that encoder. Larger mimus shapes and complete
model execution remain outstanding.

## Compiled model and target

The registered `relative-shift128` model is the pinned compiler's 3976-byte NBG,
SHA256 `327b9dbcaa3cf1b387a7f21e9bd4fef132237e55bb7108b6483a42c54462de69`.
The compiler emits INIT, one shader operation (SH), and three neural-transpose
operations (NT). Two reshapes require no separate device operation. The model
uses 576 bytes of NT descriptors, 512 bytes of shader instructions, 1047808 bytes
of external working pool and 524288 bytes of internal VIP SRAM. It emits no
coefficient payload. One command contains all four device operations and one
completion event; the existing bootstrap remains a separate command.

The pinned feature database's target `0x1000003b`, revision `0x9202`, reports
eight NN engines and zero tensor-processor (TP) engines. The compiler SDK's
`gcoVX_GetNNConfig` query independently reports zero TP and TP-lite engines and
one shader engine. Generic NBG support for a TP section does not establish a TP
engine on this product. Sources, raw query output and the compiler configuration
are retained in `~/.cache/sunstoneos/evidence/npu-tp-proposal-20261019/`.
The anchors are `vip_feature_database.h` in the pinned board SDK and
`gc_hal_base.h` in the pinned compiler SDK; `source-anchors.json` and
`target-feature-corroboration.json` identify their revisions and hashes.

The companion assembler accepts only the observed SH/NT structure. It preserves
existing section bounds and checksums, validates every patch before writing
buffers, and reproduces the SDK's relocation and minimum padding. The native
wrapper admits only the complete registered model hash. Shader instructions and
control fields remain opaque authenticated compiler output; this is not a
safety validator for arbitrary shader programs or caller models. Existing NN
catalogue entries and the ordinary small-tensor profile remain available.

## Build and memory ownership

Configure `apps/npu-convolution-single-core` with the accepted AArch64 toolchain,
FreeRTOS and TinyUSB source pins. Select all three options:

```
-DA733_NPU_INTERRUPT=ON
-DA733_NPU_RUNTIME_INPUT=ON
-DA733_NPU_LARGE_GRAPH=ON
```

Set `A733_NPU_MODEL`, `A733_NPU_INPUT` and `A733_NPU_REFERENCE` to the registered
NBG and its accepted build fixtures. Build target
`a733-npu-convolution-single-core`. The fixed large profile requires retained
single-core supervision and rejects finite-return or extended-memory profiles.
`tools/npu-relative-shift-fixture.py OUTPUT` reproduces the source ONNX graph,
FP32 input, text input and exact reference in the pinned compiler's Python
environment. It does not compile another NBG or replace the accepted artifact.

Ordinary BSS and stacks must end before `0x41400000`. CPU staging occupies
`0x41400000..0x41480000` for input and `0x41480000..0x414c0000` for output.
Neither staging range appears in the NPU page tables. The explicit 4 MiB pool
is `0x41600000..0x41a00000`, within the existing CPU write-back mapping and
disjoint from the uncached block at `0x41c00000` and session at `0x41f00000`.
The default reservation remains 2 MiB; extended 768 MiB mode is not enabled.

| Buffer | CPU/physical start | NPU start | Allocation bytes |
| --- | --- | --- | ---: |
| NT descriptors | `0x41600000` | `0xe0000000` | 4096 |
| Shader instructions | `0x41601000` | `0xe0002000` | 4096 |
| Working pool | `0x41602000` | `0xe0004000` | 1048576 |
| Command | `0x41702000` | `0xe0105000` | 524288 |
| Input | `0x41782000` | `0xe0186000` | 524288 |
| Output | `0x41802000` | `0xe0207000` | 266240 |
| Bootstrap | `0x41843000` | `0xe0249000` | 524288 |
| NPU tables | `0x418c3000` | not payload-mapped | 20480 |

The actual allocator acquires 2916352 bytes once and retains ownership through
CLOSE or failure. Guard pages separate device ranges. Input and output include
the SDK's minimum 64-byte tail; the complete controller command fetch remains
mapped and padded with END instructions. The shader's declared 512-byte payload
contains 368 instruction bytes and a zero tail. These sizes reproduce captured SDK preparation. Complete physical access
extents remain subject to confinement and board qualification.

Before each job, the native wrapper clears the entire external pool while the
service is ready and memory policy is idle. Existing publication cleans shader
instructions and input, and cleans/invalidates writable pool and output. Finish
invalidates writable buffers before the client copies output. A second input is
not accepted while output remains borrowed by the chunk protocol. Faults retain
allocations and stop further admission.

## Caller protocol

The separate USB identity is bcdDevice `0x0105`, serial
`sunstoneos-a733-npu-large`. Its version-2 frame magic is `0x415337334e504c32`.
The geometry marker identifies 64-byte request headers, 512-byte chunks,
522240-byte input and 262144-byte output. Legacy small-tensor frames retain their
existing identity and layout.

A request follows BEGIN, strictly ascending UPLOAD chunks, COMMIT, READ chunks,
and FINISH. BEGIN fixes the monotonically increasing request ID and a deadline
of at most five seconds, capped by the session deadline. COMMIT requires every
input byte and hashes the owned staging copy before release-publication to the
unchanged native client. READ copies at most 512 output bytes into a response.
Only the full response's USB transmit callback advances the delivered offset.
FINISH requires all output bytes delivered; its full callback releases the
mailbox for the next input. CLOSE requires an idle mailbox and the existing
healthy NPU stop receipt. Stale identities, offset gaps, early completion,
backward time and expiry retain failure.

The host's production capture durably records response fragments and each output
chunk before requesting more. It verifies the full input and output digests.
An optional caller oracle compares raw FP32 output; ordinary arbitrary input
does not require a canned reference. The USB owner polls eight bounded times
before yielding one scheduler tick. Physical host I/O and durable-storage latency
still must fit the bounded request; the modeled check is not a throughput claim.

## Offline evidence

`~/.cache/sunstoneos/evidence/npu-relative-shift-20261019/` retains the selected
image and actual linked dependency closure. The checks cover all six assembled
allocations against SDK bytes, thirteen malformed assembler cases, actual fixed
allocation and 707 payload mappings, per-job clearing and cache-policy calls,
chunk/callback refusal cases, and two runtime inputs followed by healthy CLOSE.
The paced task check completes each modeled request in about 1.57 seconds.
CPU translations, physical cache effects, scheduler transport and NPU replies
are modeled in those linked checks.

Separately, the pinned host SDK reuses one prepared network and external pool for
two distinct inputs. Both numerical outputs match every FP32 bit of the
relative-shift reference; the values distinguish FP32 from FP16 rounding.
This SDK execution does not establish native inference. Root owns the separate
EL3 table-preparation and linked architecture audit. Existing unchanged
controller, IRQ and mailbox evidence is reused. Board execution, physical shader
and cache ordering, confinement and larger mimus shapes remain pending.

The large-native constructor check requires the linked `relative-shift128`
model identity. The published ShuffleNet image has a different pool layout
and uses `checks/npu-nn-pool-native.py`. The constructor fixture runs the real
local-pool preparation before allocating, matching boot's publication order.
Constructor and corrupted-model cases pass against the rebuilt relative-shift
profile in `~/.cache/sunstoneos/build/npu-resume-1024/relative-shift/`.
The wrong-profile refusal is recorded separately in `large-fixture-checks.json`
under that build's parent directory. Existing runtime-copy results retain
their original image identities and modeled-output scope.
