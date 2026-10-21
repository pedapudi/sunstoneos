# SunstoneOS implementation audit

This directory records a source audit of SunstoneOS, the FreeRTOS-based
operating system for the Orange Pi Zero 3W (Allwinner A733: eight CPU cores,
six Cortex-A55 and two Cortex-A76, a GIC-600 interrupt controller and 12 GiB of DDR). The
audit looked for four kinds of problem: performance limits, design and cleanup
work, latent defects, and features still missing for bring-up. It then grouped
the confirmed findings into implementation packages.

- **Date:** 2026-10-20.
- **Code audited:** branch `bringup` at commit `70a4e63`, checked out as branch
  `audit/integration`.
- **Goal measured against:** [docs/goal.md](../goal.md). SunstoneOS must
  support the usable onboard hardware natively under FreeRTOS, run the mimus
  speech recognizer natively, and complete warm recognition in at most 0.05
  times the audio duration. Recognition currently takes about 2.2 times the
  audio duration on one core.

No physical board test was run for this audit. Every statement about runtime
behavior comes from reading the code, from host checks against modeled
hardware, or from evidence recorded before the audit. One A733 board is
connected to the development host over USB. If it loses its connection it
cannot be reconnected, so the audit did not issue any command to it.

## Reports

| Report | Contents |
| --- | --- |
| [performance.md](performance.md) | Performance changes, ordered by their effect on warm recognition latency, then data movement, then boot and build time |
| [redesign.md](redesign.md) | Design changes and cleanup: duplicated mechanisms, composition by textual patches, stale documentation |
| [latent-issues.md](latent-issues.md) | Confirmed defects, ordered by severity, each with a failure scenario, a fix and a verification method |
| [bringup-gaps.md](bringup-gaps.md) | Work still needed to meet the accepted goal: bootable images, persistent boot, recovery, multicore qualification and missing device features |

Each finding has an identifier made of the audited area and a number, such as
`storage-3`. All four reports and the tables below use these identifiers.

## Terms

- **Status levels.** The project records support as *implemented* (code
  exists), *software-tested* (host checks pass against modeled hardware),
  *physically working* (the function has worked on the board) or *qualified*
  (the function meets its acceptance criteria on the board). Almost every
  device path is implemented and software-tested; none is qualified.
- **Retained resources.** When a driver cannot tell whether hardware finished
  an operation, it keeps every clock, rail, pin, memory and lock lease it holds
  until the board is powered off. This is intended project policy. The audit
  reports it only where a harmless condition is wrongly treated as uncertain.
- **FEL.** The Allwinner boot-ROM USB mode that the host uses to load images
  into RAM. All current runtime images are loaded this way.
- **Tick.** The FreeRTOS scheduler tick, 1 ms. Most driver tasks advance one
  step per tick, which is the source of many performance findings.

## Method

1. **Audit.** Twenty-seven auditors each read one part of the tree. Nineteen
   covered subsystems: CPU startup, SMP (symmetric multiprocessing), runtime
   memory, DMA and IOMMU, clocks, power and pins, USB, storage, radio and
   networking, Bluetooth, audio, display, camera, GPU, NPU, video codecs,
   security and miscellaneous devices, buses and PCIe, the supervisor and
   applications, and mimus performance. Eight covered the build system, host
   tools, the check harness, bring-up gaps, and four cross-cutting concerns:
   cache coherence, concurrency, address-range bounds and code duplication.
   Each auditor recorded findings with a category, a severity and code
   evidence.
2. **Verification.** Every latent-bug finding, and every finding of high or
   critical severity, went to three independent verifiers. One re-read the
   code, one checked the finding against the project's documented design
   intent, and one tried to construct a concrete reproduction. A finding was
   kept only when at least two of the three supported it. Every other finding
   went to one verifier who re-read the code. Verifiers could narrow a
   finding, change its severity or replace its proposed fix; the reports show
   the corrected version.
3. **Policy filter.** Behavior that follows a documented project policy was
   not counted as a defect unless the code contradicts its own documented
   contract. These policies are: retaining resources after uncertain failures;
   refusing to reap or close an application while hardware completion is
   unknown; modeling hardware in host checks; preferring fixed board
   configuration to general frameworks; adding abstractions only for a
   concrete consumer; and temporarily accepting opaque firmware.
4. **Planning.** Findings that describe the same problem were merged under one
   kept identifier. The remaining findings were either assigned to an
   implementation package with disjoint file ownership, or deferred with a
   reason.

