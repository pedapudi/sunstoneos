# Hardware support and qualification

This inventory covers all onboard hardware and usable SoC blocks. It does not
depend on mimus using a device. The initial list comes from the A733 device tree
and datasheet. The pinned V1.2 schematic now supplies [board wiring](board-wiring.md)
for recovery, audio, cameras, and display. The attached board's revision and
component population remain unverified. Expand the list as circuit and device
inspection identifies additional blocks.

Status meanings: **planned** has no implementation; **implemented** has code;
**tested** has a functional result under stated conditions; **qualified** meets
its recorded operational and recovery criteria on physical hardware.

Every row below has **no qualification evidence**. Unless stated below, its
implementation is **planned**. Partial results apply only to the stated scope:

- PCIe: **implemented** [native root-port preparation](pcie.md) with shared
  clock, rail, pin and PMIC ownership, COMB1 PHY initialization, x1 Gen1 link
  observation and read-only configuration access to one direct endpoint.
  Source comparison, composed host checks and native compilation pass. Physical
  link operation, endpoint power admission, BAR allocation, interrupts, DMA,
  Gen2/Gen3, wider enumeration, power transitions and shutdown remain outstanding.
- HDMI: **implemented** [controller, PHY and TV0 diagnostic output](hdmi.md)
  for 720p60 and 1080p60. Composed host checks, primary-source comparisons and
  native compilation pass. The [native startup binding](hdmi-native.md) now
  acquires actual shared clock, CLDO2, PMIC, power-domain and register ownership;
  composed startup/EDID and retained-fault checks pass. The supervised
  [native colorbar application](hdmi-application.md) connects these providers
  to 720p60 output. Its central build and 17 focused software cases pass.
  Optional [RGB565 framebuffer output](hdmi-framebuffer.md) now connects actual
  DE1, IOMMU and TV0 providers. Its native build, normal start/stop and four fault
  cases pass. Analog admission, physical DMA reachability, monitor output and
  broader modes remain required.
- Boot ROM: **tested** for identity, SRAM transfer, a returning AArch32 probe,
  restoration of the modified SRAM bytes, and forced-FEL return from AArch64.
  See [boot evidence](boot.md).
- CPU: **tested** for instruction execution on the boot A55 in AArch32 and
  AArch64 EL3 and nonsecure EL1, including counter reads and SMC return.
  Physical FreeRTOS tasks now pass the [kernel check](a733-kernel.md) on that CPU.
  A separate [two-core emulator target](smp.md) tests concurrent scheduling.
  [Multicore mimus](mimus-smp.md) also passes recorded-reference comparisons there.
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
  reconnection and leaves the watchdog disabled. Three physical attempts produced
  no native result; scheduler execution remains unobserved. A returning native
  diagnostic now passes actual stack setup, admission, table construction and
  all three cache-publication calls. All five table pages match the instruction
  model; automatic ROM return and DRAM restoration pass. Native CPU/DSU identity
  and planning also pass, with no requested workaround writes. EL1 walk blocking,
  exact control restoration and GIC admission reads pass physically. Both GIC
  affinity-routing bits are already set. Complete distributor setup and exact
  restoration of its control and SPI enables now pass physically. Secondary
  admission and reset configuration also pass. Core 1 now powers on, reaches
  native entry at EL3 and powers itself off before RELEASE. The primary observes
  actual OFF before restoring control, reset configuration, GIC and DRAM state.
  Execution after RELEASE, the remaining secondary cores, cache coherence and
  scheduling remain unresolved.
  Removing a suspect SCR read did not resolve full startup. Its bounded
  refusal-report read timed out; the user reconnected. Offline stage diagnosis
  continues before another full-image attempt.
- DDR: **tested** for initialization and distinct transfers to two 4 KiB pages.
  Firmware reports 12 GiB LPDDR5; full capacity and stability remain untested.
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
- Timers and recovery: **tested** for the nonsecure tick and an EL3 secure-timer
  timeout that returns a stalled EL1 to FEL. This does not qualify the independent
  hardware watchdog, every interrupt source, or arbitrary failure recovery.
  A [physical SRAM trial](watchdog.md) also passes ROM-redirect and watchdog
  reset-policy programming/restoration with the timer disabled. Timer enabling,
  expiry, and reset retention remain unqualified.
