# Trusted compiled convolution registration and ownership

The native registration component admits a privileged build-time catalogue of
four compiled UINT8 convolution graphs. Model selection occurs before the scheduler
starts and remains fixed for that context's lifetime. Applications copy inputs,
submit repeated inference requests, collect matching completion, copy results and
explicitly reap each job. Physical NPU execution remains unqualified.

All entries target `0x1000003b` and use NBG format `0x20000`. The first three
contain one 3×3 convolution without padding and with unit stride. The fourth
contains two 3×3 stride-two convolutions, each with one-cell padding and ReLU.
Tensor order below is NCHW:
batch, channel, height, width.

| Catalogue entry | Input | Output | Model bytes | Input/output bytes |
| --- | --- | --- | ---: | ---: |
| `conv8` | `1×3×8×8` | `1×2×6×6` | 1,928 | 192 / 72 |
| `conv10` | `1×3×10×10` | `1×2×8×8` | 1,928 | 300 / 128 |
| `conv12` | `1×3×12×12` | `1×4×10×10` | 2,056 | 432 / 400 |
| `subsample2` | `1×1×8×8` | `1×2×2×2` | 2,720 | 64 / 8 |

[The catalogue](../tools/npu-models.json) records complete model/input/reference
identities, exact tensor dimensions and quantization, coefficient bytes and SRAM
requirements, neural-operation and relocation counts, and command-body size.
The native table independently fixes those model identities and metadata.
The [two-stage graph](npu-two-stage.md) uses internal SRAM for its intermediate
tensor and requires no external working pool or extra reservation.
Each accepted model passes the existing decoder, checksums and assembler policy
before native mappings are constructed. Other models, formats and operation
sequences are refused. Neither a caller-supplied hash nor a checksum adds an entry.

The catalogue contains trusted executable input produced by the pinned external
compiler. It is not a verifier for arbitrary model descriptors. Coefficient
streams can contain compression and traversal metadata. Complete actual engine
access extents, prefetch and alternate SRAM routes remain unresolved. Independently
effective hardware confinement remains a physical admission prerequisite.

