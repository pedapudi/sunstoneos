# GPU image entry checkpoint

This image returns after the frozen finite GPU image's CPU entry and memory
admission, before constructing any provider or starting the scheduler. It runs
zero GPU jobs. A successful return establishes that this variant reached the
boundary and completed its return path. It neither locates the earlier failed
attempt nor qualifies later provider, firmware, compute or shutdown stages.
A checkpoint nonreturn could also occur in its new return path.

The fixed-compute image `2ab2cb1b3bac150d5eb695793ab0eb39bb453afc7fbd7ecb2f595ebfca3fd08d`
did not return within the host's 90-second window on 2026-10-24. The attempt
record is `processor-completion-20261024/port-3-3/gpu-corrected-guard/runtime/session.json`
under the evidence cache. Its 27 FEL transactions succeeded, including execution;
no native report was captured. The frozen monitor parks on every refusal,
exception and secure deadline. That observation does not identify a failed
instruction or prove that a GPU access occurred.

## Frozen source and boundary

The baseline is `gpu-returning-repeat-fix-20261019/` under
`~/.cache/sunstoneos/evidence/`. Its `object-source-closure.json` binds 88 linked
objects to their frozen inputs. The builder verifies the baseline ELF, closure,
commands, linker script, each input and every reused object before compiling.
It preserves those headers and structure layouts, including the original
noncacheable EL1 table-walk value `0x200803019`.

The frozen runtime (`85cb108d…`) calls `platform_init`, establishes the retained
runtime view, then loops over `a733_device_memory_prepare` until READY. The
checkpoint replaces only the following `a733_smp_diagnostic_start` call. The
memory admission implementation (`4d7efffa…`) checks 200,704 pages in batches
of 64, in both translation directions, then initializes pool metadata. It does
not write the pool payloads. The accepted scan evidence is reused; the new
focused check models its successful admission predicates.

The ordinary fixed image uses the frozen PMIC polling task and provider
implementations. It does not use today's shared power-domain service. Those
later implementations are irrelevant to a failure before this checkpoint.
The [host audit](../gpu-finite/HOST.md) records the available input-binding
evidence and its limits. Physical startup timing remains unobserved.

## Return proof

The checkpoint checks that scheduling has not started and that memory admission
completed. The complete application object must remain zero, including its
session, task handles, service, backend and provider state. Both pool pointers
must lie entirely in published private memory. Each pool has its exact original
geometry, generation counter 1 and an entirely zero allocation ledger.
The receipt binds the sealed session and retained runtime contract; it does
not invent an initialized application or a STOPPED GPU.

The exit path cleans only the private interval to PoC through the Nonsecure
EL1 mapping. No pool payload was allocated or written. EL3 rechecks the published
proof, restores the inherited GIC state and follows the existing final set/way
retirement and RVBAR/RMR return. The unchanged SMC tail uses no stack after
clearing M/C/I. This boundary runs in EL1h, so the checkpoint requires saved
caller mode 5. The normal GPU workload's EL1t mode-4 requirement is unchanged.

The checkpoint packet magic is `0x3150434555504741`; its two sequences, word
count and all 64 result words are zero. Receipt magic `0x475055454e543031` and
receipt status 1 mean `ENTRY_MEMORY_READY`. The monitor's detail is also 1.
Its compiled monitor magic is `0x4153373347535232`. These values cannot satisfy
the normal GPU result parser. The checkpoint parser reports `gpu_jobs: 0` and
`gpu_functional_success: false`.

The entire application scan reads 638,048 bytes per proof invocation. It uses
character accesses, which may inspect structure padding without aliasing
violations. Normal return performs the proof five times, reading about 3.1 MiB
of application state; each proof also checks two 1,024-byte ledgers. These are
CPU counts, not physical latency measurements. Compared with the first
checkpoint, this variant removes eight successful-path GPU-CCU/PCK reads and
adds four GIC reads plus one counter sample after restoration. No GPU instruction,
allocation, shader compilation, firmware start or power transition runs.

## Admission and capture

The distinct profile is `gpu-entry-memory-checkpoint-v2`. The producer reuses
the finite bundle serializer and exact-byte validator with explicitly named
checkpoint policies. The host uses the shared returning transport and the
reviewed five-word direct-ROM guard from commit `2f086241`. It never tests the
normal-boot input at `0x03000024`. Every result journal uses `checkpoint_result`.

The coordinator must supply current CPU entry, cache-publication, counter and
writer-exclusion facts, stable USB0 supplies and exclusive USB clock/power/reset
ownership for ROM transport, plus the exact two RAM reservations below. Keeping the
large reservation is required because the unchanged tables map both pools even
though this image does not write their payloads.

| Reservation | Interval, end excluded | Disposition |
| --- | --- | --- |
| CPU image, private state and monitor | `0x41000000–0x42200000` | Restore after validated return, or explicitly authorize discard |
| Original WB and NC pools | `0x48000000–0x79000000` | Same choice; both complete ranges require authority |

