# Current work

The [accepted goal](goal.md) requires native FreeRTOS, complete board hardware
support and local mimus transcription. The [hardware matrix](hardware.md) is the
authoritative inventory. No complete subsystem has physical qualification.
The [development dashboard](progress-ui.md) reads these records automatically.

Development uses the isolated `bringup` branch in
`/home/sunil/git/sunstoneos-worktrees/bringup`. The main checkout is unchanged.
Local commits and issue updates are authorized; publishing or merging is not.
Parallel workers own separate worktrees and files. The root coordinator alone
operates the board, through the [test queue](board-test-queue.md) and exclusive
board lock. Inspect live worker state before assigning overlapping files.

## Working behavior and integration limits

FreeRTOS runs on the physical boot A55. The [kernel check](a733-kernel.md) passes
timer preemption, blocking queues and floating-point context checks. An EL3
secure timer recovers a forced EL1 stall, but did not recover the UFS probe.
Independent hardware-watchdog recovery remains required before new peripheral
access. Physical USB programmed-I/O, C++ allocation and repeated mimus requests
have passed their recorded workloads.

[Mimus](mimus.md) matches host results on 32 development recordings through the
[native USB application](mimus-usb.md). The model remains unchanged. Two-core
[emulator execution](mimus-smp.md) also passes recorded and paced-input parity.
Physical paced replay, all-core execution, accelerator backends, real audio
input and final latency/resource qualification remain outstanding. Existing
compute time exceeds the target; optimization waits until all hardware works.

The following components are integrated. Their evidence establishes the stated
software behavior; physical operation remains unqualified.

