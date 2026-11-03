# Design: NPU output conformance with the SDK simulator

Status: **implemented** for the simulator against an independent reference;
**host-tested**. The comparison of board output with the simulator is **open**
until a board application produces captures. The conformance record is
[docs/npu-conformance.md](../npu-conformance.md).

The pinned SDK simulator models the A733 NPU's revision 9.2 engine and is the
only arithmetic oracle the project has for compiled models
([npu-compiler.md](../npu-compiler.md)). The conformance harness,
`tools/npu-conformance.py`, runs on the host, contacts no USB device or board,
and makes two comparisons:

1. **Simulator against an independent reference** for every catalogue model
   with a pinned source and every input vector. Implemented.
2. **Board against the simulator** on saved board captures. Open.

## Native delivery of tensor bytes

Reserved case 0 of [npu-session.py](../../checks/npu-session.py) runs the five
catalogue models through the session runtime in QEMU. No host check compares
native allocations and relocations with the SDK loader. The QEMU register
model's `execute()` in `checks/npu-session.c` derives an output from the input
bytes only when the descriptor that writes a model output also reads a model
input. When it reads intermediate memory, it writes a constant.

- The 8×8 convolution and both two-convolution graphs have native input
  delivery checked for the first bytes of each input, up to the size of the
  output the descriptor writes.
- ShuffleNetV2 and the mimus query projection have no native input check.
  ShuffleNetV2's first descriptor writes intermediate memory. The mimus graph
  has a transpose, the neural operation and a second transpose, with
  intermediates in internal SRAM, so its output descriptor reads SRAM.

Native input delivery for those two models stays open. It needs `execute()` to
compare, at completion, every byte of each model input that no
output-producing descriptor reads with the bytes the client submitted.

## Models, inputs and references

The model set is the session catalogue
([npu-session-models.json](../../tools/npu-session-models.json)). Each model
gets five base vectors of raw tensor codes, so the harness performs no
quantization: the catalogued reference input, every code at the zero point,
every code at the minimum, every code at the maximum, and one seeded vector.
Seeded bytes are SHA-256 in counter mode over the model name, the vector index
and, for input-line vectors, the line index. A base vector equal to an earlier
one runs once; ShuffleNetV2's input zero point equals its minimum code.

`tools/npu-conformance-references.json` fixes one reference per model and pins
the SHA-256 of every file the reference reads:

| Model | Tensor types | Reference | Pinned files |
| --- | --- | --- | --- |
| 8×8 convolution | UINT8 | Compiler quantized CPU inference | Imported model JSON and data, `.quantize` |
| Two-convolution, UINT8 | UINT8 | Compiler quantized CPU inference | As above |
| Two-convolution, INT8 | INT8 | Compiler quantized CPU inference | As above |
| mimus query projection | INT16 | Exact integer accumulator with the bias held as an int32 code, rounded once to INT16 | Weight codes and scales, bias, study metadata, `.quantize` |
| ShuffleNetV2 (published SDK example) | UINT8 | None | None |

- **Compiler inference.** The harness feeds the compiler the dequantized codes
  and requires its dumped quantized input to equal the vector.
- **mimus.** The harness carries its own copy of the int32-bias rule of the
  mimus study's `npu-mimus-linear.py` (`int32_bias` in the harness). The rule
  sums raw input codes, so the harness requires input zero point 0 and the
  study's input scale. The weights come from the study's pinned weight-code,
  weight-scale and bias files.
- **ShuffleNetV2.** A different compiler produced it, and no source model or
  quantize file exists. It takes part in the board comparison only, and no
  arithmetic claim rests on it.

Every comparison requires bit-for-bit equality. A difference fails the run; it
becomes a tolerance only as a reviewed per-model entry with a computable
acceptance rule. An output whose codes all sit at the clamp limits is marked
**saturated** and does not count toward arithmetic coverage. ShuffleNetV2's
outputs are saturated for its catalogued, zero and seeded vectors. The report
lists, for each model and vector, the number of differing codes, at most 16 of
their positions and the largest code difference.

## Execution

- **Runner.** One simulator runner exists:
  [npu-conformance-runner.c](../../tools/npu-conformance-runner.c), the
  generated ovxlib program of the 8×8 convolution built against the simulator
  libraries. The harness generates each model's bindings from the catalogue's
  tensor descriptors and runs every vector as successive jobs on one prepared
  graph. A model that the runner cannot bind has no simulator output and fails
  the run.
- **Container.** A run starts one Docker container, named for the run, from the
  loaded image ID in [npu-compiler.lock.json](../../tools/npu-compiler.lock.json),
  with no network, no devices, every capability dropped, six CPUs and a 14 GiB
  memory limit with no swap. One worker pool of at most six processes, under
  `nice -n 10`, runs runner builds, runner invocations and reference
  inferences. Each runner invocation sets `VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B`
  and `VSIMULATOR_SHADER_CORE_COUNT=1`, as the fixture pipelines do.
- **Runner lanes.** Each runner process holds about 4.4 GiB from start-up, so
  at most two run at once: the longest expected invocation alone in one lane,
  the others in turn in the second. The limit of 14 GiB is the measured peak
  plus half, rounded up.
- **No result cache.** A cold run takes 46 s, so the run repeats every job
  and caches nothing.
