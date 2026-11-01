# Applications and development execution

**Status: implemented.**

SunstoneOS supplies the FreeRTOS application kit,
OS-only image, hello-compute control and optional service profiles. Applications
own their algorithms and assets in external repositories.

**Board-qualified scope:** One eight-worker CPU-only mimus recording matches its
90-frame reference, retires seven secondary cores and restores RAM, boot clocks
and CPU rails. A corrected hello-compute control returns normally with seven
secondaries off. These inputs do not establish general recognition or startup
repeatability. [CPU status](cpu.md) identifies the result and failed controls.

The [application source record](../../apps/mimus/source.json) pins the external
mimus integration checkout on `port/sunstoneos-gpu-integrate`. OS-only and
hello-compute builds explicitly clear `MIMUS_DIR`.
Recognition, models, manifests, capture wrappers and recognition checks belong
to mimus; SunstoneOS owns the [application ABI](../designs/app-abi.md) and
shared device services. The [layering audit](../layering-audit.md) lists defects.

OS-only, hello-compute and external-mimus baseline builds pass. Their current
logs are under `~/.cache/sunstoneos/build/check-selection-1031/followup-3/`.
The OS builds clear external application selectors; the mimus build uses the
checkout in the application source record.

The SPI fixture exercises DMA interrupt completion, including latched DONE,
request shutdown and reap cleanup. The provider-clock fixture supplies valid
TWI controller, divider and settling inputs. Both focused sanitizer checks pass
under `followup-e124/host-fixes/` in the check-selection build directory.
The I2S runtime initializer includes its route; `baseline-os-build.log` records
a successful `i2s-file-model` build.

The full integration selection has passing evidence for the central build,
all twelve OS variants, seven core checks and 84 indexed checks. Evidence
combines unchanged successes at `c2c18e06` with the repaired runner-delegation
fixture at `dbac57af` and nine remaining checks. The runner owns capture bounds;
the delegation fixture verifies its arguments and exit status before USB access.
No driver or application failure appears in this selection.

The evidence root is
`~/.cache/sunstoneos/build/check-selection-1031/followup-3/`:

- `gate-links-full/` records the builds, core checks and 74 indexed successes.
- `capture/focused.log` records the repaired delegation fixture.
- `gate-resumed/` records the nine previously unexecuted checks.
- `check-index-after-capture.log` verifies 790 entries, including 89 pure-host
  checks. Hook fixtures are skipped because their inputs are unchanged.

H.264 capture, SAE and other checks requiring external inputs remain pending.
These results establish no new physical qualification.

Supervised/resident builds
compose thermal, radio, I2S or headset services. Optional CPU/GPU/NPU builds
have host evidence under `concurrent-gpu-npu-1026/`,
`resident-gpu-cold-1026/` and `resume-gpu-eight-workers-20261026/` relative to
`~/.cache/sunstoneos/evidence/`; simultaneous physical operation is unqualified.

`SUNSTONE_A733_OS_GPU=ON` selects the generic GPU compute queue. Applications
own their programs and tensor placement; `apps/queue-add` supplies an independent
vector-add consumer. An optional trusted second GPU application has its own
manifest, task, client slot and completion queue. Both callers consent to one
program and memory map; the primary controls device lifecycle. Host checks cover
reopen, reset recovery preserving a queued peer and final quiescence. Physical
queue execution, recovery and application GPU recognition remain unqualified.

Recorded and paced PCM use the generic capture interface without a device grant.
The current audio fixture builds against the application kit with an empty
`MIMUS_DIR`; sanitizer, AArch64 and linked-instruction evidence is indexed by
`~/.cache/sunstoneos/build/audio-1031/frozen/README.md`. These checks model grant
publication, clock, waits and finish; they do not execute native scheduling.

The [board runner](../board-runner.md) owns fresh-FEL preparation, frozen queue
submissions, returning CPU/GPU/probe capture and bounded recovery. Its host
checks and real offline preparation are under
`~/.cache/sunstoneos/build/board-runner-1031/`; broader physical runner qualification remains pending. The finite NPU adapter
prepares native sessions and validates returning interrupt receipts; the shared
daemon must include it before NPU submissions. [Native probes](../board-probe-contract.md) share watchdog,
breadcrumb and result contracts; static secondary-OFF and peripheral retirement
still need physical evidence. Finite NPU images admit the armed watchdog and have qualified startup,
inference and two-model results; [NPU status](npu.md) identifies the exact images
and pending lifecycle operations.

Generic GPU capture accepts image-bound GPU and thermal receipts with strict
binary result decoding. The frozen proposals under
`~/.cache/sunstoneos/build/gpu-integrate-1031/images-lifecycle/` contain the known
CPU clock close defect. They need a rebuild and refreeze with the merged
correction before physical qualification.

First-failure records retain site, caller and inputs through watchdog reset;
`apps/board/main.c:board_fail_at` and `apps/os/application.c:ss_fail` record the
origin. The [watchdog guide](../watchdog.md) defines decoding and recovery.
Saved USB identities do not establish current access. The coordinator checks
live identity, lock, permission and failed-instance state before each queue run.
A host cleanup error after normal native return in
`dev-runs-1031/3-3-cpu-return-201331/run/session.json` does not establish restored
host RAM or authorize a retry.

Remaining work includes broader application inputs, returning accelerator
execution, native audio input, autonomous card boot, fault recovery and concurrent
thermal qualification. No microphone, MIPI camera or MIPI panel is attached.
A known mimus host check, `os-cpu-return-followup-integration.py`, refuses an
accepted survey whose `tools/run-launch-survey.py` hash differs. Resolve that
source/evidence mismatch without counting it as a physical execution failure.

After hardware functionality, measure warm PCM-to-formatted-transcription time
against the 0.05-times-audio-duration target; 0.01 times is a bounded stretch.
Loading and live end-of-speech latency are separate measurements. CPU fallback
does not satisfy GPU/NPU coverage. Use [the image guide](../os-image.md) for
build commands and [application evidence](../mimus-evidence.md) for exact inputs.
