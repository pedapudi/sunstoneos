The NPU finite image combines the existing supervised single-core convolution with an application-local returning monitor. It retains the original model, PMIC/PLL/startup services, session validator and private memory constructor. The two jobs still copy, compare and reap their outputs before terminal CLOSE, local gate/reset shutdown and device-task quiescence. The host compiler and compiled model artifacts remain pinned external dependencies.

The selected FreeRTOS SRE port initializes tasks at EL1t, with SP_EL0 as the stack selector. This image does not link the cached SMP SVC119 exit handler. The task-profile check executes the actual stack constructor and observes saved PSTATE 4. The privileged return path follows the selected SRE architecture.

Before providers start, the EL3 contract check copies the uploaded session into private retained memory and authenticates that copy; the unchanged monitor then seals it. The application-local monitor includes that implementation and replaces only its terminal exception entry. The retained session, its admission flags and the runtime contract survive EL1 BSS clearing. The additional return receipt occupies the same separately retained section.

After successful task quiescence, normal exit checks the numerical result and publishes a receipt bound to epoch, generation, lease, status, deadline and exact linked memory interval. It stops the EL1 timers and cleans every 64-byte line from `__npu_clean_start` through `__device_pool_end`. The interval includes retained metadata, provider state, stacks, command buffers and NPU tables. Page-boundary deadline checks bound this sweep.

The identity-mapped assembly tail disables EL1 translation and caching, then executes SMC without another stack access. EL3 consumes the cleaned receipt while retaining its separate monitor state. A normal return requires a matching receipt, unexpired contract, secondary CPUs still OFF, successful bounded GIC restoration and the shared SRAM routed back to the CPU. The monitor then completes whole-cache maintenance and uses the accepted small kernel's forced-FEL flag and RMR sequence. The final cache operation also covers stack stores made after the interval sweep.

Every unexpected SMC, exception, recovery-timer event, invalid receipt or cleanup timeout retains execution. Those paths do not write reset flags or request RMR. A numerical mismatch can return status one only after the same successful shutdown and cleanup. Software deadlines cannot recover a synchronous bus stall; the reviewed physical recovery plan remains required.

Build outside the repository:

```sh
python3 apps/npu-finite/build.py --evidence ~/.cache/sunstoneos/evidence/npu-finite-return-20261018
```

The builder reuses the selected single-core producer and replaces its monitor, exit and linker objects locally. It binds the additional runtime sources into a new immutable build identity and preserves the effective compiler dependency closure. It retains the fixed artifact-hash table for the existing image/session validator. The new manifest profile is `npu-finite-return-v1`; the earlier USB capture profile cannot represent this image.

The new instruction checks cover the complete mapped and cleaned interval, transition ordering, receipt refusals, bounded interrupt cleanup and exact reset-tail writes. A retained-session composition mode checks actual EL3 sealing, upload mutation, EL1 BSS clearing and page-table permissions. Their system-register, cache, timer, GIC and CPU effects are modeled. The accepted six client and nineteen quiescence cases are reused for unchanged device code.

The root host proposal is generated without hardware access:

```sh
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python apps/npu-finite/proposal.py \
  --evidence ~/.cache/sunstoneos/evidence/npu-finite-return-20261018 \
  --output ~/.cache/sunstoneos/evidence/npu-finite-return-20261018/root-proposal.json
```

The coordinator must supply fresh physical admission and review the changed normal-return lifetime. The existing admission tool requires retaining all resources until power removal; it must not be silently relabeled as successful returning retirement. Failure recovery continues to require full power removal. No default session or qualified admission is generated.

Before uploading, the host must save and hash the complete exclusive DRAM reservation `0x41000000..0x42200000`. After one bounded launch, it must confirm a responsive new ROM instance and copy the monitor report, platform log and return receipt. Only a validated normal completion permits restoring and comparing saved DRAM. Old SRAM must never overwrite the newly running ROM. Missing or uncertain completion retains ownership and requires explicit recovery.

On 2026-10-24 the coordinator ran the frozen numerical image `9e4adabf` on
port 3-1. Both jobs completed and were reaped, but both copied outputs failed the
reference comparison. CLOSE, local stop, provider parking and normal ROM return
completed; the host restored all 18 MiB of saved DRAM. The immutable session is
`processor-completion-20261024/port-3-1/npu-numerical/runtime/session.json` in the
evidence cache. It records status 1 and `functional_success=false`. Numerical
correctness remains unresolved, and retained PLL/domain/rail state forbids an
unreviewed repeat startup. This result does not qualify the separate thermal
profile, whose software evidence is described in [ENTRY.md](ENTRY.md).

The application-local `host.py` implements the returned-RAM host path. `prepare` requires explicit image, manifest, session, admission, compiler, linker and objcopy paths. It reuses the original NPU image/session/request producers and protected SRAM launcher. `validate --bundle PATH` verifies all copied evidence, current host source hashes, exact ELF load bytes, request packets and launcher hash without accessing hardware. The prepared bundle includes evidence and source snapshots; it creates no qualified defaults.

