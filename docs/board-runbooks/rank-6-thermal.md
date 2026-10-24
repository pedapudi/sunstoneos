# Rank 6: returning thermal application

Status: **ready** once the coordinator accepts
[the admission patch](README.md#admission-after-the-forced-stall-return).
Bundle: `~/.cache/sunstoneos/evidence/board-bundles-20261023/rank-6-thermal/`.

The image runs the production thermal driver in one FreeRTOS task. It starts
all five sensors, gathers two complete calibrated sets, disables acquisition
and returns to the boot ROM through the monitor. It configures no interrupt
distributor, takes over no USB controller, starts no secondary core and
changes no supply voltage. [The application README](../../apps/thermal-returning/README.md)
gives its contract.

## Bundle

| File | SHA-256 |
| --- | --- |
| `probe.bin` (46,688 bytes, loaded at `0x41000000`) | `e79dabc9af9cc83d4201767b800508299542b7619bdd02ad7488d716e210cf91` |
| `build/probe.elf` | `a840e7cd9e285d1aff0ed95b132cc35fd0be37af607cf1bb69465553bcca3e9c` |
| `host/tools/check-a733-kernel.py` | the kernel runner with `common/primary.patch` applied |

`apps/thermal-returning/build.py` refused to reuse the accepted objects of
the PMIC provider probe evidence because their headers have since changed:
`a733_clock_leases.h`, `a733_counter_clock.h`, `a733_gpu_power.h`,
`a733_pins.h`, `a733_platform_locks.h`, `a733_pmic.h`, `a733_rail_leases.h`
and `platform/a733/boot.h`. The bundle is built with `--platform-build`,
which compiles the same platform from the current tree. The monitor,
platform, start, vector, cache, memory, I2C, FP-context and FreeRTOS port
objects come from the central `a733-power-probe` target. The shared-lock,
counter-clock, clock-lease, task and queue units compile with the provider
probe's FreeRTOS configuration. The central build now also links
`platform/a733/cpu_coherence.S`, the EL1 table flush that `platform.c` calls,
so `tools/returning_build.py` checks the target's object list by source name
instead of by a count of twelve.

`platform-equivalence.json` records the evidence that this is the accepted
platform. The same central build reproduces, byte for byte, the kernel-check
ELF that passed ranks 4 and 5 on both boards on 2026-10-23 (`574245e2…`).
Each monitor, platform, start, vector, cache, memory, FP-context and port
object in this image is byte-identical to the kernel-check image's object.

Host checks: `apps/thermal-returning/check.py` passes all 13 linked cases
(`check-result.json`).

## Commands

Set the variables in [the README](README.md#names-used-in-every-runbook).
The board is on the forced-stall return's instance.

1. Turn the instruction cache off and survey the current instance:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/r6-cache-off 120 \
     "Board $PORT: returning instruction-cache disable after the forced-stall return" \
     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
     -- $PY -B $V/common/cache-off.py $S/r6-cache-off
   $PY -B $V/common/survey.py $PORT $S/r6-survey "Board $PORT after the forced-stall return and cache-off"
   ```

   Each prints `"result": "passed"`. Timeouts: 120 s and 240 s.

2. Run the thermal image once:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/r6-thermal 300 \
     "Board $PORT: returning thermal application" \
     "Two complete five-sensor sets, configured disable, and monitor return to a responsive ROM." \
     -- $PY -B $V/rank-6-thermal/host/tools/check-a733-kernel.py --fel $FEL \
        --image $V/rank-6-thermal/probe.bin --mode kernel \
        --expect "COMPLETE native thermal configured disable" \
        --evidence $S/r6-thermal \
        --ddr-evidence $S/ddr/session.json --platform-survey $S/r6-survey/session.json \
        --warm-return $S/kernel/session.json --warm-return $S/timeout/session.json
   ```

   Admission records: DDR `$S/ddr/session.json` (12,288 MiB), the new survey,
   and the chain kernel check → forced stall. The runner's return timeout is
   its default 10 s; the image's client deadline is 1.5 s within the
   monitor's 2 s deadline. The queue timeout is 300 s.

   Success text: `COMPLETE native thermal configured disable`, then
   `PASS: kernel check, FEL recovery, automatic USB access.` and
   `"result": "passed"`.

3. Classify the result offline:

   ```sh
   $PY -B $V/rank-6-thermal/thermal-result.py $S/r6-thermal
   ```

   It prints the ten readings in °C and exits 0 only when the log has the
   `PASS native calibrated thermal observations` marker,
   `THERMAL_OUTCOME=0`, two complete sets of channels 0 to 4, and every
   reading below the 90 °C throttle trip.

## Stop conditions

- Any nonzero exit in step 1 or 2: stop this board. `bundle-run.py` keeps
  the queue claim, and the runner keeps the port lock and quarantines the
  instance after a launch or transport failure. Do not retry. A hang inside
  the image ends at the monitor's 2 s deadline; a stalled register read
  does not, and needs a power removal.
- Step 2 passes but step 3 exits 1: the image returned normally. Record the
  outcome. Rank 7 may follow, because it applies no sustained load. Rank 8
  waits until a thermal run shows every reading below 90 °C.

The image retains its ADC clock token, bus clock and reset release, sensor
configuration and calibration after it returns; only acquisition is
disabled. Later images on this board must not assume the thermal block is
in its reset state.

## Without the patch

If the coordinator rejects `common/primary.patch`, either initialize DDR
again with `tools/run-ddr.py` before step 1 and pass only the new DDR
session and survey (no `--warm-return`), or omit `--ddr-evidence`,
`--platform-survey` and both `--warm-return` options and use the runner's
signature path. [The README](README.md#cheapest-valid-order) compares the
two. Either works with the unpatched `tools/check-a733-kernel.py` in
`bringup`.