- CPU clock diagnostics: **tested** for read-only reporting of the boot A55
  clock configuration during recognition. [Application profiling](mimus.md)
  records the register decode and its limits. The [thermal controller](thermal.md)
  implements calibrated sampling for CPU, DDR, NPU and GPU sensors, alarm and
  critical thresholds, and interrupt-status acknowledgement. Host checks, source
  comparisons and native compilation pass. Physical temperature accuracy,
  interrupt delivery, thermal policy, rate changes and regulator control remain
  outstanding.
- Storage: **implemented** [SD/eMMC identification and bounded reads](mmc.md)
  for the 1-bit legacy-speed profile. Host checks and source comparisons cover capacity, full-range
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
  eMMC routing. The [bounded write primitive](mmc-write.md) snapshots input,
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
  are integrated; focused instruction and host checks pass. Physical card
  identification, reads, writes, faster modes and recoverable boot remain outstanding.
- I²C and PMIC: **tested** single-byte reads on the inherited always-on bus,
  AXP8191 identity, and storage-rail settings. [Power evidence](power.md) records
  the conditions. The [rail ledger](rail-leases.md) coordinates all 39 source-defined
  outputs with the existing DCDC2 and GPU DCDC4 owners. Host checks and native
  compilation pass for shared selectors, snapshot ownership and retained faults.
  The [shared PMIC provider](pmic-runtime.md) owns the actual bus, pins and
  asynchronous read-only rail snapshots. Host checks, original getter comparisons
  and native compilation pass. It adopts either supported 24 MHz APBS1 parent
  and rejects later route changes; focused reference-parent checks pass.
  [GPU/NPU consumers](pmic-consumers.md) now retain
  actual asynchronous observations and supply leases; selecting the shared
  provider disables their competing inherited-bus readers. Emulator checks pass
  for both consumers and retained failures. Cold bus startup, recovery, regulator
  writes and electrical qualification remain open.
- UART: **implemented** bounded [FIFO control](uart.md) for UART0–6 and both
  always-on UARTs, with format planning, flow control where supported, interrupts,
  error reporting and transmit drain. Host checks, source comparisons and native compilation
  pass. Unread byte errors are preserved when receive capacity is exhausted.
  The [native platform binding](uart-platform.md) now composes actual clock
  and pin leases, reset sequencing, GIC masking and one FreeRTOS owner task.
  Host checks and single-core/SMP native compilation pass. Twelve explicit
  routes cover all nine controllers. Physical serial operation, Bluetooth
  binding, platform IRQ service, DMA and advanced modes remain outstanding.
  Bluetooth and the header UART1 route share one controller.
- SPI: **implemented** bounded [native master transfers](spi.md) for SPI0–4
  and the always-on controller. Host checks cover wire modes, FIFO completion,
  chip-select timing and retained failures; native compilation passes. The
  [native platform binding](spi-platform.md) composes real clock/pin leases,
  reset/GIC handling, inactive chip-select preparation and bounded restoration.
  Focused host checks and native compilation pass. SPI4 route activation refuses
  its unproven package signal mapping. Board admission, physical peers, DMA,
  advanced modes and SPIF remain outstanding.
- UFS initialization: **implemented**, but the physical attempt did not return
  to ROM. [Storage evidence](storage.md) records the failure and untested correction.
- Independent watchdog: **implemented** register operations with host checks.
  [Hardware reset and FEL return](watchdog.md) remain untested; physical
  qualification is deferred. Supervised RAM-only tests use their own reviewed
  recovery plan. Unattended recovery and the failed UFS probe remain restricted.