| Component | Implemented scope and evidence | Next dependency |
| --- | --- | --- |
| [Secondary CPU continuation](cpu-continue.md) | Per-core interrupt preparation, translation tables, exception vectors; host and instruction checks pass | Physical cached coherence and all-core startup |
| [Cached eight-core image](a733-smp-cached.md) | Coordinated cache adoption and full native-instruction scheduler checks on all cores; central build links | Physical coherence, entry admission and complete-image execution |
| [Eight-core diagnostic and launcher](a733-smp.md) | Scheduler image links; bounded watchdog launch, clean FEL refusal and immutable upload checks pass | Physical independent recovery, observable scheduler results and full image execution |
| [USB observer and launcher](a733-smp-observe-host.md) | Bounded cached diagnostic frames, durable host capture and exact offline bundle; focused host/instruction checks pass | Physical recovery, entry admission, USB timing and scheduler results |
| [Device memory reservations](cached-device-reservations.md) | Linker-owned pools, actual DMA lifetime and GPU/NPU preparation under the cached eight-core barrier; host/instruction/linker checks pass | Complete native application binding, additional capacity, physical exclusion and external drain |
| [DMA startup](dma-startup.md) | Shared clock/lock/counter ownership and deferred service activation; two-core model passes | Native reservation binding, handoff authority, physical transfer/drain |
| [System IOMMU](iommu.md) | Both-bank table construction, cold installation and fault observation; source/host checks pass | Exclusion and drain of every affected master; native mapping |
| [NPU PLL](npu-pll.md) | Source-backed enable/retune and atomic startup adoption; focused host/emulator checks and native compilation pass | Concrete boot authority and physical qualification |
| [NPU startup and convolution](npu-convolution.md) | Linker-owned session buffers, fixed model authentication, guarded startup and copied input/output ownership; focused emulator checks pass | Physical confinement, supply/domain qualification and numerical execution |
| [GPU firmware reservations](gpu-reserved.md) | Actual retained pools, firmware/state/MIPS preparation and native translation checks; instruction and linker checks pass | Physical exclusion/coherence and complete native application binding |
| [GPU compute reservations](gpu-compute-reserved.md) | Real retained payload/context/VM allocations, actual builders and service binding; native instruction checks pass | Physical startup, numerical output, complete application binding, free lists and wider jobs |
| [GPU compute](gpu-compute-service.md) | Native workload publication, compiled fixed shader, command stream and output checking; service model passes | Physical firmware startup and numerical execution |
| [GPU core mode](gpu-compute-mode.md) | Observed single-core capability required before startup/submission; instruction/service checks pass | Physical identity and wider core modes |
| [GPU isolation](gpu-isolation.md) | Only exact provider success permits reclamation; focused instruction check passes | Proven request blocking and completion of all accepted external writes |
| [Radio platform](radio-platform.md) | Actual SDC1/card, clock/pin/rail/PMIC ownership and borrowed lifetime; composed host checks and native compilation pass | Physical topology/recovery, wider SDIO modes and shutdown |
| [Native radio session](radio-session.md) | One actual owner for platform/card, transport, dispatch and socket polling; focused FreeRTOS composition passes | Physical boot-to-network, production entropy/identity and safe shutdown |
| [Wi-Fi initialization](radio-wifi.md) | Source-defined RF/firmware setup, explicit channel policy, passive scanning; packet/model checks pass | Native radio binding and physical radio operation |
| [Wi-Fi association, data and WPA2](radio-security.md) | Real RSN handshakes, confirmed keys, controlled-port authorization and CCMP replay checks; host checks and native link pass | Native radio/entropy binding and wider security modes |
| [Native IPv4 networking](radio-network.md) | Actual FreeRTOS+TCP UDP/TCP clients, bounded socket ownership and revocation; emulator checks and native link pass | Production entropy/radio binding, DHCP/DNS/IPv6, servers and reconnect |
| [Radio receive dispatch](radio-dispatch.md) | One receive owner, bounded events and complete-record retirement; actual networking composition passes | Physical radio binding and safe shutdown |
| [Bluetooth controller configuration](bluetooth-config.md) | Verified event/scan/timeout settings and incoming request preservation through ACL handoff; focused host checks and seven native consumer compilations pass | Actual firmware/UART binding, authenticated receive-service timing and physical configuration |
| [Incoming Bluetooth links](bluetooth-incoming.md) | Copied peer reservation, actual HCI acceptance/rejection and bounded quarantine; host checks and native compilation pass | Pairing, native service authority and physical connections |
| [Bluetooth channels](bluetooth-l2cap.md) | Real UART/HCI/ACL composition, response timers and guarded channel reuse; host checks and native compilation pass | Finite wire/link identities, native binding, security and headset profiles |
| [Bluetooth service discovery](bluetooth-sdp.md) | Actual L2CAP transactions, bounded attribute/continuation parsing and confirmed teardown; host checks and native compilation pass | Security, profiles and physical peer discovery |
| [Local Bluetooth discovery](bluetooth-sdp-server.md) | Actual incoming L2CAP queries, bounded continuation and audio-gateway records; host checks and native compilation pass | Actual profile capabilities, pairing and physical discovery |
| [Bluetooth serial channels](bluetooth-rfcomm.md) | Incoming/outbound credit mode, actual channel ownership and acceptance-time fences; host checks and native compilation pass | Additional channel modes, pairing and voice |
| [Hands-free audio gateway](bluetooth-hfp.md) | Initial setup, indicators and gain reports over actual incoming ACL/discovery/RFCOMM; composed host checks and native compilation pass | Mandatory remaining call procedures, security, controller voice transport and physical headset |
| [Hardware random service](entropy.md) | Supplied health provisioning, repeated requests and retained native pool; host/source checks and native composition link pass | Raw-source assessment, current-epoch startup evidence, secure platform admission and physical entropy |
| [SHA-256 streaming](crypto-hash.md) | Shared actual CE session, software padding and native descriptors/cache handling; host/source/native checks pass | Secure platform admission, physical digests and independent engine drain |
| [AES block transforms](crypto-cipher.md) | Shared actual CE session, AES-128 ECB, private output and key cleanup; focused host/source/native checks pass | Physical cipher operation, authenticated/wider modes, secure key handling and independent drain |
| [Rail ownership](rail-leases.md) | 39-output ledger with actual DCDC2/GPU guards, alias exclusion and borrowed snapshots; host checks and native compilation pass | Actual supply wiring/stability and migration of legacy readers |
| [Shared PMIC provider](pmic-runtime.md) | Native asynchronous read-only rail snapshots under common bus/pin/rail ownership; host/source checks and native compilation pass | Cold initialization/recovery and physical bus/voltage qualification |
| [GPU/NPU supply admission](pmic-consumers.md) | Actual shared-provider requests and retained rail ownership; focused native instruction checks pass | Electrical qualification, complete native boot admission and physical devices |
| [Media clocks](media-clock.md) | Native CSI/ISP/DE/DSI gates and inherited-parent admission; host checks and native compilation pass | Complete electrical, pin, memory and drain binding |
| [Camera analog startup](camera-platform.md) | Cold shared PHY setup and calibration readback; source/model checks pass | Native resource binding and actual memory/drain providers |
| [Camera capture](camera-capture.md) | All six native controllers compose in both port models; one NV16 frame becomes CPU-owned after shutdown, drain and fault scan | Actual providers, global mapping disposal and concurrent device ownership |
| [DSI video](dsi-video.md) | Four-lane RGB888/loose RGB666, continuous clock, bounded stop; source/model checks pass | Display engine, panel and physical scanout |
| [Display PLL](display-pll.md) | Source-backed enable/retune, intermediate-rate checks and retained faults; host checks and native compilation pass | Qualified initial analog profile and physical operation |
| [Display scanout](display-scanout.md) | Actual PHY/DSI/LCD/DE composition and atomic PLL/media adoption; host checks and native compilation pass | Qualified analog profile, memory/panel binding, independent drain and shutdown/release |
| [I2S capture](i2s.md) | Finite capture, DMA ownership and clock/reset/header-pin binding; host/emulator checks pass | Continuous capture, playback, Bluetooth PCM and attached microphone |
| [JPEG decode and native binding](video-platform.md) | Parser/executor, admitted cold startup and per-identity reset; host checks and native compilation pass | Board ROM profile, actual memory/cache/drain authority, physical output and remaining codec families |
| [SD/eMMC reads](mmc-platform.md) | SDC0 plus explicit SDC2/SDC3 selection, corrected eMMC pin functions and joint card ownership; focused host/native checks pass | Physical identity/reads, writes, wider modes and recoverable boot |
| [GPIO and pins](pins.md) | Shared B–M ownership, GPIO and interrupt setup; host/source checks and native compilation pass | Physical routes, GIC delivery and platform admission |
| [Thermal sensors](thermal.md) | Calibrated sampling, alarms and critical thresholds; host/source checks and native compilation pass | Physical accuracy, GIC delivery and thermal policy |
| [SPI platform](spi-platform.md) | Actual clock/pin/GIC binding and bounded master transfers; composed host checks and native compilation pass | Board admission, SPI4 route proof, DMA, advanced modes and physical peers |
| [Cold USB0 preparation](usb-platform.md) | Actual clock/reset/PHY sequencing and detached controller admission; host/source/native instruction checks pass | Full platform admission, connection, remaining USB controllers and physical operation |
| [UART](uart.md) | Real clock/pin leases, reset/GIC masking and bounded FIFO transfers; composed host checks and native compilation pass | Physical peer tests, Bluetooth binding, platform IRQ service and advanced modes |

