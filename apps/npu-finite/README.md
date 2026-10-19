The NPU finite image combines the existing supervised single-core convolution with an application-local returning monitor. It retains the original model, PMIC/PLL/startup services, session validator and private memory constructor. The two jobs still copy, compare and reap their outputs before terminal CLOSE, local gate/reset shutdown and device-task quiescence. The host compiler and compiled model artifacts remain pinned external dependencies.

The selected FreeRTOS SRE port initializes tasks at EL1t, with SP_EL0 as the stack selector. This image does not link the cached SMP SVC119 exit handler. The task-profile check executes the actual stack constructor and observes saved PSTATE 4. The privileged return path follows the selected SRE architecture.

Before providers start, the unchanged EL3 monitor authenticates the uploaded session and seals its private copy. The application-local monitor includes that implementation and replaces only its terminal exception entry. The retained session, authentication receipt and runtime contract survive EL1 BSS clearing. The additional return receipt occupies the same separately retained section.

After successful task quiescence, normal exit checks the numerical result and publishes a receipt bound to epoch, generation, lease, status, deadline and exact linked memory interval. It stops the EL1 timers and cleans every 64-byte line from `__npu_clean_start` through `__device_pool_end`. The interval includes retained metadata, provider state, stacks, command buffers and NPU tables. Page-boundary deadline checks bound this sweep.

The identity-mapped assembly tail disables EL1 translation and caching, then executes SMC without another stack access. EL3 consumes the cleaned receipt while retaining its separate monitor state. A normal return requires a matching receipt, unexpired contract, secondary CPUs still OFF and successful bounded GIC restoration. The monitor then completes whole-cache maintenance and uses the accepted small kernel's forced-FEL flag and RMR sequence. The final cache operation also covers stack stores made after the interval sweep.

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

This is a complete software image with modeled handoff evidence. Root architecture/reset review, the NPU-specific returned-RAM host wrapper, actual admission, physical inference and physical ROM return remain outstanding. Neither a build nor an instruction check establishes those physical results.
