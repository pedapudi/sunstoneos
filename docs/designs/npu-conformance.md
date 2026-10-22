# Design: NPU output conformance with the SDK simulator

Status: **design**; no code exists yet. The conformance record will be
[docs/npu-conformance.md](../npu-conformance.md).

The pinned SDK simulator models the A733 NPU's revision 9.2 engine and is the
only arithmetic oracle the project has for compiled models
([npu-compiler.md](../npu-compiler.md)). The conformance harness,
`tools/npu-conformance.py`, runs on the host and makes two comparisons:

1. **Simulator against an independent reference** for every catalogue model
   with a pinned source and every input vector.
2. **Board against the simulator** on saved board captures. This comparison
   is pending until a board application produces captures.

Native delivery of tensor bytes has partial evidence. Reserved case 0 of
[npu-session.py](../../checks/npu-session.py) runs the five catalogue models
through the session runtime in QEMU, and
[nbg-layout-reference.py](../../checks/nbg-layout-reference.py) shows that
native allocations and relocations match the SDK loader. In the QEMU register
model, `execute()` in `checks/npu-session.c` (lines 136-154) handles each
neural descriptor that writes a model output. When that descriptor reads a
model input, it derives the output from the input bytes found through the
published page tables. When it reads intermediate memory, it writes a
constant and reads no input. Two consequences follow:

- The 8×8 convolution and both two-convolution graphs have native input
  delivery checked only for the first bytes of each input, up to the size of
  the output that the descriptor writes.
- **ShuffleNetV2 and the mimus query projection have no native input check.**
  ShuffleNetV2's first descriptor writes intermediate memory. The mimus graph
  has three descriptors (a transpose, the neural operation and a second
  transpose, with intermediates in internal SRAM), so its output descriptor
  reads SRAM.

Native input delivery stays **pending** for those two models, and the record
does not cite reserved case 0 for them. This package asks the owner of
`checks/npu-session.c` to make `execute()` compare, at completion, every byte
of each model input that no output-producing descriptor reads with the bytes
the client submitted. The runtime treats tensor bytes as opaque, so simulator
bytes in a QEMU check would expose no runtime fault that this check misses.

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
| mimus query projection | INT16 | Exact integer accumulator with the bias held as an int32 code, rounded once to INT16 | mimus model file, weight codes and scales, `.quantize`, `tools/npu-mimus-linear.py` |
| ShuffleNetV2 (published SDK example) | UINT8 | None | None |

- **Compiler inference.** The convolution fixtures' exact integer references
  give float outputs for their dyadic inputs only; they give no output codes
  for other vectors. Compiler quantized inference matched the simulator bit
  for bit on all three fixtures. The harness feeds it the dequantized codes
  and requires its dumped quantized input to equal the vector.
- **mimus.** The harness imports the int32-bias rule from
  [npu-mimus-linear.py](../../tools/npu-mimus-linear.py), under which every one
  of the 1,408 simulator codes matched. That rule is written inline in the
  script's `compare` step today; the owner is asked to move it into a function
  with unchanged behaviour. No separate reproduction of the study runs. If the
  catalogued input equals the study's saved simulator input, the first base
  vector repeats the study's result.
- **ShuffleNetV2.** A different compiler produced it, and no source model or
  quantize file exists. It takes part in the board comparison only, and no
  arithmetic claim rests on it.

Every comparison requires bit-for-bit equality. A difference fails the run; it
becomes a tolerance only as a reviewed per-model entry with a computable
acceptance rule. An output whose codes all sit at the clamp limits is marked
**saturated** and does not count toward arithmetic coverage. Every ShuffleNetV2
output seen so far is `[0,255]`. The report lists, for each model and vector,
the number of differing codes, at most 16 of their positions and the largest
code difference.

## Data flow and ownership

- **Runner.** One simulator runner exists: the 8×8 convolution's generated
  ovxlib program, built against the simulator libraries. The
  [complete-graph record](../npu-complete-nn.md) already ran ShuffleNetV2
  through it by rewriting its tensor bindings. The harness generates each
  model's bindings from the catalogue's tensor descriptors and runs every
  vector as successive jobs on one prepared graph. Binding is confirmed for
  the 8×8 convolution and for ShuffleNetV2, which ran two inputs on one
  prepared graph. The first implementation step confirms two-tensor binding
  for the two-convolution graphs and INT16 binding for mimus. A model that the
  runner cannot bind has no simulator output and fails the run.
