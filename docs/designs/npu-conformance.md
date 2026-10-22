# Design: NPU output conformance with the SDK simulator

Status: **design**; no code exists yet. The conformance record will be
[docs/npu-conformance.md](../npu-conformance.md).

The pinned SDK simulator models the A733 NPU's revision 9.2 engine. It is the
only arithmetic oracle that the project has for compiled models
([npu-compiler.md](../npu-compiler.md)). The conformance harness runs a fixed
set of compiled models and input vectors through three comparisons:

1. **Simulator against an independent reference.** The simulator output must
   equal the compiler's quantized CPU inference, or the exact integer
   reference where the fixture has one.
2. **Native runtime against the simulator, in QEMU.** The product session
   runtime ([npu-session.md](../npu-session.md)) submits each model with the
   simulator's input. A register model checks the bytes that the device would
   read and returns the simulator's output bytes. The client must receive those
   bytes unchanged.
3. **Board against the simulator.** This comparison runs later, on saved board
   captures, and checks the same output files.

The first two comparisons establish that the native path delivers the
simulator's input to the addresses that the program reads, and returns the
bytes that the program writes. NPU arithmetic and cache visibility on the
board remain unqualified until the third comparison passes.

## Model set, inputs and tolerances

- **Models.** The model set is the session catalogue
  ([npu-session-models.json](../../tools/npu-session-models.json)): the 8×8
  convolution, the two UINT8 and INT8 two-convolution graphs, ShuffleNetV2 and
  the mimus query projection. A model that the runtime refuses, such as the
  shader-based relative shift, runs in the simulator comparison only. It joins
  the native comparison when a catalogue change admits it.
- **Inputs.** Each model gets five input vectors: the catalogued reference
  input, every code at the zero point, every code at the minimum, every code at
  the maximum, and one seeded uniform vector over the full code range. Inputs
  are raw tensor codes, so the harness performs no quantization.
- **Tolerances.** The board must match the simulator bit for bit. The
  simulator must match its reference bit for bit, with one declared exception:
  the mimus projection may differ by one output code where the int32 bias
  rounding explains the difference ([npu-mimus-linear.md](../npu-mimus-linear.md)).
  A new tolerance needs a reviewed per-model entry that states its cause.
- **Saturated outputs.** An output whose codes all sit at the clamp limits
  shows little about arithmetic. ShuffleNetV2's two output bytes are `[0,255]`
  for every input tried so far. The report marks such comparisons as
  saturated, and they do not count toward operator coverage.
- **Operator coverage.** The operator inventory is the set of layer types that
  the SDK's model inspection reports for the catalogue models, together with
  the NBG operation kinds that the native assembler accepts: neural network
  (kind 2) and neural transpose (kind 8). The run fails unless every layer type,
  operation kind and tensor type (UINT8, INT8 and INT16) appears in at least
  one unsaturated comparison. A layer type that appears only inside
  ShuffleNetV2 gets one small generated graph whose values are exact in
  float32, compiled once through the existing pipeline pattern
  ([npu-conv-pipeline.sh](../../tools/npu-conv-pipeline.sh)).

## Data flow and ownership

The simulator comparison runs in the pinned compiler image. One NBG runner,
built once from the SDK's NBG example program against the simulator libraries,
executes a model on one input file. The runner writes the output bytes and the
input bytes that the simulator consumed. If the example program cannot bind a
model's tensors, the saved export wrapper for that model is used. Results are
content-addressed under `~/.cache/sunstoneos/build/npu-conformance/` by model
SHA-256, input SHA-256 and the loaded image ID from
[npu-compiler.lock.json](../../tools/npu-compiler.lock.json).

The QEMU comparison builds `checks/npu-conformance.c` with the source list,
flags and link line that [npu-session.py](../../checks/npu-session.py) already
defines; the Python driver imports them. The product catalogue include is used
unchanged, so the check exercises the shipped admission path. Each client task
submits its model's input vectors as successive jobs. Buffers keep the
runtime's existing ownership states: SUBMIT copies the input into the READ
buffer, the device owns READ, WRITE and pool buffers during the job, and the
client copies the output with `npu_session_output` before REAP.

At completion the register model reads each NN descriptor through the
published NPU page tables. It resolves the input and output address fields
that the session check already decodes. It then requires that the bytes at
each input address equal the simulator's consumed input, and writes the
simulator's output at each output address. A wrong relocation, mapping, tensor
order or input copy therefore changes the received output or stops the run.
Descriptor kinds whose address fields the model does not decode fall back to
the buffer addresses, and the report records that fallback per model.
[nbg-layout-reference.py](../../checks/nbg-layout-reference.py) remains the
check that native allocations and relocations match the SDK loader. The
conformance report records the identity of its passing result for each model.

The board comparison reads output captures that a scheduled board session
saves with the image, model and input identities. The harness never contacts
USB devices or the board.

## Interrupts, memory, cache and cores

