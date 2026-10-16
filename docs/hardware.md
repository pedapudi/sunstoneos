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
  not executed. Cached coherence, physical entry, interrupts and secondary
  scheduling remain unqualified.
- DDR: **tested** for initialization and distinct transfers to two 4 KiB pages.
  Firmware reports 12 GiB LPDDR5; full capacity and stability remain untested.
- Memory mappings, interrupt dispatch, and task scheduling: **tested** on the
  boot CPU for the physical kernel check. Cacheable RAM and non-cacheable result
  storage work for this workload; full memory protection/coherence remains untested.
- Timers and recovery: **tested** for the nonsecure tick and an EL3 secure-timer
  timeout that returns a stalled EL1 to FEL. This does not qualify the independent
  hardware watchdog, every interrupt source, or arbitrary failure recovery.
  A [physical SRAM trial](watchdog.md) also passes ROM-redirect and watchdog
  reset-policy programming/restoration with the timer disabled. Timer enabling,
  expiry, and reset retention remain unqualified.
- CPU clock diagnostics: **tested** for read-only reporting of the boot A55
  clock configuration during recognition. [Application profiling](mimus.md)
  records the register decode and its limits. Rate changes, regulator control,
  and thermal management remain planned.
- Storage: **implemented** bounded eMMC identification probe. Controller access,
  pin/clock restoration, and ROM return are **tested**; CMD1 receives no response.
  [Storage evidence](storage.md) records this negative result. No media device
  is identified, and block access remains planned.
- I²C and PMIC: **tested** single-byte reads on the inherited always-on bus,
  AXP8191 identity, and storage-rail settings. [Power evidence](power.md) records
  the conditions. Regulator writes and full bus operation remain unqualified.
- UFS initialization: **implemented**, but the physical attempt did not return
  to ROM. [Storage evidence](storage.md) records the failure and untested correction.
- Independent watchdog: **implemented** register operations with host checks.
  [Hardware reset and FEL return](watchdog.md) remain untested and are required
  before further new peripheral-access sequences.
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
  Host/reference checks and native compilation pass. The [startup adapter](dma-startup.md)
  now binds the clock registry, common counter, register mutex and deferred
  service activation. Focused two-core composition passes and the integrated
  target links. Physical handoff remains unqualified.
  [DMA implementation and limits](dma.md) record the evidence. Board integration,
  both physical controllers, cache coherence, and physical transfers remain
  unimplemented or untested.
  The [system IOMMU](iommu.md) implements shared translation-table construction,
  cold installation and non-destructive fault observation for both banks.
  Source comparisons and host checks pass, including refusal of inherited
  page-walk faults before publication. Native exclusion of all affected masters,
  bus drain, memory binding and physical translation remain unqualified.

- Header pin ownership: **implemented** shared leases for PB4–PB8 with verified
  mux/pull/drive updates. Host concurrency/failure checks and pinned field
  comparisons pass; late completion retains ownership. The restricted native
  adapter compiles for AArch64. [Pin implementation and limits](pins.md) records
  pending platform binding, wider pin coverage, and physical qualification.
- I2S0 capture: **implemented** finite mono/stereo capture through the native
  DMA controller. Host checks cover producer shutdown before DMA drain and
  retained ownership after uncertain failures. Format encodings match pinned
  source functions. [Audio implementation and limits](i2s.md) records formats,
  header routes, and evidence. The [clock/reset/pin binding](i2s-platform.md)
  passes host resource/deadline checks, pinned CCU comparison, and AArch64
  compilation. The [common clock registry](clock-leases.md) passes host
  composition with I2S and the existing GPU manager; its runtime files compile
  for AArch64. The privileged DMA service schedules finite capture. Production
  startup wiring, continuous streaming, playback, Bluetooth PCM and physical
  qualification remain open.
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
  reservation remain unqualified.
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
  QEMU. Concrete allocation/publication callbacks, linked render-target recovery,
  lease disposal for reopen, and physical execution remain outstanding.
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

- MIPI DSI: **implemented** bounded [low-power command transmission](dsi.md).
  Complete packet/register comparisons and retained-failure checks pass; native
  compilation passes. The [PHY controller](dsi-phy.md) verifies inherited PLL
  factors and lock, prepares analog controls and composes with command transport.
  Source comparisons and focused admission checks pass. The [video transmitter](dsi-video.md)
  implements four-lane RGB888 and loose RGB666 with continuous clocks and bounded
  engine stop. Source-register comparisons and host lifecycle checks pass.
  Cold PLL setup, clock/pin ownership, timing-controller and display-engine
  integration, scanout, panel configuration, read transactions and
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
  host lifecycle checks pass. Physical binding and silicon identity,
  power/reset bindings, association, security, data networking,
  pairing and headset audio remain outstanding.

- Camera: **implemented** bounded [OV5640 control](camera.md) for two-lane
  640 × 480 UYVY at nominal 30 frames/s. Sensor state matches the pinned reference
  model; lifecycle checks and native compilation pass. The [CSI controller](csi-receiver.md)
  configures local PHY, packet-port and parser state for both exposed port groups;
  complete register images match source functions. The [camera DMA controller](camera-dma.md)
  configures one NV16 image and retains buffers until capture, drain and cache
  synchronization complete. Register comparisons and ownership checks pass.
  The [route controller](camera-route.md) configures pass-through ISP1 and
  unity-scale VIPP0, with exclusive shared-selector ownership. The receiver
  now supports single-image mode. Host/source checks and native compilation
  pass. The [shared analog controller](camera-platform.md) implements cold PHY
  startup with exclusive common-register ownership and calibration readback.
  Source/model checks and native compilation pass. Complete capture composition,
  module electrical binding, actual DMA mapping/drain,
  additional modes and physical qualification remain outstanding.
  Module availability remains unconfirmed.

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
  ownership, native PMIC reads and deferred service activation. Focused two-core
  checks pass with modeled device registers and real FreeRTOS locks; the central
  target links. Uncertain register operations retain mutex ownership before
  release. Physical rail/PCK qualification, cold PLL provisioning, model admission, numerical execution,
  and physical qualification remain outstanding.

- USB0 device channel: **tested** at high speed for status, packet-boundary
  echoes, logs, and commanded ROM return. [USB evidence](usb.md) records the
  programmed-I/O implementation, inherited clock state, and remaining modes.
  The [native mimus session](mimus-usb.md) also passes asset loading, status polling
  during recognition, repeated results, and host-lease return with automatic access.

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
| OV5640 | Sensor control, supported MIPI modes, exposure and frame delivery | Confirm module wiring and availability; upstream sensor reference |
| Image processing | Pixel conversion, ISP controls, scaling, exposure/white-balance integration | Sensor tuning, buffer formats, documented dimensions |
| Display engine | Scanout, composition, synchronization and supported formats | Buffer sharing, display clocks, bandwidth |
| HDMI | Output modes, detection, applicable audio | Monitor and cable; controller and PHY |
| MIPI DSI | Host operation, panel commands, scanout, backlight integration | Schematic routes four DSI1 lanes to LCD; panel undecided; timings, power sequence, touch separate |
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