The camera path also has [OV5640 control](camera.md), [local CSI control](csi-receiver.md),
[ISP/VIPP pass-through routing](camera-route.md) and [single-frame DMA](camera-dma.md).
Display command transport and inherited-clock [PHY preparation](dsi-phy.md) are
integrated. Radio [verified firmware loading](radio-firmware.md) and
[continuous receive](radio-runtime.md) precede Wi-Fi initialization.

## Active parallel work

| Worktree | Assigned work |
| --- | --- |
| Bluetooth audio worker | Actual synchronous headset transport, controller credits and copied audio buffers |
| Storage worker | Bounded SD/eMMC writes under explicit default-deny authority; native platform remains read-only |
| DMA integration worker | Actual cached-pool and native transfer-service binding, with a copy application |
| Audio worker | Continuous I2S capture/playback with bounded period ownership |
| HDMI worker | Actual PHY/controller and TV0 timing, sink discovery and diagnostic output |
| PCIe worker | Actual shared-resource binding, PHY, link and endpoint configuration |
| Independent reviewers | Changed runtime ownership, deadlines and retained failures before focused execution |

GPU isolation research found no source-defined A733 acknowledgement that proves
completion of writes already accepted beyond the GPU. The [research record](gpu-isolation.md)
identifies the missing guarantee and asynchronous shutdown requirements. Retaining
memory after failure does not restore an already changed power state.

## Observed host and device state

The attached FEL device (`1f3a:efe8`) identifies A733. The boot CPU identifies
as Cortex-A55 r2p0. DDR firmware reports 12 GiB LPDDR5, with transfers checked
at two distinct pages. [Boot evidence](boot.md) records initialization and the
EL1 handoff. Board revision and populated storage remain unconfirmed.
No persistent-storage writes have been issued.

The reconnected board at `003:064` has automatic access and answers ROM requests.
The [physical ROM capture](watchdog.md) differs from the published dump and now
supplies the board-specific reset reference. SRAM workspace restoration passed.
Read-only recovery-register inspection found no redirect owner and a disabled
watchdog. The boot input was high, so it cannot currently protect a reset from
storage boot. The user confirmed automatic FEL entry after disconnecting and
reattaching USB, with a microSD card inserted and no button held. Preserve that
card. No watchdog was armed and no reset was issued.
The failed UFS instance `003:063` remains quarantined. DDR must be reinitialized;
do not reuse previous RAM assets or signatures.
The [SRAM configuration trial](watchdog.md) now verifies ROM-redirect and
watchdog reset-policy writes with complete register/workspace restoration.
It never enables the timer. Timer enabling, expiry, and reset retention remain
unqualified. No new peripheral probe may run before independent watchdog recovery passes.
The installed
[session access rule](../tools/README.md) grants access after reconnects on
physical port `3-1`. Kernel and native USB checks regained access automatically.
The rule lasts until host reboot and can be removed with `disable-session`.
Rediscover the node and run `python3 tools/usb-access.py check-session` before
an intentional reconnect.

