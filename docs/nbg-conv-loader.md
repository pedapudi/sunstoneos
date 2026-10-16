# Capture SDK preparation of the numerical convolution

SDK preparation of the [compiled convolution](npu-compiler.md) is **captured in a
host instruction emulator**. The trace stops at the submission call before any
device operation. It supplies allocation and command bytes for extending native
assembly. The [native assembler](nbg-assembly.md) now matches these saved bytes
after address and completion-event adjustments. Physical execution remains outstanding.

The reader is assumed to understand device buffers and address relocation.
[nbg-conv-loader-reference.py](../checks/nbg-conv-loader-reference.py) reuses the
synthetic HAL and bounded calls from the [earlier loader trace](nbg-loader-trace.md).
It initializes the SDK, creates and prepares the network, binds tensors using
the directory format's quantization union, and intercepts submission.
Each SDK call has a five-second and five-million-instruction limit.

## The empty working pool needs no allocation

One trace reached the submission boundary with five device-buffer allocations.
The network's working-pool pointer is zero. Allocation sizes include the SDK's
padding and do not include the native allocator's page rounding:

| Buffer | Payload bytes | SDK allocation bytes | SDK alignment |
| --- | ---: | ---: | ---: |
| Neural descriptor | 192 | 192 | 64 |
| Coefficients | 256 | 384 | 256 |
| Command image | 136 | 136 | 64 |
| Input | 192 | 256 | 64 |
| Output | 72 | 192 | 64 |

The input and output padding agree with the existing native allocation formula:
round logical bytes to 64, then add 64. Coefficients reserve an additional
128 bytes. The earlier inspector's 128-byte output reservation is therefore
insufficient to reproduce this loader allocation; the trace records 192 bytes.

## Commands retain a format-specific state value

The command image contains a 48-byte prefix, 32 bytes for the single neural
operation, and a 56-byte footer. The task property reports an 80-byte command
body. The prefix and footer match the older capture, including SDK completion
event 4. Native assembly uses completion event 0 and must retain its larger
controller fetch reservation.

The operation's state value is `0x0493e040`; the older native template uses
`0x0491e040`. Their difference is bit 17. This observation does not establish
that bit's hardware meaning. The initialization operation also uses layer
identifier `0xffffffff`, compared with `0xffff` in the older fixture.
The initialization command bytes match the existing template.

The SDK patches the neural descriptor's coefficient, input, and output addresses,
plus the command's descriptor address. The descriptor's remaining bytes match
the compiled payload in this capture. Native comparison now passes every allocation.
Do not treat the capture as proof of cold initialization, access bounds, SRAM
routing, or numerical behavior on the board.

## Reuse the captured bytes for native comparison

The pinned loader SHA-256 is
`82f049b0ed0065dd4d443e37eeb1edfcbef24457c9c24e36170d64d5b748ca66`.
The model SHA-256 is
`db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca`.
The [source record](npu.md) and [compiler record](npu-compiler.md) establish their
provenance. The emulated hardware features and allocation policy are inherited
from the earlier synthetic HAL; they are not physical measurements.

Snapshots after creation, preparation, binding, and intercepted submission are
under `~/.cache/sunstoneos/evidence/nbg-directory-checksum-20261016/sdk/`.
The parent manifest records their hashes. `trace.json` records calls, allocations,
bindings, and task properties. Its comparison field remains empty because the
capture preceded native assembly. The assembly comparison verifies its saved
identities and intercepted-submission status, then records a separate result.
The capture reader retains its previous comparison requirement by default;
the convolution comparison explicitly accepts this hash-verified trace without
a previous comparison result.

The native comparison and resource plan are recorded in [nbg-assembly.md](nbg-assembly.md).
Complete descriptor admission, cold initialization, and physical numerical
execution remain required.