- **Board captures.** One directory per board session holds `capture.json` and
  the output files. `capture.json` records the image SHA-256, the source hashes
  of the session runtime, service and driver, and the cache maintenance form
  the runtime uses (whole buffer or written range). It has one row per job:
  model, NBG SHA-256, input SHA-256, each output file's SHA-256 and size, job
  order, completion mode (interrupt or polled), interrupt count and session
  fault state. Output files use the simulator entry's layout and names, so the
  board comparison reuses the simulator comparison code. No capture code
  exists. The producer is the board application listed under remaining work in
  [npu-session.md](../npu-session.md).

## Job order and cache visibility

Each model's job order is fixed so that a board application has a fixed
sequence. Base vectors run in canonical order, and the report lists every
consecutive pair with equal expected outputs; in the catalogue every pair
differs. On each pinned-compiler model, one input-line vector follows for every
64-byte line of every input. It equals the preceding job's vector except in
that line, which gets seeded bytes. It runs in the simulator only and needs no
reference. Its expected output must differ from the preceding job's; otherwise
the line is reported uncovered and the run fails. The session runtime aligns
every allocation to 4 KiB ([npu-session.md](../npu-session.md)), so lines
counted from a tensor's first byte are physical cache lines. The last job of
each model's invocation repeats a base vector with an unsaturated output, and
the two outputs must be equal; this detects simulator nondeterminism.
ShuffleNetV2 repeats the vector with every code at 255.

On the board, a pair whose outputs differ on a line detects only the loss of
both output cache operations, or a wrong memory attribute on the output
buffer: `a733_npu_memory_publish` cleans and invalidates every output buffer
before each job and `a733_npu_memory_finish` invalidates it again, so either
masks a missing other. A missing input clean changes the output only while the
new input bytes are still dirty in the CPU cache when the NPU reads them.
The record states "no staleness observed" per model and tensor type for the
maintenance form and runtime source hash in the capture. A change of form,
such as narrowing the clean to the written range, needs another capture.
Per-operation coverage comes from the host check
[npu-memory.c](../../checks/npu-memory.c), which asserts the operation kind for
each buffer role and the operation counts per job.

## Operator coverage

The inventory is each model's hardware layer names, as printed in the layer
table of the SDK inspector `nbinfo` 2.0.0.b7fb7b62, its NBG operation kinds and
its tensor types.

- **Simulator against reference:** the four pinned-compiler models, whose NBG
  format `0x20000` has a layer table that `nbinfo` reads.
- **Native:** output delivery for all five models; input delivery as stated
  above. No arithmetic.
- **Board:** unsaturated coverage of the four pinned-compiler models.

Layer names found only in ShuffleNetV2 are reported uncovered in both
comparisons. If `nbinfo` cannot read the layer table of ShuffleNetV2's format
`0x1001e` header, the report lists its layers as unknown.

## Failure handling

The run fails on any runner or reference failure, a timeout, a wrong output
size, a consumed input that differs from the vector, a missing or changed
pinned identity, a repeated vector with different outputs, or any mismatch.
It reports every model's state and no partial pass. Timeouts per step:

- **Runner build:** 120 s per model.
- **Reference inference:** 120 s per vector.
- **Runner invocation:** the one-vector limit, which covers graph
  preparation, plus four times the measured per-vector time for each further
  vector. The one-vector limit is 120 s for the convolution graphs and 600 s
  for mimus and ShuffleNetV2.
- **Container:** the sum of the run's step limits plus 120 s for start. On
  expiry the harness runs `docker kill` and `docker rm -f` on the container
  and reports a failure.

## Qualified figures

| Quantity | Value | Evidence |
| --- | --- | --- |
| Cold run, wall time | 46.0 and 46.4 s | `npu-conformance.jsonl`, `npu-conformance-host-run` |
| Simulator jobs | 84: 24 base vectors, 55 input-line vectors, 5 repeats | same |
| Container memory peak | 9,308 MiB, under the 14 GiB limit | same |
| Lines covered by differing job pairs | all 55 input lines and 52 output lines | same |
| Simulator against reference | bit for bit on every base vector of the four pinned-compiler models | `npu-conformance.jsonl`, `npu-conformance-references` |

## Limitations and open work

- **Board comparison.** The expected result is in the record
  `npu-conformance-board-comparison`: each job completes through the interrupt
  path with one NPU interrupt, and board outputs equal the stored simulator
  outputs. It needs a board application that produces captures.
- **Native input check.** `checks/npu-session.c` needs the input comparison
  described above for ShuffleNetV2 and the mimus query projection.
- **mimus rule.** The int32-bias rule is written inline in the mimus tool's
  `compare` step and copied into the harness; moving it into a function there
  removes the copy.
- **Duplicate comparisons.** `checks/npu-conv-reference.py` and
  `checks/npu-dual-reference.py` keep simulator comparisons that this harness
  covers for the same fixtures and inputs.
- **Not covered.** The NPU metrics of section 3 of
  [performance-architecture.md](../performance-architecture.md) are counted in
  the session and large-graph host models.

Left out as a standing constraint:

- **A QEMU comparison** would re-prove the transport that reserved case 0
  proves, with a second register model.
- **A host model of NPU arithmetic** would duplicate the simulator and need its
  own qualification.
- **A generated graph for layers found only in ShuffleNetV2** would not show
  that the published graph's descriptors, from another compiler, encode those
  layers the same way.
- **Input-line vectors for ShuffleNetV2** would add about 5.5 hours of
  simulation.
- **Dataset accuracy and simulator timing.** Conformance compares bytes, and
  host cycle counts do not predict board timing.
- **A search for an unsaturating ShuffleNetV2 input** has no stopping rule; its
  class meanings are undocumented.
