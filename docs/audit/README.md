# SunstoneOS implementation audit

This directory records a source audit of SunstoneOS, the FreeRTOS-based
operating system for the Orange Pi Zero 3W (Allwinner A733: eight CPU cores,
six Cortex-A55 and two Cortex-A76, a GIC-600 interrupt controller and 12 GiB of DDR). The
audit looked for four kinds of problem: performance limits, design and cleanup
work, latent defects, and features still missing for bring-up. It then grouped
the confirmed findings into implementation packages. Nine rounds of fixes,
features and consolidation based on those packages are merged; [Implementation rounds](#implementation-rounds)
records what each package changed and which findings remain deferred.

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
| [board-test-plan.md](board-test-plan.md) | RAM-only board tests ranked by the risk of losing the USB connection; none may run until the board can be reconnected |

Five recognition-speed changes belong in the mimus engine rather than in
SunstoneOS. They are tracked as mimus issues:

- [32-bit accumulation and four-row weight reuse in the AArch64 packed matrix kernel](https://github.com/pedapudi/mimus/issues/61)
- [`SDOT` dot-product kernel for int8-weight linear layers with exact int16 activations](https://github.com/pedapudi/mimus/issues/62)
- [More than two parallel workers and uneven splits across cores of different speeds](https://github.com/pedapudi/mimus/issues/63)
- [NEON activation quantization and max-abs scans on AArch64](https://github.com/pedapudi/mimus/issues/64)
- [Table-driven or hardware CRC32 for model verification](https://github.com/pedapudi/mimus/issues/65)

Each finding has an identifier made of the audited area and a number, such as
`storage-3`. The four audit reports and the tables below use these identifiers.

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

## Implementation rounds

Nine rounds of work followed the audit and are merged into `bringup`, followed
by WPA3-Personal support and the first packages of a performance-gated
completion tranche for the CPU, GPU, NPU and DDR:

- **Round one** ran eighteen packages: seventeen implemented by parallel
  workers, and the multicore package implemented by the primary agent. Two of
  the worker packages were added to the original plan and took findings from
  its multicore list or its deferred list: `runtime-string` took the runtime memory routines, and
  `standalone-boot` took persistent boot.
- **Round two** ran thirteen follow-up packages. Most repaired problems that
  round-one packages reported in files they did not own; four took deferred
  findings. The primary agent added the resident contract and the fixed
  interrupt table in the same round.
- **Round three** ran thirteen packages. One built the integrated OS image; the
  others took deferred findings.
- **Round four** ran five packages: I2S microphone capture in the integrated
  OS image, CPU clock and voltage control, radio transport limits, build and
  check-harness consolidation, and camera and JPEG findings.
- **Round five** ran six packages: cold SD admission in the stage-2 loader,
  a remote-writer check, cyclic I2S receive, USB host detach in the TinyUSB
  adapter, power domains and the TWI9 camera bus, and the native HEVC decoder.
- **Round six** ran sixteen feature packages. Each took device functions that
  [docs/hardware.md](../hardware.md) listed as still required. The NPU package
  also took two deferred findings.
- **Round seven** consolidated duplicated helpers. Four shared headers were
  merged first; ten packages then moved the private copies in their
  directories onto them.
- **Round eight** ran six packages: one AIC8800 module owner for Wi-Fi and the
  Bluetooth headset, one build recipe for the boot-CPU-only images, board
  service task and watchdog cleanup, a PCIe NVMe board image, longer H.264
  sequences, and repairs to three model checks.
- **Round nine** brought the shared-helper, PCIe, build, crypto and OS image
  documents in line with the code, removed unused fuzzy-patch pins, and let
  one DI301 deinterlacer session run up to four jobs with motion feedback from
  the previous job.
- **WPA3-Personal** pins OpenSSL 3.5.9 and builds a minimal static libcrypto
  seeded only from the radio entropy pool. Host checks run the SAE exchange
  against the IEEE 802.11 Annex J vectors and an access-point peer. Association
  with SAE stays refused until external authentication and protected
  management frames exist, and no board image links SAE yet.
- **The performance-gated tranche** is described in its own section below.

Each package owned a disjoint set of files, so packages in one round could be
implemented in parallel and merged without conflicts. Every package except one
passed its final review before it merged. The camera streaming package in round
six merged with one open issue from its final review; its row in the round-six
table describes the issue. In the tables below, **done** means the review
accepted the package as complete; the outcome column notes any assigned
finding that was narrowed, met through an alternative fix that the verifiers
accepted, or left. **Partial** means the package reported part of its assigned
work as not done; the outcome column gives the reason. The findings column lists audit
identifiers; a package that only repaired checks, builds or review notes, or
that worked from the feature plan alone, lists none.

Three constraints applied to every package:

- Verification used host builds, emulators and host models only. No package
  ran a board launcher, a FEL tool or any program that opens a USB device.
- Packages in rounds one to five, seven and eight did not edit
  [docs/hardware.md](../hardware.md) or [docs/handoff.md](../handoff.md). In
  round six, several packages edited their own rows of docs/hardware.md as
  their plans asked. The integrator updated the other rows and
  docs/handoff.md after merging each round.
- Changing an image changes its identity. Images that were frozen with
  recorded evidence keep that evidence; a rebuilt image needs new evidence.

No physical board test ran in any round, so no change below is physically
tested or qualified.

### Round one

| Change | Package | Findings addressed | Outcome |
| --- | --- | --- | --- |
| EL3 recovery cleanup that re-derives the GIC bank count and clears shared peripheral interrupts enabled during a run; ordered counter reads; a CPU-identity check that tests both affinity levels; an eight-core emulator scheduling soak; the mimus asynchronous two-core pipeline; corrected SMP status records | `multicore` | cpu-startup-1, cpu-startup-5, cpu-startup-7, x-coherence-10, smp-9, mimus-perf-7; smp-2 and bringup-gaps-10 in emulation only; smp-1, smp-8 and build-system-11 in part | Partial. The uncached eight-core diagnostic is marked superseded instead of corrected (smp-1). A compile-time check refuses a configuration with other than eight cores, but include order still selects the configuration (smp-8, build-system-11). Physical eight-core scheduling, recognition on the A76 cores and the lock and polling measurements need the board. Not done in any round: cpu-startup-4, cpu-startup-6, cpu-startup-8, cpu-startup-10, smp-3, smp-4, smp-5, smp-7, smp-10, x-concurrency-2, x-concurrency-7, x-concurrency-9, mimus-perf-6 and bringup-gaps-8. Later rounds addressed bringup-gaps-2, supervisor-apps-9, supervisor-apps-2, x-concurrency-5, npu-9, x-duplication-4 and buses-pcie-10 from this package's list. |
| Word-sized `memcpy`, `memmove` and `memset` that are safe at EL3 with the MMU off, and an ELF check that refuses unsafe library routines in EL3 code | `runtime-string` | runtime-memory-1, runtime-memory-2, build-system-12 | Done. |
| Bounded retry of transient lock and registry BUSY results; a time-bounded fair counter-clock lock; PMIC polls that advance several bus phases; faster Type-C and USB host polling | `contention-polling` | clock-power-pins-2, usb-3, cpu-startup-11, clock-power-pins-7, x-concurrency-3, usb-9, clock-power-pins-6, usb-4, clock-power-pins-14 | Done. clock-power-pins-11, which proposes caching the TWI adapter's registry slot, was assigned but is not among the addressed findings. |
| Display-engine replay race with frame end, HDMI PHY lock wait, HDMI-audio Normal-NC shareability check, cheaper display polling | `display-hdmi` | display-1, display-2, x-coherence-1, audio-5, display-6, display-7, display-8, display-9, display-10 | Done. For display-9 the DSI task yields while the PLL holds the register mutex, an alternative the verifiers accepted, instead of batching inside the PLL driver. |
| Decoder admission of the board's 26 MHz crystal, BUSY retry in video-engine register access, cheaper guard scans and per-tick pacing | `video` | video-1, video-2, video-3, video-4, video-11 | Done. |
| Release of unhandled HCI events, fixed-period SCO audio, acceptance of a legal codec change during audio, discard of peer data during a local disconnect, one shared range helper | `bluetooth` | bluetooth-1, bluetooth-3, bluetooth-8, bluetooth-7, bluetooth-10 | Done. |
| FEL timeouts sized by bytes moved, append-only session journals, libraries in evidence, board-test queues bound to a USB port, checked launcher text replacement | `host-tools` | host-tools-1, host-tools-2, host-tools-3, host-tools-4, host-tools-5, host-tools-8, host-tools-9, host-tools-10, host-tools-11, supervisor-apps-7, x-bounds-1, x-duplication-3 | Done. Runners that check the binding refuse a queue with no recorded port, so both live queues must be bound before the next supervised launch. |
| Stage-2 loader, two-slot card layout with fallback, and the install, update and rollback procedure | `standalone-boot` | cpu-startup-2, storage-6, bringup-gaps-3, bringup-gaps-4 | Partial. Copy and hash verification at EL3 with caches off are implemented and modeled. The loader built no launch contract; round three added it. Native cold SD admission refused, because no fixed-board subset of the clock, pin, rail and PMIC providers exists at EL3; round five admits the SD controller state that boot0 leaves instead. Remote-writer exclusion is unresolved. |
| Radio startup deadlock, address loss on group-key rekey, entropy contention, a bootstrap time-budget check, shared SHA-256 and message encoders | `radio` | radio-net-4, radio-net-5, radio-net-1, radio-net-11, radio-net-7, radio-net-13, supervisor-apps-13, supervisor-apps-14 | Done. The budget check shows that bootstrap needs at least 46.7 s at one data line and 400 kHz, more than the 40 s supervised bound, so the radio board test waits for a qualified faster SDIO mode. |
| Acceptance of OUT_OF_RANGE at the card's last sector, a separate busy budget for each SD write, whole-FIFO transfers per step | `storage` | storage-3, storage-4, storage-1, storage-2, storage-8, storage-9 | Done. |
| PCIe endpoint readiness and CRS handling, a higher UART receive trigger level and lower per-byte cost, batched PCIe PHY writes, UART and SPI cleanup | `buses` | buses-pcie-1, buses-pcie-2, buses-pcie-3, buses-pcie-6, buses-pcie-7, buses-pcie-8, buses-pcie-11, buses-pcie-12, buses-pcie-13, x-concurrency-4 | Partial at merge. The UART model check needed stubs in a file the package did not own, and the PCIe clock-reference record needed an entry. Round two applied both. |
| Reproducible builds: path prefix maps, complete build identities, inputs checked against the source lock, header checks for images that reuse old objects | `build-repro` | x-duplication-1, build-system-4, bringup-gaps-5, build-system-3, build-system-2, build-system-6, build-system-9, build-system-14, usb-10, build-system-15, bringup-gaps-13 | Done. |
| Loaded model kept between recognition requests, PCM-to-text timing, integrity verification once per asset generation, time-quantum yields, separate compiler flags for the recognition library | `mimus-warm` | mimus-perf-5, bringup-gaps-7, mimus-perf-9, smp-6, runtime-memory-3, build-system-1, mimus-perf-10, bluetooth-2, audio-8, mimus-perf-12 | Done. The dot-product instruction flag waits for a board record of `ID_AA64ISAR0_EL1`. Round two added the audio-8 ordering assertion in a file this package did not own. |
| One shared USB0 vendor service that runs until idle, diagnostics protocol resynchronization, cheaper NPU health polling | `usb0-service` | usb-1, x-bounds-2, supervisor-apps-4, x-concurrency-1, video-5, usb-2, x-bounds-3, supervisor-apps-5, x-duplication-7, usb-6, npu-2 | Done. Thirteen application USB sources keep their own USB0 copies; the camera image moved to the shared service in round three. |
| I2S end-of-input underrun, START admission and cancel of an unstarted stream; the DMA service separated from the legacy I2S capture path; DMA cleanup | `dma-audio` | audio-3, audio-4, audio-12, audio-7, audio-10, audio-13, dma-iommu-3, audio-11, dma-iommu-1, dma-iommu-5, dma-iommu-6, dma-iommu-7, x-coherence-9 | Done. |
| Benign thermal, ADC, LRADC, RTC and watchdog conditions treated as benign; crypto-engine scrub and consolidation; one PERI0 clock check; faster portable SHA-256 | `security` | security-misc-2, security-misc-3, security-misc-4, security-misc-5, security-misc-8, security-misc-9, x-coherence-3, security-misc-6, security-misc-11, supervisor-apps-11 | Partial. No shared retained-controller core for thermal, ADC and LRADC was built (security-misc-6): the thermal rule counts any nonzero entry result as held, which would turn an ADC or LRADC denial without ownership into an assertion. No ARMv8 SHA2 instruction path was added (security-misc-11): the digest runs at EL0, where the feature registers cannot be read, and no image records them. |
| Selectable color bars, a camera module profile with power-down only, buffer canaries, BUSY-tolerant authority checks, cheaper ISP0 guard scans | `camera` | camera-2, camera-3, camera-4, camera-5, camera-11 | Done. For camera-4 only the canary fill was added; the retained readout path does not permit guard reads. |
| Sanitizer builds that stop on undefined behavior, a shared harness module, a check index, complete Python requirements, removal of dead headers | `checks-harness` | checks-harness-1, checks-harness-3, checks-harness-10, checks-harness-11, checks-harness-12, checks-harness-14, checks-harness-15 | Done. Two dead headers owned by other components were kept, and 68 checks owned by other components still compile without `-fno-sanitize-recover=all`. |

### Round two

| Change | Package | Findings addressed | Outcome |
| --- | --- | --- | --- |
| A resident SMP contract for cold-booted images, and a fixed interrupt table for images with several device owners | Primary agent | bringup-gaps-2, supervisor-apps-9, supervisor-apps-2, x-concurrency-5, in part | Done for the contract and table, which are host-checked. Round three put both into use in the integrated OS image. |
| The six GPU host model targets link again | `gpu-model-links` | gpu-8 | Done. |
| Stale storage host checks and the launch-chain size repaired | `storage-checks` | None | Done. |
| UART model stubs, a 1.5 s PCIe link-timeout floor on the device, and VE and PCIe clock-reference entries | `uart-pcie-followups` | None | Done. The host preparation scripts still accept 250 ms, so the device refuses a session that the host accepts. |
| The I2S transcription image builds; mimus compiler options are part of the native, Bluetooth and I2S build identities | `i2s-mimus-build` | audio-8 | Done. |
| Standalone application projects configure and build; the central build offers the stage-2 loader | `app-configure` | None | Partial. The camera record version was not raised, because four files outside the package hard-code it. The central stage-2 build was blocked until `boot-tools` supplied the loader's FIFO callbacks. |
| Review notes in H.264, USB diagnostics, Bluetooth SCO and camera | `device-minors` | None | Done. |
| Stage-2 loader and boot-slot tool hardening; card-update planning as one storage session per changed range | `boot-tools` | storage-7 | Done. |
| Review notes in RTC, crypto, radio entropy and the warm mimus path | `security-radio-mimus-minors` | None | Done. |
| Review notes in DSI scanout, HDMI audio and the display replay checks | `display-minors` | None | Done. |
| The PMIC object released across its I2C mutex and register-lock waits, removing the remaining lock-order hazard with the USB2 platform poll | `pmic-followups` | None | Done. |
| A fixed board thermal trip, throttle and shutdown policy | `thermal-policy` | security-misc-1 | Done. The thermal application ends its observation at the critical level; no consumer reduces load at the throttle level. |
| The DMA service worker woken by the DMA0 interrupt instead of a 1 kHz poll | `dma0-interrupt` | dma-iommu-2 | Done in the service. The package wired no platform interrupt binding; round four bound the interrupt in the I2S image and the OS microphone build. |
| Host fixtures repaired after the queue port check, and earlier check failures fixed | `host-fixtures` | None | Partial. Three defects are in files the package did not own: the crypto packaging fixture passes a nonexistent queue database, `docs/hardware-dependencies.md` names the deinterlacer with the wrong identifier, and an unused function in the video encoder platform fails `-Werror`. |

### Round three

| Change | Package | Findings addressed | Outcome |
| --- | --- | --- | --- |
| An [integrated OS image](../os-image.md) with one interrupt table, a fixed start list and supervised and resident lifetimes; the stage-2 loader builds its resident contract | `integrated-os` | bringup-gaps-1, supervisor-apps-1, bringup-gaps-2, supervisor-apps-9, cpu-startup-2, supervisor-apps-2, x-concurrency-5, bringup-gaps-12 | Done in host models. On a board the loader's entry admission still refuses. |
| GPU worker steps in bursts, idle sleep while ready, one service clock, firmware statistics events, one mapping-check helper | `gpu-performance` | gpu-1, gpu-3, gpu-5, gpu-6, gpu-7, gpu-9, gpu-10 | Partial. The accelerator client still polls STATUS. DEBUG and CLOCK firmware events still fault. Per-access mapping checks remain, limited to the access direction. The 52 span helpers were not merged, because their preconditions differ. |
| NPU job hashing and cache maintenance limited to job-relevant bytes, a held WAIT reply, one buffer layout table per profile, shared command templates, a worker-affinity check for interrupt binding | `npu-performance` | npu-1, npu-3, npu-4, npu-5, npu-6, npu-9, npu-10, npu-11 | Partial. Descriptors and coefficients are still hashed per job, because no API maps them read-only. The fixed two-job client keeps its recorded STATUS sequence. npu-8 remains deferred. |
| One video-engine resource core for decoder and encoder, a clear HEVC configuration refusal, three full-HD High-profile references | `video-consolidation` | video-6, video-10; video-8 in part | Partial. video-7 is unchanged. The HEVC parser and controller were not integrated. The frozen High-profile evidence describes the earlier layout. |
| One DE1 framebuffer source shared by HDMI, DisplayPort and GPU display; DE_SYS domain 1 through the domain-ON provider; the powered-domain entry state of DSI and deinterlace documented | `display-consolidation` | display-5, display-4, display-3 | Done as reviewed. The VO1 power copies in HDMI and DisplayPort remain, because the power provider takes no domain base. DSI and deinterlace still power no domain; their entry requirement is documented instead. |
| Recovery from a clean unsolicited link loss, repeated session polls, an optional four-bit 25 MHz SDIO profile, reorder-flagged and A-MSDU receive records, fewer transmit copies | `radio-recovery` | radio-net-2, radio-net-6, radio-net-9, radio-net-10, radio-net-14 | Done. No image requests the four-bit profile until a physical identity read qualifies it. A second loss during recovery retains the radio. |
| Reuse of ACL slots from attempts that closed without a handle; retry of a headset connection that failed before it connected | `bluetooth-reconnect` | bluetooth-5, in part | Partial. Reconnect after a confirmed disconnect needs a retired-handle design across ACL, L2CAP, SCO, security, RFCOMM and HFP. bluetooth-6 and buses-pcie-4 are unchanged; see the deferred list. |
| xHCI Disable Slot on detach with page clearing for reattach, and a 1 s bound on a held transaction error | `usb-host-detach` | usb-8 | Done in the xHCI core. Round five made the TinyUSB adapter call the detach path. |
| Rearm of retained captures, discarded warm-up images, OV5640 block writes, camera USB0 through the shared board service | `camera-capture` | camera-1, camera-9; camera-7 in part | Partial. camera-10 needed TWI9 support in the I2C and clock drivers, which round five added. Moving the camera onto the board framework would change the session and wire format that the supervised camera host tool reads. |
| A separate camera TWI11 mutex; the PB4–PB8 pin entry points running on the bank-qualified implementation | `power-consolidation` | clock-power-pins-1, clock-power-pins-13 | Partial. clock-power-pins-9 and clock-power-pins-10 remain deferred. |
| The maintenance session selects the SD bus profile; backup requests read eight sectors | `storage-update-speed` | storage-5 | Done. Writes stay at one sector per request, because two sectors could exceed the one-second operation deadline on a conforming card. |
| Shared outbound iATU programming checked against a recorded request; a configurable PCIe link speed target | `pcie-general` | buses-pcie-5 | Done. BAR assignment and MSI setup stayed in the endpoint test driver; round six added a general function client that assigns BARs and sets up MSI-X. Both applications still select 2.5 GT/s. |
| Central build patches applied without fuzz; supervised profiles require an explicit supervised board mode; a refusal helper and a cache override in the check harness | `build-safety` | build-system-5, build-system-10, checks-harness-4, checks-harness-13 | Partial. About 30 standalone projects keep their own fuzzy patch function, four patches are pinned by hash because they still need fuzz, and existing checks were not migrated to the new helpers. build-system-7 remains deferred. |

### Round four

| Change | Package | Findings addressed | Outcome |
| --- | --- | --- | --- |
| The DMA0 interrupt wakes the I2S image's DMA worker at each period's queue end, with a one-tick fallback; the [integrated OS image](../os-image.md) gains a microphone build that captures each utterance over I2S0 and routes the DMA0 interrupt through its table; a supervised launcher and session preparation for the OS image | `os-image-audio` | audio-2 | Partial. audio-1 was left because its guard band changes the DMA controller, the DMA service and checks that other packages owned; round five implemented it. bringup-gaps-8, pinning recognition workers to the A76 cores, belongs to the primary agent and needs qualified eight-core scheduling and A76 clocking. The microphone build was supervised only until round five, because the stage-2 loader admitted only the 808-byte session. |
| A [CPU clock and voltage driver](../cpu-clock.md) that moves CPU-L and CPU-B among fixed operating points; CPU PLL scopes in the clock registry; bounded DCDC3 and DCDC5 selector writes that only this driver may issue; a deadline check before any PLL access | `cpu-clock` | cpu-startup-3 (with clock-power-pins-5, mimus-perf-1, bringup-gaps-6) | Done in host models. No image calls the driver. The PLL pattern and low-frequency-mode words that boot0 leaves are unknown, so the driver's refusal of those modes may reject the board's inherited state. |
| Radio receive aggregates of up to 112 SDIO blocks, read with one CMD53 of up to 57,344 bytes and a deadline scaled by length; optional block writes for long firmware patch-table runs, off by default | `radio-transport` | radio-net-3, radio-net-8 | Done. For radio-net-3 the transport reads each aggregate with one large request, as the Linux driver does, because nothing shows that the firmware accepts an aggregate split across several reads. The block writes stay off until a hardware readback confirms them. radio-net-12 remains deferred. |
| One CMake function for board-derived applications, with a build identity that hashes every compile and link input; one builder for returning images that can build their platform from source; shared claim, evidence and SRAM launcher checks; emulator hooks on modeled instructions only; hard-linked evidence snapshots | `build-harness` | x-duplication-5, x-duplication-8, checks-harness-6, checks-harness-7, checks-harness-8, host-tools-6; build-system-13, checks-harness-2 and checks-harness-9 in part | Partial. Every board-derived image has a new build ID. build-system-8 was measured and left: 34 of 151 inherited objects differ between images, so shared object libraries would need one library per configuration and would change the layout of images tested on hardware, for a build-time gain only. host-tools-7 changes FEL loops that contact the board. checks-harness-5 and the rest of build-system-13, checks-harness-2 and checks-harness-9 are in files that other components own. |
| Receiver and parser status recorded before the OV5640 stop; shared camera helpers, including one VIPP0 enable table; JPEG decode of images up to 640×480 with up to 512 KiB of input | `camera-jpeg` | camera-6, camera-8, video-9 | Partial. For camera-6 the coordinator records the status before the stop, an alternative the verifiers accepted, and keeps the stop order of the Linux driver. camera-8 shares only the helpers that had drifted; each controller keeps its own step interpreter, because their polling differs. camera-10 needed a TWI9 clock-registry scope in files the package did not own; round five added it. |

### Round five

| Change | Package | Findings addressed | Outcome |
| --- | --- | --- | --- |
| [Cold SD admission](../boot-slots.md#cold-sd-admission-starts-from-boot0s-controller-state) from the SDC0 state that boot0 leaves, with PF6 card detection; the stage-2 loader admits both OS session sizes; entry admission split into three named prerequisites; the resident OS image admits the microphone build | `stage2-entry` | None | Done. All three entry prerequisites still refuse. Admission writes PF6 and the SDC0 module clock. The card supply and exclusive use of SDC0 have no register to check and need a board observation. |
| A read-only check of the reset, clock and DMA-enable state of every DRAM bus master after a cold boot, including the SPI flash controller; a [survey](../remote-writers.md) of the masters that the cold boot chain starts | `remote-writers` | None | Done. On a board the check cannot report every master excluded, because no source proves how to exclude the OR100 processor or the crypto engine. No boot image calls it. |
| Cyclic I2S receive keeps one whole period between the checked copy and the DMA writer; the DMA worker keeps waiting for the interrupt until a period can be copied | `cyclic-receive` | audio-1 | Done. Rings shorter than four periods are refused. The I2S application still uses four periods, so its client must return each copy within one period, 128 ms; an eight-period receive ring needs changes in the application. Physical admission still needs a measured margin. |
| The TinyUSB adapter tears down a disconnected device through the xHCI detach path, so a hub child, or a device behind an adapter, can be replaced without ending the session | `usb-detach-consumer` | usb-8 | Partial. Unplugging a device directly from the Type-C connector still ends the session, because adopting a new Type-C attachment needs qualified board power sequencing. A bulk stall still ends the session, because Bulk-Only Transport reset recovery needs changes to the pinned TinyUSB storage class and to bulk-endpoint recovery in the core. |
| A TWI9 controller in the I2C engine and native camera-bus provider, with an exclusive TWI9 clock scope; the DE_SYS power provider powers PCK domains 1, 9 (VO0) and 10 (VO1) | `power-domains` | camera-10 and display-3, in part | Partial. No CAM2 binding exists: it needs a port field in the camera boot record and a sourced deskew value for receiver port 1. DSI and deinterlace do not call the provider yet, because their source lists are in CMake files the package did not own; DSI also needs PCK domains 0, 2, 3 and 8 for its IOMMU group. clock-power-pins-9 and clock-power-pins-10 remain deferred. |
| The [native HEVC decoder](../hevc-native.md) builds: the reviewed parser, controller and storage with a corrected low-delay flag, emulation-prevention checks and physical IOMMU addresses, a typed video-engine binding, an application image and host checks | `hevc-native` | video-8 | Done. Host preparation refuses to produce a launch bundle until physical allocation and page-table evidence exists. |

### Round six

| Change | Package | Findings addressed | Outcome |
| --- | --- | --- | --- |
| A radio build of the integrated OS image, with crypto engine entropy for WPA2, in both lifetimes; a supervised Bluetooth headset build that recognizes speech from the headset | `os-services` | None | Partial. The image has no SHA-256 or AES request service, because nothing in it submits such requests. The headset build is supervised only, because the Bluetooth application ends 60 s after it starts; it excludes the radio, because both own the AIC8800 module, and admits only CVSD audio. Preparation refuses both builds until it stages their profile and firmware. Round eight removed the 60 s limit and the radio exclusion and added mSBC. |
| [Display frames streamed through a replay ring](../display-engine.md#de352-replay-over-a-ring-of-two-to-four-frames) of two to four slots, whose frame size the configuration fixes; a slot becomes writable again after a frame end shows that scanout has left it | `display-streaming` | None | Done. No application produces frames into the ring yet. |
| [Graphics profile 4](../gpu-render.md#state-batches-perspective-texturing-and-stencil): up to 16 state batches per frame with depth, culling, stencil and five blend equations, perspective-correct texturing, filtering, address modes and sRGB decoding; lending a completed frame to a display consumer | `gpu-features` | None | Partial. Compute input and output beyond one 4 KiB page needs a new package format, staged input and compiler changes, so it was left for a package of its own. Arbitrary shaders need a provenance design; multisampling needs a separate render-target profile. |
| Graphs with up to eight INT8, UINT8 or INT16 inputs and outputs and compiled fully connected layers; up to eight concurrent model sessions with round-robin job starts and interrupt completion; session memory reserved by model size; a study of one mimus linear layer on the NPU | `npu-runtime` | npu-7 and npu-8, in part | Partial. 64 KiB NPU pages were not added; 4 KiB pages work at any size. No board image links the session runtime. The study shows that a mimus layer cannot run bit-exact on the NPU, because the compiler refuses INT32 output. |
| H.264 long-term references and memory-management operations, Baseline and Main streams in the High build, and level 1b; repeated JPEG decode of up to eight images per session | `codec-streaming` | None | Partial. B pictures with long-term references are refused, because neither reference driver tells the engine which references are long-term. Repeated JPEG encode, H.264 encoding beyond two frames and unrestricted H.264 streaming were not done; round eight added H.264 sequences of up to 30 pictures. The supervised JPEG tool still sends one image. |
| G2D general scaling, image-over-image blending and conversion between RGB channel orders; selectable DI301 TNR strength | `g2d-deinterlace` | None | Partial. Rotation needs `g2d_rotate.c`, which the pinned sources lack. YUV conversion changes buffers and the host contract. DI301 gain feedback, film detection and repeated jobs need a task that runs several jobs per session. |
| Two USB2 hub tiers, bus-powered hubs within the USB 2.0 current budget, and USB Audio Class 1 capture over one isochronous IN stream | `usb-host-classes` | None | Done. No board workload selects these modes yet. |
| Station power save; reconnect and loss recovery onto any access point of the same network; board transfers that survive link loss; an SNTP client that supplies UTC to the RTC service | `wifi-network` | None | Partial. WPA3 personal was not implemented: it needs a new pinned elliptic-curve library, SAE external authentication and protected management frames. No image runs the SNTP client with the RTC service. |
| Hands-free call control; an A2DP sink for SBC streams with its SDP record and local close; a link security check on every sink step | `bluetooth-profiles` | None | Done. No native application uses the A2DP sink. |
| A [read-only FAT and exFAT filesystem](../filesystem.md) over the polled SD core, from pinned FatFs R0.16 | `storage-filesystem` | None | Done. UHS-I bus modes were examined and left unimplemented: switching to 1.8 V writes the PF bank voltage that card detect and the debug pins share, and a failed switch needs whole-board power removal. No image links the filesystem. |
| SPI dual, quad and DMA transfers; UART DMA on every port, with IrDA, RS-485 and loopback modes | `serial-buses` | None | Done. No application uses these modes. The platform refuses quad SPI, because no route has IO2 and IO3 pins. |
| PWM cycle profiles with prescaled rates, either polarity and constant endpoints; GPADC compare-window threshold mode | `pwm-adc-rtc` | None | Partial. S_PWM needs a clock-registry scope. Other PWM channels, capture and the ADC burst, FIFO and DMA modes were not started. RTC calibration has no pinned reference implementation, alarm wake needs a qualified power-off path, and no reset-cause register has been identified for the watchdog. |
| A general PCIe endpoint function client with BAR assignment, INTx and MSI-X, D3hot transitions and an orderly stop; a read-only NVMe driver over it; a port stop | `pcie-endpoints` | None | Partial. Bridges, ASPM and Gen2 selection by the applications were not done. No image linked the function client or the NVMe driver until round eight added an NVMe board image. |
| AES-192 and AES-256 keys with CBC, CTR and GCM; SHA-1, SHA-224, SHA-384, SHA-512 and HMAC; SP 800-90B cutoffs for the TRNG health tests | `crypto-modes` | None | Partial. Hardware key slots were not added, because the sources do not say which world may select a hardware key. GCM and HMAC are built from the engine's CTR and hash operations; its native GCM and HMAC modes are not used. |
| Continuous camera capture into a ring of two to four slots with dropped-frame accounting; the reference driver's OV5640 modes; the frame size carried through receiver, VIPP and DMA | `camera-streaming` | None | Done, with one issue open from the final review. The first DMA status interval spans the OV5640 stream-on sequence, so a slow start can make a stream deliver nothing while every slot stays owned; [camera capture](../camera-capture.md) explains it. The status interval now has a margin under half a frame. The native camera application does not stream. |
| CPU operating points selected by the thermal policy; a session grant record for CPU clock changes; cycle-counter timing of recognition work | `cpu-performance-control` | None | Done in host models. No image carries a grant or calls the driver, so no image grants a transition. |

### Round seven

Drivers, native adapters and applications each defined their own copies of
four mechanisms: the task-context admission check, address-range and deadline
predicates, register accessors ordered by `DSB SY`, and mapping checks built on
the `AT` address-translation instruction. The copies differed in their rules,
for example in how they treat a NULL pointer, a zero length or a range that
wraps. [Four shared headers](../shared-helpers.md) give each distinct rule its
own function. A package moved a copy only when the header function returns the
same result for every input; a copy with no such function stays local.

Each package built every target that compiles a changed file, from the base
commit and from its branch, and compared the disassembly of each function.
Barrier instructions, the order of interrupt-mask and system-register accesses
and the order of scheduler calls are unchanged in every package. Beyond
register allocation, branch layout and assertion line numbers, three
differences recur:

- Register reads that return through a pointer now store the value after the
  trailing `DSB SY` instead of before it. The device access is fenced as before;
  the moved store is to ordinary task-local memory.
- Native adapters run their instance checks before reading `CurrentEL` and
  `DAIF`, and load the owner task before masking interrupts.
- A read-and-write mapping check skips the write translation when the read
  translation has already failed, and refuses a shareability value above 3,
  which the hardware field cannot hold.

CPU startup and multicore code were not migrated. In the table, *the
task-context findings* means x-concurrency-8, x-duplication-6, buses-pcie-9,
security-misc-10 and clock-power-pins-12, which the plan merged under
cpu-startup-9.

| Change | Package | Findings addressed | Outcome |
| --- | --- | --- | --- |
| Four headers: `a733_task_context.h`, `a733_range.h`, `a733_mmio.h` and `a733_mapping_check.h`, with `enum aarch64_cache_op` named as the cache-operation code; eleven callers moved to them; a host check of the predicates against 128-bit reference sums and of the barrier and interrupt-mask instruction order | Shared headers, merged before the packages | None by itself; the packages below used it | Done. [docs/shared-helpers.md](../shared-helpers.md) classifies the copies and states the migration rule. |
| The GPU driver and its platform files on the shared range, deadline, MMIO and mapping-check helpers; the GPU mapping policy expressed as one shared mapping rule | `consolidate-gpu` | x-bounds-4, x-duplication-9, x-coherence-6, x-coherence-4, runtime-memory-6 | Done. `drivers/gpu/gicv3_spi.c` keeps its own fenced accesses, because about 15 board applications compile it and each would need its own comparison. A failed register-page check now issues one `AT` instead of two. |
| The NPU driver and model loader on the shared helpers; the NPU cache-operation enum and translation structure replaced by the AArch64 ones | `consolidate-npu` | x-bounds-4, x-duplication-9, x-coherence-6, x-coherence-4, x-coherence-7, runtime-memory-6 | Done. Five local helpers stay because no header function has the same meaning, such as a 32-bit modular comparison in the model parser. The final review found one inline overlap test in `drivers/npu/nbg_relocation.c` that has the shared form and was not moved. |
| The Wi-Fi and Bluetooth stacks and the native radio adapter on the shared helpers; three radio QEMU checks configure again | `consolidate-radio` | x-bounds-4, x-duplication-9, x-coherence-6, the task-context findings | Done. Five saturating deadline sums in the Bluetooth stack stayed local, because the header had no saturating form; round eight added one. |
| The display, video and IOMMU drivers and the HDMI native adapter on the shared helpers | `consolidate-media` | x-bounds-4, x-duplication-9, x-coherence-6, x-coherence-4, runtime-memory-6, the task-context findings | Done. Three copies stay: one HDMI PHY sum guard that protects two later stores in different branches, `a733_display_native_owner()`, which checks no interrupt mask, and the Linux register shim of the vendored deinterlacer code. |
| The USB, storage and PCIe drivers and their native adapters on the shared helpers | `consolidate-io` | x-bounds-4, x-duplication-9, x-coherence-6, x-coherence-4, runtime-memory-6, the task-context findings | Partial. The SD platform adapter `a733_mmc_platform_native.c` keeps its task-context check and register accessors, because `checks/a733-storage-native.py` rewrites their exact inline assembly. The USB platform files were moved and then restored, because `checks/a733-usb-platform.py` does not copy the shared headers. Both checks belong to other owners. |
| The ADC, LRADC, clock, CPU clock, TWI, PMIC, rail, PWM, thermal, RTC and DE_SYS power drivers, their native adapters and the maintenance-watchdog reload bound on the shared helpers | `consolidate-platform-drivers` | x-bounds-4, x-duplication-9, x-coherence-6, the task-context findings | Done. The package also listed x-coherence-4, runtime-memory-6 and x-coherence-7, but its directories held no mapping check or cache-operation enum. The cycle-counter read in `a733_cpu_cycles_native.c` stays one assembly block. |
| The SPI, UART, crypto engine, camera, audio and DMA drivers and their native adapters on the shared helpers; three exported range helpers removed and their callers redirected | `consolidate-peripherals` | x-bounds-4, x-duplication-9, x-coherence-6, x-coherence-4, runtime-memory-6, the task-context findings | Done. The DMA cache enum stays, because DMA callback tables take it and `platform/a733/dma_native.c` maps it onto the AArch64 enum by name. The final review found six deadline samplers with a header equivalent that were not moved: one each in the UART driver and the I2S platform, and four in the crypto engine. |
| Applications `adc` through `hevc` on the shared helpers, with a host check that compares each replaced expression with its replacement in 56,829 cases | `consolidate-apps-a-h` | x-bounds-4, x-duplication-9, x-coherence-6, x-coherence-4, runtime-memory-6 | Done. The radio-profile page loop in `apps/board/main.c` stays, because changing that file requires the patches built from it to be regenerated in the same change. |
| Applications `i2s` through `usb-returning` on the shared helpers; the OS image's application patch regenerated | `consolidate-apps-i-z` | x-bounds-4, x-duplication-9, x-coherence-6, x-coherence-4, runtime-memory-6 | Partial. `apps/storage-read-single-core/monitor.c` and `runtime.c` keep their 64-bit GIC accesses and one mapping check, because other images' patches use their lines as context. |
| The device memory pool, the device memory layout and maintenance-watchdog mapping checks, and the storage-maintenance startup deadline on the shared helpers | `consolidate-platform-shared` | x-bounds-4, x-duplication-9, x-coherence-4, runtime-memory-6 | Done. The AArch32 device and storage launchers keep their register accessors and deadline guards: their host build tools put only the launcher's own directory on the include path, and prepared bundles record the launcher hash. |

### Round eight

| Change | Package | Findings addressed | Outcome |
| --- | --- | --- | --- |
| Wi-Fi and the Bluetooth headset served by one AIC8800 module owner, which loads the firmware once, with separate client leases and failures; the OS image combines the radio with the headset, runs the headset in the resident lifetime and admits wideband mSBC speech; a saturating deadline sum in `a733_range.h` | `radio-module-owner` | None | Done. Its first final review found that a stop stored by another task during a capture poll reached the audio close with no close bound, so a normal stop ended as a retained failure. The close now sets the bound first, and a model case covers the race. A stop during capture and module admission after an early Bluetooth failure still need a board run. |
| The five boot-CPU-only board images (board, thermal, RTC, PWM and crypto) built from one CMake function, one linker script and one set of boot headers; the PWM and crypto images no longer patch another image's patch output; every single-core `main.patch` applies without fuzz, so the PWM and crypto build options configure again; a check that compares an image built from two source trees | `single-core-variants` | buses-pcie-10, x-duplication-4, supervisor-apps-8; build-system-13 in part | Done. Object code is identical to the previous build; the PWM and crypto images link in a different order. The board session, main and identity patches remain, because replacing them with hooks in the source needs files that the board owner holds. Four standalone projects use the shared patch function; the other copies are outside the package. |
| Stopped board service tasks suspend themselves instead of waking every tick; [docs/watchdog.md](../watchdog.md) lists the five places that arm WDT0 and the validators that require its mode | `supervisor-cleanup` | supervisor-apps-12 in part; security-misc-7 in documentation only | Partial. Service tasks are not pinned to cores: the Type-C and PMIC tasks yield while an I2C transfer is in flight, so sharing one core risks starving it. supervisor-apps-3, supervisor-apps-6, supervisor-apps-10, bringup-gaps-11 and the shared WDT0 constants each need files outside the package; the deferred list gives the reasons. Every board-derived image needs a new board run before its evidence covers the suspended tasks. |
| A [supervised board image](../pcie-nvme.md) that reads one 4096-byte NVMe block over the PCIe function client with polled completion, shuts the controller down, stops the port and reports through USB0; a record of what a board image for the NPU session runtime needs first | `accelerator-images` | None | Partial. The NPU session image was not built. A board-derived image would construct a second GPU/PCK resource manager beside the board's, and its 2 MiB device pool is smaller than the 2,195,456 bytes the catalogue models need. A single-core NPU image would need `apps/npu-convolution/native.c` to select the session runtime. Both changes are outside the package. |
| [H.264 encoding](../h264-encode-native.md) of one sequence of 1 to 30 pictures: an IDR picture and up to 29 P pictures at fixed QP 26, bitstream ring reuse after each copy without device wrap, and header and register checks against the pinned library's 30-picture trace | `h264-gop` | None | Done. Frame-level rate control was not implemented, because checking it needs a rate-controlled run of the reference library. The supervised capture tool still captures and acknowledges two pictures. |
| The NPU startup and clock-handoff models pass again with an enabled, stable 24 MHz DCXO in their fixtures; the DMA pool session check no longer drops the generated FreeRTOS port when its output path contains `checks/` | `model-check-repairs` | None | Done. No driver changed. `checks/dma-interrupt.py`, `checks/i2s-interrupt.py` and `checks/i2s-application.py` keep the same unanchored source filter, and the NPU models still have no runner in the check index. |

### Performance-gated tranche

Every package in this tranche follows
[docs/performance-architecture.md](../performance-architecture.md). Before
any code is written, a short design with measurable budgets goes to three
reviewers: one for simplicity, one for hardware performance and one for
correctness. A design proceeds when two of them approve and none reports a
blocking issue. The same three lenses review the code. A final step measures
the base and the package with host models or instruction emulation, and blocks
a package when a metric is worse than the base without a reason stated in its
design. Each package's design and measurement table are in
[docs/designs/](../designs/).

| Change | Package | Outcome |
| --- | --- | --- |
| One named address limit per DMA master and a per-master rule table in [docs/dma-address-width.md](../dma-address-width.md); the DMA0 conflict scan walks the channel array by pointer | `dma-address-width` | Done. A successful DMA0 submit runs 1,121 instructions instead of 1,148 for a one-segment copy. NVMe can still reach a wrong address above 4 GiB without an error, and 847,872 bytes of DRAM must stay below 4 GiB; both need changes by the PCIe and camera owners, which the document lists. |
| The headset voice button starts recognition through HFP voice-recognition activation (`AT+BVRA`); call control is off by default behind `SUNSTONE_BT_HFP_CALL_CONTROL`; segments are hashed in place in whole 64-byte blocks | `bluetooth-voice-recognition` | Done. With call control off, the HFP audio gateway is 11,572 bytes of code instead of 15,066. Whether the target headset stays connected to a gateway that advertises no call feature needs a board run. |
| The GPU frame path keeps depth in tile memory and writes output in place, so a 1080p frame stores 16.7 MB instead of 77.0 MB and cleans 25.0 MB of cache instead of 68.5 MB | `gpu-graphics` | Done. The render image's static data grows by 256 bytes for the worker's own copy of the batch records. |
| A passive thermal trip at a DDR temperature of 85 °C | `dram-controller` | Partial. Memory-bus priority, the latency window and the copy-load measurement have designs but no code. |
| A study of exact integer sums on the NPU for mimus linear layers: base-16 digit partial sums are exact in the SDK simulator, but one layer would cost about 13 times the A76 kernel's CPU cycles, so mimus keeps its CPU kernel | `npu-exact-sum` | Done. Host tools and checks only. |
| A [conformance harness](../npu-conformance.md) that runs every catalogue model in the SDK simulator on fixed vectors and compares the results with compiler or integer references | `npu-conformance` | Done. Its measurement step objected only to the harness's status entry in docs/hardware.md, which the integrator accepted. |

Ten packages are still under review and are not merged:

- **Code written, review not settled:** the 12 GiB memory map with large
  device reservations and memory protection; the GPU runtime (concurrency,
  isolation, hang recovery and power); and the GPU conformance harness.
- **Design not approved:** NPU model breadth; NPU cold boot, recovery and
  power; placement of mimus layers on the CPU, GPU or NPU; one owner for
  every rail and power domain; bit-exact mimus matrix products on the GPU;
  the 12 GiB memory qualification image; and board-test bundles for the new
  paths.

## Deferred findings

The original plan deferred the findings below. A finding listed with merged
identifiers stands for all of them.

### Deferred findings implemented later

| Finding | Round and package | What exists now |
| --- | --- | --- |
| cpu-startup-2 (with storage-6, bringup-gaps-3) | One, `standalone-boot`; three, `integrated-os`; five, `stage2-entry` | The [stage-2 loader](../boot-slots.md) admits the SD controller state that boot0 leaves, verifies and loads a slot and builds the resident contract in host models. Its three entry prerequisites refuse, so it enters no image on a board. |
| bringup-gaps-4 | One, `standalone-boot` | Boot slots with fallback and the install, update and rollback procedure, in host tools and models. No card has been written. |
| storage-7 | Two, `boot-tools` | Card updates are planned as one storage session per changed range. |
| dma-iommu-2 | Two, `dma0-interrupt`; four, `os-image-audio` | The DMA service can sleep on the DMA0 interrupt. The I2S image and the OS microphone build bind it; other images poll every tick. |
| security-misc-1 | Two, `thermal-policy`; six, `cpu-performance-control` | A fixed thermal policy ends the thermal application and the OS thermal service at the critical level. The CPU clock driver lowers raised clusters at the throttle level, but no image runs the driver, so no image reduces load at that level. |
| gpu-8 | Two, `gpu-model-links` | [docs/gpu.md](../gpu.md) describes completed-job compute admission. |
| bringup-gaps-1 (with supervisor-apps-1) | Three, `integrated-os`; four, `os-image-audio`; six, `os-services`; eight, `radio-module-owner` | The integrated OS image, with optional microphone, radio and Bluetooth headset builds; the radio and the headset share one module owner, and the headset also runs in the resident lifetime. Display, camera, GPU and NPU are not part of it. |
| bringup-gaps-12 | Three, `integrated-os` | The OS link check reads the model, recording and arena extents from shared headers and refuses overlaps and extents outside the loader window. |
| usb-8 | Three, `usb-host-detach`; five, `usb-detach-consumer`, in part | Detach and reattach in the xHCI core, used by the TinyUSB adapter for hub children and devices behind an adapter. A new Type-C attachment and Bulk-Only Transport reset recovery still end the session. |
| storage-5 | Three, `storage-update-speed` | The selectable bus profile and eight-sector backup reads. Writes stay at one sector per request. |
| radio-net-2 | Three, `radio-recovery` | The radio owner repeats session polls while the transport advances; a host check cuts owner ticks for the setup sequence from 172 to 42. |
| radio-net-6 | Three, `radio-recovery` | An optional four-bit 25 MHz SDIO profile that no image requests yet. |
| radio-net-9 | Three, `radio-recovery` | Recovery from a clean unsolicited link loss. Loss with outstanding work still retains the radio. |
| radio-net-10 | Three, `radio-recovery` | Reorder-flagged and A-MSDU receive records. Neither occurs until an HT profile exists. |
| radio-net-14 | Three, `radio-recovery` | Transmission from the data worker's buffer. The network adapter's queue copies remain. |
| bluetooth-5 | Three, `bluetooth-reconnect`, in part | Slot reuse for attempts that never held a handle and retry before the first connection. |
| display-4 | Three, `display-consolidation`, in part | DE_SYS domain 1 uses the shared domain-ON provider. The VO1 copies remain. |
| display-5 | Three, `display-consolidation` | One DE1 framebuffer source for HDMI, DisplayPort and GPU display. |
| camera-1, camera-9 | Three, `camera-capture` | Rearmed repeated capture with discarded warm-up images, and block writes during sensor configuration. |
| camera-7 | Three, `camera-capture`, in part | The camera image uses the shared USB0 service. It does not use the board framework. |
| gpu-1, gpu-3, gpu-5, gpu-6, gpu-7, gpu-9, gpu-10 | Three, `gpu-performance`, in part | See the round-three table. |
| npu-1, npu-3 (with x-coherence-5), npu-4, npu-5, npu-6, npu-10, npu-11 | Three, `npu-performance`, in part | See the round-three table. |
| video-6, video-10 | Three, `video-consolidation` | One video-engine resource core, and three full-HD High-profile references. |
| clock-power-pins-1, clock-power-pins-13 | Three, `power-consolidation` | A separate TWI11 mutex, and PB pin entry points on the bank-qualified implementation. The I2S binding still uses the PB types. |
| buses-pcie-5 | Three, `pcie-general`; six, `pcie-endpoints` | Shared iATU programming, a configurable link speed target, and a general function client that assigns BARs and sets up INTx or MSI-X. No image links the client. |
| build-system-5 (with x-duplication-2, build-system-13, supervisor-apps-8), build-system-10, checks-harness-4, checks-harness-13 | Three, `build-safety`; four, `build-harness`; eight, `single-core-variants`; all in part | Central patches apply without fuzz, supervised profiles need an explicit mode, the harness offers the new helpers, and the freestanding compile options are defined once for the CMake recipes. Every single-core `main.patch` now applies without fuzz, and four single-core projects use the shared patch function. The other standalone projects keep their own patch function, and existing checks were not migrated. |
| x-duplication-4, buses-pcie-10 | Eight, `single-core-variants` | The [boot-CPU-only board images](../single-core-images.md) share one CMake function, linker script and boot headers, and select their branches in the shared monitor, runtime and platform by one definition each. |
| cpu-startup-9 (with clock-power-pins-12, x-concurrency-8, x-duplication-6, buses-pcie-9, security-misc-10) | Seven, `consolidate-radio`, `consolidate-media`, `consolidate-io`, `consolidate-platform-drivers` and `consolidate-peripherals`, in part | The native adapters for PCIe, USB2, SPI, UART, camera, ADC, PMIC, PWM, TWI, HDMI and the radio call `a733_task_context_owner_el1()`. The SD platform adapter, CPU startup and the FreeRTOS port keep their own checks. |
| x-bounds-4 (with x-duplication-9) | Seven, every consolidation package, in part | Range predicates and deadline samplers and sums use `a733_range.h` where a header function has the same meaning. The copies left are in the [handoff](../handoff.md#shared-helper-copies). |
| runtime-memory-6 (with x-coherence-4) | Seven, every consolidation package except `consolidate-radio` and `consolidate-platform-drivers`, in part | Mapping checks in the GPU, NPU, USB, display, video, DMA, shared platform and application code call `a733_mapping_check.h`. `apps/storage-read-single-core/runtime.c` keeps one. |
| x-coherence-6 | Seven, every consolidation package except `consolidate-platform-shared`, in part | Fenced register accesses use `a733_mmio.h` with unchanged barrier placement. The GIC SPI driver, the SD and USB platform adapters, the AArch32 launchers and the boot-CPU monitor keep theirs. |
| x-coherence-7 | Seven, `consolidate-npu` | The NPU callbacks take `enum aarch64_cache_op`. The DMA enum stays for the DMA callback tables and maps onto the AArch64 enum by name. |
| supervisor-apps-12 | Eight, `supervisor-cleanup`, in part | Stopped PMIC and resident supervising tasks suspend instead of waking every tick. Service tasks are not pinned to cores. |
| cpu-startup-3 (with clock-power-pins-5, mimus-perf-1, bringup-gaps-6) | Four, `cpu-clock`; six, `cpu-performance-control` | The [CPU clock and voltage driver](../cpu-clock.md), its thermal coupling and the session grant record, in host models. No image grants a transition. The user reviewed the CPU clock plan and agreed with the design on 2026-10-21; each physical change still needs the user's approval. |
| radio-net-3 | Four, `radio-transport` | One CMD53 of up to 57,344 bytes reads a receive aggregate of up to 112 blocks. |
| radio-net-8 | Four, `radio-transport` | Optional block writes for long patch-table runs, off until a hardware readback confirms them. |
| audio-1 | Five, `cyclic-receive` | One period between the checked cyclic receive copy and the DMA writer. The I2S application keeps a four-period ring, so its client has one period to return each copy. |
| audio-2 | Four, `os-image-audio` | The DMA0 interrupt wakes the I2S DMA worker at each period's queue end, with a one-tick fallback. |
| camera-6 | Four, `camera-jpeg`, by an accepted alternative | Receiver and parser status recorded before the sensor stop. The stop order is unchanged. |
| camera-8 | Four, `camera-jpeg`, in part | Shared overlap, deadline, admission-token and VIPP helpers. Each controller keeps its own step interpreter. |
| camera-10 | Five, `power-domains`, in part | A TWI9 controller and clock scope. No CAM2 binding exists. |
| display-3 | Five, `power-domains`, in part | The DE_SYS power provider powers domains 1, 9 and 10. DSI and deinterlace do not call it yet. |
| npu-7 | Six, `npu-runtime`, in part | A study runs one mimus linear layer through the pinned compiler. Bit-exact mimus layers on the NPU are blocked, because the compiler refuses INT32 output and a static input scale conflicts with mimus's per-row activation scales. |
| npu-8 | Six, `npu-runtime`, in part | Session memory reserved from the platform pool by model size, which is DDR in the extended cached profile. Pages stay 4 KiB. |
| video-8 | Five, `hevc-native` | The [native HEVC decoder](../hevc-native.md) image and host checks. Preparation refuses a launch bundle until physical allocation evidence exists. |
| video-9 | Four, `camera-jpeg` | JPEG images up to 640×480 with up to 512 KiB of input. Full HD needs about 5 MiB of uncached memory, which the JPEG image does not link. |
| x-duplication-5 | Four, `build-harness` | One CMake function defines board-derived applications and their build identity. |
| x-duplication-8 | Four, `build-harness` | One builder for returning images, which can build their platform from source. |
| checks-harness-2, checks-harness-9 | Four, `build-harness`, in part | Evidence snapshots hard-link identical files, and the SD0 fixture pins its evidence session. Evidence baselines and RAM checkpoints in checks that other components own are unchanged. |
| checks-harness-6, checks-harness-7, checks-harness-8 | Four, `build-harness` | Shared launcher checks, a shared ELF layout reader, and emulator hooks on modeled instructions only. |
| host-tools-6 | Four, `build-harness` | Shared claim and software-evidence validation across device runners. |

### Findings that remain deferred

| Finding | Reason it remains deferred |
| --- | --- |
| runtime-memory-5 | Needs pool initialization moved into the platform boot path, which the primary agent owns. Low severity. |
| runtime-memory-7 | Adopting a helper for the CPU-to-device ownership change spans about 35 sites in many components. Low value. |
| runtime-memory-8 | Merging `memmove.c` into `memory.c` edits the source lists of many image recipes. Low severity. |
| dma-iommu-4 | Generalizing the DMA pool session for UART and HDMI is a large design change that overlaps an unmerged DMA pool-session branch. |
| dma-iommu-8 | Saves microseconds per tick; wait until hardware function is established. |
| dma-iommu-9 | Low-severity cleanup; display and camera code read the mask fields directly. |
| clock-power-pins-9 | Removing the separate I2C engine edits NPU, GPU, check and single-core build files. Moving the PMIC onto the shared TWI engine needs board evidence of the S_TWI0 FIFO and DMA state after boot, and would undo the tuned PMIC poll latency. |
| clock-power-pins-10 | The DCDC2 and DCDC4 users in G2D, deinterlace, NPU and GPU must move together; moving only the media clock would add a second admission step without removing the bridge code. No current defect. |
| radio-net-12 | The refactor needs owned receive in the bootstrap phases and replaces four framing decoders, so it touches every radio worker without changing behavior. The faster SDIO profile it would serve is not physically qualified. |
| bluetooth-6 | Packet-loss tolerance should follow a physical measurement of the eSCO loss rate. An opt-in mode would also need the recognizer and the supervised Bluetooth host tool to accept captures with loss. |
| gpu-2 | A GPU compute interface for mimus needs a new DRAM reservation and a feasibility measurement. |
| video-7 | The verifiers withdrew the launcher-drift defect. What remains is cleanup across five applications that would change launchers, build identities and native checks, with no hardware to requalify them. |
| security-misc-7 | [docs/watchdog.md](../watchdog.md) now lists the five places that write the WDT0 values and the validators that require them, but the values are not shared. The launcher build tools copy only each launcher's own files, a host check requires the SMP launcher to match the observe launcher as text, and the expiry probe's binary hash is pinned. Low severity. |
| buses-pcie-4 | No measured need: the Bluetooth stages already drain up to about 192 KB/s against the 150 KB/s line rate with RTS/CTS enforced. A cyclic UART receive mode would also have to be added to the DMA driver. |
| supervisor-apps-3 (with bringup-gaps-11) | One shared contract version changes the contract layout in `platform/a733-smp-cached/boot.h`, every application's boot record, extended-boot patch and preparation script, and the EL3 writers, and so changes the identity of every board and extended image. Round eight examined it and left it. |
| supervisor-apps-6 | Snapshot consolidation edits the EL3 boot hooks of about 24 applications. Seven native checks call an application's session preparation without the board seal, the storage-read snapshot is the input to an SD session patch, and the DisplayPort and H.264 encoder images prepare their session before the board's. Round eight examined it and left it. |
| supervisor-apps-10 | Unifying the board and NPU provider roots changes the shared native USB header and the NPU startup and PMIC ownership, so that the NPU borrows providers instead of owning them. The same change would let a board-derived image link the NPU session runtime. |
| mimus-perf-2 | The NEON matrix kernel is in the external mimus repository, which this project does not modify. |
| mimus-perf-3 | The SDOT kernel is in the external mimus repository. |
| mimus-perf-4 | The CRC code is in the external mimus repository; `mimus-warm` removed the per-request CRC instead. |
| mimus-perf-8 | NEON quantization is in the external mimus repository. |
| mimus-perf-11 | Larger USB messages change the host protocol and need an approved board measurement. Low severity. |
| build-system-7 | The build-identity writes live in the component recipe fragments. Round three measured a 2.0 s full reconfigure and left the change to the fragment owners. |
| build-system-8 | 34 of 151 inherited objects differ between board-derived images, so shared object libraries would need one library per configuration and would change the layout of images tested on hardware. The gain is build time only. |
| host-tools-7 | The FEL transaction, observe and lock loops contact the board, and unifying them would reset existing guard budgets. Low severity. |
| checks-harness-5 | The G2D check chain is in files that the G2D owner holds, and no G2D package has taken it. |
| bringup-gaps-9 | Streaming recognition requires moving to a mimus revision with the streaming interface and re-establishing output parity. |
| x-coherence-2 | Generalizing the Normal-NC DMA policy should wait for a second Normal-NC DMA consumer. |
| x-coherence-8 | No package has taken GPU per-page cache maintenance; the GPU packages in rounds three and six changed other paths. |
| x-concurrency-6 | The interrupt table now exists and the OS image uses it. A shared task-side binding would still change about 11 applications and drivers whose recorded evidence binds their current patches. |

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
- Findings describe commit `70a4e63`. The four reports were not revised after
  the implementation rounds; the tables above record which findings those
  rounds addressed. The rounds were also verified on the host only.
- At the audited commit, some work was held on unmerged branches (HEVC decode,
  H.264 interframe references, UART DMA, DMA pool sessions), and the audit did
  not cover it. Round five reviewed the HEVC sources and integrated them.
