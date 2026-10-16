# Cached eight-core FreeRTOS diagnostic

The separate cached image boots all eight A733 CPUs into a shared FreeRTOS
scheduler. It applies the supported CPU revision workarounds, installs matching
cached RAM mappings at EL3 and nonsecure EL1, and exercises affinity, inter-core
interrupts, timer ticks and floating-point context preservation. Physical
coherence, reset recovery and board operation are not qualified.

The image is implemented in `platform/a733-smp-cached/`. The existing host launcher
admits only the uncached diagnostic. Cached-image execution requires a separate
launcher review and independent watchdog/FEL recovery qualification.

## Boot memory stays consistent while cores enable translation

Boot controllers, entry mailboxes, retained reports and EL3 stacks occupy
`0x42000000..0x421f0000`. Every EL3 participant accesses those bytes as Secure
Device-nGnRnE before and after enabling its MMU. Naturally aligned scalar
loads/stores and `DSB SY` publish boot state. No exclusive instruction accesses
this region. Boot C uses `-mstrict-align` and `-mgeneral-regs-only`.

Five immutable 4 KiB table pages occupy the separate reservation at
`0x421f0000..0x42200000`. Three pages provide EL3 level-one, level-two and
level-three tables. Two pages provide EL1 level-one and level-two tables.
EL3 CPU mappings select Secure Normal-noncacheable attributes for EL3 table
pages and Nonsecure Normal-noncacheable attributes for EL1 table pages. These
physical address spaces match the respective table walkers. All table writes
finish before publication and the first walk. Access flags are preset; hardware
access-flag and dirty-state updates are disabled. Software never reads or writes
the table pages after publication.

Ordinary RAM uses Write-Back, Write-Allocate, Inner Shareable attributes at both
exception levels. EL3 descriptors select the nonsecure physical address space,
matching nonsecure EL1. Code at `0x41000000..0x41200000` is immutable and read-only.
Other ordinary RAM is execute-never. EL1 has no CPU mapping for the boot monitor
or table reservation. MMIO remains Device memory.

The boot sequence is bounded by the contract's absolute startup deadline:

1. Core 0 validates the immutable watchdog contract and readbacks. It builds and
   publishes the tables, code and initial RAM, then applies its CPU and DSU reset
   workarounds while the other cores remain parked.
2. The primary powers and releases each secondary through the authenticated
   entry protocol. Each secondary applies its local reset workarounds and checks
   that the DSU identity matches the primary. Each core prepares its GIC
   redistributor and publishes readiness in its own Device record.
3. All cores wait for every reset/GIC record. Each enables its EL3 translation
   and caches after clearing and verifying `SCR_EL3.SIF`. That control must allow
   Secure instruction fetches from the immutable NS-mapped code. Each core
   records the actual SCTLR and TTBR readbacks, and waits until every
   core has published successful EL3 adoption. This barrier accesses only the
   Device monitor; it cannot introduce a Device/Normal alias on its flags.
4. Each core enters EL1 through a stackless assembly path. It enables the same
   cached RAM profile and verifies the control-register readbacks before any
   stack or runtime-data access. It then installs the runtime vectors and
   publishes its cached runtime record.
5. An eight-core exclusive-access check requires 32 protected increments per
   core and a final total of 256. Only then can the port start FreeRTOS. Sixteen
   pinned tasks exercise every core, timer wakeups and floating-point context.

Arm's Trusted Firmware-A documents Device memory for shared boot state during
incremental MMU/cache adoption in
[`firmware-design.rst`](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/docs/design/firmware-design.rst).
This image uses the stricter nGnRnE subtype, matching MMU-off data accesses.
Device attributes do not establish DRAM ownership or the board's security
controller configuration.

## Exception paths preserve the cache transition

The pinned CPU implementation supplies revision-specific reset controls and
patch-store sequences through [the coherence component](cpu-coherence.md).
The primary alone writes shared DSU controls. Other cores verify the required
mask without modifying the shared register.

The speculative address-translation workaround blocks both EL1 table walks,
executes an instruction barrier, sets `SCTLR_EL1.M`, and executes another
instruction barrier while EL3 runs. Before entering EL1, the image restores
SCTLR, executes an instruction barrier, and restores TCR last. The ordering
follows pinned Trusted Firmware-A `context.S` and `el3_common_macros.S`.

