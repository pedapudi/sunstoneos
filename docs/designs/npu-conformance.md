# Design: NPU output conformance with the SDK simulator

Status: **design**; no code exists yet. The conformance record will be
[docs/npu-conformance.md](../npu-conformance.md).

The pinned SDK simulator models the A733 NPU's revision 9.2 engine and is the
only arithmetic oracle the project has for compiled models
([npu-compiler.md](../npu-compiler.md)). The conformance harness,
`tools/npu-conformance.py`, runs on the host and makes two comparisons:

1. **Simulator against an independent reference** for every catalogue model
   and input vector.
2. **Board against the simulator** on saved board captures. This comparison
   is pending until a board application produces captures.

Native delivery of tensor bytes already has evidence, which the record cites.
Reserved case 0 of [npu-session.py](../../checks/npu-session.py) runs the five
catalogue models through the session runtime in QEMU. Its register model reads
each neural descriptor through the published NPU page tables, and the client
must receive the output derived from the input bytes found there.
[nbg-layout-reference.py](../../checks/nbg-layout-reference.py) shows that
native allocations and relocations match the SDK loader. The runtime treats
tensor bytes as opaque, so simulator bytes in a QEMU check would expose no
runtime fault that those checks miss. One gap remains: reserved case 0 never
checks ShuffleNetV2's 150,528-byte input, because that model's first
descriptor writes intermediate memory. This package asks the owner of
`checks/npu-session.c` to make `execute()` walk every 4 KiB page of each model
input at completion and compare it with the submitted bytes.

## Models, inputs and references

The model set is the session catalogue
([npu-session-models.json](../../tools/npu-session-models.json)). Each model
gets five vectors of raw tensor codes, so the harness performs no
quantization: the catalogued reference input, every code at the zero point,
every code at the minimum, every code at the maximum, and one seeded vector.
Seeded bytes are SHA-256 in counter mode over the model name and vector index,
independent of any library version. A vector equal to an earlier one runs
once; ShuffleNetV2's input zero point equals its minimum code.

`tools/npu-conformance-references.json` fixes one reference per model and pins
the SHA-256 of every file the reference reads:

| Model | Tensor types | Reference | Pinned files |
| --- | --- | --- | --- |
| 8×8 convolution | UINT8 | Compiler quantized CPU inference | Imported model JSON and data, `.quantize` |
| Two-convolution, UINT8 | UINT8 | Compiler quantized CPU inference | As above |
| Two-convolution, INT8 | INT8 | Compiler quantized CPU inference | As above |
| mimus query projection | INT16 | Exact integer accumulator with the bias held as an int32 code, rounded once to INT16 | mimus model file, weight codes and scales, `.quantize` |
| ShuffleNetV2 (published SDK example) | UINT8 | None | None |

- **Compiler inference.** The convolution fixtures' exact integer references
  give float outputs for their dyadic inputs only; they give no output codes
  for other vectors. Compiler quantized inference matched the simulator bit
  for bit on all three fixtures. The harness feeds it the dequantized codes
  and requires its dumped quantized input to equal the vector.
- **mimus.** The rule is the int32-bias computation in
  [npu-mimus-linear.py](../../tools/npu-mimus-linear.py), under which 1,408 of
  1,408 simulator codes matched. The harness first reproduces that count on
  the study's saved input.
- **ShuffleNetV2.** A different compiler produced it, and no source model or
  quantize file exists. It takes part in the board comparison only, and no
  arithmetic claim rests on it.

Every comparison requires bit-for-bit equality. A difference fails the run; it
becomes a tolerance only as a reviewed per-model entry with a computable
acceptance rule. An output whose codes all sit at the clamp limits is marked
**saturated** and does not count toward arithmetic coverage. Every ShuffleNetV2
output seen so far is `[0,255]`.

## Data flow and ownership

- **Runner.** One simulator runner exists: the 8×8 convolution's generated
  ovxlib program, built against the simulator libraries. The
  [complete-graph record](../npu-complete-nn.md) already ran ShuffleNetV2
  through it by rewriting its tensor bindings. The harness generates each
  model's bindings from the catalogue's tensor descriptors and runs N vectors
  as successive jobs on one prepared graph. Binding is confirmed for the 8×8
  convolution and for ShuffleNetV2, which ran two inputs on one prepared
  graph. The first implementation step confirms two-tensor binding for the
  two-convolution graphs and INT16 binding for mimus. A model that the runner
  cannot bind has no simulator output and fails the run.
- **Execution.** A run with any cache miss starts one container from the
  loaded image ID in [npu-compiler.lock.json](../../tools/npu-compiler.lock.json),
  with no network and no devices. Inside it, one runner invocation per model
  executes all that model's vectors, up to six invocations at once, and the
  reference inferences run beside them. The harness blocks on process exit.
- **Result cache.** Entries live under `~/.cache/sunstoneos/build/npu-conformance/`.
  A simulator key is the runner source hash, generated binding hash, NBG
  SHA-256, input SHA-256 and image ID. A reference key is the reference kind,
  its pinned file hashes, the input SHA-256 and the image ID. Each invocation
  writes a temporary directory, which the harness renames into place only
  after a zero exit, outputs of the catalogued sizes and a consumed-input dump
  equal to the vector. Failures are never cached. An entry stores the output
  bytes and the consumed input's SHA-256.
- **Board captures.** One directory per board session holds `capture.json` and
  the output files. `capture.json` records the image SHA-256, the source hashes
  of the session runtime, service and driver, and one row per job: model, NBG
  SHA-256, input SHA-256, job order, interrupt count and session fault state.
  Output files use the simulator entry's layout and names, so the board
  comparison reuses the simulator comparison code unchanged, and no capture
  code is written until a board session produces files. The producer is the
  board application listed under remaining work in
  [npu-session.md](../npu-session.md). The harness never contacts USB devices
  or the board.

