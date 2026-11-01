# Dependent NPU command diagnostic

**Implemented; physically unqualified.** This finite diagnostic appends two
passes to the 1,000-job resident-queue latency measurement. The reader is
assumed to know DMA buffers, interrupt completion and quantized tensors.

The producer is the admitted `conv12` convolution. A second convolution reads
its 400-byte output directly and produces 128 bytes. The runtime loader aliases
the consumer input to the producer output before binding the resident client.
It checks tensor dimensions, element format, affine quantization and allocation
size. The consumer uses the producer's existing writable device mapping.

The first pass registers both model commands, submits two records under one
interrupt lock and waits for the second fence. Both records are pending before
interrupt delivery resumes. The second pass joins the two command streams while
preserving their flush and stall operations, and emits one final completion
event. Both passes prepare buffers once, perform no CPU tensor copy between
stages and acquire outputs after the final command. The joined stream also
requires no CPU wake between stages. Both stage outputs are checked after the
final completion; no intermediate CPU observation is claimed.

The immutable `npu_dependent_return_profile` identifies the edge from slot 0,
output 0 to slot 1, input 0. The latency startup marker requires 1,004 jobs and
1,005 interrupt completions: one warmup, 1,000 measured jobs, two FIFO records
and one joined-stream record, plus bootstrap completion. The log records each
pass's interval, records submitted before waiting, completed records, output
checks, digest and first mismatch. It also records both CPU and device addresses
of the shared boundary tensor. It remains within the finite 16 KiB log.

## Compiler input and reference

[The fixture generator](dependent-fixture.py) creates a `1 × 4 × 10 × 10`
input and a `1 × 2 × 8 × 8` output convolution. Its input consists of the
producer's saved quantized output, dequantized with the producer's scale and
zero point. [The compiler recipe](dependent-pipeline.sh) retains that input
quantization during export. The compiled input descriptor matches every
representation field of the producer output: dimensions `[10,10,4,1]`, unsigned
8-bit elements, scale bits `1016995358` and zero point `122`.

The host SDK simulator received all 400 producer output bytes unchanged and
produced all 128 consumer reference bytes equal to the quantized CPU reference.
The native decoder and assembler accept the consumer. These checks establish
compatible model bytes and references; physical DMA behavior remains open.

| Artifact | SHA-256 |
| --- | --- |
| Consumer model, 1,928 bytes | `a0c56f19dc698d4af4a59593f399f83429c0a01cb6761efbd0688b70ef27fac0` |
| Producer output / consumer input, 400 bytes | `a22da3d747cb4e689785aa2b0d79feef8a96957cfadc8d3a188a7f714cf8419f` |
| Consumer reference, 128 bytes | `cd3a38b91bed91345bb87ee23e4f2d81ceda5d6828a958ab77f0c812ba96ac6a` |

The models and raw evidence stay outside tracked source under
`~/.cache/sunstoneos/build/npu-fastpath-1101/chain-compiler-work/consumer/`.
`chain-reference.json` records the compared files. The producer is pinned in
the [runtime catalogue](../../tools/npu-session-models.json); its cached compiler
directory is `~/.cache/sunstoneos/evidence/npu-models-20261018/conv12/`.

The proprietary compiler and SDK run only on the development host. Their URL,
versions, image hash and license paths are pinned in the
[compiler lock](../../tools/npu-compiler.lock.json) and described in the
[compiler record](../../docs/npu-compiler.md). The consumer NBG runs on the NPU;
its source is the fixture generator in this repository. No compiler or SDK
library is linked into FreeRTOS. Compilation used an 8 GiB RAM limit with no
additional swap, four CPUs, a network-disabled container and no board devices.
The measured maximum child RSS was 4,283,664 KiB.

The coordinator builds with `--latency queue --dependent-chain` and an artifact
map that includes `conv12-consumer`. [The chain model list](chain-models.json)
is separate from the matched legacy/queue model list. This diagnostic requires
the existing finite watchdog, RAM-only session and normal-return checks.
