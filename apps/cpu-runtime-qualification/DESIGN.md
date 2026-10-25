# Finite current-image CPU runtime qualification

Status: the idle/wake revision has passed focused scheduler, instruction and
host checks. Physical qualification of this revision remains outstanding.
Its report version is 3 and its execution profile is
`current-finite-cpu-idle-wake-v3`.

The first version completed one bounded physical run on October 24, 2026.
The preserved session is
`processor-completion-20261024/port-3-1/current-cpu-root-review/runtime/session.json`
under the evidence cache. It binds image SHA-256
`9d90b6f97b2f56a8ad4a20af0add4d31ec686c1a516918cb44c6fc52c845ef09`:
all eight cores completed 32 rounds, CPU0 received 32 timer interrupts,
secondaries received no timer interrupts, and every core received SGIs.
The session returned to a distinct ROM instance and restored its DRAM.
That prepared-lifetime pass does not qualify this revision or cold-start
repeatability. Earlier successful CPU0 publication and all-core diagnostics
had already run in that power lifetime.

The secondary-timer version also passed one bounded run on October 24, 2026,
recorded in `processor-completion-20261024/port-3-1/cpu-local-timer/runtime/session.json`.
It binds image SHA-256
`f5ba965db5797b8e5050728577462498b2959d1f3f4436e6dd90fb2fa4e98717`.
All eight cores completed 32 rounds with background and SGI progress;
CPU0 received 32 timer interrupts and each secondary received one.
All seven secondaries were observed OFF, a distinct ROM instance returned,
and the session restored all 18 MiB of DRAM. Its busy background tasks did
not provide a bounded idle/wake observation.

The harness runs the current cached A733 startup, FreeRTOS SMP port and finite
CPU diagnostic, then returns to ROM after successful cleanup. It uses the
existing 16 pinned tasks: one worker and one background task on each CPU.
Each worker completes 32 floating-point context rounds. CPU0 supplies the
production timer tick; every core receives the production SGI handler.
Each secondary also receives exactly one private physical-timer interrupt.
The existing pre-scheduler exclusive-access check requires 256 increments.
This is finite integration evidence, not sustained-duration qualification.