## Interrupts, memory, cache and cores

- **Interrupts.** The host harness has no device. On the board, conformance
  requires one recorded NPU interrupt per job.
- **Memory placement.** Only host files. No device buffer, firmware or QEMU
  image is added.
- **Cache visibility on the board.** Bit-equal output shows cache visibility
  only when a stale buffer would change the result. The harness orders each
  model's vectors so that consecutive jobs have different expected outputs
  where the simulator outputs allow, and labels each consecutive pair:
  - A missing output invalidate is detectable on each 64-byte output line
    whose bytes differ between the two jobs.
  - A missing clean of the whole input is detectable when the two outputs
    differ. A clean missing from part of the input is not reliably detectable.

  Cache visibility is qualified for a model when every output line is covered
  by some pair and at least one pair detects input staleness, and for a tensor
  type when one such model uses it. The first job after open counts for
  nothing. ShuffleNetV2's cache path stays unqualified until an input gives it
  an unsaturated output.
- **Synchronization across cores.** No product code runs. Host worker
  processes write separate temporary directories; only the parent renames
  entries and writes the report. When a rename finds its key already present
  from a concurrent run, the harness compares the bytes and fails on a
  difference, which also detects simulator nondeterminism.

## Operator coverage for each comparison

The inventory is each model's hardware layer names, as printed in the layer
table of the SDK inspector `nbinfo` 2.0.0.b7fb7b62 (for example
`ConvolutionReluPoolingLayer2`), its NBG operation kinds and its tensor types.
`nbinfo` read ShuffleNetV2's format `0x1001e` header in the recorded reports,
but its layer table has been seen only for format `0x20000`. The first
implementation step confirms the layer table for `0x1001e`.

- **Simulator against reference.** The four pinned-compiler models, plus one
  generated graph that holds every layer name found only in ShuffleNetV2. That
  graph has dyadic values, is compiled once with the
  [convolution pipeline](../../tools/npu-conv-pipeline.sh) pattern, runs only
  in this comparison and stays outside the catalogue. Its `nbinfo` table must
  list each targeted name; a name the pinned compiler does not emit is
  reported uncovered. Coverage here shows that the simulator handles the layer
  as the pinned compiler encodes it. It does not show that the published
  graph's descriptors, from another compiler, encode it the same way.
- **Native.** Tensor delivery for all five models, through the two checks
  cited above. No arithmetic.
- **Board.** Unsaturated coverage of the four pinned-compiler models. Layer
  names found only in ShuffleNetV2 stay without board coverage until the
  catalogue owner admits a graph that contains them or an unsaturating
  ShuffleNetV2 input exists.

## Reuse, removal and changes elsewhere

- **Reused.** The catalogue, the compiler lock, the adapted ovxlib runner, the
  fixture pipelines' source artifacts, the int32-bias rule, and the comparison
  rules of [npu-conv-reference.py](../../checks/npu-conv-reference.py) and
  [npu-dual-reference.py](../../checks/npu-dual-reference.py).
- **Removable by their owners.** The simulator comparisons in those two checks,
  once this report covers their fixtures.
- **Requested.** The multi-page input check in `checks/npu-session.c`. No
  product code or code reserved for the primary agent changes.
- **Not recorded here.** The NPU metrics of section 3 of
  [performance-architecture.md](../performance-architecture.md). The packages
  that reduce them count them in the existing session and large-graph host
  models.

## Budgets

Counts appear in the report with the input identities. Wall time and process
count of a cold run are recorded without a budget.

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| Simulator executions per simulator key | One per pipeline invocation, repeated each invocation (counted) | One in total; zero when every key is cached | Execution and cache-hit counters |
| Runner processes, cold cache | One process for two ShuffleNetV2 inputs | One per model: five | Process counter |
| Compiler commands per run | Each fixture pipeline: import, two inferences, quantize, export (counted) | One quantized inference per new reference key: at most 15 cold, zero when every key is cached; the generated graph's pipeline once per graph identity | Command counter |
| Container starts per run | One per pipeline invocation | One with any miss, otherwise zero | Report |
| Comparisons per output byte | Not applicable | One per comparison; at most 16 listed differences plus their count and the largest code difference | Report |
| Cache bytes per simulator entry | Not applicable | Output bytes plus two digests; at most 2,816 output bytes today | Entry sizes |
| Job pairs that detect staleness | Not applicable | Every output line covered and one input pair, for each pinned-compiler model | Report |
| Lines changed outside owned files | Not applicable | Zero | `git diff --stat` against the base |

## Failure handling

The run fails on any runner or reference failure, a timeout, a wrong output
size, a consumed input that differs from the vector, a missing or changed
pinned identity, or any mismatch. It reports every model's state and no partial
pass. Timeouts are the pipelines': 120 s per inference, 300 s per runner
invocation.

## Board evidence that would confirm conformance

For every catalogue model and vector, the board output equals the stored
simulator output, the capture's image and runtime hashes match the image under
test, each job raises one NPU interrupt, and no session retains a fault. Cache
visibility is qualified only for the models and tensor types whose job pairs
meet the staleness condition above.

## Left out

- **A QEMU comparison.** It would re-prove the transport that reserved case 0
  proves, with a second register model.
- **A host model of NPU arithmetic.** It would duplicate the simulator and need
  its own qualification.
- **Dataset accuracy and simulator timing.** Conformance compares bytes, and
  host cycle counts do not predict board timing.
- **A search for an unsaturating ShuffleNetV2 input.** Its class meanings are
  undocumented, and the search has no stopping rule.