The separate `mimus keyboard` device is an existing ESP32 board. It must not be
used as the Orange Pi console. No Orange Pi UART adapter is currently visible.

The host has Clang 21, CMake, and LLVM object tools. LLD 21, ARM system emulation,
and USB/FDT development headers are extracted under `~/.cache/sunstoneos/host`.
Pinned sources, builds, packages, and raw evidence are under `~/.cache/sunstoneos`.
The [build instructions](build.md) describe the emulator configuration.
The local Python environment supplies the dependencies in
`checks/host-requirements.txt` for USB transport and compiled-code checks.

## Reusable validation evidence

Component documents link their source identities, selected checks, artifacts and
limitations. Raw evidence lives under `~/.cache/sunstoneos/evidence/`.
The central integration records `hardware-integration-20261016f`,
`hardware-integration-20261016g`, `hardware-integration-20261016h`,
`hardware-integration-20261016i`, `hardware-integration-20261016j`,
`hardware-integration-20261016k`, `hardware-integration-20261016l` and
`hardware-integration-20261016m`, `hardware-integration-20261016n` and
`hardware-integration-20261016o` and `hardware-integration-20261016p`
preserve source comparisons, build artifacts
and source-pin verification. Integration reused component execution evidence;
no unchanged emulator or physical workload was repeated. The JPEG record explicitly
identifies its missing historical malformed-input runner and input archive.
The first network lifecycle/entropy ELF was overwritten during a fixture relink;
its digest and result logs remain, while later focused executables are preserved.
Observer compiler resource headers lacked historical hashes. These limits remain
explicit and cannot establish missing final-candidate acceptance evidence.

The shared clock registry now has independent UART, SPI, security-engine and
RC16M scopes, independent USB ports, SDC0–3, radio-module and S_TWI0 scopes,
64-bit masks and 128 slots. Its 32-byte token ABI requires all
producers and consumers to rebuild together. A focused sanitizer check and
affected native compile/link checks pass; earlier workload results retain
their recorded source and ABI identities. GPU rail guards use separate slots
from its fixed clients; the expanded private manager requires coherent consumer
compilation. Seven additional cross-component units compiled after integration.
The central dispatcher configuration passed without replaying component tests.
The combined audio-gateway check covers actual incoming connection, local
discovery, serial setup and initial control commands. Its four lower native
consumers compile against the current incoming ACL layout. Individual component
results retain their earlier inputs. NPU startup and clock-handoff targets link
with the shared PMIC dependency closure; the convolution object also compiles.
No existing runtime scenario was repeated for that build integration. The radio
session adds owned socket polling; the two older fixture consumers compile with
its current structure layout. Central configuration includes the session target.
Bluetooth configuration and SDC3 integration reuse their complete native consumer
compilation evidence. The storage check covers corrected PC pin functions for
both eMMC selections and joint controller/pin conflicts. Earlier evidence using
the NAND function does not establish eMMC routing. No additional build or runtime
replay was required for these two integrations.

The central uncached and cached eight-core diagnostics, GPU compute service and
NPU startup targets compile and link. The NPU convolution object also compiles. Build success does not establish complete-image execution.
The SMP central image differs through source-path strings and their address
references. Function locations and the checked instruction bodies match; its
separate identity and analysis are recorded in the integration evidence. Neither
uncached image has whole-machine or physical scheduler evidence.
The separate cached image has a preserved eight-core instruction-model scheduler
result and focused final correction checks. A separate preparation fixture covers
actual accelerator constructors and both memory regimes on all eight emulated
cores. The current diagnostic and observer targets link with the extracted table
constructor. The previously sealed physical observer bundle remains unchanged.
Current central builds have separate identities and compile/link evidence;
neither cached image has physical qualification.
The actual pre-scheduler device guard is now available independently of optional
accelerator construction. Its body is unchanged and the affected native platform
unit compiles. A complete DMA application link still requires its pending service
integration and actual immutable platform handoff.

Focused corrections retain inherited IOMMU page-walk faults, preserve the NPU
register mutex on uncertain operations before release, and reject uncertain GPU
isolation results. Their component records preserve the narrow checks and
previous evidence that remains applicable. The shared A733 manual and source
references are pinned in `sources.lock.json`.

