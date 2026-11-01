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
mimus port. OS-only and hello-compute builds explicitly clear `MIMUS_DIR`.
Recognition, models, manifests, capture wrappers and recognition checks belong
to mimus; SunstoneOS owns the [application ABI](../designs/app-abi.md) and
shared device services. The [layering audit](../layering-audit.md) lists defects.

The three baseline builds and recorded QEMU parity have evidence under
`~/.cache/sunstoneos/build/tool-gates-land-1031/` and
`~/.cache/sunstoneos/build/integrate-streams-1031/`. Current OS-only,
hello-compute and external-mimus baseline builds pass under
`~/.cache/sunstoneos/build/check-selection-1031/final/`. The selected kernel,
eight-core scheduling model, shared-helper, dashboard, PMIC and power-domain
integration gate passes there.

Full integration remains blocked by the missing I2S stream-profile `route`
initializer at `checks/i2s-runtime-provider.c:107`. Combined host evidence covers
83 runnable checks: 78 pass and five remain failing. The failures concern the
SPI platform expectation, obsolete supervised-claim interface, GPU texture macro,
missing NumPy and provider-clock fixture. The full attempt and failed-case logs
are in `gate-final-full/` and `full-host/` under that build directory.
The corrected toolchain-lookup case retains its resumed result in
`safety-resumed.log`; the dashboard result is in `gate-final-core/`.
H.264 capture and SAE checks require external inputs and remain pending.

Supervised/resident builds
compose thermal, radio, I2S or headset services. Optional CPU/GPU/NPU builds
have host evidence under `concurrent-gpu-npu-1026/`,
`resident-gpu-cold-1026/` and `resume-gpu-eight-workers-20261026/` relative to
`~/.cache/sunstoneos/evidence/`; simultaneous physical operation is unqualified.

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