## Counts

Auditors recorded 297 findings. Verification confirmed 286 and refuted 11.

Confirmed findings by category and severity:

| Category | High | Medium | Low | Total |
| --- | ---: | ---: | ---: | ---: |
| Latent bug | 11 | 15 | 31 | 57 |
| Bring-up gap | 9 | 27 | 10 | 46 |
| Performance | 6 | 34 | 34 | 74 |
| Design | 0 | 29 | 26 | 55 |
| Cleanup | 0 | 4 | 50 | 54 |
| **Total** | **26** | **109** | **151** | **286** |

No finding was rated critical.

Several auditors reported the same problem from different directions. The plan
merges 24 such groups. For example, `cpu-startup-3`, `clock-power-pins-5`,
`mimus-perf-1` and `bringup-gaps-6` all report that the CPU clusters run at the
494 MHz left by the boot loader, and the plan keeps `cpu-startup-3`. The
counts above include every confirmed finding before merging.

## Implementation packages

Each package owns a disjoint set of files so packages can be implemented in
parallel and merged without conflicts. A package's findings list includes the
duplicates merged into its kept findings. Priority 1 is the most urgent.
Detailed plans, file ownership, verification commands and risks are in the
audit plan; the findings themselves are described in the four reports.

| Package | Scope | Findings | Priority | Implementation status |
| --- | --- | --- | ---: | --- |
| `multicore` | CPU startup, EL3 monitor, the FreeRTOS SMP port and the multicore runtime. Reserved for the primary agent. | runtime-memory-1, runtime-memory-2, build-system-12, smp-1, smp-2, smp-4, smp-5, smp-7, smp-9, smp-10, build-system-11, smp-8, cpu-startup-1, cpu-startup-4, cpu-startup-5, cpu-startup-6, cpu-startup-7, cpu-startup-8, cpu-startup-10, mimus-perf-6, smp-3, bringup-gaps-8, x-concurrency-2, mimus-perf-7, bringup-gaps-2, supervisor-apps-9, bringup-gaps-10, x-concurrency-7, x-concurrency-9, x-duplication-4, buses-pcie-10, supervisor-apps-2, x-concurrency-5, npu-9, x-coherence-10 | 1 | planned |
| `contention-polling` | Retry transient lock and registry BUSY results within the deadline instead of faulting; bound the counter-clock lock by time; poll the PMIC, Type-C and USB host faster | clock-power-pins-2, usb-3, cpu-startup-11, clock-power-pins-7, x-concurrency-3, usb-9, clock-power-pins-6, usb-4, clock-power-pins-11, clock-power-pins-14 | 1 | planned |
| `display-hdmi` | Fix the display-engine replay race with frame end, the HDMI PHY lock wait and the HDMI-audio Normal-NC shareability check; reduce display polling cost | display-1, display-2, x-coherence-1, audio-5, display-6, display-7, display-8, display-9, display-10 | 1 | planned |
| `video` | Accept the board's 26 MHz crystal in the decoder clock check; retry BUSY in video-engine register access; reduce guard scanning and per-tick pacing | video-1, video-2, video-3, video-4, video-11 | 1 | planned |
| `bluetooth` | Release unhandled HCI events, schedule SCO audio on a fixed period, accept a legal codec change during audio, discard peer data during local disconnect, share one range helper | bluetooth-1, bluetooth-3, bluetooth-8, bluetooth-7, bluetooth-10 | 1 | planned |
| `host-tools` | Size the FEL timeout by bytes moved, append to session journals, include libraries in evidence, bind board-test queues to a USB port, check launcher text replacement | host-tools-1, host-tools-2, host-tools-3, host-tools-4, host-tools-5, host-tools-8, host-tools-9, host-tools-10, host-tools-11, supervisor-apps-7, x-bounds-1, x-duplication-3 | 1 | planned |
| `radio` | Fix the radio startup deadlock and the address loss on group-key rekey, tolerate entropy contention, check the bootstrap time budget, consolidate SHA-256 and message encoders | radio-net-4, radio-net-5, radio-net-1, radio-net-11, radio-net-7, radio-net-13, supervisor-apps-13, supervisor-apps-14 | 2 | planned |
| `storage` | Accept OUT_OF_RANGE at the card's last sector, give each SD write its own busy budget, move whole FIFO blocks per step | storage-3, storage-4, storage-1, storage-2, storage-8, storage-9 | 2 | planned |
| `buses` | Wait for PCIe endpoint readiness and handle CRS; raise the UART receive trigger level and cut per-byte cost; batch PCIe PHY writes; UART and SPI cleanup | buses-pcie-1, buses-pcie-2, buses-pcie-3, buses-pcie-6, buses-pcie-7, buses-pcie-8, buses-pcie-11, buses-pcie-12, buses-pcie-13, x-concurrency-4 | 2 | planned |
| `build-repro` | Reproducible builds: path prefix maps, complete build identities, inputs checked against the source lock, header checks for images that reuse old objects | x-duplication-1, build-system-4, bringup-gaps-5, build-system-3, build-system-2, build-system-6, build-system-9, build-system-14, usb-10, build-system-15, bringup-gaps-13 | 2 | planned |
| `mimus-warm` | Keep the loaded model between recognition requests, time from PCM input to text, verify integrity once per asset generation, yield on a time quantum, give the recognition library its own compiler flags | mimus-perf-5, bringup-gaps-7, mimus-perf-9, smp-6, runtime-memory-3, build-system-1, mimus-perf-10, bluetooth-2, audio-8, mimus-perf-12 | 2 | planned |
| `usb0-service` | One shared USB0 vendor service that runs until idle instead of one packet per tick; diagnostics protocol resynchronization; cheaper NPU health polling | usb-1, x-bounds-2, supervisor-apps-4, x-concurrency-1, video-5, usb-2, x-bounds-3, supervisor-apps-5, x-duplication-7, usb-6, npu-2 | 3 | planned |
| `dma-audio` | Fix I2S stream end-of-input underrun, START admission and cancel of an unstarted stream; separate the DMA service from the legacy I2S capture path; DMA cleanup | audio-3, audio-4, audio-12, audio-7, audio-10, audio-13, dma-iommu-3, audio-11, dma-iommu-1, dma-iommu-5, dma-iommu-6, dma-iommu-7, x-coherence-9 | 3 | planned |
| `security` | Treat benign thermal, ADC, LRADC, RTC and watchdog conditions as benign; scrub and consolidate the crypto engine; share the PERI0 clock check; faster SHA-256 | security-misc-2, security-misc-3, security-misc-4, security-misc-5, security-misc-8, security-misc-9, security-misc-6, x-coherence-3, security-misc-11, supervisor-apps-11 | 3 | planned |
| `camera` | Native camera: selectable color bars, a module profile with power-down only, buffer canaries, BUSY-tolerant authority checks; cheaper ISP0 guard scans | camera-2, camera-3, camera-4, camera-5, camera-11 | 3 | planned |
| `checks-harness` | Make sanitizer builds stop on undefined behavior, add a shared harness module and a check index, complete the Python requirements, remove dead headers | checks-harness-1, checks-harness-3, checks-harness-10, checks-harness-11, checks-harness-12, checks-harness-14, checks-harness-15 | 3 | planned |