Apply the [validation policy](validation.md) before each changed-behavior check.
Documentation, clean integration and commits do not justify rerunning workloads.
The dashboard requires no separate status maintenance or recurring browser tests.

## Next actions

1. Qualify independent watchdog recovery before new peripheral-access sequences.
   Confirm a stable, removable BOOT-to-GND connection using the
   [board wiring procedure](board-wiring.md). The user's ability to attach that
   connection is unconfirmed. Then run the queued input, enable/disarm and
   [expiry checks](watchdog-expiry.md) with current target identity and reconnect
   access. Protected ROM return alone does not establish reset cause or recovery
   from a stalled interconnect. Preserve the inserted microSD and all storage.
2. Use the prepared [cached observer bundle](a733-smp-observe-host.md) after
   independent recovery and entry admission pass. The host preserves validated
   scheduler frames before separately observing ROM return. Qualify all-core
   startup, interrupts, coherent memory, affinity and context preservation.
   ROM return alone does not establish scheduler operation.
3. Complete native memory and platform binding for DMA, IOMMU, GPU and NPU.
   Fixed NPU convolution and GPU firmware preparation use actual linker pools.
   Fixed GPU compute and VM reservations are also implemented. Their actual
   constructors now run under the cached eight-core boot barrier with matching
   CPU mappings. The native application must provide immutable model/firmware
   inputs and complete platform admission. The initial
   pools cannot also fit a 600 KiB NV16 frame in remaining write-back space;
   broader concurrent workloads require further reservations and qualified DRAM.
   Physical startup
   authority, exclusive internal SRAM ownership and numerical output remain open.
   Corrected inherited-clock guards require forced-on control,
   software enable and effective-gate readback; focused checks pass. Account for remote firmware, all shared-memory users, cache
   attributes and outstanding bus traffic. Supply real admission/drain evidence
   before device commands or buffer reuse.
4. Bind the integrated camera capture and display scanout coordinators to native
   platform providers. Qualify the display PLL initial analog profile and
   implement safe shutdown/release. Atomic PLL/media adoption into scanout is
   integrated with software evidence.
   Add panel-specific sequencing, live frame updates and wider camera modes.
   OV5640 module availability remains unconfirmed; the DSI panel is undecided.
   Keep Hawkeye 64 MP feasibility separate from the first camera implementation.
5. Bind the integrated WPA2 handshake and secure packet transport to native
   entropy and radio resources. Receive dispatch, incoming links, local discovery,
   both RFCOMM connection directions and initial audio-gateway control are integrated.
   Controller event/scan configuration and preserved incoming-request handoff are
   also integrated; native receive-service timing still needs actual authority.
   Production entropy still needs raw-source assessment and current-epoch startup
   evidence. Complete IP configuration/services, wider security modes, interrupts,
   sleep and reconnect. Extend Bluetooth links with pairing, the remaining mandatory
   hands-free procedures and voice transport. Establish actual SDIO/UART/power/pin bindings and
   radio identity before physical use. The native radio platform now composes
   actual card, pin, clock and PMIC ownership. The native session adopts that
   lifetime through firmware and protocol ownership and pauses during renewal
   contention. Its composed check reuses upload and association checkpoints;
   uninterrupted physical startup remains unqualified. No microphone is available; recorded input
   remains the application fixture while I2S and headset support proceed.
6. Supply physical admission for the integrated [fixed convolution](npu-convolution.md):
   proven address translation, exclusive internal SRAM ownership and qualified
   power/recovery. Preserve the compiled model and private command allocations. General untrusted
   model admission additionally needs complete instruction/access verification.
   Complete GPU request isolation and asynchronous shutdown, then physical
   firmware startup, shader completion and graphics/runtime integration.
7. Continue the remaining [hardware inventory](hardware.md): USB roles and cold
   PHY setup; storage identification, read/backup and recoverable boot; exposed
   buses; HDMI/DisplayPort; codecs; PCIe; thermal/power transitions; security and
   always-on services. Preserve all required functions independently of mimus.
8. Qualify physical paced replay and all-core mimus with the unchanged model.
   After all hardware functions work, optimize CPU/GPU/NPU execution and measure
   the established latency and resource targets. Complete concurrent workloads,
   error recovery, reproducible images, update procedures and final qualification.

Project [issues 1–11](https://github.com/pedapudi/sunstoneos/issues) track hardware
scope, dependencies and acceptance criteria. [Issue 12](https://github.com/pedapudi/sunstoneos/issues/12)
tracks the lower-priority native NPU development toolkit. External compiler tools
remain acceptable until complete hardware support.
