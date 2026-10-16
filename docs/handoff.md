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
| [Cached eight-core image](a733-smp-cached.md) | Coordinated cache adoption and full native-instruction scheduler checks on all cores; central build links | Physical coherence, observable results and separate launcher admission |
| [Eight-core diagnostic and launcher](a733-smp.md) | Scheduler image links; bounded watchdog launch, clean FEL refusal and immutable upload checks pass | Physical independent recovery, observable scheduler results and full image execution |
| [DMA startup](dma-startup.md) | Shared clock/lock/counter ownership and deferred service activation; two-core model passes | Native reservations, handoff authority, physical transfer/drain |
| [System IOMMU](iommu.md) | Both-bank table construction, cold installation and fault observation; source/host checks pass | Exclusion and drain of every affected master; native mapping |
| [NPU PLL](npu-pll.md) | Source-backed cold enable and enabled retuning; focused host checks and native compilation pass | Concrete boot authority, atomic clock handoff and physical qualification |
| [NPU startup and convolution](npu-convolution.md) | Fixed model authentication, private commands, guarded startup and copied input/output ownership; focused emulator checks pass | Physical confinement, supply/domain qualification and numerical execution |
| [GPU compute](gpu-compute-service.md) | Native workload publication, compiled fixed shader, command stream and output checking; service model passes | Physical firmware startup and numerical execution |
| [GPU core mode](gpu-compute-mode.md) | Observed single-core capability required before startup/submission; instruction/service checks pass | Physical identity and wider core modes |
| [GPU isolation](gpu-isolation.md) | Only exact provider success permits reclamation; focused instruction check passes | Proven request blocking and completion of all accepted external writes |
| [Wi-Fi initialization](radio-wifi.md) | Source-defined RF/firmware setup, explicit channel policy, passive scanning; packet/model checks pass | Native radio binding and physical radio operation |
| [Wi-Fi association, data and WPA2](radio-security.md) | Real RSN handshakes, confirmed keys, controlled-port authorization and CCMP replay checks; host checks and native link pass | Native radio/entropy binding, IP networking and wider security modes |
| [Bluetooth channels](bluetooth-l2cap.md) | UART/HCI/ACL and bounded L2CAP negotiation/data compose in host checks; native compilation passes | Full retries/identity reuse, native binding, SDP, security and headset profiles |
| [Media clocks](media-clock.md) | Native CSI/ISP/DE/DSI gates and inherited-parent admission; host checks and native compilation pass | Complete electrical, pin, memory and drain binding |
| [Camera analog startup](camera-platform.md) | Cold shared PHY setup and calibration readback; source/model checks pass | Native resource binding and actual memory/drain providers |
| [Camera capture](camera-capture.md) | All six native controllers compose in both port models; one NV16 frame becomes CPU-owned after shutdown, drain and fault scan | Actual providers, global mapping disposal and concurrent device ownership |
| [DSI video](dsi-video.md) | Four-lane RGB888/loose RGB666, continuous clock, bounded stop; source/model checks pass | Display engine, panel and physical scanout |
| [Display PLL](display-pll.md) | Source-backed enable/retune, intermediate-rate checks and retained faults; host checks and native compilation pass | Qualified initial analog profile and atomic consumer handoff |
| [Display scanout](display-scanout.md) | Actual PHY, command, DSI, LCD1 and display-engine composition; host checks and native compilation pass | Platform binding, cold PLL, panel, independent drain and shutdown/release |
| [I2S capture](i2s.md) | Finite capture, DMA ownership and clock/reset/header-pin binding; host/emulator checks pass | Continuous capture, playback, Bluetooth PCM and attached microphone |
| [JPEG decode and native binding](video-platform.md) | Parser/executor, resource admission and per-identity reset match recorded runtime instructions; host checks and native compilation pass | Cold startup, actual memory/cache/drain authority, physical output and remaining codec families |
| [SD/eMMC reads](mmc.md) | Cold identification, capacity and bounded block reads; host/source checks and native compilation pass | Actual card presence, platform binding, physical reads and wider storage modes |
| [UART](uart.md) | Bounded FIFO transfers, configuration, flow control, interrupt service and drain; source/model checks and native compile pass | Native resource binding, advanced modes and physical peer tests |

