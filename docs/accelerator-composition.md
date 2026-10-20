# Retained GPU compute and complete NPU graph

The combined application keeps the existing trusted GPU compute service and
complete ShuffleNetV2 NN-pool NPU service in one native FreeRTOS image. The caller
can replace GPU input, submit NPU tensors, copy both results and close each engine
independently. The native application and its software checks are implemented. Physical
execution remains unqualified.

The application reuses the NPU application's counter, clock lease registry,
register and I2C locks, pin and rail owners, PMIC task, and global GPU/PCK resource
manager. GPU resource client 7 uses firmware owner 9 and compute owner 10. Root
admission rejects collisions with the NPU, PMIC, USB and bridge owners. A borrowed
provider view refers to the original objects; it grants no authority and creates
no second provider. The NPU startup continues to own its original bootstrap task,
PLL binding, command admission and permanent allocation checks.

The root-owned combined session retains the complete NPU contract and trusted GPU
package, together with explicit GPU initial-state, memory, electrical and
supervision evidence. Root authenticates the entire envelope before application
preparation. Program hashes and package bounds do not independently verify
arbitrary USC accesses. Executable isolation depends on the admitted mappings and
trusted compiler provenance. Firmware, runtime dependencies and physical
admission requirements remain those of the component records.

The NPU reservation must precede all GPU reservations. Its 790 write-back pages
start at 0x41600000. GPU firmware, compute payload and selected VM tables consume
34, 88 and 12 further pages. The 4 MiB write-back pool therefore uses 3,784,704
bytes and retains 409,600 bytes of slack. GPU firmware and compute use 45 and six
noncacheable pages in the existing 2 MiB pool at 0x41c00000, totaling 208,896 bytes.
There are four write-back and two noncacheable parent leases. CPU tensor staging
remains separate at 0x41400000 through 0x414c0000. Root owns the final linked CPU
layout and admission; these allocation totals do not prove that layout.

Both engines use the original absolute 20-second admission end. The counter runs
at 24 MHz; provider deadlines use the integer quotient of the authenticated end
by 24. Each GPU operation also caps its one-second budget to the remaining
counter interval, rounding down. The NPU runtime retains its existing request
budgets and the same session end. Application construction starts no new window.

A shared counter, clock-registry or retained-lock failure permanently revokes
both engines. An engine-local failure retains its own resources and does not
release or reinitialize shared providers. The common PMIC and USB owners remain
available while their own lifetime and shared resources remain healthy. Healthy
CLOSE stops only that engine. Every reserved parent and copied result remains
owned for the session; uncertainty never authorizes reclamation or a reset.

One USB task recognizes the existing 32-byte GPU request and 64-byte NPU request
by their first four bytes. It consumes one complete request and retains one
response owner until all queued response bytes complete transmission. NPU
acknowledgment applies only to a matching NPU response. Partial incoming frames
have a one-second deadline capped by the session end. The original GPU
sequence-matched output pages and NPU copied chunk protocol remain unchanged.
The root-owned USB admission and existing single-core low-level bridge still
apply.

The application host consumer in `apps/accelerators/host.py` accepts an existing
coordinator-owned exchange function. It copies the initial GPU result, submits
and copies one caller-selected NPU tensor, replaces GPU input and copies its new
result, then submits and copies the second NPU tensor. It closes both services
only after all four copied results are complete. The original absolute host
session deadline bounds the entire sequence. The consumer performs no device
discovery, launch or recovery.

Focused checks pass on the combined linked image. The actual constructors
allocate the NPU parent first, then all GPU firmware, payload and table parents;
they bind one counter, clock registry, lock set, PMIC and global PCK manager.
Independent counter, registry, retained-lock and manager failure cases revoke
both engines. The native clients and common USB parser complete the interleaved
copied-result sequence and both healthy CLOSE operations. Transport cases cover
failed NPU upload draining, immutable reply completion after local failure,
terminal zero-length USB packets, partial-header expiry and stale GPU reads.

These checks model root admission, CPU translations/cache effects, FreeRTOS
constructors or scheduling, device service results and USB transfers at their
stated boundaries. Saved SDK results supply the NPU numerical output. Both SDK
inputs yield the same saturated result; the checks establish copied ownership
and input consumption, not input-sensitive inference or accuracy. Root separately
reviews session survival, CPU layout, mappings and interrupt composition.

The preserved image and compiled dependency closure are in
`~/.cache/sunstoneos/evidence/gpu-npu-composition-20261020/`. The image SHA-256 is
`ad26907f25e961815c8bc093bb36580b09377eae00c96b9c4527aabe93b47c24`.
Its 114 objects and 270 actual compiled inputs are recorded in
`compiled-closure.json`; affected results are in `providers/result.json`,
`runtime/result.json` and `ownership/result.json`. Astra reviewed the concrete
provider and response ownership corrections before execution. Existing compiler,
service and cache publication evidence is reused where its inputs and behavior
are unchanged.

Graphics composition, simultaneous submitted workloads beyond the existing
service queues, broader compute limits, further NPU models and the remaining
board devices are still required integration work.
