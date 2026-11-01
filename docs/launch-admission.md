# Board-image launch admission

This audit covers launch checks in the source checkout, using `5e0737ea` as
the comparison revision. The intended reader maintains native images or host
tools. FEL is the Allwinner USB boot-ROM transport. WDT0 is the hardware
watchdog that bounds a native run.

[The board runner](board-runner.md) owns physical image admission and transport.
Its implementation includes `tools/board_images.py`, `tools/board_probe.py` and
`tools/board_state.py`. Moving a check into those modules keeps it under the
runner. Native checks still enforce conditions that can change after host
admission, including session expiry, resource ownership and device retirement.

## Admission inventory

Each row groups predicates with the same purpose and disposition. The coverage
column describes the runner contract, including its refusal of unsupported
images. It does not imply physical qualification.

| Location outside the runner CLI | Predicate group | Runner coverage and disposition |
| --- | --- | --- |
| `tools/dev-run.py` | Watchdog marker, result symbol, manifest, input bounds and CPU-only image selection | Runner image inspection and session preparation cover these. The command delegates to `board.development`; its separate admission and transport are removed. |
| `tools/dev-run.py` | Captured identity, thermal clocks, boot PLL/mux words and speed bin | Hardware predicates move unchanged to `tools/board_state.py`. Live runs capture state under the runner's lock; supplied state selects offline preparation. |
| `tools/dev-run.py` | Fixed rail-helper digest, per-rail readbacks, write order and boot clocks | Runner calls the shared decoder and its existing pinned helper. These checks remain. Pre-launch and restoration evidence have separate directories. |
| `tools/dev-run.py` | GPU/NPU command-wide refusals | Removed. The runner inspects the supplied image and rejects unsupported watchdog or return contracts before device access. |
| `tools/dev-run.py` | Evidence path must be unused | Removed as launch authority. Runner evidence uses a unique child when the requested directory exists. Artifact builders still preserve existing outputs. |
| `tools/dev-run.py` | Force, rail-normalization and restoration options | Compatibility parsing remains. Force and skip-normalization cannot bypass runner safety. Restoration runs only after valid capture, decoder success and boot-clock confirmation. |
| `tools/a733-returning-host.py` | Separate bundle validation, target ownership, watchdog arm/return and repeat admission callbacks | Live execution delegates to the runner. Failure utilities used by native fixtures remain. |
| `apps/cpu-runtime-qualification/host.py` | Returning CPU bundle, DRAM/caller checks and disabled-watchdog precondition | Its live entry reaches the shared runner adapter. Legacy `run_once` remains for instruction/host fixtures and has no live CLI caller. Unsupported images fail runner admission. |
| `apps/gpu-finite/host.py`, `apps/gpu-entry-checkpoint/host.py` | GPU bundle, memory restoration, disabled watchdog and retirement reports | Live entries use the shared adapter. Legacy callbacks remain for device-specific validation; the runner admits only supported image/report formats. |
| `apps/npu-finite/host.py`, `apps/npu-interrupt/host.py` | NPU profile, bundle and watchdog requirements | Live entries reach runner admission. Supported finite profiles use their exact watchdog, session and manifest, with verified retirement and DRAM restoration. Older unarmed images remain refused. |
| `apps/os/cpu-return-host.py` | Result geometry, nonce, cleanup, drain and output integrity | Runner capture calls these checks. They validate returned data and remain required. |
| `tools/run-launch-survey.py` | Frozen survey sources/files, identity, workspace and launch sequence | Its launch command delegates to native-probe admission. The caller-survey helper remains available to existing preparation/fixture consumers. |
| `tools/run-a64-entry-probe.py`, `tools/run-native-startup-probe.py` | Frozen build, source/native identity, platform state and launch sequence | Launch commands delegate to the runner. Preparation and decoding remain. Old overlays need the native probe contract before physical execution. |
| `tools/check-a733-smp.py`, `tools/check-a733-smp-observe.py`, `tools/check-a733-device-launch.py` | Independent launch ownership and bridge-stability confirmation | Live CLI paths delegate to the runner. Confirmation switches remain optional compatibility arguments. Native preparation, layout and injected-transaction helpers remain. |
| `tools/check-a733-storage-launch.py` | Independent launch and bridge confirmation | Delegates with storage-write intent. The runner refuses storage execution; this adapter creates no grant or backup authority. |
| `tools/check-a733-kernel.py`, `tools/check-a733-el1.py` | Separate raw-image launch and recovery loops | Live CLI paths delegate to runner admission and terminate before legacy capture/postprocessing. Layout and result decoders remain. |
| `tools/check-a733-usb-returning.py` | Kernel transport plus USB observation | Kernel delegation terminates the old live path. An unsupported USB image cannot continue into legacy observation. |
| `tools/check-a733-smp-supervised.py:run_session` and its peripheral wrappers | Supervised ownership, capture callbacks and manual-reconnect confirmation | The shared live entry delegates to the runner. Images requiring whole-board power removal remain unsupported. Native peripheral input and result validation remains. |
| Supervised accelerator, GPU/display, NPU and storage-read CLI parsers | Required manual-reconnect switches | Removed as process authority. Runner return capability, watchdog and quarantine policy determine eligibility. Other peripheral parsers already accept the switch optionally. |
| `tools/check-a733-hdmi-audio-supervised.py` | Evidence parent must exist | Removed. The runner creates evidence parents and preserves prior records. Input snapshot and electrical checks remain. |
| `tools/prepare-board-bundles.py` | Clean row sources, historical survey build/software record, lifetime ordering and warm-return history | Removed as preparation/launch gates. Image hashes pin content; runner preparation establishes live prerequisites. Historical arguments remain accepted as metadata. |
| `tools/prepare-board-bundles.py` | Source-text completion declarations and inferred transport request counts | Removed as admission. Old proposal metadata records its queue cap; actual operation deadlines belong to the runner. |
| `tools/prepare-board-bundles.py` | Native monitor renewal limits, reused-object equivalence, controlled-outcome formats and uploaded bytes | Retained. A prepared bundle still establishes no physical authority. A base bundle needed for object comparison is a build input. |
| `tools/prepare-board-bundles.py` | Approval record for an electrical-point build | Retained with its image binding. The runner separately requires coordinator authority for a new operating point. Neither record replaces native voltage/speed-bin checks. |
| Generated native `runbook.md` files | Prior successes, external-run records, caller survey and manual warm-return chain | Replaced with the manifest renderer's short queue command and first-failure instructions. |
| `apps/usb-host/package.py` | Matching link result, class symbols and manifest device bits | Retained packaging correctness. Long per-image launch prose is replaced by the shared renderer. Accessory facts remain in `images.json` and USB documentation. |
| `tools/returning_build.py`, `apps/storage-read-returning/build.py` | Copied static image READMEs | Replaced by rendering the build's `input-identities.json`. Check/helper copies remain separate from launch admission. |
| CMake OS image composition | Application manifest, linked symbols and build identity | Link checks remain. Their result manifest supplies the shared README renderer. |
| Native CPU, probe and peripheral image entry/return code | Session binding, memory geometry, ownership, live clock/voltage/thermal limits, DMA/interrupt cleanup and DRAM restoration | Required native hardware checks remain. Host admission cannot replace them. The probe contract defines the host/native division. |
| `apps/storage-read-single-core/monitor.c`, `apps/npu-convolution/admission.c`, `apps/npu-convolution/boot-supervised.patch` and standalone NPU monitors | Refuse an armed watchdog because their launch/return protocol does not support it | Retained compatibility/safety conditions. Runner marker admission refuses these images before launch. Removing the native refusal would not supply retirement or a return receipt. |
| `platform/a733-smp/launch.c`, stage-2 loader and storage-maintenance admission | Self-armed watchdog and boot/storage session contracts | Retained hardware protocols. They are not duplicate host process checks and do not advertise generic RAM-only return capability. |
| `tools/run-ddr.py` | DDR manifest, SID/training record, restored clocks, PHY readbacks and bounded fallback | Retained prerequisite implementation called by `Live.dram`. An existing evidence directory selects a unique child; it no longer refuses preparation. |
| `tools/run-ddr-capture.py`, watchdog configuration/expiry tools and `tools/board-read.py` | Specialized training capture, watchdog qualification and conditional MMIO observations | Retained specialized hardware operations. Their register, recovery and SRAM-restoration checks are outside generic application-image admission. They grant no application launch eligibility. |
| `tools/storage-read-returning.py`, secondary/all-core host helpers | Input generation, evidence decoding and native geometry | Retained data checks. These helpers do not open a live image-launch transport. |
| Builders and evidence snapshots | Consistent source snapshot, dependency/header closure and unused artifact destination | Retained artifact integrity. HEAD/status readings record provenance; they do not authorize physical execution. No HEAD-equality or clean-tree assertion remains in the changed launch paths. |