Three constraints apply to every package:

- Verification uses host builds and host checks only. No package runs a
  board launcher, a FEL tool or any program that opens a USB device.
- Packages do not edit [docs/hardware.md](../hardware.md) or
  [docs/handoff.md](../handoff.md); the integrator updates them after merging.
- Changing an image changes its identity. Images that were frozen with
  recorded evidence keep that evidence; a rebuilt image needs new evidence.

## Deferred findings

The following confirmed findings are not assigned to a package in this round.
A finding listed with merged identifiers stands for all of them.

| Finding | Reason for deferral |
| --- | --- |
| cpu-startup-2 (with storage-6, bringup-gaps-3) | Persistent boot is assigned to the primary agent in docs/handoff.md. Exclusion of the E902 coprocessor as a remote writer is unresolved, and card writes need an established recovery procedure. |
| cpu-startup-3 (with clock-power-pins-5, mimus-perf-1, bringup-gaps-6) | CPU frequency and voltage scaling cannot be finished offline. Mapping the CPU rails (DCDC5 and DCDC3), reading the voltage-frequency bin from eFuse and writing voltages could hang the only board, which cannot be reconnected. Needs primary-agent planning and approval for physical tests. |
| cpu-startup-9 (with clock-power-pins-12, x-concurrency-8, x-duplication-6, buses-pcie-9, security-misc-10) | Consolidating the task-context check touches about 15 native adapters owned by other packages and FreeRTOS port internals changed by `multicore`. Do after those merge. |
| runtime-memory-5 | Needs pool initialization moved into the platform boot path owned by `multicore`. Low severity. |
| runtime-memory-6 (with x-coherence-4) | A shared identity-range helper would touch GPU, USB, NPU, HDMI-audio and DMA mapping checkers owned by several packages. |
| runtime-memory-7 | Adopting a helper for the CPU-to-device ownership change spans about 35 sites owned by many packages. Low value. |
| runtime-memory-8 | Merging `memmove.c` into `memory.c` edits many CMake source lists owned by several packages; `multicore` already rewrites `memory.c`. |
| dma-iommu-2 | DMA0 interrupt delivery needs GIC routing from the `multicore` interrupt table and physical interrupt qualification. Low severity. |
| dma-iommu-4 | Generalizing the DMA pool session for UART and HDMI is a large design change that overlaps recently integrated DMA work and an unmerged DMA pool-session branch. |
| dma-iommu-8 | Saves microseconds per tick; wait until hardware function is established. |
| dma-iommu-9 | Low-severity cleanup; display and camera code in other packages read the mask fields directly. |
| clock-power-pins-1 | Low severity. A separate camera-bus lock changes the layout of `struct a733_platform_locks`, which breaks reuse of old objects (`x-duplication-1`). Do after `build-repro`. |
| clock-power-pins-9 | Merging three S_TWI0 protocol engines needs engine mode changes and migration of single-core images, and conflicts with the TWI changes in `contention-polling`. |
| clock-power-pins-10 | Unifying rail authority touches media clock, G2D, deinterlace, NPU and GPU managers and overlaps the G2D and NPU work. No current defect. |
| clock-power-pins-13 | Moving I2S to the bank-qualified pin API changes I2S platform initialization while `dma-audio` edits the same file. |
| usb-8 | USB host detach, reattach and storage recovery is a proposed workstream for the USB host owner in docs/handoff.md. Large. |
| storage-5 | A faster update profile with 4 KiB chunks changes the session format, commitment-chain fixtures and eMMC limits. Do after `storage`. |
| storage-7 | Update-plan tooling depends on the card layout and boot-slot decisions in the primary agent's persistent-boot work. |
| radio-net-2 | Batching the radio worker gains little at the 400 kHz bus rate and conflicts with `apps/board/main.c` in `contention-polling`. Do with the faster SDIO mode. |
| radio-net-3 | Splitting receive aggregates needs hardware evidence that the AIC8800 accepts split CMD53 reads or a larger request size. |
| radio-net-6 | Four-bit, high-speed and IDMA SDIO need physical qualification and should reuse the SD timing code after `storage` lands. |
| radio-net-8 | Coalescing patch-table writes departs from the vendor per-word sequence and needs a hardware readback check. |
| radio-net-9 | Recovery from unsolicited link loss is a proposed workstream for the radio owner and needs evidence about firmware key state. |
| radio-net-10 | HT and A-MSDU receive matter only once HT and a faster bus are enabled. Low severity. |
| radio-net-12 | Refactoring the radio transport is large and risky; do after the radio defect fixes and the SDIO speed work. |
| radio-net-14 | Fewer Ethernet frame copies matter only once SDIO is faster. Low severity. |
| bluetooth-5 | Reusing link slots after disconnect needs a design for quarantining retired handles across the ACL, SCO and security code, and conflicts with the documented single-session scope. |
| bluetooth-6 | Packet-loss tolerance should follow a physical measurement of the eSCO loss rate. |
| audio-1 | A guard band in the cyclic receive copy changes a physical-admission condition that is already blocked and needs board timing evidence. |
| audio-2 | Low severity. Measure the real FIFO margin on hardware before redesigning capture. |
| display-3 | Powering DE_SYS and VO0 for DSI and deinterlace overlaps the proposed G2D workstream, which coordinates the shared DE_SYS and IOMMU contracts. |
| display-4 | Consolidating PCK power domains overlaps the same G2D and DE_SYS work. Low value. |
| display-5 | Consolidating the HDMI, DisplayPort and GPU-display framebuffer pipelines is large and overlaps recent GPU and G2D work. |
| camera-1 | Re-arming and continuous capture is a large lifecycle design that must keep the producer-quiet and drain proofs; no camera module is attached to validate it. |
| camera-6 | Changing the stop order depends on unverified OV5640 register semantics and departs from the Linux reference. |
| camera-7 | Moving the camera application to the board framework conflicts with `usb0-service`, which migrates the camera USB file. |
| camera-8 | Merging the register-program interpreters changes polling granularity and needs requalification. |
| camera-9 | Boot-time sensor configuration cost; no sensor is attached. |
| camera-10 | A CAM2 profile needs MCLK2 register and pin values from the pinned device tree and a review of receiver port 1. |
| gpu-1 | Batching the GPU worker touches service code just reworked by the GPU lifecycle integration; docs/handoff.md asks to confirm that lifecycle first. The GPU is not on the recognition path. |
| gpu-2 | A GPU compute interface for mimus needs a new DRAM reservation and a feasibility measurement. |
| gpu-3 | The GPU owner should schedule this after the lifecycle integration; it needs physical evidence that the firmware emits these events. |
| gpu-5 | GPU reuse after CLOSE overlaps the just-integrated disposal lifecycle and mixed-device serialization that is not yet designed. |
| gpu-6 | Clock-check consolidation must keep the post-operation resampling; low value in actively changing code. |
| gpu-7 | Grouped with runtime-memory-6. |
| gpu-8 | Fold the documentation fix into the GPU owner's next update of docs/gpu.md. |
| gpu-9 | The MMU cache flush should gain a consumer when mappings change at runtime. |
| gpu-10 | Per-word check cost matters only after gpu-1. |
| npu-1 | Per-submit hashing lives in the core of the proposed NPU runtime lifecycle workstream. |
| npu-3 (with x-coherence-5) | Avoiding per-submit republication needs an explicit integrity-policy decision and overlaps the NPU lifecycle workstream. |
| npu-4 | A completion notification changes the NPU service protocol; overlaps the NPU lifecycle workstream. |
| npu-5 | Layouts derived from the assembly plan have no consumer that uses several model families, and would invalidate recorded layout evidence. |
| npu-6 | Merging the two network-binary assemblers should wait for a concrete consumer. |
| npu-7 | A mimus network binary that keeps mimus quantization needs compiler and quantization research. |
| npu-8 | Larger NPU pages and reservations depend on mimus-scale NPU models that do not exist yet. |
| npu-10 | Changing the bootstrap encoder alters model and QEMU expectations across NPU checks. |
| npu-11 | Fold documentation fixes into the NPU owner's workstream. |
| video-6 | Consolidating the decoder and encoder platforms is large; `video` fixes both copies directly. |
| video-7 | Consolidating per-application video scaffolding is large and overlaps launcher edits in `host-tools`. |
| video-8 | HEVC integration is held on a separate unreviewed branch. |
| video-9 | A larger JPEG profile should follow the first physical decode of the existing fixture. |
| video-10 | More H.264 High references need a memory-layout redesign and overlap a separate unreviewed branch. |
| security-misc-1 | A thermal throttling policy depends on CPU frequency scaling (deferred) and qualified thermal-sensor acquisition. |
| security-misc-7 | WDT0 consolidation touches launchers owned by `multicore`. Low severity. |
| buses-pcie-4 | Streaming UART receive with DMA overlaps a separate unmerged UART DMA proposal and needs a cyclic DMA design. |
| buses-pcie-5 | PCIe Gen2, BAR and MSI support has no concrete endpoint consumer. |
| supervisor-apps-3 (with bringup-gaps-11) | A shared extended contract version depends on the resident-contract design and boot-patch rewrite in `multicore`. |
| supervisor-apps-6 | Snapshot consolidation edits boot patches owned by `multicore`. |
| supervisor-apps-10 | Unifying the board and NPU provider roots is a large change that overlaps the NPU and OS-integration workstreams. |
| supervisor-apps-12 | Service-task affinity matters only with multicore recognition; `apps/board/main.c` belongs to `contention-polling` this round. |
| mimus-perf-2 | The NEON matrix kernel is in the external mimus repository, which this project does not modify. |
| mimus-perf-3 | The SDOT kernel is in the external mimus repository. |
| mimus-perf-4 | The CRC code is in the external mimus repository; `mimus-warm` removes the per-request CRC instead. |
| mimus-perf-8 | NEON quantization is in the external mimus repository. |
| mimus-perf-11 | Larger USB messages change the host protocol and need an approved board measurement. Low severity. |
| build-system-5 (with x-duplication-2, build-system-13, supervisor-apps-8) | Applying patches without fuzz requires regenerating patch sets that `multicore` rewrites; do immediately after it lands. |
| build-system-7 | Generating identities at build time is a redesign across 46 recipes; follow `build-repro`. |
| build-system-8 | Shared object libraries for board-derived images are large and conflict with identity changes. Low severity. |
| build-system-10 | Removing the sticky cache setting edits about 20 recipe fragments and changes the documented opt-in procedure; follow `build-repro`. |
| host-tools-6 | Consolidating supervised wrappers is large and must keep a central reviewed capture-limit table; follow `host-tools`. |
| host-tools-7 | Consolidating FEL session loops follows host-tools-1. Low severity. |
| checks-harness-2 | Promoting evidence baselines into fixtures touches many checks owned by other packages. |
| checks-harness-4 | Tightening refusal matching touches many checks owned by other packages; follow the shared harness module. |
| checks-harness-5 | The G2D check chain belongs to the G2D workstream. |
| checks-harness-6 | Launcher-check consolidation should follow launcher parameterization in `host-tools`. |
| checks-harness-7 | Moving the ELF layout helper changes evidence snapshot names. |
| checks-harness-8 | Ranged emulator hooks touch many checks owned by other packages. |
| checks-harness-9 | Redesigning the evidence store changes recorded hashes. Low severity. |
| checks-harness-13 | Removing hard-coded paths follows adoption of the shared harness module. |
| bringup-gaps-1 (with supervisor-apps-1) | An integrated multi-device image is the proposed OS and mimus integration workstream and needs the `multicore` interrupt table first. |
| bringup-gaps-4 | Boot slots, update and rollback belong to the primary agent's persistent-boot work and need established recovery before card writes. |
| bringup-gaps-9 | Streaming recognition requires moving to a mimus revision with the streaming interface and re-establishing output parity. |
| bringup-gaps-12 | A central recognition memory map touches files in `host-tools`, `usb0-service` and `mimus-warm`. Low severity. |
| x-coherence-2 | Generalizing the Normal-NC DMA policy should wait for a second Normal-NC DMA consumer. |
| x-coherence-6 | MMIO accessor consolidation spans about 59 files across all packages. |
| x-coherence-7 | Unifying cache-operation enums spans DMA and NPU headers in different packages. |
| x-coherence-8 | GPU per-page cache maintenance is deferred with the GPU work. |
| x-concurrency-6 | A shared GIC SPI binding touches about 11 applications and drivers and the `multicore` interrupt table. |
| x-bounds-4 (with x-duplication-9) | A global range-helper header spans about 190 sites owned by many packages. |
| x-duplication-5 | Consolidating board-application recipes is large and conflicts with identity changes in `build-repro`. |
| x-duplication-8 | Building returning images in CMake is large; `build-repro` first adds the header check. |

