# Host validation measurement, 2026-11-01

This record measures which host checks catch defects that later fail on the board, and classifies the checks that the full gate runs. The coordinator uses it to decide which checks leave the gate. Judgments that could not be confirmed from commits, check diffs or logs are marked "unconfirmed". The classifications were produced by reading each check and what it builds. No check was re-run on pre-fix code, so every "reproduces" entry means that the fix commit adds or changes a check case that fails on the old code according to the commit and its diff.

## Method

- Board runs: every queue entry in `~/.cache/sunstoneos/board-queue/entries/` dated 2026-10-25 or later (205 entries). A run counts as a failure when its outcome is not `returned` or when it carries a first-failure record.
- Gate: the checks of the full gate run `gate-land-npu-qual-1101-1101-201649` (bringup 49280ebd), with the seconds from its `timing.txt`. The gate ran 117 check steps: 110 indexed pure-host checks and 7 named core checks. The other 729 indexed checks need inputs (evidence, sources, a board) and do not run in the gate.
- Times are gate seconds with warm build caches and 8 parallel checks; the sum is about 413 check-seconds.

## Board failures against host checks

Of the 205 queue entries since 2026-10-25, 66 did not pass: 48 image failures, 15 infrastructure failures (launch refusals, FEL or USB recovery, daemon interruption, queue invalidation) and 3 failures that the hypothesis intended (forced-failure tests and a strict-budget baseline).

| Question about the 48 image failures | Count |
| --- | --- |
| A host check covered the failing behavior before the run | 0 fully, 22 partially (a check existed but did not cover the failing condition), 24 not at all, 2 unconfirmed |
| A check added with the fix fails on the pre-fix code | 27 yes, 7 no, 14 unconfirmed |

Causes of the 48 image failures: hardware view or inherited state unknown to the code or model 17, own-logic bug 10, exploratory probe whose purpose was to learn hardware state 8, service protocol 6, performance or timing miss 5 (no host check models board lock timing), wrong expectation 2.

No failure had a host check that fully covered it before the run. In the cases where a check existed, the fixture or model shared the code's assumption. The clearest cases are the four in the brief (GIC priority view 0xb0 against 0xd8, OPEN reply BOOTING, WAIT refused) and these, all documented in the detail file:

- A fixture echoed the written value for the GPU reset register (the GPU stop host check, since removed); the board reads 0xe8000020.
- Probe entry checks required cold-boot values (secondary CPU static OFF, VIDEO_IN reset bit 16 clear, ISP_AHB_CFG0 zero, AUDIO1 reserved bit 24 clear) where the board inherits other values.
- The deadline fixture used a nonzero sealed session deadline where the board has zero (`checks/deadline-host.c`).
- The QEMU board entered cores with a different SP_EL0 than the real launch.
- The scheduler model and its expectation both omitted MPIDR.MT.
- The GPU queue check treated a transient PARKED as a failure.

Defects that a host check caught before any board run, since 2026-10-25: 3 confirmed, 3 unconfirmed, and 20 gate failures that are not defects of the code under test (fixture drift, documentation links, environment).