The implementation copies the selected complete binary before hashing or parsing it.
Its private copy has an 8 KiB capacity and deterministic zeroed unused bytes;
only the selected model length is decoded and hashed.
The private SHA-256 helper follows sections 4–6 of the
[NIST Secure Hash Standard](https://csrc.nist.gov/pubs/fips/180-4/upd1/final).
It supplies artifact identity only. There is no general cryptographic interface.
Downloaded binaries and reference data remain outside the repository.

## Private allocations bind commands to the native memory policy

Supply these allocations in the order defined by
[`npu_convolution.h`](../include/npu_convolution.h):

| Allocation | Reserved bytes | NPU permission |
| --- | ---: | --- |
| Neural descriptor | 4,096 | Read |
| Coefficients | 4,096 | Read |
| Convolution command stream | 524,288 | Read |
| Input | 4,096 | Read |
| Output | 4,096 | Read/write |
| Bootstrap stream | 524,288 | Read |

The payload reservations total 1,064,960 bytes. Tables, service storage and the
registration context require separate private RAM. Every payload has a writable
CPU mapping, stable physical address and exclusive cache-line ownership. Native
initialization checks translation and builds private NPU tables. Device addresses
come from those allocation records; the assembler applies only the accepted
relocations. CPU, physical and device ranges must satisfy the existing
[memory policy](npu-memory.md). Leave unused device ranges unmapped.

The selected full model hash fixes descriptor bytes, coefficients and relocation semantics.
Exact catalogue tensor metadata and assembly allocation sizes are checked independently.
Preparation records SHA-256 digests of the assembled descriptor and coefficient
(or shader) allocations and of both complete command allocations, including
their padding. Each command allocation is a short payload followed by END
pairs to the end of its fetch span. Preparation also records a digest of each
payload and verifies that every later eight-byte unit is an END pair; the
payload digest plus that END check binds the same bytes as the full digest.
Command 0 is bootstrap; command 1 is the convolution. Applications cannot
supply streams or register writes.

The shared SMP mappings cannot stop another privileged writer, so the session
rechecks these bytes before the NPU can read them:

- The bootstrap publication at OPEN verifies the private model copy, the
  descriptor and coefficient digests, and both command payloads with their END
  padding. Startup steps, the service activation check and stop steps hash
  nothing.
- Every job publication verifies the descriptor and coefficient digests and the
  convolution command payload with its END padding. It omits the model copy,
  which the device never reads, and the bootstrap stream, which runs only at
  OPEN.

In the NN-pool profile a job check therefore hashes 920,616 bytes and compares
the remaining 521,176 command bytes against END. The previous per-job check
hashed 2,930,304 bytes. The hash is a scalar C SHA-256; its cost on the board
is unmeasured. Hashing could be removed from the job path only if the platform
mapped the constant allocations read-only for the session, which the current
stage-1 tables do not support. A mismatch at any check faults the session
before cache maintenance or device submission and retains every reservation.

The convolution bootstrap is the 208-byte sequence written by the controller's
bootstrap encoder. After VIP SRAM setup it writes `LOAD_STATE 0x0e4f=0`
followed by `0x0e50=0`. Those writes occur in the accepted binary's INIT record
and establish its zero external-SRAM window. The final synchronization, event
and END follow them. The session fills the remaining mapped fetch bytes with
END pairs.

The source basis is the pinned board driver
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, `vip_drv_device.c` initialization
sequence and the exact accepted INIT record. The target feature record declares
512 KiB VIP SRAM and zero AXI SRAM. These zero writes do not prove effective
translation or exclude undocumented address routes.

## Startup and client calls preserve ownership

Before scheduling, call `npu_convolution_prepare`, initialize the
[startup sequencer](npu-startup.md) and bind its bootstrap task, then call
`npu_convolution_bind`. The session record's evidence identities name the
resource, memory and recovery records; a numeric identifier does not
establish any physical fact.

The bootstrap task calls `npu_convolution_startup_poll` once per tick. The
wrapper checks the calling task and the registration's selected model sizes,
then calls the sequencer's step function. A retained startup faults the
registration and keeps every acquisition. After READY, the task activates the
deferred service; its prepared callback compares the service's copy of the
backend, MMU image and both catalogue commands once and asks the startup
whether it is READY.

One live client task uses `npu_convolution_send` and
`npu_convolution_collect`. Its supported sequence is:

1. OPEN, then send WAIT (or request STATUS repeatedly) until a copied reply
   reports READY.
2. SUBMIT an input with the selected catalogue length. The wrapper copies it
   and fills the selected output length of the device output buffer with the
   word `NPU_CONVOLUTION_OUTPUT_POISON` (`0xc35a3ca5`, little-endian, repeated)
   before queuing command 1. The job publication cleans that WRITE buffer to
   memory, so an output byte the job never writes reads back as its pattern
   byte instead of the previous job's value.
3. Send WAIT (or request STATUS repeatedly) until DONE identifies the matching
   submission ticket. The live runtime client uses WAIT; the fixed two-job
   client still polls STATUS once per tick.
4. Copy the selected output length with `npu_convolution_output`.
5. Explicitly REAP before submitting another input.

The fill costs one 64-bit store per 8 output bytes, five instructions per
iteration in the compiled loop, and makes those lines dirty for the
publication's clean. For the 400-byte numerical model that is 50 stores and
seven 64-byte lines, against the 432-byte input copy that SUBMIT already
makes. At the large-graph output capacity of 262,144 bytes it is at most 32,768
stores, a quarter of the 1 MiB pool clear that SUBMIT already makes in that
profile. These are
counts from the code and the disassembly; no board measurement exists.

A finite collect timeout preserves the pending ticket. The client must collect
that request before sending another or accessing output. DONE follows the
controller's event, full-idle and cache-finish checks. REAP removes access to the
previous result; all allocations remain reserved through the diagnostic session.

After the last REAP, the client may send terminal CLOSE with zero timeout.
A copied successful reply must report CLOSED, no fault, no cancellation and no
session or job ownership. Publication uses release ordering; the original
bootstrap observes it with acquire ordering before local shutdown. Further
requests and reopening are refused. Allocation and shared-resource ownership
retain platform lifetime.

The original bootstrap can then call `npu_convolution_stop_poll`, which runs
the sequencer's local stop. Local shutdown gates the module and bus before
asserting AHB, AXI and core reset. It preserves SRAM reset, shared clock leaves,
the PLL, supply and ON domain, and keeps every token. This API does not release allocations or establish
that a particular monitor restores RAM safely.

Faults retain ownership and stop progress. The wrapper provides no cancellation,
resource release, fault reset or automatic retry. Task abandonment retains everything.
Callers must keep contexts and task handles alive and must not bypass the wrapper
through its private service or startup objects. Current shared task mappings do
not enforce this contract against malicious privileged or unprivileged code.

## Physical admission remains required

Physical execution requires an operation-specific reviewed recovery plan,
effective page-directory translation and permissions, exclusive NPU and
remote-controller handoffs, and valid power/clock/reset preparation. The supervised RAM-only image may
use its admitted monitor-return
contract and manual whole-board power removal after uncertainty. Manual recovery
does not replace the selected image's enforced admission conditions. The native
adapter uses the documented controller page at physical `0x03600000`.

Internal SRAM has a separate route from ordinary RAM page tables. Reserve all
512 KiB, including the CPU/MCU alias `0x74000..0xf3fff`, exclude other users and
retain NPU power. Establish routing and exclusion of alternate address modes.
An absent RAM page-table entry does not protect this SRAM path.

The initial platform profile is 492 MHz at 800 mV: the startup sequencer
enables PLL_NPU from its disabled state, and the power-domain owner verifies
the DCDC2 supply. A general descriptor
verifier is unnecessary for this immutable privileged entry once all access
routes are constrained; the trusted catalogue does not admit arbitrary models.
Runtime executable-model upload, hot switching, concurrent models and generalized
graph admission remain unsupported in this registration; concurrent catalogue
models use the separate [session runtime](npu-session.md). The small profile retains a 512-byte tensor
API. Separate retained profiles support the authenticated SH/NT graph and the
[complete published weight-bearing NN graph](npu-complete-nn.md), including
external working memory and chunked caller data. These implemented profiles do
not establish complete NPU or mimus accelerator support.
Export physical output before reset. Post-reset DRAM retention is not a qualified
result channel.

## Focused evidence covers new composition behavior

Evidence resides in
`~/.cache/sunstoneos/evidence/npu-convolution-20261016/evidence.json`.
The native object compiles independently. Emulator cases use two FreeRTOS cores,
actual service/controller code, native CPU translation/cache operations, private
NPU tables and the assembler. The device registers and produced output are
modeled. Injected reference bytes test copying and ownership, not inference.

The first image passes fixed artifact and relocation comparison, SHA-256 known
answers and padding boundaries, complete bootstrap comparison, copied input,
matched output completion, explicit REAP, varied input and retained publication
mutation. Its second job runs with a changed model copy and bootstrap padding,
which a job publication does not check. The `mutation` mode changes a command tail
before activation; the bootstrap publication at OPEN refuses it before the
device fetches the stream. The `job-command` mode
refuses changed convolution command padding at a job publication. An unauthorized client is refused without effects.

The successful numerical-reference and compiler comparisons and lower-level
memory checks are reused. Startup health is a modeled boundary in the
convolution lifecycle cases; the sequencer itself runs in
`checks/npu-startup-host.c` and `checks/npu-startup.c`. No previous suite, simulator inference or performance campaign was rerun.

Generate authenticated fixture headers with
[`checks/npu-convolution.py`](../checks/npu-convolution.py), passing the binary,
saved input/output, recorded assembly directory and a fresh external destination.
Configure `NPU_CONVOLUTION_FIXTURES` to that destination and build
`npu-convolution-model`; its semihosting arguments are `composition`, `mutation`,
`close`, `close-fault` and `job-command`. Archive executable and input identities before execution. The
private CMake fragment also defines the independent `npu-convolution` native
object target.

## Variable shapes and repeated inference pass focused software checks

Evidence is under `~/.cache/sunstoneos/evidence/npu-models-20261018/`.
Two compiler runs varied spatial dimensions and output channels. Their saved
floating-point results match the independent integer arithmetic reference.
Their simulator results match every quantized CPU reference byte: 128 and 400
bytes. SDK preparation captures stop before device submission. Native assembly
matches all captured descriptors, coefficients, command bytes and allocations
after address relocation and the existing completion-event adjustment.

The affected native image selects the 12×12 model. The actual constructor,
permanent memory reservation, copied 432-byte input, copied 400-byte output and
application client execute in the focused instruction check. Two distinct jobs
complete and receive successful matching REAP replies before success publication.
A first-result mismatch followed by a matching second result remains a mismatch.
A second REAP reply with the wrong job ticket faults the wrapper and refuses
success. The result includes both job identities and the first output digest.

Architecture registers, translation/cache effects, service replies and produced
output are modeled. Injected reference output checks copying, lifetime and result
verification; it does not execute numerical inference. No peripheral MMIO or board
communication occurred. Earlier fixed-model and constituent checks were reused.

The [live runtime client](../apps/npu-runtime/README.md) now supplies arbitrary
same-shape inputs to that unchanged selected model and copies numerical output
through this API. Its USB task owns one mailbox; the designated NPU client owns
all service calls, REAP and terminal CLOSE. The compiled-model catalogue and
memory/publication policy remain unchanged. Runtime input delivery does not add
model upload, shape switching or broader operator support.

The separate [fixed FP32 relative-attention shift profile](npu-relative-shift.md)
adds authenticated SH/NT execution, an external working pool and chunked live
input/output in a retained single-core IRQ application.
