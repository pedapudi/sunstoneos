# Finite current-image CPU runtime qualification

Status: the standalone target builds and its native success and refusal
paths have offline checks. Physical qualification of this image remains
outstanding; the model supplies completed workload records as inputs.

The harness runs the current cached A733 startup, FreeRTOS SMP port and finite
CPU diagnostic, then returns to ROM after successful cleanup. It uses the
existing 16 pinned tasks: one worker and one background task on each CPU.
Each worker completes 32 floating-point context rounds. CPU0 supplies the
production timer tick; every core receives the production SGI handler.
The existing pre-scheduler exclusive-access check requires 256 increments.
This is finite integration evidence, not sustained-duration qualification.

The older returning workload binds the ELF with SHA-256
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3`.
Its worker completion patch, task frames and writable bounds are specific to
that image. They cannot establish execution of the current stackless fault
path. The current cached diagnostic instead retains all resources on exit.
The missing component is successful shutdown of the current runtime.

## Execution uses the current production objects

A separate application build reuses the current cached startup, coherence
controller, continuation controller, memory tables, kernel, patched port and
fault recorder. Private source patches add qualification hooks; they do not
modify the default production targets or patch final instruction addresses.
The diagnostic workload remains unchanged except for its terminal call. The
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
   progress and SGI count. CPU0 additionally verifies the tick count. Workers
   join at their pinned high priority, so lower-priority background and idle
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
on eight modeled cores. Eleven cases cover success, ordinary exit zero,
wrong core, absent peer, active or unrelated pending interrupts, wrong
Nonsecure cache alias, expired deadline, restoration readback failure,
acknowledgment without OFF, and a bad interrupted stack. The success case
checks every writable cache line and rejects Normal-memory stores after
cleaning begins. It does not emulate the preceding scheduler workload or
establish physical interrupt delivery, cache coherence or power transitions.
The assembly checks the original deadline on every cache line. The model
advances its counter by fixed ticks on each counter read, so these checks
verify deadline branches rather than the physical elapsed time of cleanup.

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

The producer has no launch command. After a separately authorized execution,
`host.py read-report --bundle ... --report ... --output ...` validates an
already copied report against the prepared epoch. Physical entry, ROM
continuity, memory restoration and release remain the coordinator's work.
Freeze the ELF, build manifest, model results, host bundle and exact
reservation before that execution; none of the offline checks grants board
eligibility.