- DMA: **tested** finite descriptor encoding in a host execution model, including
  memory/FIFO streams, 35-bit addresses, and invalid-chain refusal.
  The polled controller core also passes host checks for noncoherent transfers,
  concurrent ownership, bounded cancellation, and retained buffers after faults.
  [Native memory operations](dma-memory.md) pass a FreeRTOS emulator check for
  translation, permissions, cache instructions, and serialized callers.
  The [privileged FreeRTOS service](dma-service.md) passes a two-core emulator
  session for copied requests, task ownership, retained buffers and I2S producer
  shutdown ordering. Its root build target links successfully.
  [DMA0 platform preparation](dma-platform.md) now validates inherited fabric
  clocks, dedicated gates/reset state and CPU/MCU routes under retained ownership.
  Host/reference checks and native compilation pass. Selected parent branches
  require forced-on control, software enable and effective-gate readback;
  focused admission checks pass. The [startup adapter](dma-startup.md)
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
  and copied-output checks pass. Physical capture and inference timing remain
  unqualified. Cyclic/full-duplex operation, wire-level drain, Bluetooth PCM and
  physical continuity remain open.
- GPU: **implemented** native [firmware decoding and staging](gpu.md) for the
  board software's target revision. A sanitizer check covers bounds, guards,
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
  The [fixed render service](gpu-render.md) submits compiled programs through
  paired geometry/fragment queues and copies output only after timeline, pixel
  and guard validation. Source comparisons and four native submission/failure
  cases pass. Six affected central targets link with the render implementation.
  The [native GPU application](gpu-application.md) connects real firmware,
  providers, IRQ routing, fixed rendering and copied USB output. Its central
  image links; focused checks verify retention when isolation is unavailable
  and prevention of device access after application failure. Its supervised
  mode also builds and passes session, launcher, packaging and refusal checks.
  All 151 repository dependencies match the integrated sources, allowing reuse
  of the linked image without another build. This mode retains resources until
  manual power removal and still requires actual initial isolation. Successful GPU
  output, general drawing, repeated submission, lease disposal for reopen and
  physical execution remain outstanding.
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
  and native compilation pass, including finalization fault retention. Complete
  DSI platform binding, shutdown/release, physical scanout, live frame
  updates, composition, additional formats and synchronization remain outstanding.

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
  completion. Source and focused host checks pass. Unresolved roaming blocks
  replacement operations. [Data transport](radio-data.md)
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
  nine native consumer compilations pass. Physical controller capabilities,
  headset audio, wired PCM routing, wider codecs and mandatory remaining
  call-control procedures remain outstanding.
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
  The [native radio platform](radio-platform.md) composes SDC1/card, pin, clock,
  rail and PMIC ownership with bounded power/reset timing and a borrowed card
  lifetime. Host checks and native compilation pass. Its initial profile uses
  one data line at 400 kHz. The [native radio session](radio-session.md) keeps
  platform startup, transport, receive dispatch and socket polling on one task.
  Emulator checks pass for actual provider ownership, bounded renewal, a TCP
  exchange and retained failure. Firmware upload and association checkpoints
  reuse earlier evidence; uninterrupted physical startup remains unqualified.
  The [radio entropy provider](radio-entropy.md) connects asynchronous crypto
  engine requests to a bounded buffer for copied network random bytes. Shared
  engine faults invalidate buffered bytes; focused checks and native compilation
  pass. The [native board application](board-application.md) connects actual
  platform identity, radio startup, entropy, association and a bounded TCP client.
  Focused application checks and central native linking pass; full startup and
  physical operation remain untested. Entropy qualification, wider modes,
  shutdown and physical topology/recovery remain outstanding.
  [Native IPv4 networking](radio-network.md) implements UDP and TCP clients
  through the actual FreeRTOS+TCP stack. Emulator checks cover packet exchange,
  deadlines, socket disposal and association revocation. Native linking passes.
  The [receive dispatcher](radio-dispatch.md) supplies one receive owner, copied
  scan/station events and bounded retirement of unsupported records. Emulator
  checks pass for sustained packet delivery, backpressure and retained ambiguous
  records. DHCP lease authority and copied DNS requests are integrated; four
  focused stack cases and the combined board link pass. Fixed-address and DHCP
  startup profiles pass sanitizer checks. An unexplained earlier DNS deadline
  miss remains a timing limitation. Physical networking, IPv6, server sockets,
  reconnection and wider Wi-Fi security modes remain open.