## Refuted findings

Verification rejected the following findings. They are listed so readers know
they were considered.

- runtime-memory-4: `_write` sends stdio output to `platform_puts` one byte at
  a time.
- clock-power-pins-3: The rail ledger turns a plain OPENING deadline expiry
  into a ledger-wide terminal fault that cascades to the PMIC provider, TWI
  and USB health.
- clock-power-pins-4: Registry-wide and lock-wide fault latches couple every
  subsystem's local uncertainty to the USB evidence channel.
- clock-power-pins-8: The TWI native pin-health check uses the stale prepare
  deadline, so TWI11 cannot run transactions after the boot deadline.
- usb-5: Writing at most 512 bytes per poll makes TinyUSB send a zero-length
  packet after every packet in the middle of a response.
- usb-7: The mimus and diagnostics runtime depends on ROM-initialized USB0
  clocks and bypasses the clock registry and register mutex.
- bluetooth-4: Incoming SDP and RFCOMM during pairing are refused with
  Security Block, and L2CAP has no Pending path.
- bluetooth-9: RFCOMM rejects spec-legal N1-sized UIH frames that carry a
  piggybacked credit.
- audio-6: Two cyclic staleness checks in different layers have conflicting
  outcomes, and a late worker poll becomes permanent RECOVERY.
- audio-9: Cyclic RX stop discards completed but uncopied periods, even after
  a verified drain.
- gpu-4: Firmware CLEANUP_BUSY on a healthy CLOSE is treated as fatal instead
  of retried.

## Limits of this audit

- No physical board test was run. Performance estimates in
  [performance.md](performance.md) are unmeasured on the board, and fixes in
  [latent-issues.md](latent-issues.md) are verified against modeled hardware
  only.
- Findings describe commit `70a4e63`. Work merged after that commit may change
  or resolve them.
- Some packages exclude files held on other unmerged branches (HEVC decode,
  H.264 interframe references, UART DMA, DMA pool sessions). Those branches
  were not audited.