- **Execution.** A run with any cache miss starts one Docker container, named
  for the run, from the loaded image ID in
  [npu-compiler.lock.json](../../tools/npu-compiler.lock.json), with no network
  and no devices. Inside it, one worker pool of at most six processes, under
  `nice -n 10`, runs runner builds, runner invocations and reference
  inferences. Each runner invocation sets the environment that the existing
  pipelines export: `VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B` and
  `VSIMULATOR_SHADER_CORE_COUNT=1` (`tools/npu-conv-pipeline.sh:53-54`). The
  harness blocks on process exit.
- **Result cache.** Entries live under `~/.cache/sunstoneos/build/npu-conformance/`.
  Each entry's key is one SHA-256 digest:
  - a simulator entry hashes the runner source, the generated bindings, the
    runner's environment and argument vector, the NBG, the input and the image
    ID;
  - a reference entry hashes the reference implementation's source, its
    command arguments, its pinned files, the input and the image ID.

  An entry is written only after a zero exit, outputs of the catalogued sizes
  and a consumed-input dump equal to the vector. Failures are never cached.
  An entry stores the output bytes and the consumed input's SHA-256. The
  first implementation step records cold-run and warm-run wall times; if the
  cold run takes under 60 s, the cache is dropped.
- **Board captures.** One directory per board session holds `capture.json` and
  the output files. `capture.json` records the image SHA-256, the source hashes
  of the session runtime, service and driver, and the cache maintenance form
  the runtime uses (whole buffer or written range). It has one row per job:
  model,
  NBG SHA-256, input SHA-256, each output file's SHA-256 and size, job order,
  completion mode (interrupt or polled), interrupt count and session fault
  state. Output files use the simulator entry's layout and names, so the board
  comparison reuses the simulator comparison code unchanged. No capture code
  is written until a board session produces files. The producer is the board
  application listed under remaining work in
  [npu-session.md](../npu-session.md). The harness never contacts USB devices
  or the board.

## Interrupts, memory, cache and cores

- **Interrupts.** The host harness has no device. The conformance run on the
  board must use interrupt completion: every row records interrupt mode and
  one NPU interrupt.
- **Memory placement.** Only host files. No device buffer, firmware or QEMU
  image is added.
- **Job order for board cache visibility.** Implemented now: each model's job
  order, so the board application has a fixed sequence. Base vectors are
  ordered so that consecutive jobs have different expected outputs where the
  simulator outputs allow. On each pinned-compiler model, one input-line
  vector follows for every 64-byte line of every input. It equals the
  preceding job's vector except in that line, which gets seeded bytes. It runs
  in the simulator only and needs no reference. Its expected output must
  differ from the preceding job's; otherwise the line is reported uncovered.
  The session runtime aligns every allocation to 4 KiB
  ([npu-session.md](../npu-session.md)), so lines counted from a tensor's
  first byte are physical cache lines. Inputs span 3 lines for the 8×8
  convolution, 3 and 1 for each two-convolution graph and 44 for mimus: 55
  input-line vectors.
- **What a job pair can show on the board.** Written with the board
  comparison, once a capture exists:
  - Before each job, `a733_npu_memory_publish`
    (`drivers/npu/a733_npu_memory.c:219-221`) cleans and invalidates every
    output buffer; after completion, `a733_npu_memory_finish` (lines 234-237)
    invalidates it again. Either operation masks a missing other one. A pair
    whose outputs differ on a line therefore detects only the loss of both
    output operations, or a wrong memory attribute on the output buffer. It
    shows nothing about either operation alone.
  - A missing input clean changes the output only while the new input bytes
    are still dirty in the CPU cache when the NPU reads them. An eviction
    before that writes them back and hides the omission.

  The record states the result per model and tensor type as "no staleness
  observed" for the maintenance form and runtime source hash in the capture.
  A later change of form, such as narrowing the clean to the written range,
  needs a new capture. Per-operation coverage comes from the host check
  [npu-memory.c](../../checks/npu-memory.c), which asserts the operation kind
  for each buffer role and the operation counts per job (lines 64-80 and
  265-293).
- **Synchronization across cores.** No product code runs. Host workers write
  separate temporary directories, and only the parent moves entries into the
  cache and writes the report. A run holds one `flock` on the cache directory
  from start to end; a second run fails at start. On a run with any miss, the
  last job of each model's runner invocation repeats its seeded vector, and
  the two outputs must be equal. This detects simulator nondeterminism.

## Operator coverage for each comparison

The inventory is each model's hardware layer names, as printed in the layer
table of the SDK inspector `nbinfo` 2.0.0.b7fb7b62 (for example
`ConvolutionReluPoolingLayer2`), its NBG operation kinds and its tensor types.

- **Simulator against reference.** The four pinned-compiler models, whose NBG
  format `0x20000` has a layer table that `nbinfo` reads.
- **Native.** Output delivery for all five models; input delivery as stated in
  the introduction. No arithmetic.
- **Board.** Unsaturated coverage of the four pinned-compiler models.