The admission schema is `a733-npu-finite-entry-v1`. It preserves the original platform, DDR, clock, power and typed session requirements. Its new supervision conditions explicitly authorize successful retirement after CLOSE, local stop, provider quiescence and ROM return. They retain failed attempts until whole-board power removal and require validated normal completion before DRAM restoration. The old retained-only admission schema is rejected.

A separate `a733-npu-finite-return-review-v1` record must bind the exact image hash, build identity, current target identity, port, supervisor, launcher hash and every host source hash. Its four hashed condition references cover architecture/reset, software handoff, normal-return retirement and failure retention. The root coordinator must review and provide those records. Software fixtures do not supply physical authority.

The `run` command requires a validated bundle, the pinned FEL executable, an evidence directory, current DDR evidence, a platform survey and an existing root-owned claim. Repeat `--warm-return` for the accepted continuity chain. The root wrapper must verify and manage that claim; this program neither accesses queues nor closes claims. Under the existing exclusive board lock, the runner revalidates target identity and evidence, saves all 18 MiB of DRAM, reuses protected upload/readback, and launches once. A transport failure forbids further requests. ROM enumeration is passive and bounded to 30 seconds; subsequent report reads require a new accessible A733 ROM instance.

The monitor report, platform log and receipt must agree on completion status, epoch, generation, lease, time bounds and exact cleanup extent. The host independently checks both copied outputs against the ELF reference. Its result also records, under `output_pattern`, the ranges of the second copied output that still hold the SUBMIT pattern `NPU_CONVOLUTION_OUTPUT_POISON`, whether the pattern covers every byte of that output, and whether the first job's digest is the digest of an output that kept the pattern in every byte. A short range can match by chance; a range covering the output means that job wrote nothing. The fields have this meaning only for images whose SUBMIT fills the pattern, which the frozen image `9e4adabf` predates. Status zero permits a successful result. Status one permits DRAM restoration only when the reports demonstrate normal shutdown and the copied output demonstrates a numerical mismatch; it returns command exit code two. Missing, malformed or conflicting reports retain ownership. Restoration hashes the exact saved bytes passed to a sealed upload, writes only DRAM and verifies every byte. Old SRAM is never restored after ROM return. A restore failure also retains ownership.

`host-check.py` uses the exact frozen native ELF, original producers and protected upload helpers with modeled FEL operations and ROM enumeration. Its fixtures cover packaging, admission, both numerical statuses and changed failure paths. They never discover a board, create a queue record or establish live admission. The shared transport loop is exercised separately with mocked port metadata, subprocesses and time. Real lock, permission, enumeration and subprocess behavior remain unobserved by these fixtures. The native image is not rebuilt by host checks.

`tools/a733-returning-host.py` owns the shared live transport loop. The NPU adapter supplies its admission callback and finite execution callback; the helper imports no device model or admission schema. The board lock uses the selected A733 physical port, and the USB churn record names that same port. Every request rechecks instance identity and physical port. The returning-ROM lookup stays on the original sysfs path. The existing USB guard still enforces its host-controller health and shared churn spacing policy.

`transport-check.py` exercises only the extracted loop and NPU callback composition. It checks terminal transport failure, changed identity, missing or inaccessible new ROM, wrong return port, dirty-state retention and numerical-status propagation. It does not repeat native handoff or pure report validation cases. The helper is part of the host source hashes required by root review, so extraction requires a freshly bound host review record.

The [entry procedure](ENTRY.md) describes explicit session construction, the
forced-ROM instruction check and the missing physical authority. The host
constructs session bytes only from a supplied typed boot payload and complete
admission, validates them before writing, and refuses existing output files.
The return/profile/client fixtures select only their consumed DWARF layouts,
so unrelated private structure names cannot block those checks.

The image routes the shared SRAM to the NPU from just before the NPU resets
are released until local stop, and the monitor sets the CPU routing again at
EL3 before the report and the ROM return; NPU register access is refused
unless the startup is READY. The design, exit paths and checks are in
[docs/designs/npu-sram-route.md](../../docs/designs/npu-sram-route.md).
`sram-route-check.py` runs the image's linked sequencer through that window:

```sh
python3 apps/npu-finite/build.py --evidence OUTPUT
python3 apps/npu-finite/sram-route-check.py --evidence OUTPUT --output OUTPUT/sram-route-check.json
```

`build.py --rate-852` builds the image with PLL_NPU at 852 MHz at the
unchanged 800 mV; its register pass refuses a speed bin whose operating-point
column does not list 800 mV for that rate
([design](../../docs/designs/npu-852mhz-plan.md)). Check it with
`sram-route-check.py --rate 852`. The client logs startup and job times in
counter microseconds as `NPU_TIME_US`, and each job's system-counter times and
NPU total and idle cycles as `NPU_JOB_COUNTERS`; the host reports both as
`timing` ([design](../../docs/designs/npu-irq-completion.md)), and each job's
SUBMIT, publication and doorbell times as `NPU_JOB_PHASES`.
`build.py --interrupt --shufflenet` runs the published ShuffleNetV2 graph
instead of the trusted convolution
([design](../../docs/designs/npu-full-model.md)).