| Date | Result | Check | Fix | Reason |
| --- | --- | --- | --- | --- |
| 2026-10-25 | catch | GPU diagnostic image host checks (first deadline check) | 22501205 | Built against the older checkpoint power.c, the GPU client read every power-service field 8 bytes from where it was written because bringup had changed embedded struct layouts; host checks failed at the first deadline check and the builder now recompiles the changed units (exact check name not recorded, so the check is inferred from the design note). |
| 2026-11-01 | catch | os-npu-queue-build (gate npu-idle-link) | 8552410f | The NPU queue OS image failed to link with undefined npu_idle_* symbols because the new idle owner source was missing from the core build list; the one-line cmake fix followed. |
| 2026-11-01 | catch | central-configure (gate merge-gfp-1101) | a740d851 | After merging the NPU EL0 ring, the GPU call patch hunk no longer applied to the FreeRTOS port assembly and configure failed; the hunk was re-anchored after the new SVC block. |
| 2026-11-01 | unconfirmed | scheduler migration drain model (negative control) | 2f375067 | A finite model with a negative control reproduced a stale drain acknowledgment race in the proposed migration design and the design was changed, but the owner fork is disabled so no shipped code failed and I could not confirm a failing run. |
| 2026-11-01 | unconfirmed | NPU PMIC pin linked regression | 18e85a4b | The commit says the old construction template made native PMIC pin acquisition refuse before READY and a linked regression reproduces it, but I cannot tell whether the board or the host saw it first. |
| 2026-11-01 | unconfirmed | thermal owner fixture (cadence and coverage cases) | f253e59d | The fixture reproduces an earlier unseen first sensor row and a coverage failure, but the commit text does not say whether the original failure was on the board or the host. |
| 2026-11-01 | not-a-catch | qemu-os-boot (gates scheduler-b-round12-full, hw-facts-1101b, hw-facts-1101c) | f145bb71 | Guest time ran on the host wall clock so the resident silent-transport and normal-start cases failed under host load (11 of 12 runs with busy loops); fixed by counting instructions, an environment flake. |
| 2026-11-01 | not-a-catch | reproduction of pre-start yield on the virtual OS board | 605adf85 | The data abort 0x9600004e was first seen on the board and the QEMU negative control only reproduced it afterward, so the board found the defect. |
| 2026-11-01 | not-a-catch | startup faults 0x757 and 0x997 host checks | 15914f7c | Both startup faults were seen on the board first and the host checks were then extended to model them. |
| 2026-10-25 | not-a-catch | GPU second-ON gate check (a733-gpu-second-on) | 3c49e2e4 | The host check reproduces the board failure exactly only when built against the tree before the fix, so the board found the defect first. |
| 2026-11-01 | not-a-catch | indexed-reset-latency (gate startup-diag-1101) | e4047776 | The host model link failed with undefined board_runtime_deadline_us after the OS code started calling it, so the fixture lacked a stub (stale fixture, not an OS defect); a96c8c1a later added the accessor again. |
| 2026-11-01 | not-a-catch | indexed-reset-latency (gate usb-vbus) | e4047776 | The host model compile failed on an undeclared xTaskGetCurrentTaskHandle in apps/os/services.c because the model lacked the FreeRTOS declaration; fixture drift fixed in the model. |
| 2026-11-01 | not-a-catch | indexed-gpu-clients (gate merge-gfp-1101b) | 87db529a | The host build lacked portMAX_DELAY and task-notify declarations once the client queue gained fast-path calls; the check was updated to model the new entry points. |
| 2026-11-01 | not-a-catch | indexed-gpu-texture-frame (gate merge-gfp-1101c) | ecc1471e | The fixture did not extract step_locked after the service step was split, so it failed to compile; the fixture was updated to the new structure. |
| 2026-11-01 | not-a-catch | indexed-integration-gate (gate npu-fastpath-2) | c25921c0 | The gate self-test expected 13 OS variants but the intended new npu-queue variant makes 14; the expectation was updated. |
| 2026-11-01 | not-a-catch | check-progress (gates gq-drt, npu-fastpath, audio-land) | 0b5a3590 | The status parser rejected a merged GPU or audio status file for breaking the status-record format, a documentation format fault with fixes 0b5a3590 and d4a7e4e5, not code. |
| 2026-11-01 | not-a-catch | indexed-a733-clock-reference (gate land-1101) | 2c036e70 | Clang -Werror rejected an unused reset helper in the shared MMC fixture preamble, a check-only warning. |
| 2026-11-01 | not-a-catch | indexed-docs-links (gate land-1101b) | 05642b81 | Twelve documentation links pointed at files moved to mimus or removed, a documentation defect and not code. |
| 2026-11-01 | not-a-catch | indexed-docs-links (gate lat-runner) | 2651238d | Four latency status links pointed at line paths that did not exist in the tree, repaired with line anchors; documentation only. |
| 2026-11-01 | not-a-catch | indexed-docs-links (gate host-shim-1101b) | c8cb41bb | Two documents still linked the retired os-image-stage2 check, a documentation defect. |
| 2026-11-01 | not-a-catch | indexed-vendor-firmware-sources (gate evidence-records-1101) | - | A firmware source link returned HTTP 500 from the remote server, an external and transient environment problem with no fix. |
| 2026-10-31 | not-a-catch | check-kernel (gate layer1-core-kernel-1031) | - | QEMU failed to load the ui-opengl module from a mismatched build, an environment problem. |
| 2026-10-31 | not-a-catch | check-mimus-smp (gate layer1-mimus-app-1031) | - | The same QEMU ui-opengl module mismatch stopped the check, an environment problem. |
| 2026-11-01 | not-a-catch | indexed-board-runner (gate gs-old2) | - | The gate ran old gate code against the new tree on purpose for timing and the old invocation lacked the required --out argument. |
| 2026-11-01 | not-a-catch | indexed-a733-gpu-isolation (gate host-shim-1101) | 237f2878 | The converted check was invoked without the required elf argument, a harness invocation problem fixed by selecting the emulator entries by flag. |
| 2026-11-01 | not-a-catch | selected steps (gate npu-qual-start-diag) | - | The gate refused to finish because the source tree changed during the run, a procedure problem. |