- **Completion.** The check binds interrupt completion with
  `npu_session_bind_irq`. As in session case 1, the register model completes a
  started job when the service re-arms the interrupt, then calls
  `npu_service_irq_notify_from_isr`. The client blocks on the held WAIT reply,
  so the job loop contains no tick wait. Startup and stop polling remain in the startup
  adapter; they run once per boot.
- **Memory placement.** The session reservation comes from a 48 MiB pool, as
  in reserved case 0 of the session check, because all five models need
  6,459,392 bytes. Inputs and simulator outputs are read-only arrays in the
  check image. No new device buffer exists.
- **Cache policy.** QEMU does not model caches, so cache visibility is
  established only by the board comparison. The link wrapper of
  `a733_npu_native_operations`, which the session check already uses, counts
  the bytes cleaned and invalidated per job. The design adds no barrier.
- **Synchronization across cores.** The check runs on two QEMU cores with the
  session check's placement: the service worker at EL1 and one EL0 client per
  model. After activation, only the service worker changes register-model
  state, through register accesses and the interrupt re-arm call. Trigger and
  completion counters use release and acquire ordering, as in the session
  check's model. A client reads its output only after the WAIT reply, which the
  service queue orders after the completion. No lock is added to product code.

## Reuse, removal and changes elsewhere

- **Reused.** The session runtime, its catalogue and admission, the session
  check's build recipe and interrupt glue, the fixture pipelines, and the
  comparison rules of [npu-conv-reference.py](../../checks/npu-conv-reference.py)
  and [npu-dual-reference.py](../../checks/npu-dual-reference.py), which the
  simulator comparison generalizes.
- **Shared, to avoid a second copy.** The page-table walk and descriptor
  decoding go in `checks/npu-conformance-device.h`. The session check can
  include it in a later change by its owner.
- **Removable later by their owners.** The two per-fixture reference checks,
  once the conformance report covers their fixtures and passes.
- **Product code.** This package changes none. It needs no change from the
  primary agent.

## Budgets

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| Instructions that the conformance build adds to driver, service and runtime objects | None; new check | Zero: each object's SHA-256 equals the session check build's | Harness compares object hashes from both builds |
| Compiler commands per conformance run | Each fixture pipeline runs import, two inferences, quantize and export per invocation (counted) | Zero; only catalogued NBGs and once-compiled operator graphs are consumed | Command counter in the report |
| Simulator executions per model, input and image identity | One per pipeline invocation, repeated on every invocation | One in total; a rerun with unchanged identities executes none | Cache-hit counter in the report |
| QEMU boots for the catalogue with five inputs per model | Session check: five boots for five cases, one input per model | One boot for up to eight models (25 jobs today) | Boot count in the report |
| Tick waits per job in the client loop | Session check case 2 and its flag waits use `vTaskDelay(1)` | Zero between the first SUBMIT and the last REAP | `--wrap=vTaskDelay` counter |
| Comparisons per output byte | Not applicable | One `memcmp` in the guest and one host comparison per reference; at most 16 listed differences, plus the count and the largest code difference | Report |
| Fixture bytes in the check image | Not applicable | At most 2 MiB; about 0.8 MiB today, mostly five 150,528-byte ShuffleNetV2 inputs | Section sizes of the ELF |

The check also records, per model and job, the NPU metrics of section 3 of
[performance-architecture.md](../performance-architecture.md). This package does
not change them; it supplies a per-model before-and-after table for the
packages that reduce them. Counted baselines are:

- one input copy per byte at SUBMIT and one output copy per byte at
  `npu_session_output`;
- a clear of the full working pool per job, 1,092,352 bytes for ShuffleNetV2;
- digests of the model's descriptor and coefficient allocations and its
  command payload, plus a check of every later eight-byte word of the
  512 KiB command stream;
- at least 3,154 address translations per job with all five models admitted,
  because the 1,577 pages of the reservation are translated at publication and
  again at completion;
- three service round trips per job: SUBMIT, WAIT and REAP.

## Failure handling

The harness fails the run on any simulator, QEMU or comparison failure; it
reports no partial pass. A register-model input mismatch writes no output and
stops QEMU with the model and input index. Product failure paths and their
retained resources are unchanged.

## Board evidence that would confirm conformance

For every catalogue model and input vector, the board output equals the stored
simulator output. Each job raises one NPU interrupt, and no session retains a
fault. Unsaturated comparisons must cover every layer type in the inventory.

## Left out

- **A host model of NPU arithmetic.** The simulator already models the engine.
  A second model would need its own qualification and would duplicate the
  simulator.
- **Compilation inside the conformance run.** Catalogue admission stays the
  only route for executable models, and the reference identities stay fixed.
- **Accuracy on datasets.** Classification accuracy and recognition error are
  model-quality questions; conformance compares bytes.
- **Simulator timing.** Host simulator cycle counts do not predict board
  performance ([npu-compiler.md](../npu-compiler.md)).
- **Tolerances for board output.** No difference is accepted before one is
  observed and explained.