Every EL3 exception entry executes the branch-history workaround and blocks EL1
walks before accessing a retained report. The branch-history sequence has 24
iterations, including the forward branch, subtract/backward branch, `DSB SY`
and `ISB` required by the pinned A76 implementation. EL1 IRQ and SVC handlers
preserve scratch registers on the trusted EL1 stack before the same sequence.
Fatal EL1 paths execute it without a stack and issue a non-returning SMC.

Before EL1 cache enablement, a separate vector table has only stackless fatal
handlers. It remains installed through control-register verification. A failed
or late enable therefore cannot write ordinary RAM using uncertain attributes.
The EL3 monitor records the failure in that core's Device slot and retains all
ownership. Unsupported contexts and recursive report failures park.

The only TLB invalidations are the cold-entry local operations in the coherence
component. They include the required duplicated invalidation sequence. This
image has no runtime remapping, shared TLB invalidation or CPU power-down path.
The A76 static store-bypass control remains set throughout execution.

## Retained results and external admission

The cached contract magic is `0x4153373343414331`; the result magic is
`0x4153373343525331`. Contract version 1 retains the bounded four-second watchdog
lease, generation, epoch and image build identity. These distinct values prevent
the uncached launcher's contract from admitting this image accidentally.

Core 0 publishes final status in the retained result at `0x42000000`. Each core
also has a retained boot/fault record. EL1 completion or failure reaches those
records through SMC; EL1 cannot write the secure monitor directly. The diagnostic
never feeds or disarms the watchdog, releases the board lease, changes ROM-return
configuration, or resets the board itself.

Before physical admission, the coordinator must establish:

- Exclusive ownership of CPUs, memory, DMA participants and the recovery lease.
- Cold-entry cache invalidity and resolution of all dirty aliases, including
  secure/nonsecure aliases left by prior firmware.
- The intended DRAM bytes are reachable in both required physical address
  spaces; descriptor NS bits do not prove the interconnect mapping.
- Supported measured CPU and DSU revisions, compatible shareability and actual
  hardware coherency across the A55/A76 cluster.
- Independent watchdog progress, expiry and qualified ROM return with storage
  preserved, plus retained-report retrieval after recovery.

Publication cleans owned ranges and issues `DSB SY`. It cannot repair an
unknown firmware cache alias, drain another bus master or recover a stalled bus.
A software deadline cannot bound an instruction blocked by the interconnect.

## Build and evidence

Build the separate target with the pinned FreeRTOS source and AArch64 toolchain:

```sh
cmake -S platform/a733-smp-cached -B /outside/repository/build \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/aarch64-clang.cmake" \
  -DFREERTOS_DIR=/absolute/path/to/pinned/FreeRTOS-Kernel
cmake --build /outside/repository/build --target a733-smp-cached-diagnostic
```

The ELF contains a build-input identity derived from source, headers, generated
port files, linker layout and compiler identity. Preserve that identity alongside
the final ELF hash. The checker snapshots inputs before building and supports
`--image` to execute a preserved ELF without rebuilding it.

The native instruction model passed all eight cores through reset, both
translation barriers, shared atomics and FreeRTOS. Every core completed 32
floating-point context rounds, progressed its background task and handled
inter-core interrupts. The primary timer advanced, and the diagnostic published
success with all eight completion bits. A missing secondary retained failure
before cached execution.

Focused checks on the corrected image passed all eight transitions with inherited
`SCR_EL3.SIF=1`. A sticky SIF bit refused entry before any EL1 core started.
Terminal report states refused repeat writes. Both pre-enable and uncertain
post-enable EL1 exceptions reached the retained EL3 report with an unusable EL1
stack and 24 branch-history iterations at each exception level.

Evidence is under `~/.cache/sunstoneos/evidence/`:

- `smp-cached-scheduler-completion-20261016` records the completed scheduler and
  missing-secondary cases, using preserved machine checkpoints.
- `smp-cached-sif-transition-20261016` records the corrected adoption and terminal
  report paths.
- `smp-cached-cold-vectors-20261016` records stackless cold and late-failure paths.
- `smp-cached-final-build-20261016` preserves the final ELF, build inputs and
  executable-object comparison supporting reuse after build dependency changes.

The model executes the image's native instructions with explicit system-register, GIC,
timer and exclusive-monitor behavior. Shared host memory does not model A733
cache tags, the DSU, interconnect ordering or physical interrupt timing.