All three confirmed catches are build, link or configure failures (structure layout mismatch in an older checkpoint, a missing source in the build list, a patch hunk that no longer applies). No confirmed catch is a behavioral defect found by a check that models behavior. The scheduler model commits e11e42f8 and 4a167cc2 and d4bd3a04 were not traced, so a few host catches may be missing.

## Gate checks by class

Classes: (a) own logic; (b) linked real code on both sides; (c) safety before a board run; (d) hardware or vendor model with a citation for each behavior; (e) hardware or vendor model without such citations; (f) other. A "+e" mark means the primary class is (a) or (b) and the check also relies on an uncited model of a device, card, register layer or service reply. These are candidates for the same review as class (e), but they carry the real code on both sides. Owners: NPU is the NPU owner, SCHED is the scheduler stream, OTHER is anyone else.

| Class | Checks | Gate seconds |
| --- | --- | --- |
| a | 46 | 127.2 |
| b | 23 | 115.6 |
| c | 23 | 46.5 |
| d | 5 | 33.6 |
| e | 9 | 45.9 |
| f | 11 | 44.3 |
| total | 117 | 413.1 |

| Class | Gate seconds | Owner | Check | Note |
| --- | --- | --- | --- | --- |
| a +e | 56.5 | OTHER | reset-latency | Compiles the real apps/os/services.c with a modeled counter and WDT0 and parks each core at every phase to check reboot within 4.02 s with the supervisor's stop detail, plus a baseline negative control (secondary e: WDT0 model). |
| a | 7.8 | OTHER | runtime-string-routines | Our memcpy/memmove/memset compiled for AArch64 and run under Unicorn against a byte-wise reference with alignment and range checks. |
| a | 7.5 | SCHED | smp-port-cost | Counts instructions of our patched FreeRTOS SMP port per operation on QEMU and gates regressions against a baseline and reduction targets. |
| a | 4.4 | SCHED | soak8 | Eight-core SMP soak of the real FreeRTOS SMP kernel image on QEMU; tests the scheduler port, secondary class (f) emulator run. |
| a | 4.1 | OTHER | usb-keyboard-client | Legacy HID keyboard mailbox under sequential and concurrent reads with a mutant negative control. |
| a | 3.8 | OTHER | bluetooth-capture-profile | Compiles the real Bluetooth application with the real FreeRTOS kernel and TCP stack in several variants to check capture-profile admission, binding and pin isolation; secondary class (b). |
| a | 3.5 | OTHER | pvr-gpu-flush | Links the real GPU MMU cache invalidation, control, queue and firmware sources under sanitizers and checks the generated commands; the pinned-driver comparison is optional (secondary d). |
| a | 3.4 | OTHER | display-command-admission | Sanitizer run of real G2D and display CPU admission and command span construction with a git baseline that must fail. |
| a | 3.4 | OTHER | memory-reservations | Real device memory pool allocator and lease ownership run on host, plus link-layout and capacity checks of the native image; the allocator logic is checked against its own rules. |
| a +e | 2.7 | OTHER | os-gpu-thermal | Runs the real native GPU thermal caller as a task on the real FreeRTOS kernel under the host shim to check reopen, close and retire protocol cases (secondary e: GPU service replies and thermal input are mocked). |
| a | 2.5 | OTHER | storage-digest | Our storage SHA-256 against hashlib and NIST vectors on host and under AArch64 emulation. |
| a | 2.4 | OTHER | power-hold-boundaries | Runs the native USB minimum-hold wait logic with fractional, costly triggers and a counter model to check hold origin and hard deadline rules; the Cadence PHY delays come from the pinned source. |
| a +e | 2.3 | OTHER | os-thermal-cadence | Runs the real thermal owner step, policy, record writer and window publication on the real kernel under the host shim to check absolute-deadline cadence (secondary e: delayed sensor reads, clock completion and GIC are modeled). |
| a | 1.8 | SCHED | scheduler-topology-app | Application topology queries over disabled, incomplete and calibrated data against our own topology rules. |
| a +e | 1.7 | OTHER | rtc-alarm-ownership | Real RTC driver runs against a hand-written alarm and callback model for ownership, callback failure and calendar rules (secondary e: uncited register model). |
| a | 1.6 | OTHER | progress | Unit tests of the progress tool parser, caching, read-only queue access and HTTP boundary on a copied fixture tree. |
| a | 1.6 | OTHER | usb-memory | USB memory pool, session geometry and AArch64 table construction and cold checksum run offline against our own layout rules. |
| a | 1.5 | OTHER | isp0-pixel-reference-check | Tests the CPU Bayer reference for framing, neutral endpoints, phase and mismatch reporting. |
| a +e | 1.5 | NPU | npu-board-capture | Runs the real NPU board-capture host tool and retained supervisor logic against a modeled USB device, covering packet framing, durability and wrong-output refusal (secondary e for the USB device stub). |
| a | 1.4 | OTHER | hw-facts | Checks the facts table layout, decoder, facts file and a scan that host GIC models do not hard-code facts-file values (secondary d). |
| a | 1.3 | SCHED | scheduler-topology | Portable CPU topology implementation with the A733 data and synthetic fixtures run under sanitizers. |
| a | 1.2 | OTHER | board-probe-abort | Runs the real EL3 probe abort code in Unicorn with a memory and write-hook model to check first-fault latch, ordering and recursion suppression; GPIO and crumb register behavior is uncited, secondary class (e). |
| a | 1.0 | OTHER | ddr-capture-analysis | Tests tools/analyse-ddr-captures.py classification on synthetic captures with planted registers (secondary c, feeds the DRAM restore set). |
| a | 0.9 | OTHER | shared-helpers | Runs range, deadline, MMIO, mapping and task-context helpers under sanitizers and checks disassembly barrier placement on AArch64 and AArch32. |
| a +e | 0.9 | NPU | npu-queue | Runs the real NPU queue and sequence code on the host against a device model to check ordered fences, ISR dispatch, retained DMA ownership and the registration race (secondary e for the device model). |
| a | 0.8 | OTHER | audit-consolidate-apps-a-h-equivalence | Compares former inline mapping, deadline and range helpers with the shared header replacements over seeded edge values. |
| a | 0.8 | OTHER | crypto-staging | Runs real crypto orchestration and staging erasure against a mocked CE driver, so the weight is our erase and sequencing logic. |
| a | 0.6 | OTHER | camera-observe | Tests the offline NV16 decoder and color-bar structure test on synthetic bars that the script says are not defined by pinned sources. |
| a | 0.5 | OTHER | audit-host-tools-hevc-journal | Exercises the HEVC capture host tool journal cost and durability against a synthetic USB device and session; geometry admission is stubbed. |
| a | 0.5 | OTHER | radio-native-clock | Runs the real radio clock-write callback in Unicorn to check the owner-context predicate and address and value filters against modeled RTOS state and MMIO. |
| a | 0.4 | NPU | npu-idle-report | Decodes a synthetic NPU idle benchmark report with the production decoder and comparison, corrupts each constraint, and diffs the decoder's labels against the producer source. |
| a | 0.4 | OTHER | usb-host-execution | Native USB execution gate functions extracted from the driver and run with modeled task admission. |
| a | 0.3 | OTHER | app-usb-service | Compiles the real USB application ownership broker with a host test main and checks grants, disconnect and release. |
| a | 0.3 | OTHER | bt-memory | Sanitizer unit run of the shared Bluetooth range and deadline helpers plus a source scan forbidding local saturating sums. |
| a | 0.3 | NPU | npu-source-output | Exercises the NPU generator tools' output handling, directory and report logic with numpy; ONNX graph writing and SDK execution are modeled when ONNX is absent. |
| a | 0.2 | SCHED | kernel | Runs the real FreeRTOS foundation image on QEMU virt and requires the PASS kernel line, with cached evidence; secondary class (f) emulator execution. |
| a | 0.2 | OTHER | crypto-health-cutoffs | Checks the script's repetition and adaptive cutoff arithmetic against values published in NIST SP 800-90B section 4.4 (secondary d, spec-cited). |
| a | 0.2 | NPU | npu-conformance | Checks the NPU conformance tool's vector plan, seeded bytes, int32 bias rule, per-model verdict, runner lanes and pinned-file refusal against the tool's own design. |
| a | 0.2 | SCHED | smp-ticket-lock | Exhaustive state-space exploration of our ticket lock algorithm model (a hand-written model of the patch, not the compiled code). |
| a | 0.2 | OTHER | storage-read-uhs-host | Decoder and launcher throughput plan for UHS-I SD0 records accept/refuse tests on synthetic records (secondary c: refusals gate record acceptance). |
| a | 0.1 | OTHER | fault-report | Tests the host fault-report reader against a device model whose rules are parsed from apps/os/usb0.c and the app header, so it checks our own protocol. |
| a | 0.1 | OTHER | gpu-compute-application | Tests the host decoder for copied GPU compute USB frames, including refusal of damaged and mismatched frames. |
| a | 0.1 | OTHER | gpu-display-host | Checks the host packer for the bounded GPU and display application ABI and its envelope limits. |
| a | 0.1 | NPU | npu-runtime-bounds | Computes the NPU runtime's start, reply and load worst-case bounds per image from the catalogue and refuses a client collect timeout shorter than the reply bound. |
| a | 0.1 | OTHER | thermal-report-cadence | Thermal report slot-lateness and missing-sample classification plus agreement of the USB and returned capture decoders on synthetic reports. |
| a | 0.1 | OTHER | usb-access-discover | tools/usb-access.py discovery on a fixture sysfs tree treats ENODEV/ENOENT as absent and propagates other errors. |
| b +e | 26.4 | OTHER | qemu-os-boot | Boots the real OS image with the real application on an eight-core QEMU virtual board to the first application task; thermal sensors, PMIC, clock providers, USB transport and NPU/GPU services come from a virtual board layer (secondary e). |
| b +e | 22.7 | OTHER | gpu-texture-frame | Real GPU render service worker and texture sources run under Unicorn to count CPU work; device progress, rings and RTOS boundary are modeled (secondary e). |
| b +e | 11.9 | OTHER | usb-generic-host | Real TinyUSB classes, xHCI driver, platform provider and OS broker client linked against a hand-written USB peripheral model (secondary e: peripheral traffic uncited except pinned RPLIDAR facts). |
| b | 8.4 | OTHER | a733-clock-reference | Builds real clock lease, rail lease, PMIC, pin and MMC/USB platform drivers together with only the registers, time and card modeled. |
| b +e | 6.5 | OTHER | usb-host-ready-work | Real board wait and xHCI core plus host IRQ code on the real kernel with delayed register effects and events modeled, plus mutant controls (secondary e: controller timing uncited). |
| b +e | 4.9 | OTHER | storage-read-retain | Additive MMC retain API run against the real storage and MMC managers with a modeled card (secondary e: card model uncited). |
| b +e | 4.6 | OTHER | sd-return-close | Real storage update, MMC platform, sunxi MMC, clock, rail and PMIC providers linked together with only the card and MMIO layer modeled (secondary e: card/PMIC model uncited). |
| b | 4.3 | OTHER | a733-mmc-adaptive-voltage | Links real storage platform, MMC, PMIC provider, clock, rail and pin code against a modeled MMIO and card; secondary class (e) for the card model and PMIC register words. |
| b | 4.2 | OTHER | a733-mmc-rom-pins | Links the real MMC platform with shared pin, clock, rail and PMIC providers for the ROM JTAG-to-SD handoff; secondary class (d) because pin functions are asserted against the vendor pinctrl source. |
| b | 2.8 | OTHER | usb-host-deadline | Real USB host waiter and common deadline runtime on the real FreeRTOS kernel with compare/GIC modeled, plus mutant negative controls. |
| b | 2.5 | SCHED | deadline | Real FreeRTOS kernel under the host shim runs the real deadline module and service supervisor with negative controls; timer and GIC are scripted. |
| b | 2.3 | OTHER | gpu-clients | Real GPU client queue code on the real kernel with modeled tasks and services, a failing baseline and a mutation control. |
| b | 2.3 | OTHER | usb-serial | Real USB serial driver linked with upstream TinyUSB CDC host and the native broker for protocol, lifetime and ownership checks. |
| b | 2.0 | NPU | npu-sdk-events | Links the real NPU application mailbox runtime, the OS NPU service and the SDK client on the real FreeRTOS kernel with only hardware wakeups modeled. |
| b | 1.6 | OTHER | a733-spi-platform | Links the real SPI platform with real SPI core, clock lease and pin drivers; secondary class (d) because routes and reset hold are checked against vendor pinctrl, CCU and reset sources. |
| b | 1.5 | OTHER | gpu-job-timing | Real apps/os/gpu-timing.c on the real FreeRTOS kernel under the host shim with device callees stubbed at the boundary and a mutation control. |
| b | 1.3 | OTHER | dma-rx-copy-ownership | Real DMA controller and DMA core check cyclic staging exclusion with only register, cache and mapping callbacks replaced. |
| b +e | 1.3 | OTHER | jpeg-advance | Real JPEG run driver and USB camera owner code against a register latency model with delayed and stuck completion (secondary e: delay values uncited). |
| b | 1.0 | OTHER | device-classes-usb | Builds the real UVC protocol and camera adapter drivers against descriptors derived per docs/usb-uvc-sources.json with a mocked control transport (secondary d). |
| b +e | 1.0 | OTHER | dma-adopted-return | Real DMA platform and clock lease code against a host register model of DMA and CCU (secondary e: register offsets and busy behavior uncited). |
| b | 1.0 | NPU | npu-app-queue | Builds the real EL0 NPU ring producer and the real kernel NPU queue driver into one program with only the hardware doorbell and cache effects stubbed. |
| b | 0.6 | OTHER | os-session-host | Checks the real host session, prepare and card tools against records produced by the compiled C manifest and result producers (secondary c for record validation). |
| b | 0.5 | OTHER | h264-native-access | Real video platform, unit and device memory code run under Unicorn with inert MMIO storage to test the native read policy. |
| c | 24.2 | OTHER | board-runner | Runs tools/board.py against a fake FEL and board covering ownership, transfers, watchdog recovery and the durable queue admission; the model is test scaffolding for refusal logic. |
| c | 6.0 | SCHED | scheduler-latency-expect | Builds the scheduler latency board expectation and drives the real runner matcher to confirm it accepts valid reports and rejects each bounded-field violation (secondary a). |
| c | 3.4 | OTHER | board-usb-class | Checks USB runner admission, receipts and typed inputs on an inert ELF fixture that is refused or admitted by host bounds (secondary a). |
| c | 2.1 | OTHER | remote-writers-model | Runs the real remote-writer exclusion code in Unicorn over synthetic register states so that unsafe boot states are refused before a board memory run; reset bit tables cite manual 4.1.6 (secondary d). |
| c | 1.9 | OTHER | memtest-map-admission | Runs the real memtest mapping-fault routine over golden and mutated page tables to prove a block is refused before any RAM write. |
| c | 1.6 | OTHER | training-record | DRAM training record format and card update plan on an image file: payload-before-header, invalidation, and refusal of writes outside the unit (secondary a). |
| c | 1.4 | OTHER | standalone-boot-slots | tools/boot-slots.py on sparse image files: compare-before-write, foreign header, address-limit and interruption refusals that gate card writes (secondary a). |
| c | 1.2 | OTHER | audit-build-safety-configure | Checks that build patches apply exactly and that supervised profiles require an explicit board mode at CMake configure time. |
| c | 1.0 | OTHER | audit-host-tools-launcher | Checks that each adapted supervised launcher generator refuses a mutated shared launch anchor before compiling. |
| c | 0.5 | OTHER | pmic-tree | Source-text rules that keep PMIC selector writes, PSCI power calls and S_TWI0 use confined to approved owners so no stray voltage write can reach a board. |
| c | 0.4 | OTHER | power-domains-tree | Source-text rules that give PCK600 domains 1, 4 and 5 and the delay row one writer each, guarding board power sequences. |
| c | 0.4 | OTHER | audit-build-harness-snapshot | Checks that evidence snapshots hard-link only verified identical earlier files, protecting record integrity. |
| c | 0.4 | OTHER | fel-input | Checks SRAM preservation, restoration and stop-on-failure of the FEL input tool against a fake transport before any board run. |
| c | 0.3 | OTHER | audit-host-tools-snapshot | Checks evidence-snapshot linked-dependency collection over archives and linker inputs on a synthetic build tree. |
| c | 0.3 | OTHER | dram-prelaunch-refusal | Patches the board tool transactions to confirm launches are refused on DRAM mismatch, restore and transport failures before a launch. |
| c | 0.2 | OTHER | a733-storage-launch-chain | Runs the real update writer and launch input validation to confirm the sealed commitment table is accepted and write grants or existing outputs are refused for read-only capture. |
| c | 0.2 | OTHER | artifact-output-paths | Confirms grant, slot plan, launch bundle and test-input preparation tools refuse existing destinations so stale artifacts cannot be launched. |
| c | 0.2 | OTHER | audit-host-fixtures-hold | Checks that a closed console cannot end the shared ownership hold or skip quarantine after a launch failure. |
| c | 0.2 | OTHER | board-expectations | Checks board queue expectation parsing, result matching and refusal of malformed frozen expectations before any device is constructed. |
| c | 0.2 | OTHER | os-cpu-return-clock-review | Checks that returning-CPU clock grants are refused without a review record or with historical qualification files, gating a board run. |
| c | 0.2 | OTHER | supervised-capture-policy | Checks the supervised launch adapter delegates image, port and inputs to the runner and propagates its exit status without device access. |
| c | 0.1 | OTHER | host-evidence | Checks that repeated run directories never overwrite earlier evidence (secondary a). |
| c | 0.1 | OTHER | usb-disconnect | USB metadata disappearance, stale ROM instance quarantine and refusal in tools/usb-access.py without opening a device (secondary a). |
| d | 28.5 | OTHER | gpu-conformance | Compares exact GPU output with a pinned Mesa llvmpipe reference and tolerances derived from board scenes, plus comparator and plan logic (secondary a). |
| d | 3.0 | OTHER | pmic-supply-decode | Compares the PMIC supply selector decoding of the real provider with both pinned vendor sources to settle disputed selectors; models the PMIC supply registers against reference source text. |
| d | 0.9 | OTHER | a733-spi | Models the A733 SPI controller; register offsets, ports, IRQs and CCU bits are asserted against the vendor spi-sunxi driver, device tree, CCU source and manual, though FIFO timing latency is uncited. |
| d | 0.7 | OTHER | isp0-reference | Compares the fixed ISP600 register image against vendor setter sources pinned by sha256 in drivers/camera/isp0-sources.json. |
| d | 0.5 | OTHER | usb-serial-capture | Replays a pinned public FTDI FT2232H USB capture (hash and expected facts pinned in docs/usb-serial-traffic-sources.json) through serial receive. |
| e | 21.6 | OTHER | a733-pcie-trace | Real PCIe driver run against hand-written models of the controller, endpoint, NVMe device and link; assumes endpoint, NVMe controller and link-training responses with no cited source; Gen1 write trace is also compared with an earlier revision. |
| e | 8.8 | OTHER | pcie-qemu | Runs the real PCIe provider, MSI and NVMe code in QEMU against hand-written C models of the PCIe controller, PHY, endpoint function and NVMe device; link training, MSI and NVMe register behavior is assumed with no cited board fact or spec section. |
| e | 6.9 | OTHER | power-write-order | Runs native PMIC and legacy USB code in Unicorn against a posted-write latency model of the register bus and PMIC; the assumed ack-before-apply behavior of Device-nGnRE stores has no board fact or spec section cited, though a mutant control is run. |
| e | 3.0 | OTHER | a733-xhci-dma | Real xHCI core DMA rings against a hand-written controller and DMA model; assumes transfer completion and DMA write behavior with no spec section cited. |
| e | 2.6 | OTHER | a733-xhci-detach | Real xHCI core against a hand-written register, ring and context controller model; assumes controller event ordering and slot-disable behavior with no spec section cited. |
| e | 1.7 | OTHER | provider-clock-reference | Runs real peripheral clock providers (TWI, USB2, radio and others) against hand-set CCU and PRCM register values; the initial register contents and parent selection decode carry no citation. |
| e | 0.9 | NPU | npu-idle | Real NPU idle gating driver runs against a hand-written delayed, access-limited MMIO register model for the NPU clock and power gating; delay and readback behavior and the retention rule carry no cited fact or manual section. |
| e | 0.3 | OTHER | io-native-ordering | Compiled TWI, watchdog and ROM-return drivers run on a hand-built posted-write queue and TWI flag/STOP model; assumes store-queue drain latency and TWI completion timing without cited facts (secondary b). |
| e | 0.1 | OTHER | memtest-machine | Unicorn model of the returning platform (counter, SMC exit, DC ZVA, AT walks, plain-memory devices) used by the memtest checks; its behaviors cite no board measurement or manual section. |
| f | 19.8 | OTHER | index | Check-index bookkeeping: classifies every check, verifies the index is untracked and selects checks by changed path. |
| f | 15.4 | OTHER | vendor-firmware-sources | Vendor firmware manifest schema, README table, local and http links check (secondary c: manifest pins board-run inputs). |
| f | 2.9 | OTHER | rail-leases-cost | Counts AArch64 instructions, cache lines and atomic accesses of rail ledger and clock registry operations in Unicorn and enforces cost budgets; a performance-budget check. |
| f | 2.0 | OTHER | affected-checks | Check-selection bookkeeping: affected-check range selection over deleted dependencies and renames in a throwaway git repository; secondary class (a). |
| f | 1.2 | OTHER | cpu-pmu-smp | Real PMU module and FreeRTOS SMP port run on QEMU cores under icount, so the PMU behavior comes from the third-party QEMU emulator rather than our model or a cited board fact. |
| f | 1.1 | OTHER | remote-writers-sources | Scans pinned boot0 and captured ROM literals for bus-master reset addresses and confirms the manual and datasheet statements cited by docs/remote-writers.md; a binary and source survey plus doc citation integrity. |
| f | 1.0 | OTHER | docs-links | Documentation link and heading fragment check. |
| f | 0.4 | NPU | npu-platform-reference | Extracts and asserts NPU startup fields (clock gates, opp, power domain, manual register sections) from pinned vendor sources by text matching; a source-grep check. |
| f | 0.2 | NPU | npu-pll | Hash-pins vendor manual and kernel sources and asserts the manual's PLL enable step order and clock-driver text match our bounded PLL profile; a source-text comparison with no executed model. |
| f | 0.2 | OTHER | usb-host-hub-reference | Source-text check of USB 2.0 and xHCI spec sections and the pinned TinyUSB hub patch text and hashes. |
| f | 0.1 | OTHER | usb-host-irq-reference | Source-text check of pinned Linux xHCI IRQ ack order, device tree route and xHCI spec phrases. |

The classification of the checks in the NPU and scheduler streams is listed above and not edited here; the coordinator forwards their changes.

## Outcome of the class (e) review

The coordinator reviewed the class (e) list on 2026-11-01. Citations were searched for the eight checks outside the NPU stream. None could cite every modeled behavior, so `a733-pcie-trace`, `pcie-qemu`, `a733-xhci-detach`, `a733-xhci-dma` (returned to the gate later the same day), `io-native-ordering`, `memtest-machine`, `power-write-order` and `provider-clock-reference` are pending re-qualification and no longer run in the full gate. `npu-idle` stays in the gate until the NPU stream changes idle gating. [The rules and the pending table](../validation.md#checks-pending-re-qualification) hold the uncited behaviors.