Peripheral wrappers sharing `run_session` cover board, scheduler, storage-read,
SPI, UART, PWM, ADC, RTC, thermal, monitor, PCIe, crypto, Bluetooth, camera,
I2S, JPEG/encode, H.264/encode, HEVC, G2D, ISP, DSI, DisplayPort, HDMI audio,
GPU/display and accelerator captures. Their live capture callback is not invoked
when runner admission refuses the image.

## Generate image instructions from the build manifest

`tools/board_readme.py` emits the image path, SHA-256, queue command, expected
result and first-failure decode. It verifies the manifest's image digest before
writing instructions. It performs no launch admission or device access.

```sh
~/.cache/sunstoneos/venv/bin/python3 -B tools/board_readme.py \
  --manifest BUILD_MANIFEST --image IMAGE_ELF --output README_PATH
```

The renderer accepts link-check manifests, returning-image input records and an
optional `board_image` object in a build manifest. That object can name `path`,
`sha256`, `kind`, `expected_result`, `first_failure`, `port`, `hypothesis`,
`expect`, `template` and `inputs`. Image, template and input paths in that object
are relative to the manifest;
an explicit `--image` is relative to the current directory. PORT and
EXPECTED_OUTPUT remain explicit placeholders when the build lacks those inputs.
The coordinator supplies them before submission. A README does not establish
that an image meets the runner's return contract.