The passive entry record requires INTID 95 disabled, inactive and not pending,
and exclusion of all device and remote writers through return. The inherited
monitor already checks that interrupt at entry. The checkpoint keeps the four
GIC-only reads of distributor RWP and INTID 95 enable, pending and active state,
including a final check after GIC restoration followed by a deadline check.
Current CPU/GIC entry and continuity facts protect those accesses.

This variant performs no GPU-engine, GPU-CCU, PPU or PCK register access. The
complete zero application and empty allocation ledgers prove that it never
constructed or opened a provider. They do not prove absence of preexisting
DMA; full RAM ownership and writer exclusion must come from the coordinator.
The inherited GPU/PCK state is unobserved and unchanged by this software, not
qualified. This image grants no PMIC, GPU clock or GPU power operation.

On 2026-10-24, a safe passive capture on port 3-1 observed PCK domain-5 policy
and status low nibbles both 8. The earlier OFF-only checkpoint was not launched.
An OFF requirement belonged to the normal GPU shutdown proof and was unnecessary
at this never-open boundary. The normal GPU workload's OFF-only predicate is
unchanged. There is one checkpoint image for either inherited state.

The frozen `a733_gpu_power.c` (`410cbb99…`) accepts domain status 0 or 8 in
`DOMAIN_STATE` before proceeding through timing writes and its ON request.
It therefore would not refuse solely because status was already ON. This source
finding and the port 3-1 capture do not establish port 3-3's earlier state or
diagnose its failed attempt.

The shared 592-byte session retains legacy owner/supply fields for syntactic
admission; no provider consumes them here. Its `gpu` evidence reference and
`cold_master` token bind interrupt state and writer exclusion only.

A root review binds the exact image, launcher, source closure, board instance,
port, RAM disposition and supervisor. Use the existing direct retained runner;
do not wrap it in a timeout that kills the process or releases its lock. A
failed launch or return retains ownership until the coordinator's recovery.
Only the coordinator can authorize a physical attempt. The software-only fixture
under `host-*/` is not admission evidence.

## Commands and scope

Build outside the checkout:

```sh
python3 apps/gpu-entry-checkpoint/build.py --output /absolute/new-build
```

With reviewed current facts, create the distinct bundle and invoke its adapter:

```sh
python3 apps/gpu-entry-checkpoint/prepare.py \
  --image /absolute/new-build/build/checkpoint.elf \
  --manifest /absolute/new-build/manifest.json \
  --admission /absolute/reviewed-admission.json \
  --session-template /absolute/session-template.bin \
  --output /absolute/new-bundle --clang /absolute/clang --lld /absolute/ld.lld
python3 apps/gpu-entry-checkpoint/host.py \
  --bundle /absolute/new-bundle --fel /absolute/reviewed-fel \
  --evidence /absolute/new-run --ddr-evidence /absolute/ddr.json \
  --platform-survey /absolute/survey.json --warm-return /absolute/continuity.json \
  --root-owned-claim CLAIM
```

`check.py` exercises the actual checkpoint proof, publication and return
instructions with the existing architectural model. It checks binding, ownership,
ledger and pointer refusals, EL1h success, EL1t refusal and publication expiry.
The focused return checks use `--return-only --cases normal late-irq
late-irq-during-restore`. They fail on any GPU-engine, GPU-CCU, PPU or PCK
access, exercise normal return with inherited domain ON, and reject a pending
interrupt introduced before or during EL3 restoration. The unchanged resource
and ownership checks reuse their earlier evidence. `host-check.py` checks the
real producer, cross-profile refusals and capture adapter using the native
model's written reports. Neither check accesses hardware.

The larger mimus proposal remains separate: one K=4, N=16, 32-row job before
and after the actual OS thermal caller's healthy CLOSE→OPEN. Weights
`[1,c,1,1]` and activations `[17+48r,3,0,0]` give exact sums
`17+3(16r+c)`. Both jobs would verify all 512 sums and 32 sequence tags before
ending the output borrow. That proposal needs its own model/arena/output
reservations and mimus lease proof. It is paused until the earlier physical
boundary is understood; no runnable mimus qualification artifact is claimed.

The current candidate is
`gpu-entry-checkpoint-private-20261024/final-image/build/checkpoint.elf` under
the evidence cache, SHA-256
`8974f81a282c213f36e95f76da617399f4d9886999df5ca0db518c8a0116ecb7`.
`final-native/` preserves the focused return results and actual model-written
reports; `final-host/` holds the synthetic bundle and adapter journals. The
earlier ten native checks remain in `gpu-entry-checkpoint-20261024/final-native/`;
their unchanged resource and retirement evidence is reused.
`snapshot/manifest.json` binds the current source, object, image, launcher and
check artifacts. `proposal-DRAFT.json` lists the remaining coordinator bindings;
it is not physical admission.
