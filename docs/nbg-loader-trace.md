# Complete model relocation is reconstructed on the host

The host reference check reconstructs a pinned model's relocated neural
descriptors and command stream. Both match the SDK at two synthetic device
allocation bases. The reconstruction uses the native address encoder, including
its command-pointer encoding. A native FreeRTOS model loader and physical NPU
execution remain outstanding.

This document assumes familiarity with binary formats, device addresses, and
command buffers. A neural descriptor contains one compiled neural operation.
The command stream programs registers and points the NPU at those descriptors.

## The trace stops before device submission

[nbg-loader-reference.py](../checks/nbg-loader-reference.py) maps the pinned
`libNBGlinker.so` in Unicorn. It invokes library initialization, model creation,
preparation, tensor-buffer creation and binding, and asynchronous triggering.
It intercepts the HAL submission call and stops without returning a submission
result. No model executes on a device or an arithmetic simulator.

The synthetic HAL supplies allocations, copies, zeroing, single-threaded mutex
operations, and feature queries. It records task creation and command-buffer
registration. It provides no device interface or host system-call bridge.
Unknown imports stop execution. Each public API call has a five-second and
five-million-instruction limit. CPU allocations occupy a bounded 32 MiB region;
device addresses are separate synthetic values. Freed allocations are not reused.
This environment establishes loader behavior under its stated conditions.
It does not model cache coherence, scheduling, device memory faults, or inference.

Feature responses come from the pinned driver's A733 entry: product
`0x1000003b`, revision `0x9202`, eight neural units, 512 KiB internal SRAM,
128-byte SRAM width, and 32-bit device virtual addresses. The database response
describes one NPU engine and one logical device. These remain reference values,
not measurements from the attached NPU.

The SDK library and model identities are the same as those in
[the address relocation record](nbg-relocation.md) and [NPU investigation](npu.md).
The check refuses any other SDK or model hash. Driver references are
`vip2/inc/vip_drv_share.h`, `vip2/inc/vip_feature_database.h`, and
`vip2/vip_drv_context.c` at board-kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
The SDK public header defines the 48-byte tensor-creation parameter structure.
The companion HAL library's task-creation and property-setting disassembly
establishes the simulated task registration interface.

## Source resolution depends on the destination operation

For the inspected format `0x1001e` neural operations, the host reconstruction
resolves these references independently of the SDK's patch routines:

| Patch destination and source | Address calculation | Encoding |
| --- | --- | --- |
| State word, source 0 | Neural-descriptor allocation base plus current operation index times 192 | Preserve low six flag bits when transformed |
| Neural descriptor, source 1 | Bound input allocation base plus source offset | Full word or shifted address |
| Neural descriptor, source 2 | Bound output allocation base plus source offset | Full word or shifted address |
| Neural descriptor, source 3 | Working-pool base plus source offset | Full word or shifted address |
| Neural descriptor, source 5 | Coefficient allocation base plus indexed coefficient-table offset plus source offset | Full word or shifted address |
| Neural descriptor, source 8 | Internal-SRAM base plus source offset | Full word or shifted address |

The shifted encoding preserves destination bits 31:26 and stores address bits
31:6. A transformed command pointer instead preserves destination bits 5:0
and stores the aligned address directly. The serialized transformation field
alone therefore does not select an encoding; patch kind and source context
also matter.

The model contains 125 neural operations. They use 534 descriptor relocations
and 125 command-pointer relocations. The SDK omits the model's initialization
operation and its one relocation from the captured command body. That observed
omission does not establish that native cold initialization may omit the
corresponding hardware setup.

The host inspector's generic name for source 5 does not imply a fixed payload
section. In the inspected neural operations, that index selects a coefficient
table entry. Other operation types need separate interpretation and evidence.

Input and output addresses remain unpatched after the public binding calls.
The SDK applies them during triggering, before the intercepted submission.
The reconstruction checks both the earlier descriptors and the final descriptors.

## Captured allocations expose padding and completion requirements

