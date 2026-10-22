# Hardware support and qualification

This inventory covers all onboard hardware and usable SoC blocks. It does not
depend on mimus using a device. The initial list comes from the A733 device tree
and datasheet. The pinned V1.2 schematic now supplies [board wiring](board-wiring.md)
for recovery, audio, cameras, and display. The user confirms that neither current
board has populated eMMC or UFS; their media functions are not applicable to
these boards. Both boards have microSD cards whose contents must be preserved.
Board revision and other optional population still require confirmation.

Status meanings: **planned** has no implementation; **implemented** has code;
**tested** has a functional result under stated conditions; **qualified** meets
its recorded operational and recovery criteria on physical hardware.

Every row below has **no qualification evidence**. Unless stated below, its
implementation is **planned**. Partial results apply only to the stated scope:

- PCIe: **implemented** [native root-port preparation](pcie.md) with shared
  clock, rail, pin and PMIC ownership, COMB1 PHY initialization, x1 Gen1 link
  observation and read-only configuration access to one direct endpoint.
  Source comparison, composed host checks and native compilation pass. The
  [supervised application](pcie-application.md) connects the existing providers
  to retained session admission, final endpoint checks and copied USB output.
  The corrected profile reserves BLDO1 for PD-bank power and DCDC2 for the
  core, with separate fitted-board analog-supply admission. One affected image
  and focused provider, native-request and host electrical-policy checks pass
  under `pcie-pd-rail-native-20261018/`; all 174 compiled repository inputs match
  integration. Earlier controller/PHY/protocol evidence was reused without replay.
  The separate [retained endpoint application](pcie-test-application.md) now
  implements selected BAR/iATU programming, control MMIO and one 4,096-byte
  endpoint-DMA COPY through the pinned `pci-epf-test` protocol. One privileged
  owner claims the port, uses one-shot MSI on CPU0 INTID185, checks completion,
  source/data/guards and link state, then publishes immutable copied USB output.
  The configuration-only profile keeps its BME-clear, interrupt-masked policy.
  Thirteen client/provider cases, linked application/host checks and thirteen root
  route/dispatch cases pass with hardware, translations, scheduling and transport
  modeled. Root verifies 451 inputs and 171 objects against revision `30e585e`,
  with no compiled-source delta, under `pcie-endpoint-20261019/root-integration.json`.
  The exact endpoint and its controller/DMA source-backed authority must establish
  COPY capability, DMA completion before endpoint mapping removal, and data
  visibility before status/MSI. No identified endpoint supplies that proof yet.
  MSI, COMMAND clear and BAR/CPU addressing alone establish no DMA completion;
  identity forwarding supplies no hostile-endpoint isolation. Success and faults
  both retain all resources. Physical endpoint power/link, DMA visibility and
  confinement, MSI, all-core startup/scheduling and recovery remain unqualified.
  The core now programs both outbound iATU regions through one helper that
  validates and records each request; the native write filter admits an iATU
  write only when it repeats that record. A configuration field selects a 2.5, 5
  or 8 GT/s target, and above 2.5 GT/s the port follows the board kernel's
  directed speed change after training. Host-model checks pass; both
  applications still select 2.5 GT/s, and no board test ran. A general
  [endpoint function client](pcie.md) is **implemented**. It scans the direct
  endpoint's functions 0–7 and refuses bridges, assigns memory BARs below
  4 GiB inside the aperture, delivers polling, INTx or MSI-X interrupts, moves
  the function between D0 and D3hot and stops it in order. A read-only NVMe
  driver uses it, and the port can stop by disabling link training and
  asserting PERST. Twenty-four host-model cases pass, and both units compile
  for AArch64. The supervised board image
  [`sunstone-a733-pcie-nvme`](pcie-nvme.md) links both. One task brings the
  port up at 2.5 GT/s, reads one admitted 4096-byte block with polled
  completion, shuts the controller down, stops the port with PERST held and
  reports the copied block through USB0. Any failure retains the port's leases
  and the queue and data pages. Linked-image and host checks pass against a
  modeled controller. Interrupt completion in that image needs GIC routing for
  INTID 184 or 185. No endpoint, NVMe device, interrupt or power transition has
  run on hardware; the endpoint test driver keeps its own BAR and MSI setup. Bridges and switches, ASPM, embedded root DMA, Gen2
  selection by the applications and physical Gen2/Gen3 links remain required.