## Validation and limits

Host evidence lives under `~/.cache/sunstoneos/build/admission-1031/`.
The active runner fixtures cover watchdog expiry before entry, missing and
wrong-session return breadcrumbs, transfer failures, invalid native reports,
decoder failures, rail deadlines and post-return restoration. CPU/GPU offline
preparation and native watchdog instruction checks complement those fixtures.
The bundle fixture retains conditional MMIO, SRAM restoration, native monitor
limits, object equivalence and controlled-outcome validation.

For the admission consolidation ending at `78888bb6`, the measured
runner/development check pair replaces duplicate transport cases
with active-runner failure cases. Its total remains 56 named cases; elapsed time
falls from 18.55 to 4.29 seconds on this host. Its check index contains 762
scripts. Across changed code and build files, line count falls from 12,912 to
9,928, excluding documentation. Raw counts and timings are in `metrics.json`
and the `before-*`, `check-runner-4` and `check-dev-final` logs.

The OS-only, hello-compute and external mimus baseline builds pass. The selected
integration gate passes its kernel build/runtime check and check-index
verification. Native watchdog checks pass 19 cases; real CPU/GPU offline
preparation passes both cases. The full bundle fixture passes in 1.73 seconds.
After the DDR evidence-directory adjustment, both affected owner-injection
cases pass separately.

Correctness, hardware-safety and recovery reviewers inspect the runner change.
The manifest change has separate correctness review and a safety audit of the
removed process checks. Review fixes preserve decoder failure handling, queued
input bytes, source-relative decoder paths, the probe-check default, unresolved
return quarantine and separate evidence for both rail-helper invocations.

No physical run or new hardware qualification is recorded here. Existing images
without a compatible watchdog, finite cleanup and return receipt remain blocked
at runner admission. Historical cached READMEs are retained as evidence; new
builds use the shared renderer.

GPU receipt/thermal capture, complete binary expectations and finite NPU queue
adapters share runner admission. Finite NPU inputs and host dependencies are
frozen at submission; optional stopped-state capture precedes verified DRAM
restoration. Thermal decode failure or incomplete evidence yields a result
mismatch after normal return and preserves every raw capture. Resuming a port
does not collect orphan jobs; daemon recovery skips live runner processes.
The merge checks and three baseline builds are recorded under the evidence
directory's `resume-merge/` subdirectory.