The camera path also has [OV5640 control](camera.md), [local CSI control](csi-receiver.md),
[ISP/VIPP pass-through routing](camera-route.md) and [single-frame DMA](camera-dma.md).
Display command transport and inherited-clock [PHY preparation](dsi-phy.md) are
integrated. Radio [verified firmware loading](radio-firmware.md) and
[continuous receive](radio-runtime.md) precede Wi-Fi initialization.

## Active parallel work

| Worktree | Assigned work |
| --- | --- |
| Display PLL worker | Review of shared clock publication and NPU startup adoption |
| Wi-Fi worker | Native FreeRTOS TCP/UDP networking |
| Bluetooth worker | L2CAP retry timing and safe channel identity reuse, then service discovery |
| NPU platform worker | Atomic clock publication and startup adoption |
| Video platform worker | Cold decoder startup |
| Pin worker | Shared B–M pin ownership, GPIO and interrupts |
| CPU worker | Cached eight-core diagnostic with observable USB results |
| Thermal worker | Native sensor calibration, temperature and interrupt support |

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
`hardware-integration-20261016i`, `hardware-integration-20261016j` and
`hardware-integration-20261016k`
preserve source comparisons, build artifacts
and source-pin verification. Integration reused component execution evidence;
no unchanged emulator or physical workload was repeated. The JPEG record explicitly
identifies its missing historical malformed-input runner and input archive.

The central uncached and cached eight-core diagnostics, GPU compute service and
NPU startup targets compile and link. The NPU convolution object also compiles. Build success does not establish complete-image execution.
The SMP central image differs through source-path strings and their address
references. Function locations and the checked instruction bodies match; its
separate identity and analysis are recorded in the integration evidence. Neither
uncached image has whole-machine or physical scheduler evidence.
The separate cached image has a preserved eight-core instruction-model scheduler
result and focused final correction checks. Its central build has a distinct
identity and compile/link evidence; neither cached image has physical qualification.

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
2. Implement an observable cached diagnostic result path and separate launcher
   admission for the integrated cached image.
   The integrated launcher safely returns pre-mutation refusals and freezes
   validated upload bytes before transfer. Its instruction and host checks pass;
   physical reset and clock behavior remain unqualified. After recovery passes,
   qualify all-core startup, interrupts, coherent memory, affinity and context
   preservation. ROM return alone does not establish scheduler operation.
3. Complete native platform ownership and memory reservations for DMA, IOMMU,
   GPU and NPU. Corrected inherited-clock guards now require forced-on control,
   software enable and effective-gate readback; focused checks pass. Account for remote firmware, all shared-memory users, cache
   attributes and outstanding bus traffic. Supply real admission/drain evidence
   before device commands or buffer reuse.
4. Bind the integrated camera capture and display scanout coordinators to native
   platform providers. Qualify the display PLL initial analog profile and bind
   atomic clock handoff, then implement safe shutdown/release. The PLL sequence
   and media gate/rate controllers are integrated with software evidence.
   Add panel-specific sequencing, live frame updates and wider camera modes.
   OV5640 module availability remains unconfirmed; the DSI panel is undecided.
   Keep Hawkeye 64 MP feasibility separate from the first camera implementation.
5. Bind the integrated WPA2 handshake and secure packet transport to native
   entropy and radio resources. Add IP networking, wider security modes, interrupts,
   sleep and reconnect. Extend Bluetooth links with pairing, hands-free
   control and voice transport. Establish actual SDIO/UART/power/pin bindings and
   radio identity before physical use. No microphone is available; recorded input
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
