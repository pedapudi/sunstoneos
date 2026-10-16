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
| [Secondary CPU continuation](cpu-continue.md) | Per-core interrupt preparation, translation tables, exception vectors; host and instruction checks pass | Coherent cached runtime and physical all-core startup |
| [Cached CPU preparation](cpu-coherence.md) | Revision-specific controls and matching EL3/EL1 RAM attributes; host/instruction checks pass | Coordinated continuation, exception/power obligations and physical coherence |
| [Eight-core diagnostic](a733-smp.md) | Dedicated scheduler image links; admission, selected instructions and layout checks pass | Independent recovery and bounded launcher; complete image has never executed |
| [DMA startup](dma-startup.md) | Shared clock/lock/counter ownership and deferred service activation; two-core model passes | Native reservations, handoff authority, physical transfer/drain |
| [System IOMMU](iommu.md) | Both-bank table construction, cold installation and fault observation; source/host checks pass | Exclusion and drain of every affected master; native mapping |
| [NPU startup](npu-startup.md) | Stable rail/clock ownership, PMIC reads and deferred activation; two-core model passes | Physical supply/domain qualification, complete command-memory admission |
| [GPU compute](gpu-compute-service.md) | Native workload publication, compiled fixed shader, command stream and output checking; service model passes | Physical firmware startup and numerical execution |
| [GPU core mode](gpu-compute-mode.md) | Observed single-core capability required before startup/submission; instruction/service checks pass | Physical identity and wider core modes |
| [GPU isolation](gpu-isolation.md) | Only exact provider success permits reclamation; focused instruction check passes | Proven request blocking and completion of all accepted external writes |
| [Wi-Fi initialization](radio-wifi.md) | Source-defined RF/firmware setup, explicit channel policy, passive scanning; packet/model checks pass | Security/data paths and physical radio binding |
| [Wi-Fi association](radio-association.md) | Open/WPA2 association and disconnect control; source/model checks pass; unresolved roaming remains retained | EAPOL, authentication/keys and data transport; WPA2 is still security-pending |
| [Camera analog startup](camera-platform.md) | Cold shared PHY setup and calibration readback; source/model checks pass | Native resource binding and actual memory/drain providers |
| [Camera capture](camera-capture.md) | All six native controllers compose in both port models; one NV16 frame becomes CPU-owned after shutdown, drain and fault scan | Actual providers, global mapping disposal and concurrent device ownership |
| [DSI video](dsi-video.md) | Four-lane RGB888/loose RGB666, continuous clock, bounded stop; source/model checks pass | Display engine, panel and physical scanout |
| [LCD1 timing](tcon-lcd.md) | DSI1 pixel-feed timing, authenticated start and bounded stop; source/model checks and native compilation pass | Display-engine framebuffer feed and real producer-drain evidence |
| [I2S capture](i2s.md) | Finite capture, DMA ownership and clock/reset/header-pin binding; host/emulator checks pass | Continuous capture, playback, Bluetooth PCM and attached microphone |

The camera path also has [OV5640 control](camera.md), [local CSI control](csi-receiver.md),
[ISP/VIPP pass-through routing](camera-route.md) and [single-frame DMA](camera-dma.md).
Display command transport and inherited-clock [PHY preparation](dsi-phy.md) are
integrated. Radio [verified firmware loading](radio-firmware.md) and
[continuous receive](radio-runtime.md) precede Wi-Fi initialization.

## Active parallel work

| Worktree | Assigned work |
| --- | --- |
| `a733-smp` | SRAM launcher and host transaction for the eight-core diagnostic |
| CPU worker, separate worktree | Full cached eight-core image using the integrated CPU/EL3 primitives |
| Display worker, separate worktree | Display-engine framebuffer scanout feeding LCD1/DSI1 |
| `radio-sdio` | Wi-Fi data frames and EAPOL transport; authentication and keys remain separate dependencies |
| UART worker, separate worktree | Native UART configuration, FIFO transfers, flow control and interrupt service |
| Storage worker, separate worktree | Read-only SD/MMC identification, capacity and block transfers |

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
`hardware-integration-20261016g` and `hardware-integration-20261016h`
preserve source comparisons, build artifacts
and source-pin verification. Integration reused component execution evidence;
no unchanged emulator or physical workload was repeated.

The central eight-core diagnostic, GPU compute service and NPU startup targets
compile and link. Build success does not establish complete-image execution.
The SMP central image differs through source-path strings and their address
references. Function locations and the checked instruction bodies match; its
separate identity and analysis are recorded in the integration evidence. Neither
image has whole-machine or physical scheduler evidence.

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
2. Finish the bounded eight-core launcher and cached CPU integration. The launcher
   remains under review: pre-mutation refusals need a safe FEL return, and upload
   readback must match the already validated artifact identities. Qualify
   physical startup, per-core interrupts, coherent memory, affinity and context
   preservation after recovery passes. Keep the complete diagnostic's execution
   status explicit; selected instruction checks do not establish scheduler operation.
3. Complete native platform ownership and memory reservations for DMA, IOMMU,
   GPU and NPU. Account for remote firmware, all shared-memory users, cache
   attributes and outstanding bus traffic. Supply real admission/drain evidence
   before device commands or buffer reuse.
4. Bind the integrated camera capture coordinator to native platform providers
   and complete the display-engine framebuffer feed
   through the integrated LCD1/DSI1 controllers. Add panel-specific sequencing and wider camera modes.
   OV5640 module availability remains unconfirmed; the DSI panel is undecided.
   Keep Hawkeye 64 MP feasibility separate from the first camera implementation.
5. Extend the integrated Wi-Fi association control with authentication/key
   management, data networking, interrupts, sleep and reconnect. Implement Bluetooth HCI, pairing, hands-free
   control and voice transport. Establish actual SDIO/UART/power/pin bindings and
   radio identity before physical use. No microphone is available; recorded input
   remains the application fixture while I2S and headset support proceed.
6. Resolve complete NPU descriptor access lengths, stride units, prefetch and
   instruction-family semantics before numerical device admission. Reuse the
   [compiled convolution](npu-compiler.md) and [saved SDK trace](nbg-conv-loader.md).
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