The older returning workload binds the ELF with SHA-256
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3`.
Its worker completion patch, task frames and writable bounds are specific to
that image. They cannot establish execution of the current stackless fault
path. The current cached diagnostic instead retains all resources on exit.
This standalone target supplies successful shutdown for that runtime.

## Secondary timers prove local delivery without multiplying kernel time

The private continuation hook runs after acquiring the primary initialization
flag and before publishing secondary readiness and issuing SVC106. Interrupts
remain masked. Each secondary verifies its timer is disabled and PPI30 is
inactive, not pending, disabled and configured as a level interrupt. Startup
already validated its redistributor, interrupt group and CPU interface. The
hook uses the same Nonsecure priority as CPU0 (`0xe0`), preserves SGI0, and
checks priority and enable readbacks within the original startup deadline.

Each secondary publishes an armed state before setting a compare value one
millisecond ahead and enabling its physical timer. A timer that expires before
SVC106 remains pending until the scheduler restores an interruptible task.
The private dispatch routes INTID30 on cores 1–7 to a one-shot callback.
It reads `CNTP_CTL_EL0` before disabling the timer, requires enabled,
unmasked and asserted status, and accepts exactly one armed-to-delivered
transition with an initial count of zero. Disabling the timer and completing
its barriers precede the real port's EOI. An unarmed, repeated or inconsistent
interrupt enters retained failure.

A retained failure publishes a reason code in the report's error word and in
the failing CPU's error word; `REASONS` in `host.py` names each code, and the
host decoder reports it. Missing idle/wake proof is reason 8, and a timeout or
peer failure at the drained barrier after timer quiescence is reason 9.

CPU0's setup, compare advancement and FreeRTOS tick call remain unchanged.
Secondary callbacks never invoke `FreeRTOS_Tick_Handler` or change the shared
`next_tick`. The kernel has one global tick count; calling its tick handler on
every CPU would advance that count multiple times per interval. Worker
completion and the final masked capture both require one secondary delivery.
The final capture remains authoritative. Existing shutdown disables both
local timer generators, drains SGI0/PPI30, restores the saved Secure timer
priority word and timer controls, and independently observes secondary OFF.

The added budget is seven IRQ entries, fourteen local GIC writes, fourteen
timer arm writes and seven timer-disable writes. Each callback adds one
status read. Per-core delivery state occupies 32 bytes in total; no recurring
secondary tick, shared lock or new deadline is added. In the instruction model,
one arm executes 89 instructions and one complete IRQ, including the vector
and port frame, executes 263 instructions. These counts apply to the nominal
readback path, with no RWP wait. The original one-second startup and 20-second
session bounds remain. The secondary-timer revision increased linked text by
352 bytes relative to the first physical image; its delivery state is 32 bytes.

Arm's [Generic Timer guide](https://documentation-service.arm.com/static/6294c39db334256d9ea8ae53),
revision `102379_0100_02_en`, section 3, describes per-core physical timer
interrupts, meaningful ISTATUS while enabled, and level deassertion by
disabling the timer. Its PDF SHA-256 is
`a067f58710ea3ce55532b6a23ce904f1b4e9a87f6478d7c5e489fbec1098ddd6`.
[Arm IHI 0069G](https://documentation-service.arm.com/static/601412d54ccc190e5e681269)
section 4.1 defines level-sensitive pending/active transitions
and combined priority drop/deactivation when EOImode is zero. Sections 4.8.1
and 4.8.4 define the Secure priority-mask restriction and Nonsecure priority
view. The cached GIC manual has SHA-256
`f5461ced0b908141c87f655e602ff6340a90f14028ea009239c388ed69b78ac6`.
The actual FreeRTOS ARM_CR82 port is pinned at commit
`97f50c0ecf5b3f68aa9d1c1fb27f4ad707ddb5c2`; its patched IRQ assembly saves
integer/FP state, calls the application handler and then writes EOI.

## A finite idle phase exercises the existing wait and wake paths

After its 32 rounds, each worker suspends its own pinned background task
through the actual kernel API. That background cannot be running on another
core. Each worker publishes readiness; secondary workers then wait for one
counted task notification. CPU0 publishes a fixed 16 ms phase deadline, waits
for all readiness records and sends those notifications. A secondary that
finishes before CPU0 uses the remaining original session bound for its
initial wait. No deadline is renewed.

Each notified worker then blocks in two separate two-tick delays. CPU0 also
blocks twice. The private idle hook wraps the unchanged `DSB SY; WFI` helper
and counts a completion only if that idle invocation entered after its own
worker's readiness publication. That eligibility remains local across WFI;
no older invocation can acquire eligibility while sleeping. A notification
before the worker's wait remains counted by FreeRTOS.

An interrupt can arrive immediately before WFI, or preempt immediately after
it. Waiting for an idle-completion counter before sending a notification
would risk withholding the next wake. This phase sends notifications without
that dependency. The first delayed interval lets an idle context reach WFI;
the second lets a context interrupted after WFI publish completion. Each
worker checks its core after waking. Completion requires a WFI return on
every core and new SGI delivery on every secondary. Missing progress or an
expired bound retains the session; it does not diagnose a hardware failure.

The phase adds no tasks, stacks, interrupt sources, timer owners, MMIO or
power transitions. Its private counters and handles occupy 392 bytes. Linked
text grows by 3,880 bytes, including the newly used kernel notification and
suspend paths; BSS grows by 400 bytes including alignment. CPU0 alone advances
kernel time. There are seven
notification requests and fourteen secondary delayed-wake requests; existing
scheduler activity can add SGIs, so these are not a total interrupt budget.
The original one-second startup and 20-second session bounds remain. Once
all workers finish, their existing high-priority terminal barrier excludes
background and idle execution. The final masked capture verifies the idle
completion count and notification/delay flags before the unchanged writer
barrier, interrupt drain, clean, OFF and ROM return.

Two previously reserved words in each 1,024-byte CPU record now carry the WFI
completion count and wake flags; the report remains 8,880 bytes. The flags
require both delayed resumes on CPU0, plus notification and both delayed
resumes on each secondary. These observations prove executed waits and
subsequent interrupt/task progress. They do not measure WFI duration, power
gating, energy reduction or the precise interrupt that ended each wait.
The existing idle-hook, nTWI and pinned-idle rationale is in
[cpu-power.md](../../docs/cpu-power.md). The thermal owner's checkpoint
backoff uses the same scheduler blocking path, but this application does not
start the thermal sensor service or qualify physical thermal control.

## Execution uses the current production objects

A separate application build reuses the current cached startup, coherence
controller, continuation controller, memory tables, kernel, patched port and
fault recorder. Private source patches add qualification hooks; they do not
modify the default production targets or patch final instruction addresses.
The diagnostic adds background-handle registration and the final idle phase. The
private continuation patch omits resident previous-boot report rotation:
that routine reads a stage2 report at `0x47008200`, outside this supervised
reservation. The supervised launcher already clears the complete 16 KiB
result area, including all fault slots. The current stackless fault recorder,
pending bytes and exception classification remain linked unchanged.

The app uses the existing supervised RAM contract from
`platform/a733-smp-supervised/admission.c`. The contract binds the build,
epoch, generation, reservation and external admission digest. Its startup
bound remains one second and its total observation bound remains 20 seconds.
It does not arm a watchdog or start the USB observer. The host must bind the
complete linked RAM reservation, including Device monitor storage and fault
slots. A nonzero digest or reservation identifier supplies no authority by
itself.

## Success requires all eight writers to stop

1. Before production initialization changes shared state, capture the
   distributor and secondary CPU entry/control words. Each core captures its
   redistributor, timer and system controls before local initialization.
   Redistributor admission requires readable, powered, stable state before
   SGI/PPI accesses. Snapshot both the SGI0 priority word and the Secure
   priority word containing PPI30.
2. Each worker completes its existing rounds and verifies its own background
   progress, SGI count and assigned timer count. The idle phase then verifies
   WFI completion and notification/delay resumes. Workers join at their pinned high priority, so lower-priority background and idle
   tasks cannot resume while the workers wait.
3. A dedicated terminal SVC verifies the qualification epoch, core and caller
   PC. It masks interrupts and stops both local timer generators. Its
   corresponding SMC requires status zero, detail zero and marker zero.
   An ordinary successful application exit is not a qualification terminal.
4. The SMC traverses the current EL3 exit-capture path. That path writes
   `a733_fault_pending` in Normal Nonsecure RAM and an exit record in the
   Device fault page. Only the qualification success hook publishes the
   matching capture-complete epoch in Device monitor storage.
5. Every core waits for all eight capture-complete epochs. This is the final
   Normal-memory writer barrier. A vector fault or missing participant
   cannot satisfy it. Stop owned interrupt delivery, require no active
   interrupt, refuse unrelated pending state, and clear only SGI0/PPI30.
   Every core then publishes `drained_epoch`; all eight must match before
   any core cleans memory.
6. Clean the entire linker-derived cached writable range through the current
   EL3 Nonsecure write-back mapping, with translation still enabled. Include
   task, idle and IRQ stacks, kernel state and the fault-pending bytes.
   Fault slots and completion records remain Device memory. Every line
   operation is bounded by the original session deadline.
7. After cleaning, use only registers and verified Device monitor storage.
   Any C cleanup routine must run on a range-checked Device EL3 stack.
   Restore and read back the saved interrupt and system controls where
   architectural transitions permit it. Preserve enabled GIC system-register
   interfaces as described below. Complete
   each secondary's redistributor sleep handshake before requesting CPU OFF.
8. CPU0 requires both the epoch-matched shutdown acknowledgment and an
   independent OFF observation for each secondary. Only then may it restore
   CPU entry/control words and distributor state, preserve enabled affinity
   routing, and use the existing RVBAR/RMR ROM-return sequence.

The current EL3 fault handler writes Normal memory, so cleaning before the
SMC is insufficient. Moving its capture after cleanup would invalidate the
no-writer condition. The capture-complete barrier must precede the final
clean and follow the actual recorder.

## Failure retains the current runtime

A vector exception, wrong terminal provenance, missed deadline, mismatched
readback, missing participant or uncertain interrupt state does not enter
successful restoration. The existing stackless fault recorder and retention
path remain active. A failed session requires coordinator-directed recovery;
a host observation deadline does not release its ownership.

The bad-stack check must enter the actual fault path with initialized native
core records and an invalid interrupted stack. It must observe a completed
fault slot and no success-cleanup publication or restoration writes.

## Cleanup follows existing source-backed ordering

The implementation reuses the established order and checks from:

- `boot/a733/all-core-first-task-probe.S`: final writer stop, owned interrupt
  drain and full writable cache publication;
- `boot/a733/secondary-local-probe.c`: redistributor restoration and sleep;
- `boot/a733/secondary-distributor-probe.c`: distributor restoration while
  preserving affinity routing;
- `boot/a733/secondary-identity-probe.S`: matched acknowledgment and CPU
  power-down, including the Cortex-A76 barrier before the final ISB;
- `boot/a733/native-el1-cache-probe.S` and `native-el3-cache-probe.S`:
  translation-control restoration with blocked EL1 walks;
- `platform/a733-smp-cached/continue.S` and `platform/a733/fault.S`: current
  stackless exception classification and retained per-core capture.

Arm IHI 0069G section 12.1.6, pages 12-193 and 12-194, makes clearing an
already-set `ICC_SRE_EL3.SRE`, `ICC_SRE_EL2.SRE` or Secure
`ICC_SRE_EL1.SRE` unpredictable. The current entry enables these interfaces.
Cleanup preserves those enables rather than replaying a saved zero. This
matches the existing returning helpers, which record the earlier SRE value
without restoring it. The Nonsecure EL1 rule differs and must not be used to
justify clearing the other interfaces.

Reuse of these sequences does not transfer the old binary's physical result
to the new application. Secure and Nonsecure system-register banks, current
linker bounds, and the final write set require checks against the new ELF.

## Validation stays bounded

Build one supervised CPU-only image with the current port and fault page.
Check source and linked-object identities, actual vector targets, terminal
PCs, Device stack bounds and every cached writable page. Run one complete
success model and focused refusal cases from preserved checkpoints: wrong
terminal identity, absent peer, active or unrelated pending interrupt,
restoration mismatch and the actual bad-stack fault path. Reuse unchanged
fault-recorder, clock, driver and historical startup evidence within their
original scope. Do not rerun performance or broad stress suites.

The native check is `checks/cpu-runtime-qualification.py`. It executes the
linked success terminal, actual SVC/SMC vectors, fault recorder and shutdown
on eight modeled cores. Fifteen cases cover success, ordinary exit zero,
wrong core, absent peer, active or unrelated pending interrupts, wrong
Nonsecure cache alias, expired deadline, restoration readback failure,
acknowledgment without OFF, missing or duplicate secondary timers, missing
idle/wake proof, and a bad interrupted stack. The success case
checks every writable cache line and rejects Normal-memory stores after
cleaning begins. It does not emulate the preceding scheduler workload or
establish physical interrupt delivery, cache coherence or power transitions.
The assembly checks the original deadline on every cache line. The model
advances its counter by fixed ticks on each counter read, so these checks
verify deadline branches rather than the physical elapsed time of cleanup.

The secondary-timer revision preserves 22 focused cases from
`checks/cpu-local-timer.py`; its timer hook and dispatch are unchanged here. Seven execute the linked arm hook, actual continuation SVC106,
task restoration, vector and IRQ frame on each secondary. They check all
integer and FP registers, SP, NZCV, FPCR and FPSR, exact timer counts, unchanged
global tick state, and disable-before-EOI ordering. The remaining cases reject
bad arm state, busy RWP at the deadline, lost enable/priority writes and bad
IRQ state. A restrictive inherited Secure priority mask prevents modeled
delivery, showing why startup's existing Secure initialization remains
necessary. The model represents Nonsecure priority rounding to four bits.
It supplies admitted local controller state and task frames; it does not
rerun the complete scheduler or establish physical interrupt delivery.

`checks/cpu-idle-wake.py` executes actual task creation, the 32-round workload,
idle hooks, notifications and delays from admitted EL1 fixtures. It reuses
the existing cached scheduler and WFI model. The workload prefix executes once up to the first worker entering the idle
stage. Each of five cases resumes that checkpoint and completes any remaining
rounds: nominal completion, notification before blocking, interrupts
immediately before and after WFI, and phase-deadline refusal. Success requires
all eight real background suspensions, matching worker contexts and WFI
instructions on all eight cores. Startup/coherence and pre-scheduler behavior
remain admitted boundaries. The scheduler check stops at the terminal call;
the separate native check executes retirement. Neither models physical cache
or power behavior.

Offline results and frozen inputs for this revision are under
`cpu-idle-wake-20261024` in the evidence cache. The earlier timer cases remain
under `cpu-local-timer-20261024`. The host checks reject the
older report version, missing idle/wake proof, missing or duplicate secondary
timer counts, and a
mismatched execution profile. The two focused transport cases use the actual
new native model report for successful restoration and rejected-report
retention. Unchanged geometry and producer checks retain their earlier proof.

`apps/cpu-runtime-qualification/host-check.py` checks the compiled C report
layout, actual ELF allocations, missing-admission refusal and mutations of
the native model's output. The report is 8,880 bytes at the linked
`a733_qualification_report` symbol; each of its eight CPU records is 1,024
bytes. Epoch-matched capture, interrupt drain, clean and restore publications
are required. Each secondary additionally needs a request, acknowledgment
and independent OFF observation. A copied report alone does not prove ROM
continuity or release the reservation.

## Build and prepare without device access

Configure `apps/cpu-runtime-qualification` as a standalone CMake project with
`cmake/aarch64-clang.cmake`, the pinned FreeRTOS source in `FREERTOS_DIR`, and
the existing Clang/LLD tools. Build only `a733-cpu-runtime-qualification`.
The outputs are `a733-cpu-runtime-qualification.elf` and
`qualification/manifest.json` under that build directory. The manifest binds
the sources, compiler options, build ID and final ELF digest.

Run the focused native check with the ELF path and an `--out` directory.
It needs the existing Python environment containing Unicorn, Capstone and
pyelftools. Run `host-check.py --help` for the host check's inputs; its nominal
report input must come from the native check or a separately captured run.

The offline producer is `host.py prepare`, with required `--image`,
`--manifest`, `--admission`, `--output`, `--clang`, `--lld` and `--objcopy`
arguments. It reuses the supervised admission validator, nonce-bound
contract and launcher. Admission must already bind the actual image,
exclusive RAM interval `0x41000000..0x42200000`, disabled watchdog, and
coordinator-directed recovery evidence. No admission is generated by these
checks. The enlarged monitor allocation, cleanup stacks and fault slots
must fit that same explicitly authorized interval.

`host.py run` uses the existing returning-ROM transport with required
`--bundle`, `--fel`, `--evidence`, `--ddr-evidence`, `--platform-survey` and
`--root-owned-claim` arguments. Supply each intervening successful session
with `--warm-return` in execution order. The coordinator must already own the
matching queue claim; the command records that claim and neither creates nor
releases queue authority. Current identity, DDR continuity, launcher survey
and the complete warm-return chain must match the admission evidence.

Before upload, the adapter checks the forced-ROM entry words and the current
A32 caller stack interval. The launcher's 60-byte save area must not overlap
its SRAM reservations. The adapter saves all 18 MiB of admitted DRAM in
bounded chunks, then uses the existing supervised preload and disabled
watchdog checks. It performs one execution at `0x52000`. The native contract
retains its 20-second deadline; host discovery allows 50 seconds to cover
that interval and 30 seconds for a distinct ROM instance on the same port.
An expired window cannot force a stalled processor to return.

After ROM identity succeeds, the adapter reads the ELF-derived 8,880-byte
report and requires its matching success epoch and all eight completion
records. It verifies every saved DRAM digest before restoration and checks
each write by reading it back. Old SRAM backups remain evidence: the new ROM
owns its current stack and scratch memory. A transport, report or restore
failure stops the attempt and retains ownership; it does not trigger a retry
or permit further device requests. Successful completion records the actual
native report, ROM continuity and restored DRAM for the existing warm-return
validator.

`host.py read-report --bundle ... --report ... --output ...` remains available
for an already copied report. It validates report contents without claiming
transport continuity or restoration. Freeze the ELF, build manifest, model
results, host implementation, bundle and exact reservation before execution.
Offline checks do not grant physical authority.

## Remaining physical robustness gaps

One prepared-lifetime run does not resolve the earlier unlocated cold-start
failure or establish repeatable cold startup. The secondary one-shot timers have one prepared-lifetime physical pass on
both A55 and A76 cores. The added idle phase still requires physical
qualification. These bounded runs do not establish sustained eight-core load,
long-running coherence, nested interrupt stress, runtime core
hotplug, or coordinated fault recovery. Normal retirement executing the fault
recorder does not qualify induced faults or their recovery.

Secondary OFF at test completion is distinct from runtime hotplug and idle
power management. This application neither changes CPU voltage/clock points
nor qualifies thermal throttling. Supported operating points, their shared
power-controller permission boundary and sustained thermal operation remain
separate requirements. Existing fault retention preserves evidence; it does
not supply a reset or recovery guarantee.