- Camera: The [native application](camera-application.md) connects actual provider
  startup, completed frame copies and USB0 capture. Its supervised mode includes
  explicit manual-power-removal admission, frozen host packaging and durable
  full-frame collection. Native linking and focused mode, admission, launcher and
  host checks pass; all 155 native dependencies match preserved evidence. No
  camera module is connected, so physical capture remains unqualified.
  The underlying driver provides **implemented** bounded [OV5640 control](camera.md) for two-lane
  640 × 480 UYVY at nominal 30 frames/s. Sensor state matches the pinned reference
  model; lifecycle checks and native compilation pass. The [CSI controller](csi-receiver.md)
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
  module electrical binding, physical DMA/coherence and drain, additional modes
  and physical qualification remain outstanding.
  No MIPI camera is connected; physical capture qualification awaits a module.

- NPU: **implemented** offline [page-table construction](npu-mmu.md), with host
  translation and permission checks. The [controller core](npu-controller.md)
  passes host startup, completion, cancellation, and retained-fault checks.
  The [memory policy](npu-memory.md) passes host integration for private mappings,
  table validation, cache publication, and retained faults.
  The [native CPU adapter](npu-native.md) passes a FreeRTOS emulator check for
  translation, cache maintenance, MMIO, deadlines, and retained failure.
  The [FreeRTOS service](npu-service.md) passes a two-core ownership check with
  EL0 clients and a preemptible EL1 worker.
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
  release. [Fixed convolution registration](npu-convolution.md) authenticates
  the complete compiled model, private allocations and commands before startup
  and publication. Copied inputs, matching output completion and explicit result
  reaping pass focused emulator checks. The output bytes are modeled, so these
  checks do not establish inference. The [native memory session](npu-reserved.md)
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
  The attached board
  ROM profile, actual memory/cache/drain authority, physical decoding,
  remaining decode formats and all encoder paths remain outstanding.

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
  require no entropy-quality assertion. [AES-128 block transforms](crypto-cipher.md)
  use the same retained session for encryption and decryption, with private output
  staging and successful-task key cleanup. Host checks, independent vectors and
  native composition pass. Physical hashing/cipher operation, authenticated and
  wider cipher modes, other hashes, asymmetric operations and secure key handling
  remain outstanding. Internal hardware key erasure is unproven.
  The [native SHA/AES application](crypto-application.md) now connects actual
  shared providers, the singleton retained page and copied USB results. Its
  native link and focused admission, orchestration, publication and host checks
  pass; all 223 preserved repository inputs match integration. A read-only board
  snapshot confirms the adopted CE clock configuration. Actual predecessor task
  accounting, future launch exclusion, secure status access and memory admission
  remain required before physical operation. No TRNG capability is claimed by
  this application.

- USB0 device channel: **tested** at high speed for status, packet-boundary
  echoes, logs, and commanded ROM return. [USB evidence](usb.md) records the
  programmed-I/O implementation, inherited clock state, and remaining modes.
  The [native mimus session](mimus-usb.md) also passes asset loading, status polling
  during recognition, repeated results, and host-lease return with automatic access.
  [Cold USB0 preparation](usb-platform.md) now supplies native clock/reset/PHY
  sequencing, detached-controller admission and shared-reference publication.
  Host checks, source comparisons and native instruction checks pass. It keeps
  the device disconnected and does not enable DMA or host power. Actual
  fabric/power/security/recovery admission, connection, USB1 and USB2/DWC3 modes
  and physical cold-start qualification remain outstanding.

