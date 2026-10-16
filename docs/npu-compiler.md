# Compile a numerical A733 NPU fixture

The A733 host compiler is **downloaded and exercised**. A deterministic convolution
compiles to a 1,928-byte NBG file and passes a simulator comparison against the
compiler's quantized CPU reference. Its floating-point CPU inference also matches
an exact integer calculation. These are host results; native assembly and physical
NPU execution remain outstanding.

The fixture uses format `0x20000`, which the [native decoder](nbg-model.md)
now parses. The Python inspector remains limited to `0x1001e`. The convolution
supplies a numerical workload for extending the loader and for eventual
hardware qualification. The existing ShuffleNet fixture remains a
reference for format `0x1001e`.

## Pin the host compiler separately from the board runtime

The [published setup instructions](https://docs.radxa.com/en/cubie/a7s/app-dev/npu-dev/cubie-acuity-env)
link the A733 compiler archive. The downloaded package contains ACUITY 6.30.22
and Vivante IDE 5.11.0. [npu-compiler.lock.json](../tools/npu-compiler.lock.json)
records the download URL, archive sizes and hashes, image configuration identity,
loaded image identity, target, and license locations.

The outer archive is 2,903,273,080 bytes. Its nested ZIP contains a 7,883,452,416-byte
Docker image archive. Extraction verified the selected ZIP entries' CRCs; SHA-256
identities are recorded separately. Docker 29.1.3 loaded the image successfully.
The loaded image ID differs from the archive configuration's hash, while its
configuration and root-filesystem layer identities match. Both identities are
retained in the lock file. Use the loaded image ID when invoking Docker.

Downloaded archives, original notices, and image metadata remain under
`~/.cache/sunstoneos/sources/npu/acuity-download/`. Compiler component hashes and
Python dependency versions are in the evidence directory below. The image is an
x86-64 Linux host tool; the board runtime remains FreeRTOS.

The compiled target is `VIP9000NANODI_PLUS_PID0X1000003B`, with
`VSIMULATOR_SHADER_CORE_COUNT=1`. This selector agrees with the product ID in the
board driver's feature database. Physical identity still requires confirmation.

## A small convolution has an exact arithmetic reference

[npu-conv-fixture.py](../tools/npu-conv-fixture.py) generates an ONNX model using
opset 11 and IR version 7. It uses NCHW tensor order: batch, channel, height, width.
The operation has no padding, a unit stride, and two output channels:

| Tensor | Shape | Values |
| --- | --- | --- |
| Input | `1 × 3 × 8 × 8` | 192 |
| Weights | `2 × 3 × 3 × 3` | 54 |
| Bias | `2` | 2 |
| Output | `1 × 2 × 6 × 6` | 72 |

Input and weight values are integer multiples of one eighth. Bias values are
one quarter and minus one half. The generator computes the convolution with
integer products and sums, then divides by 64. Every resulting value is exactly
representable as a 32-bit float. This reference does not depend on a framework's
convolution implementation.

The generated input metadata selects the NumPy input directly, retains NCHW
order, and disables channel reversal. No image conversion or resizing is needed.
Quantization uses unsigned 8-bit asymmetric affine parameters and one calibration
input. This single synthetic input qualifies the fixture workflow; it does not
establish a calibration procedure for application models.

## Saved host results agree

One floating-point inference preserved all 192 input values and matched all 72
exact outputs. One quantized CPU inference and one execution of the emitted NBG
in the SDK simulator produced identical output bytes. The simulator also received
the same 192 quantized input bytes.

| Quantity | Result |
| --- | --- |
| Input scale and zero point | `0.01568627543747425`, `127` |
| Output scale and zero point | `0.02028186246752739`, `108` |
| Simulator versus quantized CPU output | All 72 bytes equal |
| Maximum dequantized error against exact float output | `0.015134785324335098` |
| Root-mean-square dequantized error | `0.0065643259496975725` |

[npu-conv-reference.py](../checks/npu-conv-reference.py) compares the saved inputs
and outputs, checks the target ID, and records their hashes. It executes neither
the compiler nor device commands. Its artifact comparison passed once after the
inline comparisons were consolidated into this reusable check. Compiler execution
was not repeated for that consolidation.

The SDK export generated and executed a temporary graph while creating the NBG.
That export run used its own initialization data and is not the numerical oracle.
The separate NBG simulator invocation used the fixture's quantized input and
produced the compared output. Host timings do not predict board performance.

## Reproduce the pipeline inside the pinned image

[npu-conv-pipeline.sh](../tools/npu-conv-pipeline.sh) records the exercised commands:
generate, import, create input metadata, infer float output, quantize, infer
quantized output, export, build the simulator wrapper, execute the NBG, and compare.
It requires a new output directory and stops at the first failed command. Each
compiler or simulator command has a timeout. The shell wrapper passed a syntax
check and command review; the complete wrapped invocation has not been repeated.

Load the hash-verified image archive with `docker load --input IMAGE_TAR`.
Use the loaded image ID from the lock file. Mount the repository at `/source`
read-only and a dedicated writable cache directory at `/work`. Invoke:

```sh
bash /source/tools/npu-conv-pipeline.sh /work/conv3x3
```

The recorded container had no network, no board devices, four CPUs, an 8 GiB
memory limit, and a 256-process limit. All capabilities were dropped except
`CHOWN`, used to assign and restore ownership of the dedicated output directory.
No host IPC namespace was shared. The compiler files reside under `/root`, so the
recorded invocation used container root. Restore output ownership to the host
account before removing the container.

Raw logs, source-generated artifacts, original license text, component identities,
and comparisons are under `~/.cache/sunstoneos/evidence/npu-compiler-20261016/`.
The initial fixture-generation attempt failed because container root lacked write
permission on the host-owned output directory. Assigning that directory to
container root resolved it; model generation had not written any files.

## Native assembly matches the saved SDK preparation

The model SHA-256 is
`db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca`.
The SDK inspector reports one convolution, one initialization operation, a
192-byte neural descriptor, 256 coefficient bytes, zero working-pool bytes,
and `0x18b00` bytes of internal SRAM. Logical input/output sizes are 192 and 72
bytes; the inspector reports a 128-byte output memory reservation.

The image's SDK version file advertises NBG `0x10020`, but this export emits
`0x20000`. The emitted header uses a different section-directory structure.
Its stored checksum is at byte 12; zeroing that field and applying the existing
whole-file CRC method does not reproduce the stored value. The
[directory investigation](nbg-directory.md) establishes the fixture's record
layouts and matches its checksum over normalized section payloads. That checksum
excludes the header and directory. Native directory decoding and version-specific
accessors now pass host checks.
The [checksum verifier](nbg-checksum.md) now passes primary and section checks.
A [saved SDK capture](nbg-conv-loader.md) records the model's allocation and
command requirements for native assembly.

The host inspector returned `unsupported NBG version 0x20000`, as intended.
The [native assembler](nbg-assembly.md) now handles the empty working pool and
matches the saved SDK allocations and command image after address and
completion-event adjustments. The older format still passes its focused checks.
Physical inference, complete descriptor admission,
and all other NPU capabilities remain required by [issue 9](https://github.com/pedapudi/sunstoneos/issues/9).