| Allocation | SDK-requested bytes | Captured contents |
| --- | --- | --- |
| Neural descriptors | 24,000 | 125 descriptors of 192 bytes |
| Coefficients | 892,736 | 892,608 payload bytes plus 128 extra bytes |
| Working pool | 1,092,352 | Model-specified scratch allocation |
| Command buffer | 3,112 | 3,056-byte registered body plus 56 bytes |
| Input tensor | 150,592 | 150,528 logical bytes plus 64 extra bytes |
| Output tensor | 128 | Two logical bytes; larger than the inspection tool's 64-byte size report |

The command body consists of a generated 48-byte prefix and concatenated
per-operation state records with relocated descriptor pointers. The 56 bytes
after the registered body contain generated commands and trailing zeros in
the synthetic allocation. The coefficient payload is copied without changes.

The generated completion word is `0x44`: event 4 with the processing-engine
source bit. The [native controller](npu-controller.md) currently requires event 0.
Its command-fetch reservation is also larger than the SDK allocation captured
here. Native assembly must generate a compatible completion sequence and
reserve the full fetch span before publication. Captured SDK bytes cannot be
submitted directly through the current controller contract.

## Focused comparison and qualification limits

The final check passed at device allocation bases `0x30000000` and `0xe0000000`.
The second base exercises addresses with bit 31 set. Each case compares all
neural-descriptor bytes, every command-buffer byte, and the coefficient payload.
It checks 659 active relocations, deferred input/output binding, and the
registered command-body length. The remaining initialization relocation is
reported explicitly.

The native encoder passed one updated ASan/UBSan check for bounds, atomic
refusal, every low-six-bit flag pattern, and the maximum batch size. One
AArch64 object build passed with 400 bytes of code, no static data, and no
undefined symbols. Existing controller, service, kernel, DMA, and mimus checks
were not repeated. No board request or kernel rebuild occurred.

The first complete comparison failed because the harness selected the
coefficient pointer as the neural-descriptor pointer. Disassembly identifies
the descriptor CPU pointer at network-object offset `0x2f8`. The corrected
comparison passed at both allocation bases. Earlier trace stages and the
failed comparison remain in the evidence directory.

Local evidence is under `~/.cache/sunstoneos/evidence/nbg-loader-20261016/`.
Final captures are in `final/low-corrected/` and `final/high/`; each contains
stage snapshots, allocation metadata, intercepted submission details, native
reconstructions, and a comparison report. A hash manifest records relevant
inputs and outputs.

Run the check with the pinned SDK, model, host encoder library, and a new
output directory:

```sh
python3 checks/nbg-loader-reference.py /path/to/libNBGlinker.so \
  /path/to/network_binary.nb /path/to/native.so /path/to/new-evidence-directory
```

The host library is built from `drivers/npu/nbg_relocation.c` with `-shared
-fPIC -Iinclude`. Use the dependencies in `checks/host-requirements.txt`.
The optional `--video-base 0xe0000000` selects the high-address comparison.
The tool refuses an existing output directory to preserve captured evidence.

The reconstruction checks most descriptor references as one-byte addresses.
It does **not** establish the complete access extent of each neural operation.
Native model admission still requires descriptor semantics, padded access
bounds, internal-SRAM mapping, and controlled command
registration. Other model formats and operation types remain required.
Numerical output, physical completion, and recovery have not been tested.
The separate [checksum verifier](nbg-checksum.md) now validates the complete
`0x1001e` file before native assembly.
Native [ring validation](nbg-rings.md) also checks full encoded circular spans
and reproduces the saved submission. The host reconstruction itself retains
its original one-byte data-address checks.

The [native assembler](nbg-assembly.md) now reuses the saved capture to verify
allocation planning and command construction in C, including the native
completion event and full fetch reservation. It remains an offline component
until complete command admission is implemented.

The [native decoder](nbg-model.md) now supplies checked C views to this
reconstruction. Its comparison reads these saved SDK snapshots without rerunning
the loader library. Native allocation planning and command assembly remain outstanding.