- HDMI: **implemented** [controller, PHY and TV0 diagnostic output](hdmi.md)
  for 720p60 and 1080p60. Composed host checks, primary-source comparisons and
  native compilation pass. The [native startup binding](hdmi-native.md) now
  acquires actual shared clock, CLDO2, PMIC, power-domain and register ownership;
  composed startup/EDID and retained-fault checks pass. The supervised
  [native colorbar application](hdmi-application.md) connects these providers
  to 720p60 output. Its central build and 17 focused software cases pass.
  Optional [RGB565 framebuffer output](hdmi-framebuffer.md) now connects actual
  DE1, IOMMU and TV0 providers. Its native build, normal start/stop and four fault
  cases pass. Startup now retains the complete authenticated session before
  counter and provider initialization; the framebuffer image and five focused
  binding checks pass under `hdmi-binding-20261018/`. All 97 preserved repository
  inputs match integration; existing display evidence was reused. Analog
  admission, physical DMA reachability, monitor output and broader modes remain
  required.
  The [clock-source correction](provider-clock-reference.md#hdmi-native-evidence)
  admits authenticated raw 24/26 MHz for the TOP PLL, selects normalized 24 MHz
  for SFR, and retains REF ownership. Raw 19.2 MHz and reference mismatches are
  refused. Actual compiled objects pass modeled raw-26 720p prepare/start/retained
  stop and three saved-state refusals. Its calculated pixel rate is 73.9375 MHz;
  the 147.875 MHz 1080p plan has source-row and host-profile checks only.
  Neither rate establishes nominal timing compliance or physical output.
  Root verifies 75 repository inputs at `c8c214b` under
  `hdmi-ref26-20261020/root-integration.json`; the initial checker and corrected
  continuation remain separately preserved. GPU consumer source was updated;
  its earlier frozen image and evidence retain their original scope.
  [Finite HDMI audio](hdmi-audio.md) now sends one caller PCM file through DMA0,
  internal I2S3 and the HDMI sampler alongside the TV0 pattern. It accepts
  1–48,000 stereo S16LE frames at 48 kHz, with EDID LPCM admission and retained
  normalized AUDIO1/HDMI clock ownership. GPL DesignWare/BSP/CCU adaptations run
  under FreeRTOS; no new opaque audio firmware is required.
  Finite-audio image `432b7c10…` executes actual admission, construction and PMIC/output/DMA
  tasks over models, consuming 8,196 bytes (2,049 frames), observing DMA completion
  and FIFO empty, then publishing a retained 160-byte RAM result. Two fault
  continuations retain uncertainty. The [host tools](hdmi-audio-preparation.md)
  prepare sealed PCM and decode supplied result files; twenty-three refusals,
  two durability failures and bounded boot/launcher checks pass. Session version 1
  retains offline-only observation; running native RAM is never read through FEL.
  Root's `hdmi-audio-nc-20261020/root-integration.json` binds
  618 saved inputs. Its recorded `7fa75cc` comparison found an ISP0 constructor
  declaration as the sole repository delta, with the declared native/host sources
  matching that integration. This frozen comparison does not describe later HEADs.
  Optional version-2 [copied USB observation](hdmi-audio-preparation.md#optional-copied-usb-observation)
  now binds separate USB authority and cold-reference ownership. Current observation
  image `0c661c29…` copies the published 160-byte audio result into one 512-byte,
  identity/session-bound reply retained through terminal packet completion. It admits
  at most 200 sequential requests in 19 seconds within the native 20-second lifetime;
  partial/late transfer retains the slot, with no host ACK or resource release.
  Seven constructor/provider/mailbox/actual TinyUSB cases model scheduling, PMIC and
  DCD effects, explicitly seeding the prior audio disposition without PCM/DMA replay.
  Host refusal/durability/budget cases and supervisor readiness pass. Four pre-GIC
  guard assertions passed before reporting serialization failed; repaired reporting
  source was not replayed. Root accepts all 123 compiled repository inputs and
  nineteen final host sources under `hdmi-audio-usb-20261020/root-integration.json`;
  two external host fixes are separately frozen from the native image identity.
  Common provider failure may park USB before a retained fault can be retrieved.
  All allocations and leases remain retained. FIFO empty does not establish
  serializer drain, transmitted packets or sound. Physical audio, SMP/preemption,
  interrupt delivery, MMIO timing, visibility and recovery remain unqualified;
  streaming, wider formats/rates and broader HDMI audio remain required.
  The [GPU-to-HDMI application](gpu-display.md#copied-usb-observations) copies
  two trusted 720p RGBA8 renders into separate immutable RGB565 display frames.
  GPU CLOSE precedes HDMI start. RCQ replay 0→1→0 requires fresh queue completion,
  source-address readback and later frame-end. Healthy terminal state requires
  GPU CLOSE, HDMI/TV0 stop, DE disable and PMIC park; all resources remain retained.
  Version-2 admission binds typed USB authority and the full private session
  digest. USB0 PIO borrows the existing providers and exposes one immutable
  1,024-byte response: two input/output hash records, sixteen samples per frame,
  three presentation records and terminal state. Hashes/samples establish neither
  full pixel correctness nor visible output. GPU retains its private MMU;
  system-IOMMU master 8 maps only the display bank. USB performs no DMA, and
  all-master exclusion/drain remains separate from the display access mask.
  Final image `70041393…` has 104 objects and 248 compiled inputs. Fifteen final
  framing/refusal/expiry cases cover copied snapshots and full/high-speed packet
  completion, including the terminating zero-length packet. Earlier component
  and eight root native cases remain bound to `2f60a47e…` and are reused through
  102 unchanged objects; seven exact SRAM launcher cases are reused separately.
  Root's source audit at `f1c7e44` under
  `gpu-display-capture-20261020/root-integration.json` confirms 194 native sources
  and 24 host sources; the remaining host snapshot difference is instruction-only.
  Native and host freezes retain 720 and 937 files respectively. Worker snapshot
  verification was reused without another build or case replay.
  The [host coordinator](gpu-display-preparation.md) durably saves copied replies
  before decoding and checks frozen identity/input/terminal records. One 19-second
  host deadline stays within native 20-second admission; no retry or frame reuse
  follows. Production validation refuses modeled positive authority. Hardware,
  arithmetic, RTOS, cache and USB transfer effects remain modeled at the recorded
  boundaries. Physical GPU/HDMI/USB operation, startup, visibility, drain and
  recovery remain unqualified. Concurrent live GPU/HDMI, broader formats/modes,
  streaming and frame retirement remain required.
- DisplayPort: **implemented** a bounded [native TYPEC24 path](displayport-native.md)
  for four lanes at 1.62 Gb/s, 720p60 RGB8 and Type-C assignments C/E through the
  documented V1.2 [lane/AUX route](board-wiring.md#typec24-has-a-documented-displayport-route).
  TYPEC16 remains USB0/power input; PD messages supply HPD because the dedicated
  pad is unconnected. COMB0/SERDES/VO1 ownership excludes USB3 and PCIe sharing
  in this profile; retained master-8 memory supplies the display frames.
  Final image `91646247…` passes actual linked construction, Type-C negotiation,
  controller/PHY/AUX/DE lifecycle and retained shutdown with modeled device effects.
  Twelve host capture cases and one linked high-speed TinyUSB 512/512/zero-length
  packet case pass. Evidence: `displayport-native-20261020/root-integration.json`.
  Its 177 objects and 463 compiled inputs remain frozen; root verifies 195 source
  matches and classifies three later generic-board/DI-only differences. Earlier
  admission, boot, launcher and controller checks retain their recorded image scopes.
  The nominal schedule defers ticks while the register mutex is held; it establishes
  one noncontending ordering, without SMP or contention qualification. Native
  deadlines and conservative shutdown remain unchanged. AUX APB frequency still
  requires independent source/rate/stability authority; no 200 MHz rate is assumed.
  All resources remain retained. Actual board revision, physical negotiation,
  trained link, visible output, memory visibility/drain and recovery remain
  unqualified. Broader rates/modes, audio, HDCP, MST and live-frame replacement
  remain required.
- Boot ROM: **tested** for identity, SRAM transfer, a returning AArch32 probe,
  restoration of the modified SRAM bytes, and forced-FEL return from AArch64.
  See [boot evidence](boot.md).
  The [microSD monitor-container packager](microsd-boot-package.md) creates
  ordinary files only, using pinned boot0 and the published host packer with one
  caller-supplied flat monitor. It inserts no stock monitor, U-Boot or SCP.
  One actual packer invocation and seventeen host checks pass; root verifies
  27 frozen files and five current sources under
  `microsd-boot-package-20261020/wrapper-root-integration.json`.
  The [cold-entry recorder](cold-entry.md) supplies a 4,164-byte native monitor
  that guards CPU entry, captures the boot0-mutated header and exception state,
  publishes a retained report and parks. The actual boot0 parser, cleanup and
  main-tail instructions plus eight guarded AArch64 entry cases pass with
  independently modeled reset registers. One ordinary-file package contains this
  actual monitor. Evidence is under `cold-entry-20261020/frozen/manifest.json`
  and `instructions/result.json`. It does not enter FreeRTOS or establish
  remote-writer exclusion, memory ownership/publication, physical reset/boot,
  media loading/writes or recovery.
  A [stage-2 loader and two-slot card layout](boot-slots.md) are **implemented**.
  The loader verifies both slot headers and their SHA-256 extent hashes, loads
  the newest valid slot and reports the outcome; the host tool plans installs,
  updates and rollback as one storage session per changed card range. With its
  entry prerequisites granted, the loader arms WDT0, builds the resident
  contract of the [integrated OS image](os-image.md), for the 808-byte session
  or the microphone build's 992-byte session, and branches to it. Host and
  instruction-model checks pass.
  [Cold SD admission](boot-slots.md#cold-sd-admission-starts-from-boot0s-controller-state)
  is **implemented**. The loader compares the SDC0 bus, clock, pin, bank-voltage
  and controller state that boot0 leaves with the pinned boot0 and the manual.
  It then makes PF6 a card-detect input and sets the SDC0 module clock to
  400 kHz, reading back each write. Host models cover the comparison and its
  refusals. The card supply after boot0, boot0's module clock and exclusive use
  of SDC0 need a board observation. Admitted slots load at about 25 KB/s, so a
  multi-megabyte slot takes several minutes. The three entry prerequisites still
  refuse: remote-writer exclusion, watchdog recovery after a cold boot and a
  comparison of the boot0 entry state. The loader therefore enters no image on
  a board. No card has been written and no board has run the loader. Standalone OS boot
  therefore remains incomplete; packer licensing and exact source equivalence
  remain unresolved.
  The [cache-wrapper audit](cold-entry.md#the-parser-cleans-cpu-caches-before-returning)
  separately executes the actual parser enable/clean-disable wrappers and all
  twenty expected set/way operations for one synthetic hierarchy. Earlier parser
  checks substituted those wrappers; their header/entry evidence retains its
  original scope. `cold-boot-cache-20261020/audit.json` establishes no physical
  publication, full boot0 execution or remote/DMA exclusion; no image was rebuilt.
- Always-on processors and mailboxes: **planned**, with source assessment only
  for the internal E902 management core. The pinned A733 manual, sections
  5.2.1–5.2.4, identifies its configuration block at `0x07032000`; the shipped
  monitor corroborates that map. The [cold-entry assessment](cold-entry.md#remote-processor-exclusion-needs-the-effective-a733-register-map)
  distinguishes those sources from conflicting U-Boot directory definitions.
  Management firmware, shared memory, mailbox services and clock/power ownership
  remain unresolved. Reuse suitable existing firmware/services where their
  authority and provenance are established. Cold-start application admission
  requires remote-writer exclusion; omitting SCP firmware or reversing observed
  startup writes does not establish E902 shutdown or transaction drain.
  A [remote-writer check](remote-writers.md) is **implemented**. It reads the
  reset, clock and DMA-enable registers of the E902, the DMA controllers and
  each peripheral with its own DMA engine, including the SPI flash controller,
  and reports every master whose exclusion it cannot prove. It writes no
  register. Checks against modeled register states pass. No source documents
  an OR100 reset or clock, and the board ROM leaves the crypto engine out of
  reset with no documented idle status, so on a board the check cannot report
  every master excluded. No boot image calls it, and no board has reported the
  registers it reads. No native
  management-core control or physical qualification is claimed. This inventory
  does not require a separate general-purpose application runtime on E902.
- CPU: **tested** for instruction execution on the boot A55 in AArch32 and
  AArch64 EL3 and nonsecure EL1, including counter reads and SMC return.
  Physical FreeRTOS tasks now pass the [kernel check](a733-kernel.md) on that CPU.
  A separate [two-core emulator target](smp.md) tests concurrent scheduling.
  An [eight-core emulator soak](smp.md#eight-emulated-cores-pass-a-scheduling-soak)
  passes ten trials of cross-core queue traffic, shared-mutex exclusion,
  migration through all cores and preemption of non-yielding tasks on every
  core. QEMU models identical Cortex-A55 cores with sequentially consistent
  memory, so the soak establishes no A733 timing, A55/A76 or coherence result.
  [Multicore mimus](mimus-smp.md) also passes recorded-reference comparisons
  in the two-core target, with the engine's asynchronous jobs running on the
  second core.
  One focused [runtime cleanup case](smp-runtime.md) passes actual FreeRTOS/newlib
  task reclamation after migration in both directions, private stream closure,
  heap restoration and static task storage reuse. Evidence under
  `smp-runtime-cleanup-20261018/` uses QEMU and changes no production startup code.
  The [secondary power controller](cpu-start.md) matches isolated board-firmware
  writes for all seven secondary cores and compiles for AArch64. The
  [secondary entry and mailbox](cpu-entry.md) waits for verified power cleanup
  before releasing a core to its continuation. Host composition and instruction
  emulation pass; native objects compile. The [EL3-to-EL1 continuation](cpu-continue.md)
  supplies per-core interrupt preparation, translation tables and fault vectors.
  Host and instruction checks pass. The [eight-core diagnostic](a733-smp.md)
  links with bounded startup and scheduler checks, but the complete image has
  not executed. The separate [cached eight-core image](a733-smp-cached.md)
  integrates revision-specific controls, coordinated EL3/EL1 memory transitions
  and exception handling. Its instruction-model scheduler completes 32
  floating-point context rounds on every core, with timer and inter-core
  interrupts. Focused checks cover inherited SIF clearing and terminal faults.
  The central image also links; it has a distinct build identity. Physical
  cache coherence, entry, interrupts and secondary scheduling remain unqualified.
  The [USB observer](a733-smp-observe.md) publishes validated bounded result
  frames from the cached diagnostic. Host and instruction checks pass. Its
  [separate launcher](a733-smp-observe-host.md) preserves received frames before
  observing ROM return. The exact offline bundle is prepared; physical USB
  timing, independent recovery and complete-image execution remain unqualified.
  The separate [supervised observer](a733-smp-supervised.md) uses manual
  reconnection and leaves the watchdog disabled. Physical attempts, including the corrected secure-priority image, produced
  no native result; scheduler execution remains unobserved. The full instruction
  model now covers Secure priority access and passes actual FreeRTOS scheduling
  with the observer at background priority. Evidence:
  `smp-secure-pmr-20261018/scheduler-observer-priority/` and `full-image/runtime/`. A returning native
  diagnostic now passes actual stack setup, admission, table construction and
  all three cache-publication calls. All five table pages match the instruction
  model; automatic ROM return and DRAM restoration pass. Native CPU/DSU identity
  and planning also pass, with no requested workaround writes. EL1 walk blocking,
  exact control restoration and GIC admission reads pass physically. Both GIC
  affinity-routing bits are already set. Complete distributor setup and exact
  restoration of its control and SPI enables now pass physically. Secondary
  admission and reset configuration also pass. Core 1 now powers on, reaches
  native entry at EL3, accepts RELEASE and completes CPU/DSU preparation.
  The diagnostic stops before target translation changes or redistributor access,
  then verifies shutdown acknowledgment and actual OFF. Shared control and DRAM
  restoration pass, and ROM returns. Evidence is under
  `reconnect-20261018-086/native/runtime/`.
  The updated complete-image primary setup now also passes in a returning
  diagnostic, including Nonsecure publication and exact EL3/EL1 control
  restoration. ROM returns as `003:109` after saved DRAM restoration; evidence
  is under `primary-publication-probe-20261018/physical-108/`. This result ends
  before GIC configuration and secondary release.
  CPU0 also passes production EL3 cache adoption and exact control restoration
  under `primary-cache-probe-20261018/physical-120/`. ROM return and saved DRAM
  restoration pass. CPU0 production local interrupt setup and cached EL1 entry
  also pass under `primary-el1-probe-20261018/physical-121/`, followed by control
  restoration, saved DRAM restoration and ROM return. This checkpoint precedes
  EL1 interrupt delivery and performs no EL1 data stores or scheduling.
  A later CPU0 diagnostic receives one timer PPI30 and one self-directed SGI0,
  then disables delivery and verifies empty pending/active state. Timer/vector,
  local/distributor and translation controls restore before saved DRAM restoration
  and responsive ROM return. Evidence:
  `primary-interrupt-probe-20261018/physical-002/runtime/session.json`.
  Its stackless handler does not establish general context saving or scheduling.
  The subsequent [production-handler diagnostic](a733-smp-supervised.md) passes
  timer/self-interrupt delivery through the actual FreeRTOS vector and IRQ handler.
  It checks nine general registers, all 32 vector registers, FP controls and SP.
  Cached stack/nesting writes and cleaning of all 192 lines in the three owned
  pages pass, followed by restored controls, DRAM and ROM return. Evidence:
  `primary-context-probe-20261018/physical-005/runtime/session.json`.
  Its bounded application callback does not request a task switch; task selection
  and eight-core scheduling remain unqualified.
  All eight CPUs also pass the production pre-scheduler barrier and 256 atomic
  lock-protected increments. All participants finish before cleaning both shared
  pages and their own stack page. CPU0 restores awake interrupt state; every
  secondary restores local state and reaches observed OFF. Exact physical page
  contents, saved DRAM restoration and responsive ROM return pass under
  `all-core-pre-scheduler-20261018/entry-corrected/physical-008/runtime/`.
  All eight CPUs physically execute FreeRTOS task construction and initial
  context restoration through idle-task entry. Task pointers, arguments, stacks
  and exception state match. Every core cleans the complete 145-page writable
  interval before CPU0 restoration and acknowledged, independently observed
  secondary shutdown. Saved DRAM restoration and responsive ROM return pass.
  Evidence: `all-core-first-task-20261018/physical-010/runtime/session.json`.
  This diagnostic stops before task bodies; task switching and sustained
  eight-core scheduling remain physically unqualified. The complete returning
  workload passes instruction checks through 32 floating-point rounds per core,
  background progress, production timer/SGI handlers and FreeRTOS switching.
  Thirteen host cases validate copied results, context and interrupt restoration,
  saved RAM and failure retention. Evidence: `all-core-task-workload-20261019/`.
  Cores 2–7 now also pass production entry, identity observation, matching shutdown
  acknowledgment, independent OFF observation and configuration restoration under
  `secondary-identity-probe-20261018/physical-109/`. Cores 2–5 are A55 r2p0;
  cores 6–7 are A76 r4p1. ROM returned as `003:110`, with saved DRAM restored.
  Every secondary also passes production CPU preparation and local interrupt
  initialization under `secondary-local-probe-20261018/physical-110/`. Local and
  shared state restore before responsive ROM return as `003:111`.
  All seven secondaries also pass cached EL3/EL1 entry and SMC return under
  `secondary-cache-probe-20261018/physical-111/`. Translation and local interrupt
  controls restore before each shutdown; shared state and saved DRAM restore
  before responsive ROM return as `003:113`. Each target runs separately with
  no EL1 data stores or interrupt delivery. A subsequent concurrent diagnostic
  passes 224 atomic increments and 224 ordered payload transfers across all seven
  secondaries. Every target cleans the single shared page before restored shutdown;
  its final 4 KiB physical copy matches exactly. ROM and DRAM restoration pass
  under `secondary-coherence-probe-20261018/physical-113/`. This establishes
  coherent cached exchange for that workload. A subsequent diagnostic passes one
  timer PPI30 and one peer SGI0 on every secondary, followed by disabled delivery,
  empty pending/active state, cache cleaning, restored shutdown and ROM return.
  Evidence: `secondary-interrupt-probe-20261018/physical-114/`. Boot-CPU
  participation, general interrupt/task-context handling, broader runtime memory
  use and eight-core scheduling remain unresolved. Earlier interrupt diagnostics returned no report;
  their sessions were closed after physical recovery. A missing GIC-600
  redistributor power-up handshake is now implemented. Focused host checks and
  primary/secondary native instruction cases pass under `gic600-power-20261018/`;
  subsequent returning physical results appear below. One read-only ROM batch identifies
  all eight frame affinities and reports their power-down permission bits clear
  under `gic600-registers-20261018-089/`. This does not establish local interrupt
  initialization or identify the earlier failure location.
  A subsequent target diagnostic executes core 1's native walk-blocking helper,
  reads its frame identity/power and restores both translation controls exactly.
  Matching shutdown and observed OFF, shared-state/DRAM restoration and ROM return
  pass under `native-target-walks-frame-20261018/native/runtime/`. The subsequent
  local snapshot also passes: core 1's frame is asleep and has no enabled,
  pending or active interrupts. Controls restore and ROM returns after observed
  OFF under `native-local-interrupt-read-20261018/native/runtime/`. The returning
  overlay now includes the corrected production controller and bounded local
  restoration; five instruction cases pass. One physical initialization attempt
  produced no ROM return or result buffer. Its failure location remains unknown,
  and physical recovery is complete. The
  [Arm/Linux audit](a733-multicore-reference-audit.md) and subsequent returning
  diagnostics identify a group-register verification refusal described below.
  The narrower EL3 GIC-interface observation passes physically under
  `native-cpu-interface-20261018/native/runtime/`: SRE reads back `0xf` and
  both physical interrupt groups are disabled. No local controller writes run;
  matching shutdown, observed OFF, restoration and ROM return pass. This
  establishes CPU-interface access, not interrupt delivery or scheduling.
  Controller discovery and the GIC-600 power handshake now pass physically under
  `native-before-power-current-20261018/` and `native-before-wake-20261018/`.
  The latter stops before WAKER, with PWRR `0x100` and WAKER `6` preserved;
  shutdown/OFF, restoration and ROM return pass. The frame was already powered.
  Wake and return-to-sleep also pass under `native-wake-sleep-20261018/`, with
  WAKER `0` while awake and restored `6` before shutdown. Full local configuration
  and restoration now pass under `native-local-group-mask-20261018/`. The driver
  now checks required SGI/timer bits instead of requiring unimplemented PPI group
  bits to read as one. Matching OFF and ROM return pass. These early runs cover
  initialization; the later cached exchange and SGI/PPI results appear above.
  Core 1 also passes production EL3 translation/cache adoption and exact control
  restoration under `native-el3-cache-20261018/native-attempt/runtime/`.
  Local cleanup, observed OFF, shared-state/DRAM restoration and ROM return pass.
  This probe writes only Device memory during the cached interval; it does not
  establish cached EL1 execution or cross-core coherence.
  Core 1 now also completes production cached EL1 entry and SMC return after
  explicit Nonsecure code/table publication. Physical readbacks, exact EL1/EL3
  control restoration, local cleanup, observed OFF and ROM return pass under
  `native-ns-publication-20261018/`. Four affected modeled cases pass. This executes
  immutable code without EL1 data stores; mutable cached memory, interrupt
  delivery, other cores and scheduling remain unqualified. The earlier empty EL1
  report remains preserved as a failed observation, not a successful entry.
  The complete supervised eight-core image now includes this publication path
  for code and initial data together with the corrected interrupt mask. Four
  routine cases and one actual-boot instruction case pass under
  `smp-ns-publication-20261018/`. A further model case reaches cached EL1 on all
  eight cores. Its subsequent physical attempt disconnected without a native
  USB observer result; startup location, scheduling and coherence remain
  unobserved. The original board requires recovery before reuse.
  Removing a suspect SCR read did not resolve full startup. Its bounded
  refusal-report read timed out; the user reconnected. Offline stage diagnosis
  continues before another full-image attempt.
- DDR: **tested** independently on both attached boards for initialization and
  distinct transfers to two 4 KiB pages. Firmware reports 12 GiB LPDDR5 on each;
  full capacity and stability remain untested. The second board also passes its
  returning platform survey with SRAM restoration under
  `second-board-20261018-3-3/platform-attempt1/`.
  The thermal policy throttles at 85 °C on the DDR temperature channel; this is
  **implemented** and host-modeled, and its board cost is unmeasured.
  [Memory-bus priority for display scan-out](designs/dram-controller.md) and a
  display-port latency window are **planned**; they land together with the
  framebuffer and gpu-display edits that call them. DRAM training, frequency
  scaling, voltage and the firewall remain boot0 dependencies, and the memory
  configuration is not decoded.
- Memory mappings, interrupt dispatch, and task scheduling: **tested** on the
  boot CPU for the physical kernel check. Cacheable RAM and non-cacheable result
  storage work for this workload; full memory protection/coherence remains untested.
  [Cached device reservations](cached-device-reservations.md) now prepare actual
  NPU/GPU buffers under the eight-core boot barrier and matching EL3/EL1 mappings.
  Emulator checks pass for translation on every core and retained construction.
  Native application binding and physical exclusion/coherence remain required.
  The [larger native profile](device-memory-capacity.md) reserves 768 MiB of
  write-back memory and 16 MiB of uncached memory, with privileged mappings and
  bounded validation before publishing either pool. Its native publication and
  refusal checks pass. Physical range ownership and device reachability remain
  unqualified; external pools still need application-specific mapping access.
  A [fixed interrupt table](a733-smp-cached.md#a-fixed-interrupt-table-serves-images-with-several-device-owners)
  is **implemented** for images with several device owners. EL3 programs the
  route of each listed shared peripheral interrupt once and reads it back, and the shared handler dispatches
  each interrupt only on its routed core. A host model covers validation,
  dispatch, route programming and nine refusals. Existing single-device images
  keep their per-device patches.
  The [integrated OS image](os-image.md) is **implemented** and host-modeled.
  It runs the mimus board application (USB0 device service, USB2 host with
  keyboard and read-only storage, Type-C and PMIC owners, EL0 recognition)
  together with a continuous thermal service, using that table and one fixed
  start list. The supervised lifetime keeps the bounded FEL contract with WDT0
  disabled. The resident lifetime runs after a cold boot from the stage-2
  loader and renews the armed WDT0 only while every listed service reports
  progress within its window. Optional builds add I2S microphone capture, with
  the DMA0 interrupt (INTID 143) in the table; the board's Wi-Fi station, with
  crypto engine entropy for WPA2; or Bluetooth headset recognition, which needs
  `SUNSTONE_BT_SECURITY_DIR`. With both the radio and the headset, the board's
  radio owner owns the AIC8800 module and loads its firmware once, and the
  Bluetooth application attaches to that session as a second client with its
  own UART1, H4/HCI and wake-pin leases. The headset runs in either lifetime,
  an mSBC option admits wideband speech, and the headset excludes the
  microphone. Nine variants build: supervised, resident, resident with I2S
  audio, resident with radio, resident with radio and audio, supervised
  headset, resident headset, radio with headset, and resident radio with
  headset. Each build runs the image's link check, and host models cover
  interrupt delivery, supervision and admission for each combination of
  lifetime and options. A supervised launcher and session preparation
  exist; preparation needs a reviewed entry record from physical evidence and
  refuses the radio and headset builds until it stages their profile and
  firmware. The supervised radio and headset builds cannot finish firmware
  loading within their 40 s and 30 s bootstrap bounds at the admitted SDIO
  clock, and the resident image depends on the stage-2 entry prerequisites.
  No board has run any variant. Display, camera, GPU and NPU are not part of
  the image.
- Timers and recovery: **tested** for the nonsecure tick and an EL3 secure-timer
  timeout that returns a stalled EL1 to FEL. This does not qualify the independent
  hardware watchdog, every interrupt source, or arbitrary failure recovery.
  A [physical SRAM trial](watchdog.md) also passes ROM-redirect and watchdog
  reset-policy programming/restoration with the timer disabled. Timer enabling,
  expiry, and reset retention remain unqualified.
- RTC and watchdog: **tested** coherent RTC reads in inherited normal RC mode,
  with source control `0x401c`, status zero and prescaler `0x0f`. Two reads about
  1.1 seconds apart show one second of calendar progression. No RTC register
  is written; monitor completion and responsive ROM return pass. The kernel
  runner does not restore prior DRAM contents. Absolute time,
  accuracy, alarms and retention remain unqualified. Evidence:
  `rtc-rc-readonly-20261018/physical/observation.json`.
  Separate finite source-selection and alarm applications pass twelve and
  seventeen linked cases respectively, with focused Astra review. Secure
  interrupt-state admission and restoration are included. Their compiled inputs
  match integration under `rtc-source-returning-20261018/` and
  `rtc-alarm-returning-20261018/`; physical source switching and alarms remain
  untested.
  **Implemented** [native RTC operations](rtc-application.md)
  include coherent reads, non-waking alarms, cold-start crystal/calendar
  initialization and reusable calendar setting. One owner serializes two registered
  native clients through a static FreeRTOS queue. Reads can proceed during an
  alarm; calendar changes and another alarm return busy until it finishes.
  Client-bound request generations prevent stale or cross-client cancellation
  and result access. Clean alarm completion permits another operation; uncertain
  failures retain resources and refuse subsequent requests.
  Five cases using the actual FreeRTOS POSIX scheduler, native binding/provider
  checks, launcher and host checks pass under `rtc-service-native-20261018/`.
  Integration reconciles all 182 compiled repository inputs, with unused additive
  CLDO3 decoding and PWM pin support recorded separately. Prior finite RTC evidence
  remains valid for its frozen inputs. Source switching, wider runtime client
  registration, wake, calibration, reset-cause reporting and applicable retention
  modes remain required. Five additional frozen-image cases execute actual RTC
  serialization and TinyUSB transfer/event flow under modeled controller and
  lower-provider boundaries. Fragmented repeat copies and failure refusal pass
  under `rtc-usb-native-20261018/` without a rebuild. The fitted board revision,
  physical operation, UTC accuracy, USB hardware and full provider initialization
  remain unqualified. A boot-CPU-only service image now passes twelve modeled
  startup/provider and five host cases under `rtc-single-core-20261018/`. All
  182 compiled repository inputs match integration; no rebuild or test replay
  was needed. Physical service operation remains outstanding. An
  [SNTP client](radio-network.md#an-sntp-client-supplies-utc-to-the-rtc-service)
  produces RTC SET requests from network time; no image runs it with the RTC
  service. Independent watchdog evidence is recorded above.
- Combined monitoring: **implemented** [repeated thermal/ADC sampling](monitor-application.md)
  through separate native owners with a shared stable clock parent and copied
  coherent snapshots. Stop requires both owners to verify converter-disable
  readback; all resources remain retained. Thirteen controller stop cases,
  twenty modeled native composition/USB cases and four host groups pass, and
  one native image links under `monitor-application-20261018/`. Physical sampling,
  interrupt delivery, analog drain and recovery remain unqualified.
  The monitor retention adapter now checks actual mutex ownership independently
  of provider health: unowned entry failure returns without asserting ownership,
  while known-held faults retain once and allow no later MMIO. Four focused host
  cases cover monitor and camera adapters under
  `mutex-enter-20261020/root-integration.json`; RTOS/context effects are modeled.
  Existing native images predate the fix; no new native image is claimed.
- CPU clock diagnostics: **tested** for read-only reporting of the boot A55
  clock configuration during recognition. [Application profiling](mimus.md)
  records the register decode and its limits.
- Thermal and frequency control: **implemented** [controller](thermal.md) and
  [native application](thermal-application.md). The controller
  implements calibrated sampling for CPU, DDR, NPU and GPU sensors, alarm and
  critical thresholds, and interrupt-status acknowledgement. Host checks, source
  comparisons and native compilation pass. The native application connects shared
  clock and register ownership, deferred interrupts and copied USB output for
  two successive sets across all five sensors. One affected image, admission
  checks, 13 modeled native cases and four host cases pass; all 175 compiled
  repository inputs match integration under `thermal-application-20261018/`.
  A boot-CPU-only profile also passes thirteen startup/IRQ/provider cases and
  six host cases under `thermal-single-core-20261018/`. The subsequent clock fix
  supports normalized 24 MHz references from 19.2/26 MHz oscillators and chooses
  the matching GPADC source. One focused sanitized driver check and review pass
  under `thermal-clock-reference-20261018/`; the unchanged startup layout allows
  reuse of earlier startup and host cases. All 581 frozen inputs
  match integration; calibration, thresholds and service code are unchanged.
  Second-board PMIC reads establish enabled DCDC2 programmed to 800 mV and
  CLDO3 programmed to 1.8 V under `sys-supply-probe-20261018/runtime/`.
  These register settings do not measure analog supply stability.
  Three calibration-shadow reads also pass physically in nonsecure EL1 under
  `thermal-sid-shadow-20261018/runtime/`, followed by successful ROM return.
  This proves access to the calibration words, not temperature acquisition.
  Existing calibration and controller evidence was reused. Physical acquisition,
  accuracy, interrupt delivery and recovery remain unqualified.
  A fixed board [thermal policy](thermal.md#a-fixed-board-policy-throttles-and-stops-work)
  is **implemented**. Its trips come from the pinned device tree. The CPU and
  GPU channels throttle at 90 °C, and every channel is critical at 110 °C. A
  channel without a valid reading throttles after 1 s and is critical after
  5 s. The thermal application and the thermal service of the
  [integrated OS image](os-image.md) end their session at the critical level;
  no image reduces load at the throttle level yet. Host checks pass. The
  policy reads temperatures whose physical acquisition is unqualified.
  A [CPU clock driver](cpu-clock.md) is **implemented** and host-modeled. It
  changes CPU-L and CPU-B among the pinned operating points. It raises the
  DCDC5 or DCDC3 voltage before a frequency increase and lowers it after a
  decrease. Its thermal coupling
  admits a raise only at NORMAL after a reading from that cluster's sensor,
  lowers raised clusters to 1014 MHz at THROTTLE and returns them at NORMAL.
  Every transition needs a grant, which a defined session record carries with
  the hash of the user's approval; no image carries one or calls the driver.
  Its cycle timing reports the measured and configured rate of the core that
  ran timed work. Host and instruction models pass. Physical rate changes,
  rail settling, lock time and PMU access remain unqualified. The user reviewed
  the CPU clock plan and agreed with the design on 2026-10-21. Each physical
  clock or voltage change still needs the user's approval, and none can run
  until the board can be reconnected.
  A separate [finite returning application](../apps/thermal-returning/README.md)
  gathers two sets with the actual driver and disables configured acquisition.
  Thirteen linked cases and Astra review pass; all 46 compiled inputs match
  integration under `thermal-returning-final-20261018/`. Clocks and configuration
  remain retained. Physical acquisition and return remain untested.
- Storage: **implemented** [SD/eMMC identification and bounded reads](mmc.md)
  for the one-bit legacy-speed profile and negotiated read-only SD profiles.
  Host checks and source comparisons cover capacity, full-range
  addressing, automatic stop, completion/status and retained partial buffers;
  native compilation passes. Controller access, pin/clock restoration, and ROM
  return are **tested** in the earlier eMMC probe; CMD1 received no response.
  [Physical storage evidence](storage.md) records that negative result. No media
  device is physically identified. The [native storage binding](mmc-platform.md)
  composes SDC0 reads and explicitly selected SDC2 or SDC3 eMMC reads with actual
  clock, pin, rail and PMIC ownership. Both eMMC selections reserve the two
  controllers together because they share one card. The corrected PC route uses
  mux 3 for SDC2 and mux 4 for SDC3. Focused host checks cover both routes,
  conflicting ownership and controller refusal; native compilation passes.
  Earlier model evidence used the NAND pin function and cannot establish correct
  eMMC routing. The [native read-only eMMC application](emmc-read-application.md)
  connects either explicitly selected controller to the existing copied-client
  service. Both controller instruction checks, admission and host capture pass.
  Evidence: `emmc-native-20261018/`. Neither current board has eMMC populated;
  physical eMMC reads are not applicable. The optional application's single-core
  lifetime retains resources permanently and has no qualified ROM return.
  The [bounded write primitive](mmc-write.md) snapshots input,
  authenticates each request and confirms each sector through card status.
  Focused host checks cover SD/eMMC writes, authority loss and retained partial
  failure; affected native consumers compile. The [storage update service](storage-update.md)
  binds the actual platform to one identified card range, preserves a durable
  host backup, authenticates candidate chunks and checks readback before advancing.
  Connected host/core checks pass; the default platform entry remains read-only.
  The [maintenance watchdog](maintenance-watchdog.md) passes focused progress,
  cutoff and permanent-loss checks. The native tasks and USB binding
  are implemented. Focused supply, native MMC admission and USB callback checks
  pass. The complete [maintenance image and launcher](storage-maintenance-launch.md)
  are integrated; focused instruction and host checks pass. A session word now
  lets the maintenance image select the four-bit default-speed or high-speed SD
  profile; zero keeps one data line at 400 kHz. Backup reads take up to eight
  sectors per request, while writes stay at one sector per request because the
  one-second operation deadline must cover SD programming busy time. Connected
  host checks pass, including a high-speed backup, update and restoration of 20
  sectors against modeled media. No profile has written physical media. The separate
  [supervised read-only SD service](storage-read-application.md) identifies the
  card and serves repeated ranges of one to eight sectors to two native clients.
  One controller owner copies results into separate client slots; matching
  acknowledgments permit slot reuse. The native task reads its configured range
  twice, and USB uses the same API. One image, eleven modeled native cases and
  host/package checks pass under `storage-read-service-20261018/`.
  Negotiated SD profiles now select four-bit default speed up to the card's CSD
  limit and 25 MHz, or require four-bit high speed at 50 MHz. The original
  one-bit 400 kHz profile is preserved. Native SCR/ACMD6/CMD6, actual provider,
  linked two-client read, preparation and host checks pass under
  `sd-bus-modes-20261019/`. The SD image rebuilt for HID layouts includes those
  headers and class source consistently across all consumers. Linked SD/client, USB0
  copied-read, moved-field liveness and host identity checks pass; focused review
  is closed. At integration revision `894051d`, the audit verified 182 sources,
  1,219 artifacts and all 411 compiled inputs under
  `sd-shared-usb-20261019/root-integration.json`. Its 163 linked
  objects and full source closure remain frozen together. The preceding image's
  six later USB-input differences remain historical; unchanged protocol, clock
  and refusal evidence was reused. Actual mode negotiation, read data,
  electrical/timing margins and recovery remain unqualified. No card write or
  voltage change is part of these profiles.
  The second board's inherited JTAG pin pattern now has an exact read-only
  handoff with explicit debug-client exclusion. Focused provider, host-policy and
  native parser checks pass; three objects were rebuilt and 163 reused under
  `second-board-20261018-3-3/sd-rom-pins/`. Its observed adaptive voltage mode
  is now supported with sensed/selected 3.3 V, unchanged mode and stable owned
  supplies. Four provider cases pass, including retention on voltage-state
  changes. Only two objects were rebuilt and 164 reused. No card command has run.
  Read-only pin, fabric and PMIC clock observations now supply the second board's
  configuration under `second-board-20261018-3-3/`. Mandatory native providers
  now have reviewed oscillator/reference corrections with focused software
  evidence. Source-backed CMD0 delays also pass six focused cases. An independent
  single-core SD/USB image now links and passes focused startup, native composition
  and host acceptance checks. Its explicit ROM USB takeover is integrated.
  Physical ROM capture matches the inspected PIO implementation, with scratch
  restoration verified. A 19-register PMIC probe establishes enabled USB supplies
  at programmed 1,240 mV and 800 mV, with normal ROM return. Warm launcher state
  now passes after the accepted cache-off helper. DMA0 reset is asserted with
  both clocks disabled; the maintained reset state supports current inactivity
  without reading gated registers. Final image admission remains outstanding.
  Card operations remain untested.
  Detected removal permanently stops both clients. Unnoticed card replacement,
  hotplug recovery, physical reads, writes and faster modes remain unqualified.
  A [read-only FAT and exFAT filesystem](filesystem.md), FatFs R0.16 pinned from
  ESP-IDF v6.1, is **implemented**. It loads whole files through the polled SD
  core into a private buffer. After a read failure that may have started, it
  keeps that buffer and makes no further device call. Twenty host cases over
  FAT16, FAT32 and exFAT card images pass, including reads through the actual
  core with a modeled controller and refusals of corrupt metadata; the native
  objects compile. No image links it, and no card has been read through it.
  GPT cards, several volumes, several client tasks and writes are unsupported.
  UHS-I SDR50 and SDR104 remain unimplemented: the 1.8 V switch writes the PF
  bank voltage that card detect and the debug pins share, and a failed switch
  needs whole-board power removal ([mmc.md](mmc.md)).
- I²C and PMIC: **tested** single-byte reads on the inherited always-on bus,
  AXP8191 identity, and storage-rail settings. [Power evidence](power.md) records
  the conditions. The second board also passes 21 register reads, including
  the DCDC2 system-supply selector, with normal ROM return and automatic USB
  access under `sys-supply-probe-20261018/runtime/`. No regulator settings change.
  The [rail ledger](rail-leases.md) coordinates all 39 source-defined
  outputs with the existing DCDC2 and GPU DCDC4 owners. Host checks and native
  compilation pass for shared selectors, snapshot ownership and retained faults.
  The [shared PMIC provider](pmic-runtime.md) owns the actual bus, pins and
  asynchronous read-only rail snapshots. Host checks, original getter comparisons
  and native compilation pass. A physical returning test now completes production
  provider initialization, one DCDC1 snapshot, request reap, client-lease release
  and idle-state checks with inherited pin/TWI settings restored. The selector
  decodes as an enabled, programmed 3.3 V; this is not a voltage measurement.
  Evidence: `pmic-provider-probe-20261018/physical/runtime/session.json`.
  It adopts either supported 24 MHz APBS1 parent
  and rejects later route changes; focused reference-parent checks pass.
  [GPU/NPU consumers](pmic-consumers.md) now retain
  actual asynchronous observations and supply leases; selecting the shared
  provider disables their competing inherited-bus readers. Emulator checks pass
  for both consumers and retained failures. The provider also accepts bounded
  DCDC3 and DCDC5 selector writes, and only the [CPU clock driver](cpu-clock.md)
  may submit them; no image does. Cold bus startup, recovery, regulator
  writes and electrical qualification remain open.
  [Provider clock admission](provider-clock-reference.md) validates normalized
  REF24 for generic TWI11, USB2, radio, CE, I2S AUDIO1 and PWM core/APB0 paths,
  retaining each consumer's selected parent and divider arithmetic. CE retains
  separate TRNG RC16M requirements. Type-C S_TWI1 source 0 is raw DCXO. Optional
  LED APBS0 source 0 and S_SPI source 0 remain raw-24-only; LED source 4 and
  SPI0–4 source 0 use normalized REF24. HDMI selects normalized 24 MHz SFR,
  retains REF, and binds authenticated raw 24/26 MHz separately for its TOP PLL.
  Its compiled-object model evidence is recorded above; earlier GPU images retain
  their frozen clock scope.

  Actual-function sanitizer/source evidence is under
  `provider-clock-reference-20261020/root-integration.json` and
  `audio-pwm-hdmi-clock-20261020/root-integration.json`; SPI's separate fourteen
  parent cases are recorded in [its platform record](spi-platform.md).
  These checks model dependency observations and qualify no native application,
  measured frequency or physical operation. Earlier frozen image scopes remain intact.
- UART: **implemented** bounded [FIFO control](uart.md) for UART0–6 and both
  always-on UARTs, with format planning, flow control where supported, interrupts,
  error reporting and transmit drain. Host checks, source comparisons and native compilation
  pass. Unread byte errors are preserved when receive capacity is exhausted.
  The [native platform binding](uart-platform.md) now composes actual clock
  and pin leases, reset sequencing, GIC masking and one FreeRTOS owner task.
  Host checks and single-core/SMP native compilation pass. Twelve explicit
  routes cover all nine controllers. The [native Bluetooth application](bluetooth-native.md)
  integrates the polled UART1 binding. The [finite UART2 application](uart-application.md)
  connects actual header pin/clock ownership, FIFO exchange, bounded drain and
  release to copied USB results. Interrupt service now masks the owned source,
  notifies the owner task and rearms after bounded receive/transmit service.
  Selected sanitizer, session, host and linked routing checks pass, and the
  native image links. Evidence: `uart-native-20261018/` and
  `uart-irq-20261019/root-integration.json`.
  The host-only UART supervisor correction at `2e0cac6` passes caller entry to a
  readiness sentinel and a static audit of 21 production callers under
  `uart-supervisor-entry-20261019/`. These checks stop before queue/device access
  and establish no native startup or physical serial operation.
  The [finite UART2 DMA profile](uart-application.md#finite-dma-transfers) now
  accepts 1–1,024 caller bytes through RX channel 0 and TX channel 1, request 16.
  Both channels must be active before UART DMA requests are enabled. Success
  requires DMA stop, request-disable readback, true transmitter-empty (TEMT),
  RX-empty/idle, copied echo comparison, matched REAP/CLOSE and UART release.
  Known-live failures attempt bounded cancellation; uncertain stops retain ownership.
  The DMA0 parent lease and allocation remain reserved for the board lifetime,
  even after successful UART teardown. Line errors are aggregate observations,
  not per-byte status. Linked native/provider, copied USB, host and refusal checks
  pass with device, cache, RTOS and provider effects modeled. Root verifies
  471 inputs and 183 objects against revision `efc1adc`, with no compiled-source
  variance, under `uart-dma-20261019/root-integration.json`. The unchanged selected
  ELF is `49603358…`; `root/layout-review.json` preserves its static session,
  privilege and memory review. Physical wiring/bank voltage, DMA0 exclusion,
  prior bus drain, cache/DMA visibility, startup and sustained scheduling remain
  unqualified.
  It requires a passive PB0/PB1 loopback and electrical admission; inherited
  multicore startup remains physically unqualified. Physical serial and interrupt
  operation and DMA remain unqualified.
  The driver permits the DMA lease on all nine controllers, using DMA0
  requests 14–20 and 50–51, and selects IrDA SIR, RS-485 9-bit (UART1–6) or
  internal loopback on a quiet line. Host models and the linked native write
  policy pass; no application uses these modes or DMA beyond UART2. The board
  device tree's `uart-v100` driver data disables hardware RS-485 and 9-bit
  mode, which conflicts with the manual, so RS-485 needs physical resolution.
  Runtime format changes and suspend/resume remain required.
  Bluetooth and the header UART1 route share one controller.
- SPI: **implemented** bounded [native master transfers](spi.md) for SPI0–4
  and the always-on controller. Host checks cover wire modes, FIFO completion,
  chip-select timing and retained failures; native compilation passes. The
  [native platform binding](spi-platform.md) composes real clock/pin leases,
  reset/GIC handling, inactive chip-select preparation and bounded restoration.
  Focused host checks and native compilation pass. SPI4 route activation refuses
  its unproven package signal mapping. The driver also implements dual and quad
  data phases after a single-lane prefix, and DMA transfers on requests 23–26
  and 53 with verified request gating and abandonment; host and platform
  models pass. Routes lack IO2/IO3 pins, so the platform refuses quad, and
  SPI4 has no DMA request in the manual. No application uses these modes.
  Board admission, physical peers, three-wire and bit-aligned modes and SPIF
  remain outstanding.
  The [supervised SPI3 application](spi-application.md#deferred-interrupt-software-evidence)
  supports polling version 1 and deferred-IRQ version 2 for one authenticated
  FIFO-sized WRITE_READ exchange on the 1.8 V PE0–3 route. CPU0 INTID59 masks
  and notifies; the owner alone handles SPI/FIFO/CS checks and copied output.
  A missing ISR cannot be replaced by active polling. Route deferral and the
  operation deadline are bounded; uncertainty retains resources.
  Five linked native cases and three host groups pass, with root routing/private
  boot checks separate. Fourteen parent cases cover the earlier clock-source
  correction, not IRQ-image execution. Evidence is under
  `spi-irq-20261020/root-integration.json` and `root-frozen/manifest.json`.
  Frozen image `149f7635…` remains authoritative. The sole source-audit difference
  is in a later Type-C task body, not created for SPI workload 2; no hypothetical
  rebuilt-image equivalence is claimed. SPI/GIC, PMIC, scheduling and transport
  effects are modeled. Physical exchange, IRQ delivery, signal timing, electrical
  compatibility and recovery remain unqualified; application DMA and broader modes remain required.
- PWM and ADC: **implemented** [continuous fan and GPIO LED control](pwm.md).
  The complete application has a boot-CPU-only profile under
  `pwm-single-core-20261018/`; one build, sixteen startup/IRQ/profile cases and
  focused host checks pass. Physical waveform and delivery remain unqualified.
  This image and the board, thermal, RTC and crypto boot-CPU-only images now
  build from [one shared recipe](single-core-images.md). A comparison with the
  previous build finds identical object code in all five; the PWM and crypto
  images link in a different order, so their frozen evidence keeps its earlier
  image. The central build configures `SUNSTONE_A733_PWM` again.
  PWM0 channel 5 on PD5 supports 40 kHz duty changes, verified GPIO-low stop and
  restart under retained ownership. Optional PL7 LED commands preserve neighboring
  outputs. Actual provider/pin, four linked cases and host command/admission
  checks pass under `pwm-service-20261018/`; the frozen image is reused without
  replay. Duty changes include a low interval. An uncertain failure can leave
  continuous output active until full power removal. Physical waveform, fan-off
  current and visible LED behavior remain unqualified. Cycle mode now plans the
  smallest exact prescaler for any period up to 10 ms, in either polarity, with
  constant levels at zero and full-period duty. The fan application accepts
  a duty of 600 ticks, which is full speed, but the supervised host tool still
  refuses that value.
  Host and instruction-model checks pass. S_PWM needs a clock-registry scope
  before an owner can reserve it. Other channels, capture, input counting and
  other operating modes remain required.
  [Board routing](board-wiring.md#pwm-outputs-and-analog-inputs-have-specific-board-uses)
  identifies the analog inputs. [Native ADC monitoring](adc-application.md) is
  **implemented** for two raw observation sets from the GPADC0–3 board-selection
  dividers, with actual shared providers, deferred interrupts and copied USB output.
  Eighteen core cases, admission checks, sixteen modeled native cases and four
  host groups pass under `adc-application-20261018/`. Integration reuses the frozen
  image with an unrelated additive PWM pin helper recorded separately.
  Physical sampling, voltage/reference accuracy and recovery remain unqualified.
  The GPADC driver implements compare-window threshold mode for channels 0–3,
  latching low and high events; host and instruction-model checks pass. No
  application selects windows, and the native adapter refuses the compare
  registers. Repeated/concurrent monitoring and FIFO/DMA remain required.
  GPADC4 reaches the display connector; the remaining GPADC channels have no
  documented usable connection. Accessory GPADC4 qualification remains outstanding.
  [LRADC0 raw observation](adc-application.md#lradc0-raw-observations-require-separate-electrical-authority)
  is implemented as ADC session version 2, preserving GPADC version 1. It takes
  two fresh channel-0 six-bit observations with timestamps/sequences, then verifies
  configured disable while retaining all resources. It provides no key map,
  voltage conversion or measured sample-frequency claim. Physical use is blocked:
  the V1.2 drawing's R23 pull-up to nominal 1.8 V AVCC conflicts with the documented
  0–1.35 V converter range; no idle-overrange authority or matching key ladder exists.
  Final image `7f390582…` passes the linked function-call composition with raw 0/63
  supplied by the model, plus scoped driver/lease/write/host refusals and separate
  boot/SRAM guards. RTOS, PMIC, analog conversion, translations and USB transport
  remain modeled; actual task contexts/SMP are unqualified. Root verifies all 185
  compiled repository inputs at `a67f381` under
  `lradc-20261020/root-integration.json`. The corrected cold-USB fixture reuses the
  same ELF/launcher; its runtime stamp is derived, not a second launcher execution.
  The operation-source difference adds configuration guidance only. Physical
  safety, population, reference accuracy, timing, IRQ delivery, analog drain and
  recovery remain unresolved.
  A separate finite ADC application now uses the actual CLDO3 provider and
  normalized clock decoder. Nine linked application/provider cases and fourteen
  sanitized clock cases pass; all 45 compiled inputs match integration under
  `adc-returning-final-20261018/`. It retains clocks, power, pins and pending IRQ
  state after configured disable. Electrical admission and physical sampling
  remain outstanding.
- UFS initialization: **not applicable** to the current boards, which have no UFS
  device populated. The optional controller code is implemented, but its earlier
  physical attempt did not return to ROM. [Storage evidence](storage.md) preserves
  that failure and untested correction. Keep probing disabled on these boards.
- Independent watchdog: **implemented** register operations with host checks.
  [Hardware reset and FEL return](watchdog.md) remain untested; physical
  qualification is deferred. Supervised RAM-only tests use their own reviewed
  recovery plan. Unattended recovery and the failed UFS probe remain restricted.
  The stage-2 loader arms WDT0 in its four-second mode, without the FEL
  redirect, before entry. The resident lifetime of the [integrated OS image](os-image.md)
  renews it only while every listed service makes progress, through its own
  reload with the register mutex and mode readback. Host models cover renewal,
  each service stalling or never reporting, and a refused renewal. After a
  refused renewal the supervising task suspends itself instead of waking every
  tick, and the armed watchdog resets the board. Every runtime image that runs
  under the watchdog finds WDT0 already armed by its SRAM launcher or by the
  stage-2 loader. The [watchdog record](watchdog.md#launchers-arm-the-watchdog-for-runtime-images)
  lists the five files that write the four-second mode and the validators that
  require it; a different interval must change all of them together. The
  save-and-restore driver is linked only by the protected expiry diagnostic.
  Watchdog expiry after a cold boot has not run on a board.
- DMA: **tested** finite descriptor encoding in a host execution model, including
  memory/FIFO streams, 35-bit addresses, and invalid-chain refusal.
  The polled controller core also passes host checks for noncoherent transfers,
  concurrent ownership, bounded cancellation, and retained buffers after faults.
  [Native memory operations](dma-memory.md) pass a FreeRTOS emulator check for
  translation, permissions, cache instructions, and serialized callers.
  The [privileged FreeRTOS service](dma-service.md) passes a two-core emulator
  session for copied requests, task ownership, retained buffers and I2S producer
  shutdown ordering. Its root build target links successfully.
  An optional [DMA0 interrupt binding](dma-service.md#an-optional-dma0-interrupt-replaces-tick-polling-between-dma-events)
  (GIC shared peripheral interrupt 111, INTID 143) is **implemented** in the
  service. While every
  progressing channel waits for an enabled DMA event, the worker sleeps until the
  interrupt, a request or a fallback interval instead of polling every tick;
  failures fall back to tick polling. Emulator checks pass. The I2S image and
  the integrated OS image's microphone build bind the interrupt through their
  interrupt tables, with a one-tick fallback; other images poll once per tick.
  [DMA0 platform preparation](dma-platform.md) now validates inherited fabric
  clocks, dedicated gates/reset state and CPU/MCU routes under retained ownership.
  Host/reference checks and native compilation pass. Selected parent branches
  require forced-on control, software enable and effective-gate readback;
  focused admission checks pass. Fabric admission now validates normalized REF24
  for SYS_CLK24M and PERI0 profiles, accepting valid raw 24/26/19.2 MHz reference
  ratios and retaining REF ownership. Ten focused sanitizer cases pass under
  `dma-clock-reference-20261020/frozen/manifest.json`; they add no native-image
  or physical transfer qualification. The [startup adapter](dma-startup.md)
  now binds the clock registry, common counter, register mutex and deferred
  service activation. Focused two-core composition passes and the integrated
  target links. Physical handoff remains unqualified. The [native reservation
  pool](memory-reservations.md) supplies linker-owned page allocations and binds
  their lifetime to actual DMA controller and cache completion. Host checks and
  native linking pass. The [native DMA session](dma-pool-session.md) binds the
  actual startup/service to permanently reserved, narrowly authorized buffers.
  One two-core composition passes copy/reap, uncertain drain and late shared
  authority failure. Its complete eight-core copy image links with privileged
  SVC/IRQ routes and a native USB result observer. Focused instruction and host
  capture checks pass on the linked image. Genuine platform handoff, entry
  qualification and physical USB observation remain required.
  Physical memory exclusion, external bus drain and larger pool binding remain open.
  [DMA implementation and limits](dma.md) record the evidence. Board integration,
  both physical controllers, cache coherence, and physical transfers remain
  unimplemented or untested.
  The [system IOMMU](iommu.md) implements shared translation-table construction,
  cold installation and non-destructive fault observation for both banks.
  Source comparisons and host checks pass, including refusal of inherited
  page-walk faults before publication. Native exclusion of all affected masters,
  bus drain, memory binding and physical translation remain unqualified.
  [DMA address widths](dma-address-width.md) record one rule per DMA master.
  DMA0 and the IOMMU enforce named limits at admission, and host checks pass.
  The NVMe refusal of pools above 4 GiB is **planned**, and no transfer above
  4 GiB has run on hardware.

  DMA-copy [host admission](dma-pool-session.md#the-host-command-reuses-the-sealed-compact-launcher)
  now requires external digest/session-bound authority for selected-channel
  Nonsecure access, controller EL1 access and descriptor/data access through
  recovery. Eight focused host cases pass under
  `dma0-security-admission-20261020/root-integration.json`. Production refuses
  modeled authority; the source establishes no safe Nonsecure read of DMA security-register (`+0x20`).
  UART DMA, I2S and HDMI audio now share that host authority schema, bound to
  each immutable DMA configuration and actual session IDs. Thirty host-only
  cases pass with all twenty sources matching under
  `dma-consumer-security-20261020/root-integration.json`. UART DMA adopts the
  independently reviewed IDs before sealing its session. That host-admission batch
  changed no native/runtime image or wire layout; HDMI USB observation has separate
  typed authority and image evidence above. Production refuses modeled records; independently established
  physical access and each device's remaining admission prerequisites stay required.
  [DMA1 applicability](dma.md#dma1-request-routing-remains-unresolved) remains
  source-blocked. Its register layout, dedicated gates/reset and interrupts are
  identified, but the CPUX request table does not establish the CPUS controller's
  DRAM encoding. The pinned Linux DMA0 encoding differs from that table; neither
  value authorizes DMA1. The bounded primary-source search found no resolution.
  No DMA1 implementation, build, descriptor or launch is claimed.
- Header pin ownership: **implemented** shared [bank-qualified pin leases](pins.md)
  across 201 documented pins in banks B–M, including GPIO, mux/pull/drive,
  interrupt configuration and shared debounce ownership. Host checks, source
  comparisons and native compilation pass. Explicit UART routes exclude shared
  controller conflicts. Always-on output updates require a trusted bank latch
  shadow. Physical pin availability, interrupt delivery, unresolved signal maps
  and platform admission remain unqualified.
- I2S0 capture: **implemented** finite mono/stereo capture through the native
  DMA controller. Host checks cover producer shutdown before DMA drain and
  retained ownership after uncertain failures. Format encodings match pinned
  source functions. [Audio implementation and limits](i2s.md) records formats,
  header routes, and evidence. The [clock/reset/pin binding](i2s-platform.md)
  passes host resource/deadline checks, pinned CCU comparison, and AArch64
  compilation. Cold AUDIO1 preparation waits for effective output gates before
  publishing its stable lease; focused clock-admission checks pass.
  The [common clock registry](clock-leases.md) passes host
  composition with I2S and the existing GPU manager; its runtime files compile
  for AArch64. The [stream service](i2s-stream.md) adds bounded capture and playback
  periods, client-held buffer exclusion and explicit FIFO discontinuities.
  A two-core composition passes actual resource ownership, delayed DMA drain and
  retained failures; affected central targets link. The [native I2S application](i2s-application.md)
  connects the stream service to DMA0, a retained memory pool and USB0 result
  capture. Capture and recorded playback constructors pass instruction checks;
  USB failure revokes further device work. Optional native mimus transcription
  accepts one to five seconds of retained 16 kHz mono PCM with the existing model
  and formatter. Its native link and focused capture, cancellation, admission
  and copied-output checks pass. Startup now retains the complete authenticated
  session before provider initialization; the affected image and five binding
  checks pass under `i2s-binding-20261018/`. Existing audio and inference evidence
  was reused. Physical capture and inference timing remain unqualified.
  Simultaneous capture/playback now uses separate DMA channels and period
  catalogues with shared clock/pin lifetime. Both directions must complete and
  drain before success; uncertainty retains both. Composed stream, native
  client/pool/USB and host checks pass under `i2s-duplex-20261018/`.
  Repeated capture/playback/duplex now reuse the existing period service with
  four copied capture slots. Full USB delivery and exact acknowledgment precede
  slot reuse; cancellation preserves backlog through terminal drain. Focused
  ownership, USB and host checks pass under `i2s-repeated-20261018/`; all 163
  compiled inputs matched that image's integration. Fixed hardware-linked TX
  rings and checked-copy cyclic RX/duplex are now integrated. TX sources remain
  immutable; RX uses private staging before the existing USB slots. Complete USB
  delivery and exact ACK still precede slot reuse. Selected descriptor/provider
  checks and three standalone-client cases pass; client service calls are modeled.
  The native image link is separate evidence. Three focused review findings are
  resolved. Root integration verifies 168 compiled inputs, 69 linked objects,
  127 sources, 224 artifacts and four tools under
  `i2s-hardware-cyclic-20261019/root-integration.json`. Earlier unaffected cases
  were reused within the scope in `identity-reuse-clarification.json`. The manual's
  completed-package counter lacks explicit destination-write/cursor ordering.
  Cyclic receive therefore copies a period only while one whole period
  separates it from the DMA writer, and refuses rings shorter than four
  periods. With the application's four periods, the client must return each
  copy within one period, 128 ms. Cyclic RX/duplex physical admission requires
  `cyclic-rx-completion-and-reuse-ordering-established`, which only a board
  measurement can meet: destination writes must trail the package counter,
  and the writer must lead the counter and cursor, by less than one period
  each. No such record exists; CPU-cache review alone does not establish it.
  Runtime PCM direction 6 accepts copied 16 kHz mono S16LE periods through
  finite TX leases. The [file-input protocol](i2s-application.md#complete-file-input-retains-unknown-serial-completion)
  adds BEGIN_FILE and END_INPUT, with an exact short final period and no guard
  padding. Normal EOF requires every declared frame's DMA consumption and an
  observed empty FIFO. Serial-wire completion remains unknown: TX/GEN, clocks,
  pins, catalogue and buffers stay owned; REAP and CLOSE are refused. Explicit
  later CANCEL may truncate audio. The original prefix-plus-guards/CANCEL mode
  remains separate. Actual native runtime/service/controller/cache/audio/pin
  composition, compiled client/USB and host checks pass with register/DMA/FIFO
  effects modeled. Root verifies 171 inputs and 70 objects against revision
  `88bf392`, with no compiled-source divergence, under
  `i2s-file-playback-20261019/root-integration.json`. Earlier refill evidence
  remains under `i2s-runtime-pcm-20261019/` within its recorded scopes.
  The I2S image routes the DMA0 interrupt through a fixed table, so the end of
  a finite period's queue wakes the DMA worker at once; an emulator check
  passes. The [integrated OS image](os-image.md) captures recognition input
  through the same receive path in its microphone build.
  Physical continuity, playback, wire drain, cache visibility, Bluetooth PCM
  routing and broader audio functions remain unqualified or required work.
- G2D: **implemented** one retained fill, source-copy, source-over-solid, exact 2× enlargement, general scaling or
  image-over-image operation per session through the [native application](g2d.md), with copy and scaling
  converting between the eight 32-bit RGB channel orders. Caller geometry is bounded
  to 1280×720 with byte stride at most 5120. Authenticated source input, RCQ
  commands and destination use guarded allocations. Typed global IOMMU admission
  excludes other masters while master 10 alone translates; shared DE_SYS/PCK,
  clocks, rails, PMIC and USB providers retain the original native lifetime.
  Fresh task-end, fault/guard checks and bounded copied output precede publication.
  The [GPL source adaptation](../apps/g2d/sources.json) preserves register/header
  notices; no opaque G2D firmware or Linux runtime is required.
  Actual allocator, task, admission, boot and SRAM checks pass with their recorded
  model boundaries. Twelve linked task cases include fill/copy effects interpreted
  from actual RCQ words; they establish no physical pixels. Three actual TinyUSB
  stack cases check FS/HS packet completion with modeled DCD effects; one uses
  synthetic terminal-length metadata. Host capture, geometry/allocation refusals
  and supervisor readiness checks pass; production refuses modeled authority.
  Earlier image `4e043dad…` retains 169 objects and 448 compiled dependencies.
  Root accepts the native/host/USB closures at `a7d2ece` under
  `g2d-native-20261020/root-integration.json`; the only native snapshot difference
  is an appended component-document evidence section. Existing verification was reused.
  Power-initialization image `81916537…` adds source-backed DE_SYS domain-1 initialization:
  coherent OFF requests ON, while coherent ON is adopted without domain writes.
  Fixed 10 ms polling and mutex deadlines stay within the original session bound.
  Uncertain writes, incoherent state or timeout retain resources without retry.
  Five changed native cases and seven host-authority cases pass with task identity,
  mutexes, device effects, translations and pixels modeled. The host-authority
  cases stop before allocation validation; they establish no complete admission.
  Root verifies 187 compiled repository inputs and 199 declared sources at
  `7104245` under `g2d-power-20261020/root-integration.json`; use
  `frozen-final/manifest.json`, not the older snapshot reference in `closure.json`.
  One image was built. All twelve allocated boot-object sections and symbolic
  relocations match; unchanged launcher inputs reuse binary `1f0fa59a…`.
  Earlier boot/SRAM, core and transport results retain their original scope,
  without replay. Physical power transition, SMP/cache behavior and pixels remain
  unqualified.
  Blend image `b27476ca…` adds operation 3: straight per-pixel source alpha
  over one opaque solid color, using the existing rectangle/source bounds and
  mappings. One actual submission/copied-result case passes with pixels/device
  effects modeled. Two host policy cases keep authenticated differing output
  eligible for ACK but refuse digest corruption; ideal integer blending is only
  an unqualified diagnostic. Two geometry refusals and two new SRAM cases pass.
  Root verifies all 187 compiled repository inputs at `1dbcf70` under
  `g2d-blend-20261020/root-integration.json`; boot and eight USB-unit comparisons
  preserve earlier evidence without replay. The changed geometry uses a new launcher.
  The normal-success correction now follows pinned Linux's task-end acknowledgment
  and mixer AHB reset pulse before output reads, including retained TASK_APPLY.
  It verifies acknowledgment and both reset writes while preserving configuration
  status, rotation reset and neighboring fields. Fresh fault/guard/status checks
  surround copying. Normal-success image `01b71b3f…` passes six completion forks from one
  submitted-job checkpoint, with no resubmission. One image was built. Normal
  completion copies 16,384 bytes; refusals permit no output read. Root verifies
  all 187 compiled repository inputs and 202 declared sources at `6f39897` under
  `g2d-success-20261020/root-integration.json`. Startup, pixels, task scheduling and
  device effects remain modeled or outside those cases. Earlier no-reset blend
  evidence retains only its original scope. Old boot/launcher checks remain
  scoped separately; seven USB units match and the eighth corrects one private
  session-digest offset immediate. No repeated transport matrix is claimed.
  Neither reset nor task-end proves physical visibility, drain or safe reuse;
  all resources stay retained.
  Operation 4 now enlarges a half-sized source crop exactly 2× into a positive,
  even-sized destination rectangle, using the existing ARGB8888 buffers, API and retained lifetime.
  Current image `f77be8b2…` passes three pinned register geometries, five changed
  native geometry refusals and one linked submission/retained copy. One high-speed
  C/TinyUSB delivery/ACK resumes that copied checkpoint; three new SRAM cases pass
  separately. Host geometry/authentication checks and exact capture pass, including
  refusal of digest-valid nonzero padding before ACK. Pixels outside the destination
  and row padding must remain zero; interpolation, filtered alpha and rounding
  are unchecked. Root matches all 187 compiled repository inputs under
  `g2d-scale-20261020/root-integration.json`; the sole declared native difference
  is a separately frozen host refusal fix, with all 23 final host sources matching.
  One native build and one launcher are preserved, without an old-matrix replay.
  The [host coordinator](g2d-preparation.md) uses one 19-second window within
  native 20-second admission, durably saves output before delivery-gated
  generation/digest ACK. Source-over CPU comparison is diagnostic, without a
  qualified hardware-rounding oracle. Every allocation,
  mapping and provider identity remains retained; timeout or ACK establishes no
  reclamation or drain. General scaling at any other ratio, including reduction with
  the pinned coarse decimation, image-over-image blending with per-pixel and plane alpha,
  and RGB order conversion reuse the same buffers, mappings and lifetime. Image
  `08e56fc5…` passes register-image comparisons with the pinned setters, the linked
  AArch64 validator and builder, a rebuilt 3008-byte SRAM launcher model and host
  sealing/ACK-policy checks; earlier operations emit identical commands. Rotation and
  mirroring (the pinned set lacks `g2d_rotate.c`), YUV color-space conversion, other
  pixel sizes and a translucent solid background remain required. Physical pixels, DMA confinement/visibility, IRQ
  delivery, startup, recovery and performance remain unqualified.
- GPU: **implemented** native [firmware decoding and staging](gpu.md) for the
  board software's target revision. The integrated [single-core compute
  application](gpu-application.md) permits initial physical validation independently
  of SMP. The earlier returning profile's healthy CLOSE validates firmware power
  states, completes asynchronous platform power-off and releases buffers only
  after verified OFF. Application
  success requires STOPPED. Focused checks and Astra review pass under
  `gpu-returning-integration-20261018/`; physical shutdown remains untested.
  Compute-only CLOSE now requests exact firmware common-context cleanup before
  existing healthy shutdown. It requires matched receipt/slot and success status;
  BUSY, missing/error receipt, stale timeline or revoked admission retains ownership
  without healthy power-off. An unused context first initializes its client ring.
  Seven actual-service composition cases pass with firmware, GPU/MMIO, isolation
  and admission effects modeled under
  `gpu-compute-cleanup-20261020/root-integration.json`. Root verifies all 83 compiled
  repository inputs and 86 declared sources at `e01d065`. Three composition build
  attempts, two linked images and setup failures retain their separate scopes.
  A separate full compute-only application build `dae5a387…` matches all 166
  compiled repository inputs and 371 declared sources under
  `gpu-compute-cleanup-native-20261020/root-integration.json`; it was not executed.
  Default compute backing/VM mappings stay retained and reopen is refused; no full-application
  result transfers from the composition model.
  [Render firmware cleanup](gpu-render.md#healthy-close-cleans-firmware-objects-before-stopping)
  is also implemented before healthy shutdown: fragment/geometry contexts, supporting
  compute context, reverse-order render-target records and local/global free lists
  require exact successful receipts. Partial receipts stay retained, and late
  grow/reconstruction events fault through shutdown polling before resource handlers.
  Eight modeled cases use actual reserved native owners and one built model image
  with per-case selector patches. Root accepts the model's 85 compiled repository
  inputs with one verified header-comment difference and 89 declared sources under
  `gpu-render-cleanup-20261020/root-integration.json`. A separate texture application
  build `6c45bdc3…` matches all 166 compiled repository inputs and 476 declared sources
  under `gpu-render-cleanup-native-20261020/root-integration.json`; it was not executed.
  Firmware, GPU pixels, MMIO and isolation remain modeled. Physical cleanup/visibility
  remains unqualified. Default retained profiles still do not unmap, reclaim or
  reopen; the optional standalone path below has separate disposal evidence.
  The [texture frame path](gpu-render.md#cpu-work-per-frame) moves each upload
  byte once into its GPU-read area, lends the output frame in place for PRESENT
  and READ, and keeps depth and stencil in tile memory with no DRAM load or
  store. Counted under Unicorn, the worker's instructions for a 1080p profile-4
  frame fall from 619 million to 9.5 million, its per-frame translations from
  66,457 to none, and its tick waits between CPU-only steps from 65 to none.
  Profile-5 trusted programs, general blending, clipping, mipmaps, B5G6R5,
  4× multisampling and scanout of the rendered frame without a copy remain
  required.
  Optional [standalone child disposal/reopen](gpu-render.md#standalone-sessions-can-dispose-and-reconstruct-child-resources)
  is now implemented for the admitted sole-client single-core image. After healthy
  CLOSE, exact owner/epoch and stopped-backend checks precede child cache ownership,
  offline translation removal and release of three compute/four graphics parent
  leases. Two global firmware parents stay retained. OPEN rebuilds children with
  fresh generations/queue epoch; failed partial release/reconstruction prevents retry.
  Mixed-device images remain excluded. DISPOSE and reconstruction each have 15 s
  within the original deadline; CLOSE retains 1 s.
  Actual native leaf checks use original image `e26b9632…` with modeled healthy-stop
  admission. Final depth image `780d9544…` exercises fourteen app/mailbox/TinyUSB
  exchanges with modeled service/GPU completions; healthy plus twelve host failure
  cases consume those replies. Two successful builds retain 87 of 89 objects after
  the application-only budget change. All 169 compiled repository inputs match
  final integration; ten noncompiled declared-source differences are separately
  classified under `gpu-resource-disposal-20261020/root-integration.json`.
  That depth batch compiled compute/triangle leaves only. Version-8 texture runtime
  and pending texture OPEN via STATUS remain unexercised.
  Separate [compute-only lifecycle evidence](gpu-compute-service.md#compute-only-disposal-and-reopening)
  now covers actual three-parent disposal/reconstruction and seventeen application/
  TinyUSB exchanges on image `06315568…`. The immutable 8,288-byte package and
  global parents stay retained. Fifteen modeled 1 ms owner steps rebuild fresh
  generations/queue identity. Two 65-word outputs use pages of 32, 32 and 1 word,
  before and after CLOSE/DISPOSE/OPEN. Pending STATUS preserves old epoch/sequence;
  coherent READY publishes new identity/result and permits immediate READ.
  One healthy host replay and four tail-page refusals pass, preserving each exact
  256-byte prefix. Root matches all 169 compiled repository inputs at `7d6b89a`;
  later changes add checks/docs only. Three noncompiled declared-source differences
  are classified under `gpu-compute-lifecycle-20261020/root-integration.json`.
  The corrected runtime image retains 86 of 89 objects from the initial build-only
  image; their runtime evidence is separate. GPU/service completion, CPU
  cache/translation and healthy-stop
  authority remain modeled. No numerical or physical execution is added.
  Physical firmware/pixels, cache visibility, reclamation/reopen, SMP and mixed-device
  allocator serialization remain unqualified.
  The finite GPU adapter is integrated with eight passing linked cases and
  Astra review. Its worker/PMIC quiescence and retained object placement are
  checked. The returning composition's Nonsecure publication and host capture
  have separate modeled evidence. Its corrected fixed-workload candidate passes
  seven helper instruction cases and one normal two-job/CLOSE/provider-parking
  model under `gpu-returning-repeat-fix-20261019/`. It reuses 86 of 88 objects
  with unchanged return objects and structure layouts. Root source/binary review
  passes under `root-review.json`, verifying 88 objects, 218 frozen inputs and
  eight unchanged linked return functions. Physical admission remains false.
  Evidence for the preceding finite/publication paths is
  preserved under `gpu-finite-adapter-20261018/layout-correction/` and
  `gpu-returning-application-20261019/ns-publication/`.
  The earlier single-core application's startup, packaging and USB profile checks
  pass; all 175 frozen source inputs match its integration. The first physical single-core attempt produced
  no native USB result; its refusal-report read timed out. GPU initialization and
  computation remain unobserved. Evidence:
  `gpu-single-core-20261018/physical-candidate-116/runtime/`. That failed session
  was recovered by power removal; do not repeat the image unchanged.
  The second board's GPU clock and reset/bus registers both read zero under
  `gpu-clock-read-20261018/`, satisfying that current-state prerequisite.
  A returning 20-register PMIC probe also observes DCDC4 enabled with selector
  30, the programmed 800 mV setting, under `gpu-supply-probe-20261018/`.
  This is a register observation rather than a voltage measurement. Supply
  settling and exclusive ownership were reviewed for the first attempt. Its
  reset-held baseline does not establish historical transaction drain or safe
  resource reclamation.
  A sanitizer check covers bounds, guards,
  malformed-image refusal, and buffer aliasing; staged allocations match an
  independent ELF reader. Native [MIPS preparation](gpu-mips.md) also builds
  firmware page tables, boot data, and wrapper register values; complete outputs
  match isolated reference functions. The [firmware queue transport](gpu-queues.md)
  passes host checks for publication, return ownership, events, deadlines, and
  retained failures. Native [initialization records](gpu-fw-state.md) match the
  reference initializers and pass integration with queue formatting and MIPS
  mappings. Native [GPU workload tables](gpu-vm.md) support six page sizes and
  match the pinned entry encoders and firmware context initializer. Native
  [workload table publication](gpu-vm-memory.md) passes a FreeRTOS emulator check
  for CPU mappings, cache operations, reclamation, and retained mapping faults. These
  components compile for AArch64. The [startup controller](gpu-start.md) passes
  host ordering, deadline, and retained-failure checks; complete register traces
  match the reference startup functions. The [control worker](gpu-control.md)
  passes host compatibility, health-command, bounded event, and retained-failure
  checks. [Shutdown](gpu-stop.md) also passes host handshakes, idle/reset ordering,
  retirement, and retained-failure checks. Physical isolation, identity,
  firmware boot, graphics/compute execution, and recovery remain untested.
  The [FreeRTOS lifecycle service](gpu-service.md) passes a two-core emulator
  check integrating resource preparation, startup, control, and shutdown under
  one privileged worker. It covers client ownership, reopen, event service,
  and retained failures. Physical backend operations remain outstanding.
  The [native firmware memory adapter](gpu-memory.md) passes emulator integration
  for CPU translation, uncached shared resources, cached firmware publication,
  reclamation, and retained permission failures. Physical coherence and board
  reservation remain unqualified. The [native firmware constructor](gpu-reserved.md)
  now reserves actual write-back and uncached linker pools and prepares firmware
  state and MIPS tables. Emulator checks pass for translation, NPU coexistence
  and retained allocation failure; linker layout checks pass. The
  [fixed-compute constructor](gpu-compute-reserved.md) now allocates payload,
  context and planner-sized VM tables, runs actual builders and binds the service.
  Native instruction checks pass for construction and retained capacity failure.
  Native reservation admission now accepts acquired completion for another
  submission, checking owner, epoch, command serials and the actual firmware
  timeline. Focused checks exercise the real callback and reject changed
  ownership/completion. Earlier repeated-job checks substituted that callback;
  the historical returning image contains its previous NEW-only check and is
  superseded by the corrected fixed-workload candidate. No physical operation
  is established by these software checks.
  Physical computation, complete application binding, exclusion/drain and wider
  GPU workloads remain open.
  The [power controller](gpu-power.md) has host/reference evidence for GPU
  clock/reset and domain transitions. The [resource manager](gpu-resources.md)
  has host/native-adapter evidence for leases, inherited settings, and retained
  I2C restoration failures. The [native platform binding](gpu-platform.md) now has
  instruction-model evidence for admission, power transitions, and release;
  shared mutexes pass a two-core FreeRTOS check. Cold initialization and physical
  composition remain required. Asynchronous power sequencing passes in the service model.
  [Interrupt delivery](gpu-interrupts.md) passes actual QEMU GIC delivery across
  cores, queue saturation, backlog draining, deferred rearm, and retained failures.
  Physical GPU power and interrupt behavior remain unqualified.
  The [translation-cache invalidation protocol](gpu-flush.md) has host evidence
  for publication ordering, bounded command completion, and retained failures.
  Actual mapping changes and physical invalidation remain unqualified.
  [Firmware event dispatch](gpu-events.md) has host ABI/control integration
  evidence. Native [free-list management](gpu-freelist.md) now has host/reference
  evidence for backing ownership, growth, reconstruction, and deferred firmware
  responses. [Service integration](gpu-freelist-service.md) now passes bounded
  construction, deferred responses, shutdown draining, and retained faults in
  QEMU. The [native graphics owner](gpu-graphics-memory.md) now allocates real
  backing, builds two hardware render-target records and publishes graphics
  before compute and power activation. Source comparisons and native preparation,
  capacity-refusal and deadline cases pass; six affected central targets link.
  The [retained depth/blend profile](gpu-render.md#depth-testing-and-source-over-blending)
  extends copied indexed textures through 1080p with fixed trusted shaders.
  Per-frame state selects depth OFF/LESS/LEQUAL/ALWAYS, optional writes while depth is enabled, and
  straight-alpha source-over blending. State is revalidated before replacing input
  or revoking copied output. Depth is cleared per frame; its extra mapping brings
  the maximum plan to sixteen mappings and 27 GPU table pages. Guarded framebuffer
  snapshots retain their download slots until full USB completion; replacement
  and CLOSE refuse borrowed slots.
  The shared render worker now submits geometry plus partial-fragment FRAG_PR
  (type 204), then standalone final FRAG (type 203). Frame n uses geometry
  completion n, intermediate fragment 2n−1 and final fragment 2n. The partial
  fragment carries no cleanup reference; the final carries one. Both rings remain
  owned through the tail, and only acquired geometry/final-fragment completion
  admits output or reuse. Uncertain final submission retains both owners.
  Compiler/reference, actual VM/service and affected application checks pass with
  GPU/firmware effects modeled; linked application checks also model service
  replies and USB effects. A separate legacy-profile-2 sequence check passes.
  Root verifies 220 inputs and 88 objects for unchanged ELF `d9a6fcfe…` at
  `17fa005` under `gpu-depth-blend-20261019/root-integration.json`; the sole source
  delta is a verified declaration newline. `root/layout-review.json` preserves
  static session/layout scope without startup execution. Full-speed TinyUSB reuse
  is source/object-bound; older single-final-fragment ownership evidence remains
  historical and is superseded for the corrected shared rendering sequence.
  Compute queues retain their separate behavior. Physical graphics, cache visibility,
  confinement and recovery remain unqualified.
  [Graphics profile 4](gpu-render.md#state-batches-perspective-texturing-and-stencil)
  draws up to sixteen index ranges per frame, each with its own depth compare,
  culling, stencil test and one of five fixed blend equations. It adds
  perspective-correct texturing, filtering and address modes, sRGB decoding and
  a packed D32S8 attachment sized from the session. A completed frame can be
  [lent to a display consumer](gpu-render.md#lending-a-completed-frame-for-presentation)
  by physical address. Pinned-Mesa reference, QEMU native-service and linked
  application checks pass with GPU, firmware and USB effects modeled; physical
  rendering is unqualified. Arbitrary shaders and blend factors, clipping,
  multisampling, mipmaps, other texture formats, wider compute input/output and
  display use of the lent frame remain required.
  The [native GPU application](gpu-application.md) connects real firmware,
  providers, IRQ routing, fixed rendering and copied USB output. Its central
  image links; focused checks verify retention when isolation is unavailable
  and prevention of device access after application failure. Its supervised
  mode also builds and passes session, launcher, packaging and refusal checks.
  The refreshed supervised image includes PMIC APBS1 source4 support. Its affected
  link and nominal offline packaging pass; all 307 preserved repository inputs
  match integration under `gpu-pmic-refresh-20261018/`. Existing GPU and PMIC
  checks were reused. The software-only package supplies no physical admission.
  This mode retains resources until
  manual power removal and still requires actual initial isolation. Successful GPU
  output, general drawing, lease disposal for reopen and physical execution
  remain outstanding.
  The [native lifecycle backend](gpu-backend.md) now has instruction-model
  evidence for identity admission, restricted MMIO, memory/GIC composition,
  retained faults, and actual queue event reception. A proven physical memory
  isolation provider, board reservations, and firmware boot remain required.
  The [client command transport](gpu-cccb.md) passes host/reference checks for
  complete ring and KICK bytes, wraparound, queue pressure, deadlines, and retained
  faults. The [offline compute-context builder](gpu-compute-context.md) matches
  complete reference bytes and composes with native VM/client-queue formatting.
  Both compile for AArch64. The [context program producer](gpu-compute-programs.md)
  now constructs executable USC/PDS programs from pinned compiler output and
  matches complete reference allocations. The [fixed numerical job](gpu-compute-job.md)
  constructs a real compiled shader and command stream, then requires timeline
  advancement and correct guarded output. Source-byte and host lifecycle checks
  pass; no GPU instructions executed. The [compute service](gpu-compute-service.md)
  publishes native workload memory and firmware records before startup, then
  runs owner-only jobs with timeline and guarded-output checks. Focused FreeRTOS
  emulator cases pass; central compute, lifecycle and IRQ targets link.
  Repeated fixed arithmetic submissions now preserve live command queues and
  require advancing completion counters before acquiring and copying results.
  Cancellation revokes further publication and retains all resources. Five new
  composition cases and host decoding checks pass, and the native compute mode
  links under `gpu-completion-20261018/`. The
  [caller-selected compute application](gpu-compiled-programs.md) now accepts
  trusted compiled programs and copied input through a sealed session without
  rebuilding firmware. Both checked array kernels pass compiler-reference,
  sanitizer, native service and host checks with modeled device writes.
  Bounded multi-dispatch now supports up to 31 direct dispatches, each with one
  32-lane workgroup, and at most 992 elements within package input/output bounds.
  One final firmware timeline covers all dispatches. Compiler-reference,
  sanitizer, native service and host checks pass with focused review under
  `gpu-dispatch-geometry-20261019/`. Integration verifies 85 objects, 169 sources
  and 66 artifacts. Of 215 compiled inputs, 214 match; the sole monitor source
  difference adds inactive `SUNSTONE_NPU_INTERRUPT` branches. Frozen monitor
  source/object identities remain authoritative without active GPU monitor or
  structure changes.
  The [workgroup-grid profile](gpu-compiled-programs.md#a-real-workgroup-grid)
  now submits one direct dispatch over a bounded 1D or 2D grid of at most 31
  workgroups and 992 logical elements. Trusted compiler-generated programs use
  32-lane local shapes, workgroup IDs and counts, and reject padded lanes.
  Transpose and horizontal-neighbor kernels pass compiler/reference and native
  application checks, including two changing inputs, copied results and CLOSE;
  GPU arithmetic and writes are modeled. Root verifies 215 compiled inputs,
  85 objects, 178 sources and 50 artifacts under
  `gpu-workgroup-grid-20261019/root-integration.json`. The sole current-source
  exception at integration `dfedc52` is an unused board workload selector change;
  frozen inputs and the original modeled image remain authoritative. Preserved
  image equivalence, recorded in `root-reuse-frozen/manifest.json` under that
  directory, supports reuse of accepted checks. Physical execution remains
  unqualified.
  Optional [runtime input](gpu-compiled-programs.md#runtime-input-in-one-session)
  now replaces copied same-shape input after acquired prior completion, with
  exact-sequence copied READ and healthy device CLOSE. Program/geometry/mappings
  stay fixed. Native service and linked owner/mailbox/parser/host checks pass
  with device writes and selected service replies modeled. Device-cache ordering
  relies on the pinned firmware's documented per-job coherency contract; CPU
  maintenance alone does not establish GPU-cache visibility. Evidence:
  `gpu-runtime-input-20261019/`. Root verifies 215 compiled inputs, 85 objects,
  172 sources and 14 artifacts against revision `5954081` under
  `dma-header-integration/root-integration.json`. Load-segment equivalence apart
  from build identity supports reuse of accepted checks; later shared-header
  changes do not invalidate this frozen candidate.
  The [cache-ordering source audit](gpu-render.md) confirms existing per-job
  firmware flushing and matching completion updates for compute and rendering;
  it identifies no missing host cache command. Evidence:
  `gpu-cache-visibility-20261020/assessment.json`. This reuses pinned source and
  accepted image comparisons, without qualifying physical visibility or opaque
  firmware internals.
  Wider dispatch geometry, shape/program replacement, graphics, concurrency,
  physical computation and mimus acceleration remain required. The accepted
  fixed-workload returning profile is preserved separately.
  The [combined GPU/NPU application](accelerator-composition.md) retains the
  trusted GPU compute service and complete ShuffleNetV2 NPU graph in one image.
  Both reuse one counter, clock registry, locks, PMIC owner and global resource
  manager; NPU reservations precede GPU reservations. One USB owner interleaves
  copied GPU results/input replacement and two NPU tensor/result exchanges, then
  closes each engine independently within the original absolute session deadline.
  Shared-provider failure revokes both engines; local faults retain that engine's
  resources. Actual constructors, clients and response ownership checks pass with
  admission, CPU/RTOS, device results and USB effects modeled. Saved NPU SDK outputs
  are saturated and do not establish input-sensitive accuracy. The native image
  is linked. Physical execution remains unqualified; graphics composition and
  broader concurrent workloads remain required. Evidence is under `gpu-npu-composition-20261020/handoff.json` and its
  provider/runtime/ownership results; component firmware/compiler dependencies
  remain unchanged. Root verifies all 270 compiled inputs and 114 objects at
  `214f3fe` under `gpu-npu-composition-20261020/root/integration.json`.
  Host preparation and supervised capture are implemented. The actual linked
  producer yields 666 durable frames, four copied outputs and both healthy CLOSE
  states; six affected host refusals pass. The actual 19-second supervisor policy
  reaches readiness and stops before queue/USB access. Positive bundle fixtures
  substitute the NPU physical validator; production refuses synthetic proofs.
  Root verifies 3,433 frozen files and all 24 current sources at `f645c89` under
  `accelerator-host-20261020/root-integration.json`; no native rebuild or physical
  execution occurred. Hardware, arithmetic and RTOS effects remain modeled.
  Trusted copied frames also feed the [GPU-to-HDMI application](gpu-display.md)
  described above; GPU CLOSE precedes display start, and physical output remains
  unqualified.
  The [core-mode check](gpu-compute-mode.md) requires observed single-core compute
  capability after power-up and before submission. Instruction and focused
  service checks pass; physical identity remains unobserved. The
  [isolation contract](gpu-isolation.md) requires exact callback success before
  memory reclamation; uncertain results retain memory. A physical request-block
  and transaction-drain protocol, multicore addressing, repeated-job resource
  disposal and actual GPU execution remain outstanding.

- Display engine: **implemented** [immutable framebuffer scanout control](display-engine.md)
  for XRGB8888 through one UI channel into DE1 and LCD1/DSI1. Host checks,
  source comparisons and native compilation pass. Preparation defers producer enable;
  stop and faults retain the framebuffer and resource leases. Memory reuse
  requires independent drain evidence. The [scanout coordinator](display-scanout.md)
  composes the actual PHY, panel-command, DSI, LCD1 and display-engine controllers.
  Host checks cover start/stop ordering, authenticated frame/resource receipts and
  retained failures; native compilation passes. Native [media clock preparation](media-clock.md)
  supplies real shared-register ownership and inherited-parent checks. Host checks
  and native compilation pass, including finalization fault retention. The native
  DSI binding connects these providers to retained framebuffer/table allocations,
  actual IOMMU mappings and ordered controller accesses. Focused composed checks
  and AArch64 compilation pass under `display-native-20261018/`. Panel selection,
  analog-profile qualification and initial isolation authority remain required;
  STOP retains resources because display-engine drain is unsupported.
  The [standalone DSI application](display-scanout.md#caller-uploaded-immutable-frame-replay)
  now copies and authenticates two complete XRGB8888 frames before scanout, then
  selects those immutable slots repeatedly through the register command queue
  (RCQ). Actual provider composition passes 0→1→0 selection at 720p and 1080p.
  Each result binds the requested sequence/slot, fresh RCQ completion and a later
  DE frame-end observation; the next request waits for full copied USB delivery.
  Both framebuffers and RCQ pages remain permanently immutable. These observations
  establish no panel acceptance, outstanding-read retirement or safe overwrite.
  Input/refusal, retained-fault, linked submit/serializer/credit, launcher and host
  capture checks pass with hardware and critical primitive boundaries modeled.
  Root verifies 464 native inputs, 177 objects and 198 host-composition inputs under
  `display-frame-present-20261019/root-integration.json`; its static retained-layout
  review is `root/layout-review.json`. Shared startup is unexecuted.
  No panel is selected or attached. Image-bound electrical/PLL, rail/bank-voltage,
  wiring, isolation, fetch-padding and panel facts remain physical prerequisites;
  model-only records are refused. Physical pixels, DMA confinement and drain remain
  unqualified. Application use of ring streaming and slot recycling, successful
  STOP, release/retry, composition and wider display modes remain required. The generated
  single-frame profile retains its separate accepted evidence.
  The HDMI framebuffer, DisplayPort and GPU display images now share one
  retained DE1 framebuffer source. Its DE_SYS domain-1 step uses the domain-ON
  provider that G2D uses: a domain that is already ON is adopted without a
  command write, and an OFF domain receives the existing power-up sequence.
  Host checks pass, including a run from an inherited ON domain. The DSI image
  requires PCK domains 0, 1, 2, 3, 8 and 9 to be ON at entry and powers none of
  them; no qualified VO0 or IOMMU domain sequence exists. The DE_SYS power
  provider also powers domains 9 (VO0) and 10 (VO1), and host checks pass, but
  neither DSI nor deinterlace calls it yet; DSI also needs domains 0, 2, 3 and
  8 for its IOMMU group. Deinterlace still fails setup on an unpowered DE_SYS
  domain. Physical domain power-up remains untested.
  [Replay over a ring](display-engine.md#de352-replay-over-a-ring-of-two-to-four-frames)
  of two to four frames is **implemented** in the display engine. The
  configuration fixes the frame size, and a slot becomes writable again only
  after a frame end shows that scanout has moved past it. Host checks pass,
  including refusal of ring readings that the configuration does not declare.
  No application produces frames into the ring yet. The ring stays retained
  after STOP, because no drain proof exists, and physical slot retirement
  remains unqualified.

- MIPI DSI: **implemented** bounded [low-power command transmission](dsi.md).
  Complete packet/register comparisons and retained-failure checks pass; native
  compilation passes. The [PHY controller](dsi-phy.md) verifies inherited PLL
  factors and lock, prepares analog controls and composes with command transport.
  Source comparisons and focused admission checks pass. The [video transmitter](dsi-video.md)
  implements four-lane RGB888 and loose RGB666 with continuous clocks and bounded
  engine stop. Source-register comparisons and host lifecycle checks pass.
  The [LCD1 timing controller](tcon-lcd.md) supplies DSI1 timing, authenticated
  start and bounded stop after producer drain. Source-profile and fault checks
  pass; native compilation passes. [Display PLL activation and retuning](display-pll.md)
  now follows the source sequence with intermediate-rate bounds, filtered lock
  checks and retained ownership. Host checks and native compilation pass.
  [Atomic scanout handoff](display-clock-handoff.md) now transfers the completed
  PLL reservation into persistent ownership and refreshes the actual media
  controller token before further access. Focused host composition and native
  compilation pass. Qualified initial analog provisioning, pin ownership,
  native frame/panel binding, read transactions and
  physical qualification remain outstanding.

- Radio: **implemented** [identity-message encoding and decoding](radio-codec.md),
  with host sanitizer and AArch64 compile evidence. The
  [bootstrap and board mapping](radio.md) remains source-based research, with a
  host-tested Bluetooth patch-table inspector. Published software identifies
  an AIC8800D80 candidate and loads Bluetooth patches over SDIO before UART use.
  The [SDIO controller/card worker](sdio.md) passes host checks for bounded
  preparation, enumeration, CMD52/CMD53, interrupt ownership and retained faults.
  Its runtime files compile for AArch64. The [bootstrap transport](radio-transport.md)
  now prepares the SDIO function, performs the fixed setup write and reads both
  identity words. The [firmware loader](radio-firmware.md) verifies immutable
  artifacts, uploads Bluetooth patches and Wi-Fi firmware, and waits for start
  confirmation. The [runtime worker](radio-runtime.md) computes binding replies,
  checks status and preserves continuously received data/control records.
  Host composition and native compilation pass. [Wi-Fi initialization and scan](radio-wifi.md)
  implement the source-defined RF setup and firmware startup sequence, explicit
  channel policy and bounded passive scanning. Source-packet comparisons and
  host lifecycle checks pass. [Station association](radio-association.md) adds
  copied targets, explicit transmission authority and separate command/connection
  completion. Source and focused host checks pass. A connect indication that
  the firmware marks as roaming leaves the link unresolved and blocks replacement
  operations; firmware-initiated roaming and fast transition are unsupported. [Data transport](radio-data.md)
  adds owned Ethernet packets, reserved transmit credits and EAPOL authentication
  frames with firmware confirmations. Open-network access requires an explicit
  association-bound authorization. Host checks, source comparisons and native compilation
  pass. [WPA2 station security](radio-security.md) now composes the real RSN
  handshake with confirmed keys, group renewal, controlled-port authorization
  and CCMP receive replay checks. Host checks and freestanding native linking
  pass. Actual firmware encryption,
  integrity and entropy quality remain unqualified.
  [Bluetooth H4/HCI](bluetooth-hci.md) implements bounded UART packet framing,
  retained receive ownership, command credits and controller identification.
  Host checks and native compilation pass, including exclusion of unread stale
  replies before transmission. It requires fresh firmware/UART generations and
  scheduled polling with the UART interrupt masked. [Controller configuration](bluetooth-config.md)
  sets Classic events, disables automatic peer acceptance and verifies connection
  timeout, page-scan activity and page-only scan settings. It preserves complete
  and partial incoming requests through the transfer to the ACL owner. Focused
  host checks and seven native consumer compilations pass. Actual firmware/UART
  binding, authenticated receive-service timing and pairing remain required.
  [Bluetooth ACL/link ownership](bluetooth-acl.md)
  adds outbound BR/EDR connections, bounded packet transmission and controller
  credit accounting. [Incoming admission](bluetooth-incoming.md) reserves copied
  peer identities and performs actual HCI acceptance or rejection with ordered
  completion. Host checks and native compilation pass; uncertain partial
  transfers and disconnect results remain retained. [L2CAP channels](bluetooth-l2cap.md)
  add bounded setup in both directions, configuration, disconnection and data
  fragmentation/reassembly. Composed host checks and native compilation pass.
  Response and extended-response timers, independent caller deadlines, opaque
  channel generations and confirmed-slot reuse pass focused composed checks.
  Wire identifiers and lower link quarantine still impose finite lifetimes.
  A rejected request, or an attempt that closed before the controller assigned
  a handle, now frees its ACL slot. The headset application retries a page
  timeout or failed accept, at most three times, until the connection deadline.
  Host checks pass. A link that connected and then closed still ends the
  session; reconnect after a confirmed disconnect remains required.
  [Service discovery](bluetooth-sdp.md) now opens actual L2CAP channels,
  assembles bounded continuation responses and exposes service attributes and
  advertised RFCOMM endpoints. Host checks and native compilation pass.
  Confirmed teardown retains transport buffers until controller completion.
  The [local SDP server](bluetooth-sdp-server.md) serves search, attribute and
  combined requests with bounded continuation and explicit audio-gateway records.
  Host checks and native compilation pass; advertised features require actual
  application support and do not establish pairing or voice capability.
  [RFCOMM serial channels](bluetooth-rfcomm.md) compose service discovery with
  incoming and outbound credit-based transport, flow control and confirmed
  teardown. Host checks and native compilation pass, including exact incoming
  channel ownership and acceptance-time response fences. The initial
  [hands-free audio gateway](bluetooth-hfp.md) implements service-level setup,
  indicator reporting and gain reports over the actual incoming stack. Host
  checks and native compilation pass. [Synchronous headset transport](bluetooth-audio.md)
  adds actual SCO/eSCO setup and acceptance, CVSD with signed 16-bit PCM over HCI,
  copied audio buffers and separate controller credits. Focused host checks and
  nine native consumer compilations pass. The gateway answers the headset's
  status queries without call control. Call control, which adds the mandatory
  HFP call procedures (answering, rejecting, terminating and placing calls,
  redial, memory dialing and DTMF), exists only for HFP conformance and is a
  build option that is off by default. The headset's voice button starts one
  recording through HFP voice-recognition activation (`AT+BVRA`) in the
  Bluetooth image and the integrated OS image; host checks pass and physical
  button behavior remains unqualified. An [A2DP sink](bluetooth-a2dp.md)
  for SBC streams is **implemented** in the AVDTP layer, with an optional SDP
  record and a link security check on every step; no native application uses
  it. Host checks pass. Physical controller capabilities, headset audio, wired
  PCM routing and wider codecs remain outstanding.
  The [native Bluetooth application](bluetooth-native.md) connects firmware
  bootstrap, UART1, H4/HCI identification and copied USB output. Its headset mode
  reuses Zephyr secure pairing and RAM key policy, requires observed authentication
  and encryption, and publishes copied 8 kHz PCM after synchronous disconnect.
  Finite capture-to-mimus is also integrated through bounded 8-to-16 kHz
  conversion and the existing recognizer and formatter. Its native image links;
  actual host recognition matches the same converted-input reference, and
  mid-recognition cancellation suppresses copied output. Outgoing composition,
  native USB audio framing, physical headset operation and inference timing
  remain unqualified. The earlier identification evidence remains valid.
  [Startup binding](board-session-seal.md) now retains authenticated session and
  Bluetooth profile copies before SID and provider access. The board and headset
  images and 15 focused binding cases pass. The recognition-enabled image also
  links, and one focused case verifies its retained model-configuration handoff.
  Earlier recognition results retain their original scope; inference was modeled
  in this new startup case.
  Repeated Bluetooth-headset recognition is implemented in the supervised
  CVSD profile. Two copied PCM slots feed successive bounded 8 kHz segments to
  the unchanged converter, recognizer and formatter. A private input copy precedes
  slot acknowledgment; each transcript remains immutable until complete USB
  transmission and matching host acknowledgment. Backlog, loss or cancellation
  stops capture without overwriting accepted segments. Success also requires the
  matched synchronous disconnect and all accepted transcript acknowledgments.
  Two distinct speech segments copied through the actual stack match portable
  references for features, log probabilities, frames and formatted text. Stack,
  native USB and host checks pass with controller events, scheduling and USB
  hardware modeled; focused review is closed. The recognizer remains at EL0 and
  uses the existing copied APIs and privilege wrappers.
  Evidence: `bluetooth-repeated-recognition-20261019/root-integration.json`,
  `root-native-audit.json` and `root-integration-frozen/manifest.json`. Root verifies
  the selected image's complete compiled closure and six archives at integration
  `854c1d9`. Nine newer
  USB/board source differences are classified, including changed embedded layouts;
  the frozen inputs remain authoritative and must not be mixed with newer objects.
  The original five repeated-profile USB cases use an earlier preserved image;
  the changed finished/status ordering case uses the final image, as recorded in
  `identity-reuse.json`. Physical headset operation, receive-service bounds,
  recognition within the session deadline and wideband mSBC remain unqualified.
  The optional [mSBC profile](bluetooth-native.md#wideband-capture-through-the-existing-recognizer)
  now negotiates transparent eSCO and decodes copied 16 kHz PCM into the unchanged
  repeated recognizer. Explicit CVSD fallback retains its separate codec identity.
  Two decoded speech segments match portable recognition references; the actual
  linked codec also reproduces 96,000 PCM bytes from 400 encoded frames. Framing,
  capability/refusal, copied ownership, revocation and USB/host checks pass with
  controller/radio events, scheduling and USB effects modeled.
  Native CPU encoding/decoding uses pinned BlueZ libsbc 2.0, revision
  `8dc5d5ba381512ad5b1afa45c63ec6b0a3833244`, under LGPL-2.1-or-later.
  `apps/bluetooth/msbc-source.json` records source hashes; the freeze retains source,
  notices and relinkable objects. The default CVSD build omits this dependency.
  Root verifies the compiled closure and six archives under
  `bluetooth-msbc-20261019/root-integration.json`. Later shared USB/board layouts
  remain distinct from the internally consistent frozen image; future builds must
  compile affected consumers together. Physical transparent eSCO, headset capture,
  playback/duplex, receive-service bounds and session timing remain unqualified.
  [Caller PCM playback and duplex](bluetooth-native.md#caller-pcm-playback-and-duplex)
  are implemented for mono 16 kHz signed 16-bit little-endian PCM through mSBC.
  Playback-only starts
  no recognizer; duplex combines playback with repeated capture and the unchanged
  recognizer. Both require negotiated mSBC before consuming caller audio. Two
  private 2,400-byte period slots remain immutable through terminal packet
  accounting, complete USB result delivery and exact ticket acknowledgment.
  Backpressure preserves accepted data; END seals admission, and CANCEL can truncate
  audio. HCI credits and matched disconnect account for controller buffers, not
  headset rendering or serial-wire drain.
  Seven composed cases and eight native operation cases retain their earlier ELF
  `36bfa257…`. Final ELF `388f9f93…` adds exact profile-version guards, covered by
  its separate profile-gates case. Five objects changed and 221 remain unchanged;
  `bluetooth-pcm-duplex-20261019/profile-gate-delta.json` preserves that boundary.
  The final-image case covers version admission and copied PCM commit only.
  Root verifies 800 inputs, 226 objects and six archives against revision
  `3138e7e`, with no compiled-source delta, under
  `bluetooth-pcm-duplex-20261019/root-integration.json`. Controller/radio behavior,
  transport timing, scheduling and USB effects are modeled. Physical playback,
  capture/duplex, audible output, wider rates/codecs and wire-drained completion
  remain unqualified or required work.
  The application can also [attach to an AIC8800 module](bluetooth-native.md#attachment-to-a-wi-fi-module-owner)
  that a full Wi-Fi session owns. The Bluetooth client then holds UART1,
  H4/HCI, the wake pins and its own recovery latch. Its failure leaves the
  module and Wi-Fi running, and a module fault ends the client. Besides the
  60 s supervised lifetime, the application admits a resident lifetime with
  120 s for firmware loading, no capture window and a 2 s audio close after a
  stop. Module-owner and repeated-headset model cases pass, including a stop
  that another task stores during a capture poll. No board has run either
  mode. The stack's saturating deadline sums now use the shared
  `a733_deadline_after_saturating()`.
  The [native radio platform](radio-platform.md) composes SDC1/card, pin, clock,
  rail and PMIC ownership with bounded power/reset timing and a borrowed card
  lifetime. Host checks and native compilation pass. Its initial profile uses
  one data line at 400 kHz. An optional four-bit 25 MHz SDIO profile is
  **implemented** with the bootloader's default-speed delays and verified
  readback; host checks pass. No image requests it until a physical identity
  read qualifies it, so radio bootstrap still exceeds the 40 s supervised bound.
  Receive accepts aggregates of up to 112 blocks, read with one CMD53 of up to
  57,344 bytes whose deadline scales with its length. Firmware upload can write
  long patch-table runs as block writes; that option stays off until a hardware
  readback confirms it. Host checks pass. The [integrated OS image](os-image.md)
  radio build runs this station beside recognition.
  The radio owner now repeats session polls while the transport or session
  advances, and receive accepts reorder-flagged MPDUs and validated A-MSDU
  subframes; neither form occurs until an HT profile exists. The [native radio session](radio-session.md) keeps
  platform startup, transport, receive dispatch and socket polling on one task.
  Emulator checks pass for actual provider ownership, bounded renewal, a TCP
  exchange and retained failure. Firmware upload and association checkpoints
  reuse earlier evidence; uninterrupted physical startup remains unqualified.
  The [radio entropy provider](radio-entropy.md) connects asynchronous crypto
  engine requests to a bounded buffer for copied network random bytes. Shared
  engine faults invalidate buffered bytes; focused checks and native compilation
  pass. The [native board application](board-application.md) connects actual
  platform identity, radio startup, entropy, association and a bounded TCP client.
  A boot-CPU-only image now retains all four USB/radio workload selectors.
  Fifteen actual startup cases, nine host cases and native session-identity
  acceptance/refusal pass under `board-single-core-20261018/`. Focused review
  is accepted; all 569 frozen inputs match integration. Physical startup and
  operation remain untested. Entropy qualification, wider modes,
  shutdown and physical topology/recovery remain outstanding.
  [Native IPv4 networking](radio-network.md) implements UDP/TCP clients and one
  admitted IPv4 TCP listener through the actual FreeRTOS+TCP stack. Emulator checks
  cover packet exchange,
  deadlines, socket disposal and association revocation. Native linking passes.
  The [receive dispatcher](radio-dispatch.md) supplies one receive owner, copied
  scan/station events and bounded retirement of unsupported records. Emulator
  checks pass for sustained packet delivery, backpressure and retained ambiguous
  records. DHCP lease authority and copied DNS requests are integrated; four
  focused stack cases and the combined board link pass. Fixed-address and DHCP
  startup profiles pass sanitizer checks. An unexplained earlier DNS deadline
  miss remains a timing limitation. The [listener application](board-radio.md#an-admitted-listener-verifies-one-copied-application-exchange)
  accepts one profile-selected peer IPv4 address, exchanges 1–256 copied bytes each
  way and requires child/listener disposal receipts before terminal USB success.
  Backlog one bounds pending children; cancellation, deadlines and address loss
  retain ownership until the IP-task handoff completes. Seven actual-stack model
  cases, four application sanitizer cases and five compiled-producer/host cases
  pass under `net-inbound-20261020/`; their modeled boundaries are in `closure.json`.
  The native image is linked, not executed. The pinned MIT FreeRTOS+TCP revision
  `b86ad086e9870099bb0ac18a75dcb7f6dd508cc7` uses a generated-source IPv4 reply
  checksum patch; the original external source and notices remain preserved.
  Frozen radio-only workload 3 retains earlier USB host inputs, inactive in that
  selection, as classified in `bringup-impact.json`. Root verifies 431 compiled
  dependencies and 161 objects under `root-integration.json`; do not mix current
  objects into that image. Physical networking, entropy quality and RF/wire drain remain
  unqualified. [Explicit healthy reconnect](radio-session.md#explicit-healthy-reconnect-preserves-the-existing-service)
  now preserves the retained session/IP stack for the same AP with fixed addressing
  or DHCP,
  after every socket reply/disposal and an IP-task offline receipt. Fixed-address
  OPEN emitted PASS. The WPA2 fixture executes actual `board_radio_reconnect` and session polling:
  retirement, fresh handshake/keys, address restoration, new TCP exchange and CLOSE
  are verified from terminal RAM and client suspension. A reporting assertion
  prevented PASS; its corrected one-line fixture guard remains unbuilt/unexecuted.
  `board_radio_poll`, startup, initial firmware/association/authorization and lower
  RF/entropy effects are unexecuted or explicitly modeled. Root accepts the scoped
  closure under `radio-reconnect-20261020/root-integration.json`; four linked images
  and eight configuration/build/preflight attempts remain preserved. The 82-input
  repository comparison differs only in that final fixture guard; the declared
  source difference is unrelated hardware documentation. Separate DHCP reconnect
  and acquisition-timeout cases execute the actual IP task and socket service.
  The old DHCP socket/timer retire before fresh transaction/address publication;
  stale ACK is refused, and a new lease permits UDP exchange/CLOSE. Timeout keeps
  client admission closed. Association, firmware and peer effects remain modeled;
  this adds no WPA2 handshake, radio session-loop or board-poll execution.
  Root verifies all 43 model, 168 native-build and 181 declared repository inputs
  at `3d363f1` under `radio-dhcp-reconnect-20261020/root-integration.json`.
  The native board consumer was built but not executed.
  Recovery from a clean unsolicited link loss is **implemented**. An AP
  deauthentication, beacon loss or AP restart may arrive while no transfer,
  command, security or supplicant work is outstanding. The adapter then takes
  the interface offline through the IP-task fence, and existing sockets stay
  open. The owner reassociates with the admitted target and credentials until
  the recovery deadline. A host composition covers loss, a failed and a successful
  reassociation, socket survival and a new TCP exchange. Loss with outstanding
  work, and a second loss during recovery, still retain the radio. Firmware key
  state after the loss indication needs hardware observation. Physical
  reconnection and loss recovery, IPv6, broader server operation and wider
  security modes remain unqualified or required.
  Native startup now hashes its private profile before deriving runtime settings,
  closing the earlier live-memory hash/copy gap; focused binding checks pass.
  [Station power save](radio-wifi.md#station-power-save-follows-the-source-management-configuration)
  is **implemented** as the source management configuration, gated by the
  firmware's power-save feature; source-structure host checks pass. Reconnect
  and loss recovery admit any access point of the same network (same SSID
  and security, own beacon evidence and RSN element), so the station can roam
  by disconnection and new association; the loss-recovery composition recovers
  onto a second access point. The board's outgoing client and listener repeat
  a SEND or RECEIVE that timed out during a link loss, keeping reported bytes,
  so an exchange survives recovery; host checks pass. An
  [SNTP client](radio-network.md#an-sntp-client-supplies-utc-to-the-rtc-service)
  resolves its server, validates replies and exports an RTC SET request; host
  checks and an actual-stack composition pass, but no image runs it yet.
  WPA3 personal is not usable yet: the SAE exchange runs on a pinned minimal
  OpenSSL build in host and AArch64 model checks, but SAE external
  authentication, protected management frames and SAE association are missing
  ([details](radio-security.md#wpa3-personal-authenticates-but-cannot-yet-associate)).
  Physical power save, roaming, time accuracy and loss timing remain unqualified.
  A full Wi-Fi session can also [serve a Bluetooth client](radio-session.md#a-bluetooth-client-shares-the-firmware-generation)
  on the same module. The session reports its firmware generation while it
  reconnects or recovers a lost link, so the client keeps its H4 binding, and
  the native adapter accepts one added admission, for the Bluetooth wake pins,
  after the owner's own admission. The reconnect model covers the firmware
  generation after a healthy reconnect. The integrated OS image uses this to
  serve Wi-Fi and the headset from one module owner.

- Camera: The [native application](camera-application.md) connects actual provider
  startup, completed frame copies and USB0 capture. Its supervised mode includes
  explicit manual-power-removal admission, frozen host packaging and durable
  full-frame collection. Native linking and focused mode, admission, launcher and
  host checks pass. Startup now retains the complete authenticated session before
  provider initialization; the affected image and five binding checks pass under
  `camera-binding-20261018/`. All 120 preserved repository inputs match integration.
  No camera module is connected, so physical capture remains unqualified.
  Its retention adapter has the same host-checked mutex-ownership correction
  described under combined monitoring; prior native images retain their original
  scope and do not include that source fix.
  The underlying driver provides **implemented** bounded [OV5640 control](camera.md) for the
  reference driver's ten two-lane output sizes, from 160 × 120 to 2592 × 1944, in
  UYVY, YUYV and 8-bit raw Bayer, with a selectable frame length. Complete register
  images and derived rates of 36 modes match the pinned driver's own mode
  functions; lifecycle checks and native compilation pass. The native application
  uses 640 × 480 UYVY at nominal 30 frames/s. The [CSI controller](csi-receiver.md)
  configures local PHY, packet-port and parser state for both exposed port groups;
  complete register images match source functions. The [camera DMA controller](camera-dma.md)
  configures one NV16 image and retains buffers until capture, drain and cache
  synchronization complete. Register comparisons and ownership checks pass.
  The [route controller](camera-route.md) configures pass-through ISP1 and
  unity-scale VIPP0, with exclusive shared-selector ownership. The receiver
  now supports single-image mode. Host checks, source comparisons and native compilation
  pass. The [shared analog controller](camera-platform.md) implements cold PHY
  startup with exclusive common-register ownership and calibration readback.
  Source/model checks and native compilation pass. The [capture coordinator](camera-capture.md)
  composes the sensor, analog controls, receiver, route, frame DMA and IOMMU.
  Both port models pass one-frame NV16 delivery and guarded CPU ownership
  transfer after path shutdown, drain and fault scan. Global mappings and all
  other IOMMU masters remain reserved; concurrent device use and disposal are
  not established. Native [media clocks](media-clock.md) prepare CSI/ISP gates
  and rates under actual registry and domain leases. Host checks and native
  compilation pass. The [native camera binding](camera-native.md) connects
  actual shared resources, TWI, frame allocations and device controllers. Ten
  focused cases pass; five native units and two existing embedding fixtures
  compile. Completed payload is copied into a CPU-owned destination while source
  buffers, tables and hardware leases remain reserved. Complete image admission,
  module electrical binding, physical DMA/coherence and drain, native use of
  further modes and physical qualification remain outstanding.
  Repeated capture is **implemented**. The coordinator rearms a retained,
  readable capture into the same planes, mappings and leases with the sensor
  stopped, after two 100 ms quiet intervals and checked receiver and DMA status.
  The boot record can request up to fifteen discarded images before the
  readable one, so automatic exposure can settle. Sensor configuration now
  uses block writes of up to 40 bytes, 125 transactions instead of 308, and the
  camera image serves USB0 through the shared board service. Host-model checks
  pass. Register auto-increment, the quiet intervals and exposure behavior
  while packets are disabled need a module on the board.
  The coordinator records receiver and parser status before each sensor stop,
  so a board run can show whether stopping the sensor latches receiver errors.
  A TWI9 controller and an exclusive TWI9 clock scope are **implemented** for
  the CAM2 sensor bus. No CAM2 binding exists: the camera boot record has no
  port field, and receiver port 1 has no sourced deskew value.
  No MIPI camera is connected; physical capture qualification awaits a module.
  Continuous capture is **implemented** in the [capture coordinator](camera-capture.md#continuous-capture-delivers-complete-ring-slots)
  for any UYVY or YUYV mode, whose size now reaches the receiver, VIPP and DMA.
  Frames stream into two to four slots; each frame start delivers the previous
  complete slot and programs the next free one, and frames overwritten for lack
  of a free slot, incomplete or unattributable after a start that raced the
  address writes are counted as dropped. A modeled frame engine checks delivery,
  held slots, both races, faults and refusals; route, receiver and DMA register
  images for four further sizes match the pinned functions. Delivery stays
  exact only while consecutive DMA status reads lie within the status
  interval, which keeps a margin under half a frame period; after a longer
  gap the stream delivers nothing more until the owner stops it. The first
  interval spans the OV5640 stream-on sequence, so a slow start can make a
  stream deliver nothing while every slot stays owned
  ([camera capture](camera-capture.md)); whether a real start fits is
  unmeasured. Address latching at frame start and frame-done timing need a
  module; the native application does not stream yet. Live sensor frames do not reach ISP0: raw receiver input,
  parser-to-ISP0 routing and live ISP0 operation remain required.
  A separate [recorded-image ISP0 application](camera-isp0.md) now accepts one
  640×480 BGGR12 frame and produces an immutable copied NV16 frame through
  TOP's DDR reader, ISP600, VIPP0 and camera DMA0. It needs no sensor. Pinned
  GPL ISP600 defaults/setters run natively; no opaque tuning or runtime blob is
  required. Guarded mappings retain both IOMMU masters 1 and 2, every parameter
  extent, input/output buffers and provider leases through completion or ACK.
  Final image `32eb5b93…` has one nominal EL1h bounded-call result, seven classified
  failures and five serializer cases. Device effects and output pixels are modeled;
  the fixtures establish neither numerical processing nor full task scheduling.
  Old invalid negative runs are excluded. The [host workflow](isp0-preparation.md)
  passes fifteen stream cases, geometry/input/proof and six allocation refusals,
  plus readiness; seven root boot guards and three SRAM cases remain separate.
  Evidence is under `isp0-native-20261020/`. The only current compiled/declared
  source difference adds an independent CE clock writer without changing existing
  functions. No full TinyUSB/DCD, physical, numerical, sensor or scheduler
  qualification is established. Independent pixel comparison, wider processing,
  scaling, 3A tuning, interrupt completion and physical recovery remain required.

- NPU: The application clock admission now accepts the board's normalized
  reference and recorded PERI0 settings. Six focused final-image cases pass
  under `npu-single-core-20261018/reference-refresh/`; unchanged startup and
  inference evidence is reused. Physical NPU computation remains unobserved.
  The first physical attempt returned no USB result and required power removal.
  The application now preserves failure reporting over independently valid USB;
  ten affected native cases and source review pass under
  `npu-usb-terminal-20261018/`. Physical reporting and inference remain untested
  with the corrected image.
  Terminal CLOSE and authenticated local gate/reset shutdown now pass focused
  checks and Astra review. The finite inference adapter is integrated with
  retained session and complete memory cleanup in the returning platform.
  The optional single-core [interrupt profile](../apps/npu-interrupt/README.md)
  runs bootstrap and two inferences through deferred interrupt 97 service.
  Only the owner task acknowledges events and acquires output. Source-off CLOSE
  and verified GIC stop precede ownership release. Host success requires three
  notified completions, the stopped receipt and both copied numerical outputs.
  Nine service scenarios, controller/native-client/host checks and Astra review
  pass; eighteen root routing/dispatch/monitor instruction cases also pass.
  Integration matches all 184 compiled inputs and verifies 144 sources, 1,026
  artifacts and four tools under `npu-interrupt-service-20261019/root-integration.json`.
  The [retained runtime application](../apps/npu-runtime/README.md) now accepts
  repeated copied inputs for the same selected model. SUBMIT, request-bound READ,
  copied output and REAP preserve mailbox ownership through full USB delivery.
  Healthy CLOSE requires source-off/IRQ stop and local gate/reset shutdown;
  OS/USB remain live, with no ROM-return claim. Linked USB/mailbox/client, lifetime
  and host checks pass with service replies and output modeled under
  `npu-runtime-input-20261019/`. Root verifies 186 compiled inputs, 74 objects,
  297 sources and 329 artifacts against revision `5954081` under
  `dma-reconcile/root-integration.json`. Load-segment equivalence apart from build
  identity supports reuse of accepted checks; later shared-header changes do not
  invalidate this frozen candidate.
  Physical interrupt delivery, inference, numerical output, shutdown and ROM
  return remain unqualified. The integrated device quiescence path suspends
  workers, parks PMIC outside active ownership, verifies interrupt inactivity
  and enters a task critical section before exit. Six client and nineteen
  quiescence cases pass with Astra review; all 172 compiler inputs match.
  Evidence: `npu-finite-quiescence-20261018/`. The separate DCDC2 observation image has six
  passing linked cases and 40 matching compiled inputs under
  `npu-supply-returning-20261018/`. Physical inference and shutdown remain open.
  **Implemented** offline [page-table construction](npu-mmu.md), with host
  translation and permission checks. The [controller core](npu-controller.md)
  passes host startup, completion, cancellation, and retained-fault checks.
  The [memory policy](npu-memory.md) passes host integration for private mappings,
  table validation, cache publication, and retained faults.
  The [native CPU adapter](npu-native.md) passes a FreeRTOS emulator check for
  translation, cache maintenance, MMIO, deadlines, and retained failure.
  The [FreeRTOS service](npu-service.md) passes a two-core ownership check with
  EL0 clients and a preemptible EL1 worker.
  The pinned board feature database for target `0x1000003b`, revision `0x9202`,
  reports eight neural-network cores, zero tensor-processor (TP) cores and neural
  transpose support. An independent query of the configured compiler SDK reports
  zero TP/TP-lite cores and one shader core. These published/configured target
  records do not establish physical chip identity. Generic NBG tensor-processor
  records describe a file-format capability; they do not require a TP engine on
  this product. The fixed shader/neural-transpose graph below now supplies bounded native
  execution and external intermediate-memory support. Wider operator coverage
  remains required. Evidence:
  `npu-tp-proposal-20261019/target-feature-corroboration.json`,
  `sdk-feature-query.json` and the source anchors in `frozen/manifest.json`.
  The [model inspector](npu.md) passes a reference comparison and malformed-file
  checks. The [address encoder](nbg-relocation.md) passes host bounds checks and
  comparison with SDK relocation instructions. The [host loader reconstruction](nbg-loader-trace.md)
  matches complete descriptor and command buffers at two allocation bases.
  The [native container decoder](nbg-model.md) passes malformed-input checks
  and reconstructs saved SDK output through C records and the host assembler.
  The [native assembler](nbg-assembly.md) plans page-aligned buffers and matches
  the saved neural-operation submission after address and completion adaptation.
  [Complete-file integrity](nbg-checksum.md) passes two published model checks
  and is required before native assembly.
  [Encoded ring validation](nbg-rings.md) checks complete ranges and tensor
  membership, with isolated SDK packing and saved-submission comparisons.
  The [pinned host compiler](npu-compiler.md) produces a numerical convolution
  with matching CPU and simulator output. Native decoding now supports its
  directory format, and checksum verification passes host checks. A saved
  [SDK preparation trace](nbg-conv-loader.md) supplies allocation and command
  evidence now matched by native assembly with five buffers and an empty pool.
  The [NPU platform controller](npu-platform.md) verifies inherited parent clocks
  and leased supply, programs local clock/reset state, and powers domain 4.
  Source/model checks and native compilation pass. It preserves shared SRAM
  reset and requires exclusive ownership of that memory's existing users.
  The [startup adapter](npu-startup.md) binds shared clocks, stable DCDC2 supply
  ownership, native PMIC reads and deferred service activation.
  [Shared-provider admission](pmic-consumers.md) retains two actual observations
  around the required settling interval and checks their ownership before readiness.
  Focused native instruction checks pass with modeled scheduling and hardware.
  Earlier two-core checks pass with modeled device registers and real FreeRTOS locks; the central
  target links. Uncertain register operations retain mutex ownership before
  release. [Trusted convolution registration](npu-convolution.md) authenticates
  the complete compiled model, private allocations and commands before startup
  and publication. Copied inputs, matching output completion and explicit result
  reaping pass focused emulator checks. The output bytes are modeled, so these
  checks do not establish inference. Initial registration accepts three reviewed
  compiler artifacts with different tensor sizes, selected before scheduling
  and fixed for the context lifetime. The native application runs two successive
  jobs and publishes both completion identities and up to 400 result bytes.
  Two additional compiler/simulator cases match CPU references; focused native
  construction, result and failure checks pass with modeled device output under
  `npu-models-20261018/`. The software-validation image is not deployable.
  The [complete weight-bearing neural-network graph](npu-complete-nn.md) is implemented
  for the original published ShuffleNetV2 compiled artifact: 125 sequential NN
  operations with immutable coefficients and an external intermediate pool.
  The fixed selection accepts 150,528 UINT8 input bytes and returns two copied
  output bytes through the existing large-tensor owner protocol. One completion
  covers the complete graph; pool clearing, coefficient revalidation, complete
  output transmission and FINISH precede reuse. Two copied jobs and healthy CLOSE
  pass native allocation/table/assembly and host checks with NPU replies, IRQ,
  cache effects and transport modeled. Root verifies 189 inputs and 75 objects
  against revision `0dcf977`, with no source delta, under
  `npu-complete-nn-20261019/root-integration.json`. Its ELF SHA-256 is
  `5e578a3c8253fa00868a8d7cb7b9ca0f62a9091dfec38270feaf5be9af1ae9d1`.
  The SDK consumed two distinct input patterns, but both outputs saturated to
  `[0,255]`; class meanings are undocumented. This establishes fixed whole-graph
  software coverage, not input-sensitive classification, accuracy, physical
  inference or unchanged mimus acceleration. Broader operators, models,
  concurrency, physical confinement and numerical qualification remain required.
  The complete graph also shares providers and copied USB ownership with the
  GPU in the [combined application](accelerator-composition.md). Host preparation
  and copied capture are software-checked; physical execution remains unqualified.
  The [fixed FP32 relative-attention shift](npu-relative-shift.md) is implemented
  through one shader (SH) operation and three neural-transpose (NT) operations, with an
  external intermediate pool. Four heads and 128 steps use 522,240 input bytes
  and 262,144 output bytes. Strictly ordered chunked upload, copied output and
  completed-transmission acknowledgments precede mailbox reuse; healthy CLOSE
  retires the service while retaining allocations. The wrapper clears the entire
  external pool before every job under idle ownership and publishes it through
  the existing cache policy. Two inputs through one prepared SDK network match
  all reference FP32 bits. Linked native allocation, assembly, copied protocol
  and CLOSE checks pass with NPU replies, IRQ, cache effects and transport modeled.
  Root verifies 189 inputs and 75 objects under
  `npu-relative-shift-20261019/root-integration.json`; later COMB0 clock-source
  differences do not replace the consistent frozen inputs. This fixed graph adds
  native operator/memory coverage; it does not replace or accelerate the unchanged
  mimus encoder. Physical inference, confinement and larger shapes remain required.
  The [two-stage NPU graph](npu-two-stage.md) is implemented as another fixed
  catalogue selection. It consumes 64 UINT8 input bytes and produces eight output
  bytes through two convolution stages, each fused with ReLU. Intermediate data
  stays in internal SRAM; one completion covers the complete graph job. Independent
  CPU/simulator references and native construction checks pass. The linked runtime
  application handles two changing inputs, copied results, REAP and CLOSE with
  service responses and NPU output modeled. Root verifies 186 compiled inputs and
  74 objects under `npu-two-stage-20261019/root-integration.json` at integration
  `7cddb66`; the sole source difference is a verified API comment, requiring no
  rebuild. The frozen ELF is
  `651e1c7688c82bfc028ecdab6bca23d2d6d8a2305f3dd2c282ec36d036c76f4c`.
  Physical inference, SRAM confinement and interrupt delivery remain unqualified;
  this graph does not run or replace unchanged mimus.
  Arbitrary model uploads, runtime switching, other operators and mimus
  acceleration remain outstanding. The [native memory session](npu-reserved.md)
  now reserves one linker-owned allocation for all six buffers and private
  tables, then invokes the actual authenticated preparation path. Focused
  emulator checks execute native translation/cache operations and retain the
  parent allocation on failure. Missing startup authority still refuses
  service binding. The [NPU PLL controller](npu-pll.md)
  implements bounded cold enable and enabled retuning, consumer exclusion and
  retained faults. Host checks, source comparisons and native compilation pass.
  [Atomic startup adoption](npu-clock-handoff.md) transfers the PLL reservation
  directly into startup ownership; focused emulator checks pass without a
  release window. The [native convolution application](npu-application.md)
  connects cached startup, reserved memory, actual providers, the convolution
  client and checked USB results. Native linking and modeled application checks
  pass; modeled output does not establish numerical inference. The supervised
  image connects actual PMIC sampling, enabled-PLL retuning and startup token
  adoption; focused checks pass with modeled device responses. Its host path
  passes 33 packaging/capture cases and five frozen-input binding regressions.
  An explicit supervised mode now enables a disabled PLL while DRAM remains
  active and adopts a quiescent, already-powered NPU domain. Its first native
  link and focused startup checks pass; modeled startup reaches service
  activation without power-domain writes or changes to SRAM reset. Unsuitable
  supply voltage remains unsupported. Concrete boot
  authority, physical rail/PCK qualification, effective memory confinement,
  numerical execution and physical qualification remain outstanding.

  The independent single-core convolution image is also integrated. Eleven
  startup/provider cases, seven host cases and six unchanged SD/GPU preprocessing
  comparisons pass under `npu-single-core-20261018/`; focused review accepted
  the image and source closure. Physical inference and the remaining NPU modes
  remain unqualified. The software fixture package must never deploy.
  The [session runtime](npu-session.md) runs up to eight compiled models from a
  build-time catalogue as concurrent sessions of the shared service, with
  round-robin job starts and interrupt completion. Native assembly accepts one
  to eight UINT8, INT8 or INT16 inputs and outputs and the transpose steps
  around fully connected layers, and matches the pinned SDK loader for every
  catalogue graph. Sessions reserve memory from the platform pool by model
  size, which is DDR in the extended cached profile. QEMU checks with modeled
  device output pass for polled, interrupt, integrity-fault and 6.2 MiB
  reserved cases. No board image links the runtime. An image derived from the
  board image would construct a second GPU/PCK resource manager beside the
  board's, and its 2 MiB device pool is smaller than the 2,195,456 bytes that
  the three small catalogue models need. An image derived from the single-core
  NPU images needs their provider setup to select the session runtime
  ([remaining work](npu-session.md#remaining-work)). The NPU startup and
  clock-handoff QEMU models pass again now that their fixtures report the
  enabled, stable 24 MHz DCXO that the drivers require. A
  [mimus linear-layer study](npu-mimus-linear.md) finds the simulator's integer
  arithmetic exact but its INT16 output not bit-exact with mimus, and the
  compiler refuses the INT32 output that a bit-exact split would need. Physical
  execution remains unqualified.
- Video and image codecs: **implemented** [baseline JPEG preparation and execution control](video-codecs.md)
  for the documented YCbCr420 profile. Host checks, runtime instruction comparisons
  and native compilation pass. Identity admission precedes reset/engine setup;
  decoded output requires independent drain and cache synchronization. Part of
  the historical malformed-input evidence lacks preserved inputs and its runner.
  The [native platform binding](video-platform.md) now supplies register access,
  clock/domain/interrupt ownership and identity-specific reset preparation.
  Host checks and runtime instruction comparisons pass; native compilation
  succeeds. Cold startup now validates an immutable ROM profile, sequences
  domain/reset/gates and performs the source-defined VCU reset before identity
  admission. Host checks and native compilation pass. The [native JPEG application](jpeg-application.md)
  connects shared providers, decoder IOMMU mappings, guarded buffers and copied
  USB output. Its central image builds; focused checks cover exact reference
  comparison, guards, faults, admission and framing with modeled hardware.
  A nonconstant diagnostic now has an exact CPU-decoded reference and integrated
  native copy/layout and host checks. Its sparse-coefficient oracle does not
  establish general JPEG numerical conformance. Physical decoding remains untested.
  Runtime JPEG upload now accepts supplied baseline YCbCr420 images within
  bounded input and padded-plane capacities, authenticates them before decoder
  acquisition, and returns copied pixels with parsed dimensions. Optional
  independent reference pixels support qualification. Partial-block geometry,
  immutable input, native copy and fragmented host capture checks pass under
  `jpeg-runtime-input-20261018/`. The decode application now accepts up to
  512 KiB of JPEG input and padded luma planes up to 640×480, each buffer
  between guard pages at a fixed device address. Full HD needs about 5 MiB of
  uncached memory, which the image does not link. A version 3 session decodes a manifest of up
  to eight images in order, refilling the retained buffers after each delivered
  healthy result; sanitized input and linked-image checks with modeled device
  effects pass. The supervised capture tool and bundle still carry one image.
  Repeated encoding and physical conformance remain outstanding.
  The complete JPEG session is now copied and verified against its admitted
  digest before provider startup. USB failure revokes board liveness before
  parking. The affected native link and focused session/failure checks pass;
  prior decoder and numerical evidence was reused.
  The [High-profile H.264 build](h264-native.md#high-profile-runtime-clips-and-presentation-ordering)
  is implemented for progressive Baseline, Main and High profile streams, 8-bit
  4:2:0, with CABAC or CAVLC, scaling matrices, weighted prediction, I/P/B
  pictures including reference B pictures, and long-term references in P pictures.
  Fixed storage now holds four full-HD picture slots, which admit three
  short-term references at full HD, or four at HD, with at most four active
  entries per list. Each coded picture may use up to 2 MiB of device input, and
  the pool stays inside the shared 16 MiB uncached reservation. The parser,
  controller, layout and host limits use these capacities, and the bounds check
  admits a three-reference full-HD clip in host models. The frozen evidence
  described next records the earlier two-reference full-HD layout; the current
  layout needs a new evidence freeze. Authenticated 128-byte picture descriptors
  bind decode/presentation order; five copied-output slots support reorder and
  ACK backpressure. Copy acknowledgment, reference retirement and healthy input
  refill remain separate. Uncertain completion retains all allocations.
  Native compositions complete twelve full-HD, twelve reference-B HD and twelve
  four-reference HD pictures; normal mode decodes three and copies two.
  FFmpeg-derived register oracles and host capture pass. Device pixels, MMIO,
  readiness, translation/fault scans and USB are modeled; shared startup is not
  executed. Root verifies 462 inputs and 175 objects at revision `32826e2`, with
  no compiled-source divergence, under `h264-high-20261019/root-integration.json`.
  Its unchanged ELF is `ecd05003…`; `closure.json` and `root/layout-review.json`
  preserve the accepted case and static layout boundaries.
  Selected GStreamer 1.26.0 parser/DPB routines, pinned at
  `d31ce8e5e1aacf8f5e5beabb5c81ce2e4da5c202`, execute as adapted FreeRTOS CPU code;
  no GStreamer/GLib/V4L2/Linux runtime is linked. `apps/h264/sources.json` and
  `apps/h264/LICENSE.gstreamer` preserve upstream LGPL and Chromium BSD notices.
  The original build-identity manifest remains in
  `h264-high-20261019/compiled/build-identity-sources.json`; final provenance-only
  edits require no rebuild and extend no execution claim. Physical decoding,
  cache/reference visibility, confinement, timing and drain remain unqualified.
  Header-rewritten clips exercise long-term marking, memory-management operations
  1 to 6, POC and frame_num wraparound and Baseline/Main streams through an
  independent FFmpeg-syntax model and the linked image with modeled device effects;
  B pictures with active long-term references are refused because the engine is
  not told which references are long-term. Long-term B references, unrestricted
  streaming, broader formats and broader encoding remain required. Baseline
  builds retain their separate scopes.
  The separate [full-HD H.264 profile](h264-native.md#full-hd-streaming-and-extended-memory)
  is implemented for progressive Baseline IDR/P clips with one short-term reference
  and picture-order count type 2. Its coded 1920-by-1088 references retain all rows;
  copied output removes exactly eight bottom rows for visible 1920-by-1080 YU12.
  The authenticated clip admits at most 256 access units, each bounded to 4 MiB.
  Fixed extended-memory allocations retain input, references, scratch and two
  private copied-output slots. Healthy input refill, reference retirement and
  completed-output acknowledgment remain separate; uncertainty retains every lease.
  Six-picture checks execute the actual allocator, tables, parser, controller,
  cropped copy and serializer. Independent syntax/register comparisons, private
  launcher/sealer and host capture checks pass. Device pixels, registers,
  translation, scheduling and transport are modeled; normal-mode evidence checks
  one copied picture without numerical qualification. Shared startup and secondary
  execution are not exercised. Root verifies 434 inputs and 171 objects under
  `h264-fullhd-20261019/root-integration.json`, with private startup placement review
  preserved separately. Fourteen later shared USB/clock/board source differences
  are classified; the consistent frozen image remains authoritative, and future
  builds must compile affected consumers together. The full-HD ELF SHA-256 is
  `b48123e301ec40ae4ec104e58d4c57b347e619b6a0a62a82263e0d0fe491a643`.
  Physical startup, pixels, reference visibility, confinement, timing and drain
  remain unqualified. Wider profiles/reference patterns, unmanifested input and
  allocation reclamation remain required. Existing VGA and small-picture profiles
  retain their separate accepted images and evidence.
  The [VGA H.264 streaming application](h264-native.md) implements a finite
  prepared clip of up to 256 progressive 640-by-480 Baseline IDR/P pictures, with
  one short-term reference and at most 512 KiB per access unit. A fixed retained
  pool alternates decoder reference/output slots across GOPs. Indexed copied
  outputs require complete transmission and exact acknowledgment before their
  slots can be reused;
  uncertain completion retains every allocation. The selected ELF SHA-256 is
  `24144b2acd358a8a905f76ebc94119274589bce0a5a883992082ad20ca52ddb4`.
  It includes the decoded-macroblock counter correction and actual lower-native
  IOMMU scan/poll policy checks. Parser, independent syntax/register comparisons,
  linked refill/copy and host checks pass with device effects and pixels modeled.
  CPU translation and initial platform/IOMMU authority are also modeled.
  Root verifies 429 compiled inputs and 169 objects under
  `h264-vga-20261019/root-integration.json`. Normal-mode native evidence covers
  only the first frame with terminal status still pending.
  Newer shared USB/board layouts and memory/clock changes differ; the internally
  consistent frozen image supplies no current-tree rebuild evidence. Future
  builds must compile all affected consumers consistently.
  The launcher correction at `a939b25` accepts producer-built legacy version-1
  and VGA version-2 sessions through the actual generated AArch32 instructions
  to the modeled reset handoff. Unsupported version 3 and the preserved old
  launcher with a VGA session refuse with reason 14, restore entry state and
  perform no hardware writes. Earlier bundle checks substituted launcher creation
  and missed that guard. Evidence: `h264-launcher-20261019/result.json` and
  `freeze/manifest.json`. The native ELF above is unchanged and was not rebuilt.
  Prepared VGA bundles must use this corrected launcher; older frozen bundles
  remain incompatible. Root verifies 15 compiled launcher inputs and 69 frozen
  files under `h264-launcher-20261019/root-integration.json`. The later board
  workload-6 preparation branch does not change private H.264 launcher construction.
  System registers, counter values and MMIO are modeled; these checks establish
  neither AArch64 startup nor a physical launch. Physical preparation and recovery
  remain required.
  The actual shared supervised runner now accepts the H.264 caller's
  `h264_result` policy with a 59-second capture bound. The entry check reaches the
  readiness callback; 60 seconds and an unknown result key are refused. Evidence:
  `h264-supervisor-entry-20261019/result.json` and `frozen/manifest.json`.
  The sentinel stops before queue checks, evidence creation or USB access; it
  establishes no transport, native execution or physical launch.
  Physical reference visibility, pixels, confinement, timing and fault drain remain
  unqualified. Allocation reclamation, unmanifested live input, wider profiles,
  multiple references/slices and other required codec modes remain outstanding.
  The legacy small-picture profile remains implemented. Historical ELF
  `862fae444a481eb9505d8ba7ef3835760e208764663c1e7aef8459f8b9be43ac`
  omits the counter-read permission and cannot publish completed output; its
  substituted-read compositions retain only their historical modeled-path scope.
  The [shared clock policy](clock-leases.md) reserves exclusive encoder scope
  bit 71 and excludes encoder ownership during generic and private JPEG/H.264/HDMI
  IOMMU preparation. A focused registry/media ownership case passes with modeled
  callbacks; two core and three application units compile for AArch64. Evidence
  is under `encoder-clock-scope-20261020/`. These checks establish neither a linked
  encoder application nor encoder initialization, DMA completion or physical
  operation. Earlier frozen images retain their own clock policies.
  The [native JPEG encoder](jpeg-encode-native.md) implements one retained
  caller-supplied 640×480 NV12 job, stride 640, quality 75, with at most 1 MiB
  of copied baseline JPEG output. It adopts inherited powered PCK3 state,
  reserves exclusive encoder ownership and maps IOMMU master 3. Published opaque
  libraries provide development-only instruction references; no such library
  is linked or loaded natively. Actual provider, lower-MMIO, IOMMU and copy checks
  pass with completion and entropy modeled. The complete native image is linked,
  not executed; JPEG numerical correctness, burst confinement, visibility, drain
  and physical operation remain unqualified. Buffers, mappings and leases remain
  retained; no repeated-job or release claim is made.
  Root verifies 447 compiled inputs, 169 objects and the frozen snapshots under
  `jpeg-encode-native-20261020/root/integration.json`. Eight current input deltas
  belong to inactive radio/USB-host functions; frozen workload 2 uses private
  USB0 DEVICE output and does not establish those newer functions. Broader
  dimensions, formats, qualities and broader encoding remain required.
  The [native H.264 encoder](h264-encode-native.md) implements one retained
  sequence of 1 to 30 pictures from two caller-supplied 640×480 NV12 frames;
  picture k encodes frame k mod 2. The sequence is Baseline level 3.1, CAVLC,
  fixed QP 26, and one IDR picture followed by P pictures, each referencing the
  previous reconstructed picture. Each picture starts in the 4 MiB bitstream
  ring after the previous picture, whose bytes are copied first, and returns to
  the ring start before the device would need to wrap. A copied output slot is
  reused only after the host acknowledges the picture two earlier. Actual
  provider, lower-MMIO, IOMMU, copied output and USB durability/ACK checks pass
  with entropy, completion and resource observations modeled. All resources
  remain retained. For 30 pictures, the 2,128 native programming writes and
  every CPU-written slice header match a run of the pinned opaque A733 SDK
  library; no opaque CPU library is linked or loaded natively. The
  [source record](../apps/h264-encode/sources.json) pins that development-only
  dependency and its notices. The frozen evidence covers the earlier
  two-picture image `2e4a93b0…`, whose 140 programming writes match the same
  library: root verifies 2,328 frozen files, 170 objects and 451 compiled
  inputs, including all 185 repository inputs, under
  `h264-encode-20261020/root/integration.json`. A two-picture run of the
  current image programs the same writes and produces the same outputs; the
  current image is not frozen, and the supervised capture tool still captures
  two pictures. Real entropy, reconstructed-reference contents, numerical
  encoding, DMA visibility, confinement and safe drain remain unqualified.
  Wider dimensions and formats, sequences longer than 30 pictures, a new frame
  for each picture, rate control, High/CABAC/B pictures, multiple references,
  unrestricted streaming and H.265 encoding remain required.
  [HEVC decoding](hevc-native.md) is **implemented** for one uploaded
  two-picture clip: progressive Main profile, 8-bit 4:2:0, 640×480, one IDR
  and one P picture, with copied NV12 output over USB0. The reviewed parser,
  controller and storage build as `sunstone-a733-hevc` on a typed video-engine
  binding with its own H.265 register allowlist. Host models check parsing,
  storage and register programming; for the IDR picture, the final values of
  22 registers and the write order match a trace of the A733 vendor library.
  GPL Cedrus control and LGPL GStreamer parsing are the native dependencies
  ([source record](../apps/hevc/sources.json)). Host preparation refuses a
  launch bundle until physical allocation and page-table evidence exists, and
  no vendor trace covers the P picture's control bits. No numerical or
  physical decode result exists. Main10, larger pictures, several slices,
  tiles, wavefront parallel processing and B pictures remain required.
  The attached board ROM profile, actual memory/cache/drain authority, physical
  decoding and remaining codec modes remain outstanding.

- DI301 deinterlacer: **implemented** retained [offline WEAVE, initial motion and fixed or selected-strength TNR](deinterlace-native.md)
  for caller 640×480 NV12 combined-field frames, top field first and 640-byte
  strides. Pinned GPL DI301 configuration from Linux revision `2ac08e8…` runs
  against a private CPU register image; whitelisted masked writes configure the
  live controller. No opaque DI runtime or Linux kernel executes on the board.
  Existing clock/power/PMIC providers, guarded master-9 IOMMU mappings and copied
  USB output preserve exclusive ownership. Matched finish/nonbusy, IOMMU and
  guard checks precede immutable output publication. Delivery-gated ACK permits
  neither reuse nor another job; all resources remain retained.
  The earlier WEAVE image passes thirteen linked native cases, one actual full-speed
  TinyUSB case and separate boot/SRAM checks, with device, scheduling and
  memory-authority effects modeled.
  The [host path](deinterlace-preparation.md) passes thirteen capture cases,
  five allocation refusals and supervisor readiness without device access.
  Root accepts frozen image `a472162b…`, 172 objects and 460 compiled inputs at
  `81e55ee` under `deinterlace-native-20261020/root-integration.json`.
  The only native snapshot difference is completed host-evidence documentation;
  all nineteen host sources match. Physical processing, visibility, confinement,
  USB, power and recovery remain unqualified. Finish is not independent fabric
  drain. Mode 2 adds one initial motion operation with two authenticated previous/
  current frames and zero history, producing one copied frame. Dedicated DI reset
  must be asserted with local bus/module gates off before the source-defined
  initialization sequence; no confidence defaults are invented from CPU-shadow zeros.
  Motion image `2cff56ab…` passes one native operation, six refusals and copied
  serializer/ACK checks, plus scoped host allocation/input/result/capture checks.
  Synthetic device writes establish neither motion arithmetic nor physical reset,
  DMA visibility or numerical output. Two new SRAM cases are separate; earlier
  boot cases reuse identical allocated sections/relocations within their old scope.
  Root verifies all 185 compiled repository inputs at `3a1f219` under
  `deinterlace-motion-20261020/root-integration.json`; declared differences affect
  future metadata wording and final host documentation only. The retained
  descriptive WEAVE label does not replace mode-2 session/geometry admission.
  Fixed-strength mode 3 now consumes two 640×480 NV12 frames and copies one
  460,800-byte planar YUV420 output. Each zeroed flag buffer is 307,200 bytes,
  with separate guarded master-9 mappings. Reset-held/gates-off entry precedes
  one initial operation with source-fixed strength and no adaptive feedback. Actual compiled source-plan,
  allocation/mapping, resumed completion, flag-boundary refusal and serializer/ACK
  checks pass; host capture, five admission refusals and two SRAM cases pass.
  Completion and pixel data, services, cache/translation, authority and timing
  remain modeled, without numerical filtering or physical qualification.
  The first allocation fixture omitted its continuation checkpoint. One affected
  allocation replay after the serialization repair reproduced proof/table bytes
  identically; completion resumed the saved full state without further allocation
  or provider replay. One successful native image and one launcher are preserved.
  Root matches all 185 compiled repository inputs under
  `deinterlace-tnr-20261020/root-integration.json`; three external host-file differences
  are separately frozen, with all nine final host sources matching. Earlier
  WEAVE/motion/power/USB matrices retain their original scopes without replay.
  TNR strength 1–3 now selects the pinned adaptive mode's LOW, MIDDLE or HIGH initial
  gain (38, 48 or 62) for the one operation; strength 0 keeps the fixed 58. Image
  `9a4c1c38…` reports geometry revision 4 and passes host and linked-image plan
  generation, host predicate and capture-label checks and a rebuilt SRAM launcher
  model. Gain feedback between frames, other shapes/formats, film detection,
  crop/scale and repeated jobs remain required. All buffers/providers stay retained; no drain,
  reclamation or physical reuse is qualified.

- Crypto, random, identity and secure storage: **implemented** bounded
  [hardware random-number requests](entropy.md) through the native crypto engine.
  The service provisions supplied health thresholds and returns complete
  1–256-byte results using one retained linker-pool page. Host checks, source
  comparisons, native compilation and a relocatable composition link pass.
  Output remains unqualified for production security use: raw-source assessment,
  an assessed profile and current-epoch startup testing are unresolved. Secure
  access, physical operation and independent engine drain remain outstanding.
  [SHA-256 streaming](crypto-hash.md) shares the same retained controller and page
  with entropy. Host checks compare modeled results with independent digests;
  source/native checks and a relocatable composition link pass. SHA-only sessions
  require no entropy-quality assertion. The same streams provide SHA-1, SHA-224,
  SHA-384, SHA-512 and HMAC over all five hashes on the engine.
  [AES transforms](crypto-cipher.md) use the same retained session for
  128/192/256-bit keys in ECB, CBC, CTR and GCM, with private output staging,
  software IV chaining, software GHASH, constant-time tag verification and
  successful-task cleanup of key, IV and counter lines. Host models compare
  every mode and hash with NIST vectors, OpenSSL and Python `hashlib`/`hmac`;
  the board image builds. A host calculator derives SP 800-90B health cutoffs
  from an assessed entropy, which still does not exist. Physical hashing/cipher
  operation, the engine's other modes and native GCM, MD5/SM3, asymmetric
  operations, hardware key selection and key ladders remain outstanding.
  Internal hardware key erasure is unproven.
  A boot-CPU-only SHA/AES application is integrated under
  `crypto-single-core-20261018/`, with twelve startup groups and six host groups
  passing. This permits independent device validation; physical CE operation and
  entropy quality remain unqualified. The image now builds from the
  [shared single-core recipe](single-core-images.md) with unchanged object
  code, and the central build configures `SUNSTONE_A733_CRYPTO` again.
  The [native SHA/AES application](crypto-application.md) now connects actual
  shared providers, the singleton retained page and copied USB results. Its
  native link and focused admission, orchestration, publication and host checks
  pass; all 223 preserved repository inputs match integration. A read-only board
  snapshot confirms the adopted CE clock configuration. Actual predecessor task
  accounting, future launch exclusion, secure status access and memory admission
  remain required before physical operation. No TRNG capability is claimed by
  this application. Startup now binds the private session to the admitted digest;
  USB failure revokes board liveness before parking. The affected image and
  focused binding/failure checks pass without repeating arithmetic checks.
  Caller-supplied SHA-256 and AES-128 ECB requests are now integrated, with private
  authenticated input, deferred engine construction and request-bound copied
  output. Focused native, sanitizer, admission and host checks pass under
  `crypto-runtime-input-20261018/`. Messages are bounded to 2048 bytes; AES requires
  a positive multiple of 16 bytes. Each retained session accepts one request.
  Optional output references do not replace physical numerical qualification.
  [Fixed cold CE provisioning](crypto-application.md#fixed-cold-clock-provisioning-for-caller-requests)
  now supports version-3 caller sessions. After request authentication, it admits
  independently excluded, reset-held/gated CE state, preserves neighboring MBUS
  gates and uses retained parent/CE ownership to program the fixed CE400 profile.
  A restricted writer verifies each transition; uncertainty retains ownership
  without retry or rollback. Versions 1/2 keep their inherited-clock behavior.
  Eighteen owner cases execute the linked provider, writer, registry, allocator
  and existing SHA client; the SHA device result and RTOS/device effects are
  modeled. Two root SRAM cases have separate scope. Root's
  `ce-clock-20261020/root-integration.json` verifies all 179 compiled repository
  inputs and declared native/operation sources at `8a7aedb`; one image was built.
  Unchanged arithmetic evidence is reused. Physical prior-DMA exclusion, secure
  access, timing, clock rate and cryptographic operation remain unqualified;
  reset-held readback establishes no drain or production authority.

- USB0 device channel: **tested** at high speed for status, packet-boundary
  echoes, logs, and commanded ROM return. [USB evidence](usb.md) records the
  programmed-I/O implementation, inherited clock state, and remaining modes.
  The [native mimus session](mimus-usb.md) also passes asset loading, status polling
  during recognition, repeated results, and host-lease return with automatic access.
  The [combined keyboard/recognition/storage application](../apps/mimus-board/README.md)
  repeats workload-4 recorded recognition within the retained session. Every
  generation requires a fresh neutral→unmodified Enter trigger and a fresh
  generation-bound sector-zero copy while recognition is active. One privileged
  USB2 owner handles HID/MSC; EL0 consumes only copied mailboxes. Models and
  weights remain unchanged. Transcript, sector and trigger evidence remain
  immutable until all exports and the generation-matched ACK response complete
  USB delivery. Only then can the recognition task advance and rearm; held Enter,
  stale ACK or an earlier storage read cannot satisfy the next generation.
  Generation stops before wrapping past `UINT32_MAX`; the original absolute
  session deadline still bounds reuse. Workload 4 uses version-3 result frames;
  workload 0 retains version-1 one-shot behavior. Root verifies 1,335 frozen files, 203 objects, six archives and 748
  compiled inputs, including all 179 repository inputs, under
  `mimus-repeated-board-20261020/root/integration.json` and `root-frozen/manifest.json`.
  Final image `05c4b4d9…` was built once; unchanged arithmetic retains earlier
  model parity. Lifecycle, recognition, transport, hardware and scheduling are
  modeled. Physical startup, concurrent operation, visibility, recovery,
  timing and performance remain unqualified. All memory remains retained; keyboard/hub/MSC
  and image-bound electrical/DDR/model-memory admission are still required.
  No desktop keyboard events are sent.
  The [combined coordinator](../tools/check-a733-mimus-board-supervised.py)
  validates the outer prepared bundle and supplies exact frozen assets through
  the shared sealed-source/readback launcher. Workload 4 captures 1–32 generations
  (default one); workload 0 remains one-shot. All generations share one 59-second
  launch/capture deadline within the native 60-second session. Raw fragments,
  complete frames, transcript, sector and trigger evidence are durable before
  ACK. Failure preserves earlier outputs and observed ACK evidence; no retry or
  relaunch follows. Host fixtures and actual supervisor readiness checks pass;
  the final raw-ACK-fragment persistence-failure branch is source-reviewed.
  Root verifies 1,102 frozen files and 16 current sources under
  `mimus-board-supervised-host-20261020/root-integration.json`. Native image
  `05c4b4d9…` is unchanged; no native execution, model-parity replay or physical
  capture is established by these host checks.
  [Cold USB0 preparation](usb-platform.md) now supplies native clock/reset/PHY
  sequencing, detached-controller admission and shared-reference publication.
  Host checks, source comparisons and native instruction checks pass. It keeps
  the device disconnected and does not enable DMA or host power. Actual
  fabric/power/security/recovery admission, connection, USB1 and USB2/DWC3 modes
  and physical cold-start qualification remain outstanding.
  A finite composition with actual PMIC/USB tasks now passes fifteen native
  and twenty-three host cases under `usb-returning-20261018/`. It requires a
  complete 40-byte response, parks PMIC activity, quiesces USB and returns through
  the matched ROM initialization path. All 65 newly compiled inputs match
  integration. Thirteen additional cases pass for BUSY handling and preservation
  of the first preparation fault; all consumers of the enlarged structures were
  rebuilt. A physical attempt with that report returns native status 1 before
  any USB-controller writes. It rejects the inherited value `0x29940` at
  `0x04100040` under mask `0x700`. All four PMIC snapshots matched and were reaped;
  no USB exchange began. Source investigation identifies a device-session bit
  rejected by the earlier guard. The integrated correction allows device SESSION
  while requiring device role, excluding host request/mode and retaining DMA
  checks. Affected checks and focused review pass under `usb-rom-session-20261018/`;
  physical takeover and quiescence remain unqualified.
  Evidence: `usb-first-fault-20261018/physical-064/runtime/`. User-confirmed power
  removal recovered the board, and the retained processes and failed claim are
  closed. This does not qualify the native USB composition.

- USB host and Type-C: **implemented** native [source power and attachment
  control](typec.md), plus [high-speed USB2 host transfers](usb2-host.md) through
  xHCI and TinyUSB. Focused checks cover attachment, independent power cutoff,
  mass-storage enumeration, copied sector reads, malformed responses,
  cancellation and disconnects. The [native board application](board-application.md)
  connects startup, separate Type-C tasks, host reads and simultaneous USB0
  output. A supervised mode retains resources until manual power removal;
  USB0-only and USB0-with-radio selections can run without a powered host
  accessory. Its central build passes, and 25 host cases cover capture,
  refusal and retained ownership. Native HID interrupt-IN input now uses the
  actual TinyUSB class at low, full and high speed. The board application copies
  four reports to USB0 output without sending desktop input events. Sanitizer,
  native consumer/serializer and host checks pass. The original polling HID
  image's audit matched 408 compiled inputs under
  `usb-host-interrupt-20261018/root-integration.json`.
  Optional supervised board CPU interrupt completion now routes INTID 187 to
  CPU0 and wakes the existing HID/MSC owner task. The ISR masks and notifies;
  only the owner acknowledges/drains events and checks empty/race state before
  rearming. Other profiles retain polling. Eighteen root routing/dispatch cases,
  ten composed device cases and owner/native-capture checks pass with review.
  Integration verifies 413 compiled inputs, 161 objects, 32 sources, 59 artifacts
  and three tools under `usb-host-cpu-irq-20261019/root-integration.json`.
  All selected consumers use the changed xHCI layouts consistently. Earlier SD,
  UART and other images retain their own frozen input/layout authority; their
  audits do not establish matches at future revisions or require catch-up builds.
  Supervised board workload 4 now supports one self-powered high-speed USB2 hub
  with one low/full/high-speed HID and one high-speed MSC device simultaneously.
  The actual client/serializer/host copies four HID reports and one read-only
  sector into a joint payload, immutable through transmission. Both results
  and fresh attachment authority are required; partial failure cannot publish
  joint success. Ten focused component cases, two affected single-result branches
  and native client/serializer/host checks pass with review. Root's selector-4
  route case passes; the earlier eighteen cases are reused. Existing CPU0 IRQ187
  service is unchanged. Only the supervised board target admits workload 4;
  derived profiles retain workloads 0–3. Root verifies 414 inputs, 162 objects,
  23 sources, 82 artifacts and three tools against revision `1bb08d5` under
  `usb-host-dual-leaf-20261019/root-integration.json`. Final runtime bytes match
  the accepted route-case image apart from build identity, so checks were reused.
  Earlier images retain their own frozen input/layout authority. Type-C VBUS
  cutoff does not remove external hub power.
  The [SuperSpeed Gen1 storage profile](usb2-host.md#one-direct-gen1-storage-device-uses-comb0)
  is implemented for one BOT leaf, directly or through the bounded hub below. Supervised workload 6 uses
  the source-backed COMB0 PHY path, actual TinyUSB enumeration/MSC and xHCI contexts,
  then copies one read-only LBA 0 sector through USB0. It refuses USB2 fallback.
  Twenty focused compositions, two affected USB2 cases, linked publication/host
  checks and focused review pass with hardware effects modeled. Root verifies
  435 compiled inputs and 163 objects under
  `usb-host-superspeed-20261019/root-integration.json`;
  the original frozen inputs remain authoritative across later memory-provider
  changes. Admission requires an authenticated source of at least 900 mA and
  image-bound physical COMB0 route, rail, clock and source-switch evidence. No such
  physical admission record exists. Physical link operation, source capacity,
  DMA, interrupt delivery and recovery remain unqualified. Wider HID profiles,
  Gen2, streams, wider classes and drain remain required.
  [Direct Gen1 keyboard reports](usb2-host.md#direct-gen1-keyboard-reports-use-an-explicit-workload)
  are implemented under explicit supervised workload 8. One boot keyboard with
  a zero-burst companion and 1–64-byte packets supplies four copied raw reports;
  fresh ownership precedes publication. High-speed fallback, malformed companions
  and uncertain completions refuse success. Nine actual TinyUSB/xHCI/provider
  cases, linked native client/serializer/host checks and workload/route admission
  checks pass with device, RTOS and interrupt effects modeled. The native image
  is linked, not executed. Preparation requires an immutable ELF capability plus
  image-bound keyboard/cable and ≥900 mA source admission; no physical record
  exists. Mimus keyboard profiles remain USB2. Evidence and exact scope are in
  `usb-ss-hid-20261020/handoff.md`, `root/identity.json` and
  `root-route/route-ss-hid.json`. Root verifies 436 compiled inputs and 163
  objects at `214f3fe` under `usb-ss-hid-20261020/root/integration.json`.
  [Bounded hub and companion-root operation](usb2-host.md#one-gen1-hub-or-the-usb2-companion-root-serves-one-leaf)
  supports direct attachment or one self-powered hub with one selected leaf.
  Workloads 6/8 select Gen1 MSC/boot keyboard; workloads 10/12 select a USB2
  companion-root boot keyboard/high-speed MSC. Only one root enumerates. The
  592-byte session binds typed `usb_topology` at offset 404: zero for direct,
  one for a single hub. Actual TinyUSB classes/controller/provider and linked
  copied-client/host checks pass with hardware, RTOS and IRQ effects modeled.
  Six root identity/layout cases and two new route cases pass. Root verifies
  2,255 frozen files, 163 objects and 436 compiled inputs under
  `usb-ss-hub-20261020/root/integration.json`. The native image is linked, not
  physically executed. Admission binds root, topology and class, a ≥900 mA source,
  and actual hub supply/downstream power; no physical bundle exists. Physical
  topology, DMA visibility, IRQ delivery and recovery remain unqualified. More
  than two hub tiers, simultaneous roots, additional leaves and broader USB
  modes remain required.
  GET_MAX_LUN protocol-stall recovery now resets endpoint zero and sets its
  dequeue pointer before notifying TinyUSB, which falls back to LUN 0 and
  completes the copied READ10 path. Eleven affected high-speed TinyUSB/provider
  cases pass with device effects modeled; SuperSpeed recovery is source-reviewed
  only. Other control stalls, bulk clear-halt and uncertain recovery retain
  resources. The native image was compiled, not executed. Its integration audit
  verifies 436 dependencies and 163 objects at `ad1f680` under
  `usb-msc-stall-20261020/root-integration.json`. Frozen image `70e32112…`
  retains the older clock-scope header; it links no encoder or media-IOMMU
  preparation and does not establish the newer encoder exclusion.
  Detach and reattach are **implemented** in the xHCI core. A root-port
  disconnect or hub port change sends REMOVE, stops or resets each endpoint and
  issues Disable Slot, children before their hub. Only the Disable Slot
  completion clears the slot's contexts, rings and bounce pages, after which the
  port can enumerate a new device on the same pages. A transaction error holds
  its endpoint for at most 1 s. A modeled controller passes root and hub-child
  detach followed by a second enumeration and bulk read. The TinyUSB adapter
  uses this path. An orderly disconnect behind an adapter or hub, or a hub
  child's disconnect, unmounts the device, and a new device may then mount
  within the startup budget; a single-device client repeats a request it has
  not yet published. Unplugging a device directly from the Type-C connector,
  or a bulk stall, still ends the session, because adoption of a new Type-C
  attachment and Bulk-Only Transport reset recovery are not implemented.
  Three admission options are **implemented** for a session: a second tier of
  high-speed hubs, bus-powered hubs within the USB 2.0 current budget, and
  capture from one USB Audio Class 1 microphone over one isochronous IN stream
  in the xHCI core. A ring-level controller model checks them. No board
  workload selects them, and no check covers isochronous audio with interrupt
  completion. Full startup, physical power/switch
  qualification, actual transfers and report timing, DMA drain, CPU interrupt
  delivery, repeated physical attachment, more leaves, wider classes,
  isochronous OUT, wider SuperSpeed modes, role swaps, USB PD, VCONN and
  applicable alternate modes remain outstanding.

| Hardware | Required functions and application interface | Dependencies and physical checks |
| --- | --- | --- |
| Boot ROM and boot firmware | Recovery, image loading, repeatable boot | Confirm SoC identity, DDR firmware, entry contract, storage preservation |
| CPU cores | AArch64 tasks on all six A55 and two A76 cores; floating-point/vector context | Secondary startup, coherent memory, interrupt routing, affinity |
| DDR and CPU memory management | Usable RAM, permissions, cache policy, allocation | Discover actual RAM; reserve boot firmware; test address boundaries |
| Interrupt controller and timers | Interrupt dispatch, tick, monotonic time, deadlines | GICv3 distributor/redistributors; per-core timers |
| Clocks, resets, regulators, power domains | Dependency-aware enable, disable, rate control, power transitions | Confirm power ICs and safe board operating points |
| Pin control and GPIO | Pin functions, levels, pulls, interrupts | Confirm header routing, voltage domains, shared-pin exclusions |
| DMA and device address translation | Transfers, mapped buffers, cache synchronization, completion, recovery | Address widths and transfers above 4 GiB, alignment, ownership, IOMMU faults |
| UART | Console and exposed serial channels | Pin routing, baud rates, flow control, receive overrun |
| I2C | Transactions and recovery on exposed and internal buses | Clock dependencies, power devices, sensor control |
| SPI | Transfers and exposed controller modes | Chip selects, DMA, shared pin functions |
| PWM and ADC | Applicable waveform and sampling interfaces | Routed channels, clocking, signal verification |
| SD, eMMC, UFS, SPI storage | Block access and recovery where populated or exposed; eMMC and UFS media absent on both current boards | Preserve the existing contents of both microSD cards and follow the write targets in goal.md section 4; qualify SD and applicable SPI storage, boot conflicts, voltage, capacity and power-loss behavior |
| USB controllers and PHYs | Host/device transfers and supported speeds; console and HID classes | Confirm physical ports, role switching, disconnect recovery |
| Type-C and display multiplexing | Role/orientation and applicable display alternate modes | Confirm controller, wiring, shared PHY modes |
| Wi-Fi | Firmware transport, scan, association, supported connection modes, packet interface | Confirm radio variant; SDIO/USB wiring; coexistence and reconnect |
| Bluetooth controller and profiles | Bonds, BLE HID, hands-free audio gateway and voice packets | Firmware, HCI transport, SCO/eSCO routing, codec support |
| Audio controllers | I2S input/output and other routed audio functions; timestamped PCM | No microphone present; schematic routes I2S0 to the header and I2S1 PCM to Bluetooth; qualify both |
| CSI receivers and PHYs | Camera capture, modes, synchronization, errors, frame buffers | Schematic routes four lanes each to CAM1/MCSIA and CAM2/MCSIB; qualify sensors, DMA, and clocks |
| OV5640 | Sensor control, supported MIPI modes, exposure and frame delivery | No module connected; confirm wiring when available; upstream sensor reference |
| Image processing | Pixel conversion, ISP controls, scaling, exposure/white-balance integration | Sensor tuning, buffer formats, documented dimensions |
| Display engine | Scanout, composition, synchronization and supported formats | Buffer sharing, display clocks, bandwidth |
| HDMI | Output modes, detection, applicable audio | Monitor and cable; controller and PHY |
| MIPI DSI | Host operation, panel commands, scanout, backlight integration | Schematic routes four DSI1 lanes to LCD; no display connected, panel undecided; timings, power sequence, touch separate |
| DisplayPort | DP output through documented V1.2 TYPEC24 lanes/AUX and Type-C alternate mode | Qualify actual revision, negotiation/orientation, HPD handling, COMB0 exclusion, link training and sink output |
| G2D | Native 2D fill/copy, scaling, blending, rotation and format conversion | Shared DE_SYS power/clocks, RCQ, IOMMU master 10, memory ownership and physical output correctness |
| GPU | Memory management, submissions, graphics/compute runtime, recovery | Exact BVNC, firmware, compiler/runtime, power and synchronization |
| NPU | Model loading, inference, compiler/runtime interface, recovery | Operator/shape support, numerical behavior, buffer ownership |
| Video and image codecs | Supported encode/decode operations through application APIs | Firmware/runtime, formats, dimensions, shared memory bandwidth |
| DI301 deinterlacer | Offline weave, motion-adaptive processing, TNR, film detection and supported formats | Shared DE_SYS power/clocks, guarded master-9 mappings, DMA visibility and physical field processing |
| PCIe | Link, configuration, interrupts, DMA and endpoint interface | Connector and endpoint; link recovery and boot conflicts |
| RTC and watchdog | Time retention, alarms, independent reset/recovery | Backup power population, clock source, reset-cause reporting |
| Thermal and frequency control | Temperature, throttling, supported operating points | Calibration, power coordination, load measurements |
| Crypto, random, identity and secure storage | Applicable hardware operations and trusted firmware interfaces | Confirm silicon features, ownership, immutable provisioning state |
| Always-on processors and mailboxes | E902 management-core ownership, existing firmware/mailbox services and sleep/wake coordination | Resolve firmware/shared-resource ownership and cold-start remote-writer exclusion; no physical control or drain qualification |
| Remaining SoC/board functions | Audit LEDs, buttons, infrared, Ethernet, debug and other blocks | Distinguish routed devices, internal functions, and unavailable pins |

Missing peripherals block their physical qualification. Continue implementation
of unrelated devices. Record shared-resource constraints and mutually exclusive configurations
with each functional result. All required rows must be qualified before declaring
complete hardware support.

Hawkeye 64 MP is a separate feasibility investigation. The published A733 ISP
dimensions are below a full 64 MP frame. Lower-resolution modes and any viable
bypass path need independent evidence; camera branding does not establish
compatibility.
