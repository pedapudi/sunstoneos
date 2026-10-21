# SunstoneOS implementation audit

This directory records a source audit of SunstoneOS, the FreeRTOS-based
operating system for the Orange Pi Zero 3W (Allwinner A733: eight CPU cores,
six Cortex-A55 and two Cortex-A76, a GIC-600 interrupt controller and 12 GiB of DDR). The
audit looked for four kinds of problem: performance limits, design and cleanup
work, latent defects, and features still missing for bring-up. It then grouped
the confirmed findings into implementation packages. Three rounds of fixes
based on those packages are merged; [Implementation rounds](#implementation-rounds)
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

Three rounds of fixes followed the audit and are merged into `bringup`:

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

Each package owned a disjoint set of files, so packages in one round could be
implemented in parallel and merged without conflicts. Every package passed
review before it merged. In the tables below, **done** means the review
accepted the package as complete; the outcome column notes any assigned
finding that was narrowed, met through an alternative fix that the verifiers
accepted, or left. **Partial** means the package reported part of its assigned
work as not done; the outcome column gives the reason. The findings column lists audit
identifiers; a package that only repaired checks, builds or review notes lists
none.

Three constraints applied to every package:

- Verification used host builds, emulators and host models only. No package
  ran a board launcher, a FEL tool or any program that opens a USB device.
- Packages did not edit [docs/hardware.md](../hardware.md) or
  [docs/handoff.md](../handoff.md); the integrator updated them after merging.
- Changing an image changes its identity. Images that were frozen with
  recorded evidence keep that evidence; a rebuilt image needs new evidence.

No physical board test ran in any round, so no change below is physically
tested or qualified.

### Round one

| Change | Package | Findings addressed | Outcome |
| --- | --- | --- | --- |
| EL3 recovery cleanup that re-derives the GIC bank count and clears shared peripheral interrupts enabled during a run; ordered counter reads; a CPU-identity check that tests both affinity levels; an eight-core emulator scheduling soak; the mimus asynchronous two-core pipeline; corrected SMP status records | `multicore` | cpu-startup-1, cpu-startup-5, cpu-startup-7, x-coherence-10, smp-9, mimus-perf-7; smp-2 and bringup-gaps-10 in emulation only; smp-1, smp-8 and build-system-11 in part | Partial. The uncached eight-core diagnostic is marked superseded instead of corrected (smp-1). A compile-time check refuses a configuration with other than eight cores, but include order still selects the configuration (smp-8, build-system-11). Physical eight-core scheduling, recognition on the A76 cores and the lock and polling measurements need the board. Not done in any round: cpu-startup-4, cpu-startup-6, cpu-startup-8, cpu-startup-10, smp-3, smp-4, smp-5, smp-7, smp-10, x-concurrency-2, x-concurrency-7, x-concurrency-9, x-duplication-4, buses-pcie-10, mimus-perf-6 and bringup-gaps-8. Later rounds addressed bringup-gaps-2, supervisor-apps-9, supervisor-apps-2, x-concurrency-5 and npu-9 from this package's list. |
| Word-sized `memcpy`, `memmove` and `memset` that are safe at EL3 with the MMU off, and an ELF check that refuses unsafe library routines in EL3 code | `runtime-string` | runtime-memory-1, runtime-memory-2, build-system-12 | Done. |
| Bounded retry of transient lock and registry BUSY results; a time-bounded fair counter-clock lock; PMIC polls that advance several bus phases; faster Type-C and USB host polling | `contention-polling` | clock-power-pins-2, usb-3, cpu-startup-11, clock-power-pins-7, x-concurrency-3, usb-9, clock-power-pins-6, usb-4, clock-power-pins-14 | Done. clock-power-pins-11, which proposes caching the TWI adapter's registry slot, was assigned but is not among the addressed findings. |
| Display-engine replay race with frame end, HDMI PHY lock wait, HDMI-audio Normal-NC shareability check, cheaper display polling | `display-hdmi` | display-1, display-2, x-coherence-1, audio-5, display-6, display-7, display-8, display-9, display-10 | Done. For display-9 the DSI task yields while the PLL holds the register mutex, an alternative the verifiers accepted, instead of batching inside the PLL driver. |
| Decoder admission of the board's 26 MHz crystal, BUSY retry in video-engine register access, cheaper guard scans and per-tick pacing | `video` | video-1, video-2, video-3, video-4, video-11 | Done. |
| Release of unhandled HCI events, fixed-period SCO audio, acceptance of a legal codec change during audio, discard of peer data during a local disconnect, one shared range helper | `bluetooth` | bluetooth-1, bluetooth-3, bluetooth-8, bluetooth-7, bluetooth-10 | Done. |
| FEL timeouts sized by bytes moved, append-only session journals, libraries in evidence, board-test queues bound to a USB port, checked launcher text replacement | `host-tools` | host-tools-1, host-tools-2, host-tools-3, host-tools-4, host-tools-5, host-tools-8, host-tools-9, host-tools-10, host-tools-11, supervisor-apps-7, x-bounds-1, x-duplication-3 | Done. Runners that check the binding refuse a queue with no recorded port, so both live queues must be bound before the next supervised launch. |
| Stage-2 loader, two-slot card layout with fallback, and the install, update and rollback procedure | `standalone-boot` | cpu-startup-2, storage-6, bringup-gaps-3, bringup-gaps-4 | Partial. Copy and hash verification at EL3 with caches off are implemented and modeled. The loader built no launch contract; round three added it. Native cold SD admission refuses, because no fixed-board subset of the clock, pin, rail and PMIC providers exists at EL3. Remote-writer exclusion is unresolved. |
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
| The DMA service worker woken by the DMA0 interrupt instead of a 1 kHz poll | `dma0-interrupt` | dma-iommu-2 | Done in the service. The platform interrupt binding is not wired, so every image still polls. |
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
| xHCI Disable Slot on detach with page clearing for reattach, and a 1 s bound on a held transaction error | `usb-host-detach` | usb-8 | Done in the xHCI core. The TinyUSB adapter does not call the detach path yet. |
| Rearm of retained captures, discarded warm-up images, OV5640 block writes, camera USB0 through the shared board service | `camera-capture` | camera-1, camera-9; camera-7 in part | Partial. camera-10 needs TWI9 support in the I2C and clock drivers. Moving the camera onto the board framework would change the session and wire format that the supervised camera host tool reads. |
| A separate camera TWI11 mutex; the PB4–PB8 pin entry points running on the bank-qualified implementation | `power-consolidation` | clock-power-pins-1, clock-power-pins-13 | Partial. clock-power-pins-9 and clock-power-pins-10 remain deferred. |
| The maintenance session selects the SD bus profile; backup requests read eight sectors | `storage-update-speed` | storage-5 | Done. Writes stay at one sector per request, because two sectors could exceed the one-second operation deadline on a conforming card. |
| Shared outbound iATU programming checked against a recorded request; a configurable PCIe link speed target | `pcie-general` | buses-pcie-5 | Done. BAR assignment and MSI setup stay in the endpoint test driver until a second endpoint driver exists. Both applications still select 2.5 GT/s. |
| Central build patches applied without fuzz; supervised profiles require an explicit supervised board mode; a refusal helper and a cache override in the check harness | `build-safety` | build-system-5, build-system-10, checks-harness-4, checks-harness-13 | Partial. About 30 standalone projects keep their own fuzzy patch function, four patches are pinned by hash because they still need fuzz, and existing checks were not migrated to the new helpers. build-system-7 remains deferred. |

## Deferred findings

The original plan deferred the findings below. A finding listed with merged
identifiers stands for all of them.

### Deferred findings implemented later

| Finding | Round and package | What exists now |
| --- | --- | --- |
| cpu-startup-2 (with storage-6, bringup-gaps-3) | One, `standalone-boot`; three, `integrated-os` | The [stage-2 loader](../boot-slots.md) verifies and loads a slot and builds the resident contract in host models. On a board its entry and SD admissions refuse. |
| bringup-gaps-4 | One, `standalone-boot` | Boot slots with fallback and the install, update and rollback procedure, in host tools and models. No card has been written. |
| storage-7 | Two, `boot-tools` | Card updates are planned as one storage session per changed range. |
| dma-iommu-2 | Two, `dma0-interrupt` | The DMA service can sleep on the DMA0 interrupt. No platform binds it yet. |
| security-misc-1 | Two, `thermal-policy` | A fixed thermal policy ends the thermal application and the OS thermal service at the critical level. No consumer reduces load at the throttle level. |
| gpu-8 | Two, `gpu-model-links` | [docs/gpu.md](../gpu.md) describes completed-job compute admission. |
| bringup-gaps-1 (with supervisor-apps-1) | Three, `integrated-os` | The integrated OS image. I2S capture, radio, crypto, display, camera, GPU and NPU are not part of it. |
| bringup-gaps-12 | Three, `integrated-os` | The OS link check reads the model, recording and arena extents from shared headers and refuses overlaps and extents outside the loader window. |
| usb-8 | Three, `usb-host-detach` | Detach and reattach in the xHCI core. The TinyUSB adapter does not use them yet. |
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
| buses-pcie-5 | Three, `pcie-general`, in part | Shared iATU programming and a configurable link speed target. BAR and MSI setup stay in the endpoint test driver. |
| build-system-5 (with x-duplication-2, build-system-13, supervisor-apps-8), build-system-10, checks-harness-4, checks-harness-13 | Three, `build-safety`, in part | Central patches apply without fuzz, supervised profiles need an explicit mode, and the harness offers the new helpers. Standalone projects and existing checks were not migrated. |

### Findings that remain deferred

| Finding | Reason it remains deferred |
| --- | --- |
| cpu-startup-3 (with clock-power-pins-5, mimus-perf-1, bringup-gaps-6) | CPU frequency and voltage scaling cannot be finished offline. Mapping the CPU rails (DCDC5 and DCDC3), reading the voltage-frequency bin from eFuse and writing voltages could hang the only board, which cannot be reconnected. The thermal policy that any increase must depend on now exists, but it reads unqualified temperatures. Needs primary-agent planning and approval for physical tests. |
| cpu-startup-9 (with clock-power-pins-12, x-concurrency-8, x-duplication-6, buses-pcie-9, security-misc-10) | Consolidating the task-context check touches about 15 native adapters and FreeRTOS port internals. Not yet scheduled. |
| runtime-memory-5 | Needs pool initialization moved into the platform boot path, which the primary agent owns. Low severity. |
| runtime-memory-6 (with x-coherence-4) | A shared identity-range helper would touch the GPU, USB, NPU, HDMI-audio and DMA mapping checkers. Round three added one helper inside the GPU only (gpu-7). |
| runtime-memory-7 | Adopting a helper for the CPU-to-device ownership change spans about 35 sites in many components. Low value. |
| runtime-memory-8 | Merging `memmove.c` into `memory.c` edits the source lists of many image recipes. Low severity. |
| dma-iommu-4 | Generalizing the DMA pool session for UART and HDMI is a large design change that overlaps an unmerged DMA pool-session branch. |
| dma-iommu-8 | Saves microseconds per tick; wait until hardware function is established. |
| dma-iommu-9 | Low-severity cleanup; display and camera code read the mask fields directly. |
| clock-power-pins-9 | Removing the separate I2C engine edits NPU, GPU, check and single-core build files. Moving the PMIC onto the shared TWI engine needs board evidence of the S_TWI0 FIFO and DMA state after boot, and would undo the tuned PMIC poll latency. |
| clock-power-pins-10 | The DCDC2 and DCDC4 users in G2D, deinterlace, NPU and GPU must move together; moving only the media clock would add a second admission step without removing the bridge code. No current defect. |
| radio-net-3 | Splitting receive aggregates needs hardware evidence that the AIC8800 accepts split CMD53 reads or a larger request size. |
| radio-net-8 | Coalescing patch-table writes departs from the vendor per-word sequence and needs a hardware readback check. |
| radio-net-12 | Refactoring the radio transport is large and risky; the faster SDIO profile it would serve is not physically qualified. |
| bluetooth-6 | Packet-loss tolerance should follow a physical measurement of the eSCO loss rate. An opt-in mode would also need the recognizer and the supervised Bluetooth host tool to accept captures with loss. |
| audio-1 | A guard band in the cyclic receive copy changes a physical-admission condition that is already blocked and needs board timing evidence. |
| audio-2 | Low severity. Measure the real FIFO margin on hardware before redesigning capture. |
| display-3 | DSI needs PCK domains 0, 2, 3, 8 and 9 as well as domain 1, and no qualified VO0 or IOMMU domain sequence exists. Round three documented the required powered entry state instead of powering the domains. |
| camera-6 | Changing the stop order depends on unverified OV5640 register semantics and departs from the Linux reference. |
| camera-8 | Merging the register-program interpreters changes polling granularity and needs requalification. |
| camera-10 | A CAM2 profile needs a native TWI9 controller and a TWI9 clock-registry entry, and the receiver port-1 deskew value has no qualified source. [docs/camera.md](../camera.md) lists the missing pieces. |
| gpu-2 | A GPU compute interface for mimus needs a new DRAM reservation and a feasibility measurement. |
| npu-7 | A mimus network binary that keeps mimus quantization needs compiler and quantization research. |
| npu-8 | Larger NPU pages and reservations depend on mimus-scale NPU models that do not exist yet. A new layout would also need a DDR reservation outside image RAM and would invalidate recorded page-table evidence. |
| video-7 | The verifiers withdrew the launcher-drift defect. What remains is cleanup across five applications that would change launchers, build identities and native checks, with no hardware to requalify them. |
| video-8 | The HEVC parser, controller and bridge remain on a separate unreviewed branch. Configuration now stops and lists the missing files. |
| video-9 | A larger JPEG profile should follow the first physical decode of the existing fixture. |
| security-misc-7 | WDT0 consolidation touches launchers owned by the primary agent. Low severity. |
| buses-pcie-4 | No measured need: the Bluetooth stages already drain up to about 192 KB/s against the 150 KB/s line rate with RTS/CTS enforced. A cyclic UART receive mode would also have to be added to the DMA driver. |
| supervisor-apps-3 (with bringup-gaps-11) | The resident contract now exists, but replacing the private extended records of at least seven applications with one shared contract version has not been scheduled. |
| supervisor-apps-6 | Snapshot consolidation edits the EL3 boot hooks of about 24 applications. |
| supervisor-apps-10 | Unifying the board and NPU provider roots is a large change that overlaps the NPU and OS-integration work. |
| supervisor-apps-12 | Service-task affinity matters only once recognition runs on several cores. |
| mimus-perf-2 | The NEON matrix kernel is in the external mimus repository, which this project does not modify. |
| mimus-perf-3 | The SDOT kernel is in the external mimus repository. |
| mimus-perf-4 | The CRC code is in the external mimus repository; `mimus-warm` removed the per-request CRC instead. |
| mimus-perf-8 | NEON quantization is in the external mimus repository. |
| mimus-perf-11 | Larger USB messages change the host protocol and need an approved board measurement. Low severity. |
| build-system-7 | The build-identity writes live in the component recipe fragments. Round three measured a 2.0 s full reconfigure and left the change to the fragment owners. |
| build-system-8 | Shared object libraries for board-derived images are large and change image identities. Low severity. |
| host-tools-6 | Consolidating supervised wrappers is large and must keep a central reviewed capture-limit table. |
| host-tools-7 | Consolidating FEL session loops. Low severity. |
| checks-harness-2 | Promoting evidence baselines into fixtures touches many checks owned by other components. |
| checks-harness-5 | The G2D check chain belongs to the G2D work. |
| checks-harness-6 | Launcher-check consolidation. Not yet scheduled. |
| checks-harness-7 | Moving the ELF layout helper changes evidence snapshot names. |
| checks-harness-8 | Ranged emulator hooks touch many checks owned by other components. |
| checks-harness-9 | Redesigning the evidence store changes recorded hashes. Low severity. |
| bringup-gaps-9 | Streaming recognition requires moving to a mimus revision with the streaming interface and re-establishing output parity. |
| x-coherence-2 | Generalizing the Normal-NC DMA policy should wait for a second Normal-NC DMA consumer. |
| x-coherence-6 | MMIO accessor consolidation spans about 59 files across all components. |
| x-coherence-7 | Unifying cache-operation enums spans DMA and NPU headers. |
| x-coherence-8 | GPU per-page cache maintenance was not part of the round-three GPU work. |
| x-concurrency-6 | The interrupt table now exists and the OS image uses it. A shared task-side binding would still change about 11 applications and drivers whose recorded evidence binds their current patches. |
| x-bounds-4 (with x-duplication-9) | A global range-helper header spans about 190 sites in many components. |
| x-duplication-5 | Consolidating board-application recipes is large and changes image identities. |
| x-duplication-8 | Building returning images in CMake is large; the header check for reused objects already guards their inputs. |

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
- Some packages exclude files held on other unmerged branches (HEVC decode,
  H.264 interframe references, UART DMA, DMA pool sessions). Those branches
  were not audited.
