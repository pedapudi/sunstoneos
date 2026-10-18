# Trusted compiled convolution registration and ownership

The native registration component admits a privileged build-time catalogue of
three compiled UINT8 convolutions. Model selection occurs before the scheduler
starts and remains fixed for that context's lifetime. Applications copy inputs,
submit repeated inference requests, collect matching completion, copy results and
explicitly reap each job. Physical NPU execution remains unqualified.

All entries target `0x1000003b` and use NBG format `0x20000`, one neural operation,
a 3×3 convolution without padding, and unit stride. Tensor order below is NCHW:
batch, channel, height, width.

| Catalogue entry | Input | Output | Model bytes | Input/output bytes |
| --- | --- | --- | ---: | ---: |
| `conv8` | `1×3×8×8` | `1×2×6×6` | 1,928 | 192 / 72 |
| `conv10` | `1×3×10×10` | `1×2×8×8` | 1,928 | 300 / 128 |
| `conv12` | `1×3×12×12` | `1×4×10×10` | 2,056 | 432 / 400 |

[The catalogue](../tools/npu-models.json) records complete model/input/reference
identities, exact tensor dimensions and quantization, coefficient bytes and SRAM
requirements. The native table also fixes those model identities and metadata.
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
Exact catalogue tensor metadata and assembly allocation sizes are checked independently. Separate
digests bind the assembled descriptor, coefficients and complete command
allocations, including their padding. Publication checks those digests before
native cache maintenance or device submission. Command 0 is bootstrap; command 1
is the convolution. Applications cannot supply streams or register writes.

The convolution bootstrap preserves the existing initialization sequence and
inserts `LOAD_STATE 0x0e4f=0` followed by `0x0e50=0` after VIP SRAM setup.
Those writes occur in the accepted binary's INIT record and establish its zero
external-SRAM window. The final synchronization, event and END follow them.
The 208-byte image and all remaining mapped fetch bytes contain terminating
padding. The generic bootstrap encoder remains unchanged.

The source basis is the pinned board driver
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, `vip_drv_device.c` initialization
sequence and the exact accepted INIT record. The target feature record declares
512 KiB VIP SRAM and zero AXI SRAM. These zero writes do not prove effective
translation or exclude undocumented address routes.

## Startup and client calls preserve ownership

Before scheduling, call `npu_convolution_prepare`, obtain the handoff with
`npu_convolution_handoff`, initialize and bind the existing startup adapter,
then call `npu_convolution_bind`. Supply independently authenticated resource,
memory and recovery evidence in the handoff. A numeric evidence identifier does
not establish any physical fact.

The designated privileged bootstrap task must use
`npu_convolution_startup_poll` for every startup step. This wrapper checks the
bound model, both catalogue copies and immutable allocation digests before
delegating to the actual startup adapter. Mutation stops further delegation and
retains partial acquisitions. After READY, the task activates the deferred
service; its prepared callback checks byte identity and actual startup health.

One live client task uses `npu_convolution_send` and
`npu_convolution_collect`. Its supported sequence is:

1. OPEN, then request STATUS until a copied reply reports READY.
2. SUBMIT an input with the selected catalogue length. The wrapper copies it before queuing command 1.
3. Request STATUS until DONE identifies the matching submission ticket.
4. Copy the selected output length with `npu_convolution_output`.
5. Explicitly REAP before submitting another input.

A finite collect timeout preserves the pending ticket. The client must collect
that request before sending another or accessing output. DONE follows the
controller's event, full-idle and cache-finish checks. REAP removes access to the
previous result; all allocations remain reserved through the diagnostic session.

Faults retain ownership and stop progress. The wrapper provides no cancellation,
close, release, reset or automatic retry. Task abandonment retains everything.
Callers must keep contexts and task handles alive and must not bypass the wrapper
through its private service or startup objects. Current shared task mappings do
not enforce this contract against malicious privileged or unprivileged code.

## Physical admission remains required

Physical execution requires an operation-specific reviewed recovery plan,
effective page-directory translation and permissions, exclusive NPU and
remote-controller handoffs, and valid power/clock/reset preparation. The watchdog
image retains its independently qualified watchdog/ROM-return admission
requirement. The supervised RAM-only image may use its admitted monitor-return
contract and manual whole-board power removal after uncertainty. Manual recovery
does not replace the selected image's enforced admission conditions. The native
adapter uses the documented controller page at physical `0x03600000`.

Internal SRAM has a separate route from ordinary RAM page tables. Reserve all
512 KiB, including the CPU/MCU alias `0x74000..0xf3fff`, exclude other users and
retain NPU power. Establish routing and exclusion of alternate address modes.
An absent RAM page-table entry does not protect this SRAM path.

The initial platform profile uses inherited 492 MHz/800 mV settings. Unsuitable
inherited PLL state still requires cold provisioning. A general descriptor
verifier is unnecessary for this immutable privileged entry once all access
routes are constrained; the trusted catalogue does not admit arbitrary models.
Runtime upload, hot switching, concurrent models, larger allocations, working
pools, other operators and generalized graph support remain unsupported. The
current private input/output API has a 512-byte capacity per tensor. These limits
do not establish complete NPU or mimus accelerator support.
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
mutation. A separate terminal mode refuses a changed command tail before service
activation. A focused follow-up calls the actual startup and platform controllers
through bounded modeled acquisition, then verifies refusal and token retention
after bootstrap corruption. It also refuses an unauthorized client without effects.

The successful numerical-reference and compiler comparisons, lower-level memory
checks and prior actual startup resource checks are reused. Full rail/startup
health is a modeled callback boundary in the convolution lifecycle cases.
Its independent evidence is recorded in `npu-startup-20261016/evidence.json`,
`npu-startup-retention-20261016/evidence.json` and the central integration reuse
record. No previous suite, simulator inference or performance campaign was rerun.

Generate authenticated fixture headers with
[`checks/npu-convolution.py`](../checks/npu-convolution.py), passing the binary,
saved input/output, recorded assembly directory and a fresh external destination.
Configure `NPU_CONVOLUTION_FIXTURES` to that destination and build
`npu-convolution-model`; its semihosting arguments are `composition`, `mutation`
and `startup`. Archive executable and input identities before execution. The
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