Layer names found only in ShuffleNetV2 are reported uncovered in both
comparisons. Evidence that would cover them is a catalogue graph that contains
those layers or a ShuffleNetV2 input with an unsaturated output. `nbinfo` has
read ShuffleNetV2's format `0x1001e` header; if its layer table cannot be read,
the report lists ShuffleNetV2's layers as unknown.

## Reuse, removal and changes elsewhere

- **Reused.** The catalogue, the compiler lock, the adapted ovxlib runner, the
  fixture pipelines' source artifacts, the int32-bias rule, and the comparison
  rules of [npu-conv-reference.py](../../checks/npu-conv-reference.py) and
  [npu-dual-reference.py](../../checks/npu-dual-reference.py).
- **Requested from owners.**
  - `checks/npu-session.c`: the input check described in the introduction.
  - `tools/npu-mimus-linear.py`: the int32-bias rule as a function.
  - `checks/npu-conv-reference.py` and `checks/npu-dual-reference.py`: remove
    their simulator comparisons once this report covers the same fixtures and
    inputs.

  No product code or code reserved for the primary agent changes.
- **Not recorded here.** The NPU metrics of section 3 of
  [performance-architecture.md](../performance-architecture.md). The packages
  that reduce them count them in the existing session and large-graph host
  models.

## Budgets

The report records each count with the input identities, and the cold-run wall
time without a budget.

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| Simulator executions per simulator key | One per pipeline invocation, repeated each invocation | One in total; zero when every key is cached | Execution and cache-hit counters |
| Simulator jobs, cold cache | One per pipeline invocation | At most 25 base vectors, 55 input-line vectors and five repeats | Job counter |
| Runner builds | One per pipeline invocation | One per runner-and-binding digest: five cold, zero when cached | Build counter |
| Runner invocations, cold cache | One process for two ShuffleNetV2 inputs | One per simulated graph: five | Process counter |
| Compiler commands per run | Each fixture pipeline: import, two inferences, quantize, export | One quantized inference per new reference key: at most 15 cold, zero when every key is cached | Command counter |
| Peak concurrent processes | Not measured | At most six | Pool high-water mark |
| Container starts per run | One per pipeline invocation | One with any miss, otherwise zero | Report |
| Lines covered by a job pair with different expected outputs | Not applicable | Every input and output line of each pinned-compiler model | Report |
| Lines changed outside owned files | Not applicable | Zero | `git diff --stat` against the base |

## Failure handling

The run fails on any runner or reference failure, a timeout, a wrong output
size, a consumed input that differs from the vector, a missing or changed
pinned identity, a repeated vector with different outputs, or any mismatch.
It reports every model's state and no partial pass. Timeouts per step:

- **Runner build:** 120 s per model (`tools/npu-conv-pipeline.sh:61`,
  `tools/npu-mimus-linear.sh:56`).
- **Reference inference:** 120 s per vector (`tools/npu-conv-pipeline.sh:49`).
- **Runner invocation:** the existing one-vector limit, which covers graph
  preparation, plus four times the measured per-vector time for each further
  vector. The one-vector limit is 120 s for the convolution graphs
  (`tools/npu-conv-pipeline.sh:64`, `tools/npu-dual-pipeline.sh:68`) and
  600 s for mimus (`tools/npu-mimus-linear.sh:59`); ShuffleNetV2, which has no
  pipeline, uses 600 s. The first implementation step records the per-vector
  time of each model.
- **Container:** the sum of the run's step limits plus 120 s for start. On
  expiry the harness runs `docker kill` and `docker rm -f` on the run's
  container and reports a failure.

## Board evidence that would confirm conformance

For every catalogue model and vector, the board output file matches the SHA-256
and size in `capture.json` and equals the stored simulator output. The
capture's image and runtime hashes match the image under test. Each job
completes through the interrupt path with one NPU interrupt, and no session
retains a fault. Cache visibility is recorded as "no staleness observed", with
the limits stated above, for the maintenance form named in the capture.

## Left out

- **A QEMU comparison.** It would re-prove the transport that reserved case 0
  proves, with a second register model.
- **A host model of NPU arithmetic.** It would duplicate the simulator and need
  its own qualification.
- **A generated graph for layers found only in ShuffleNetV2.** Its result would
  not show that the published graph's descriptors, from another compiler,
  encode those layers the same way, so it would support no claim.
- **Input-line vectors for ShuffleNetV2.** Its outputs are saturated, so no
  pair would have different expected outputs.
- **Dataset accuracy and simulator timing.** Conformance compares bytes, and
  host cycle counts do not predict board timing.
- **A search for an unsaturating ShuffleNetV2 input.** Its class meanings are
  undocumented, and the search has no stopping rule.