- USB host and Type-C: **implemented** native [source power and attachment
  control](typec.md), plus [high-speed USB2 host transfers](usb2-host.md) through
  xHCI and TinyUSB. Focused checks cover attachment, independent power cutoff,
  mass-storage enumeration, copied sector reads, malformed responses,
  cancellation and disconnects. The [native board application](board-application.md)
  connects startup, separate Type-C tasks, host reads and simultaneous USB0
  output. A supervised mode retains resources until manual power removal;
  USB0-only and USB0-with-radio selections can run without a powered host
  accessory. Its central build passes, and 25 host cases cover capture,
  refusal and retained ownership. Full startup,
  physical power and switch qualification, actual transfers, DMA drain,
  repeated attachment, other speeds, hubs, periodic transfers, role swaps, USB PD,
  VCONN and applicable alternate modes remain outstanding.

| Hardware | Required functions and application interface | Dependencies and physical checks |
| --- | --- | --- |
| Boot ROM and boot firmware | Recovery, image loading, repeatable boot | Confirm SoC identity, DDR firmware, entry contract, storage preservation |
| CPU cores | AArch64 tasks on all six A55 and two A76 cores; floating-point/vector context | Secondary startup, coherent memory, interrupt routing, affinity |
| DDR and CPU memory management | Usable RAM, permissions, cache policy, allocation | Discover actual RAM; reserve boot firmware; test address boundaries |
| Interrupt controller and timers | Interrupt dispatch, tick, monotonic time, deadlines | GICv3 distributor/redistributors; per-core timers |
| Clocks, resets, regulators, power domains | Dependency-aware enable, disable, rate control, power transitions | Confirm power ICs and safe board operating points |
| Pin control and GPIO | Pin functions, levels, pulls, interrupts | Confirm header routing, voltage domains, shared-pin exclusions |
| DMA and device address translation | Transfers, mapped buffers, cache synchronization, completion, recovery | Address widths, alignment, ownership, IOMMU faults |
| UART | Console and exposed serial channels | Pin routing, baud rates, flow control, receive overrun |
| I2C | Transactions and recovery on exposed and internal buses | Clock dependencies, power devices, sensor control |
| SPI | Transfers and exposed controller modes | Chip selects, DMA, shared pin functions |
| PWM and ADC | Applicable waveform and sampling interfaces | Routed channels, clocking, signal verification |
| SD, eMMC, UFS, SPI storage | Block access and recovery where populated or exposed | Population, boot conflicts, voltage, capacity, power-loss behavior |
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
| DisplayPort | Output and applicable Type-C integration | Verify board routing, shared PHY and monitor |
| GPU | Memory management, submissions, graphics/compute runtime, recovery | Exact BVNC, firmware, compiler/runtime, power and synchronization |
| NPU | Model loading, inference, compiler/runtime interface, recovery | Operator/shape support, numerical behavior, buffer ownership |
| Video and image codecs | Supported encode/decode operations through application APIs | Firmware/runtime, formats, dimensions, shared memory bandwidth |
| PCIe | Link, configuration, interrupts, DMA and endpoint interface | Connector and endpoint; link recovery and boot conflicts |
| RTC and watchdog | Time retention, alarms, independent reset/recovery | Backup power population, clock source, reset-cause reporting |
| Thermal and frequency control | Temperature, throttling, supported operating points | Calibration, power coordination, load measurements |
| Crypto, random, identity and secure storage | Applicable hardware operations and trusted firmware interfaces | Confirm silicon features, ownership, immutable provisioning state |
| Always-on processors and mailboxes | Existing firmware services, notifications, sleep/wake coordination | Identify firmware ownership before repurposing any processor |
| Remaining SoC/board functions | Audit LEDs, buttons, infrared, Ethernet, debug and other blocks | Distinguish routed devices, internal functions, and unavailable pins |

Missing peripherals block their physical qualification. Continue implementation
of unrelated devices. Record shared-resource constraints and mutually exclusive configurations
with each functional result. All required rows must be qualified before declaring
complete hardware support.

Hawkeye 64 MP is a separate feasibility investigation. The published A733 ISP
dimensions are below a full 64 MP frame. Lower-resolution modes and any viable
bypass path need independent evidence; camera branding does not establish
compatibility.
