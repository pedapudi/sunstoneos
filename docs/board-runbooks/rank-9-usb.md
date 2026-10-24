# Rank 9: native USB device channel

Status: **ready** once the coordinator accepts
[the admission patch](README.md#admission-after-the-forced-stall-return)
and reviews the two records that step 4 writes. Runs last on each board.
Bundle: `~/.cache/sunstoneos/evidence/board-bundles-20261023/rank-9-usb/`.

The image runs the production PMIC and USB providers as two FreeRTOS tasks.
It takes over USB0 from the boot ROM, enumerates a vendor interface as
`sunstoneos returning USB`, answers one 4-byte OUT request with a 40-byte IN
response, parks the PMIC, detaches, and returns to the ROM through the
monitor. [The application README](../../apps/usb-returning/README.md) gives
its contract. A failure of the native stack or of the handback leaves the
board unreachable until its power is removed.

## Bundle

| File | SHA-256 |
| --- | --- |
| `probe.bin` (110,912 bytes) | `27cef572d8e3865629fb26ea56f811b5a843382b20b04b3406cc2112baac6b8f` |
| `build/probe.elf` | `18cf5ea4e2a8f5a71c568f473da55d2a261b8027e95c516814f8a82884a243da` |
| `host/tools/check-a733-usb-returning.py`, `check-a733-kernel.py`, `usb-access.py`, `check-a733-smp.py` | the runner closure, with `common/primary.patch` applied to the kernel runner; `host-inputs.json` lists their hashes |
| `usb-capture.py` | read-only capture of the 17 words the input needs |
| `usb-package.py` | offline input builder and checks |

No frozen image of the current provider code existed: the 2026-10-18
images predate later provider changes, including the growth of
`struct a733_platform_locks` from two locks to four, and
`apps/usb-returning/build.py`
refused their accepted provider objects for the same changed headers as rank
6. The bundle is built with the new `--platform-build` option: platform and
providers come from the current tree as for rank 6, and only the five
accepted TinyUSB objects (`dcd_sunxi_musb`, `tusb`, `tusb_fifo`, `usbd`,
`vendor_device`) are reused, after their header closures match the current
tree. `platform-equivalence.json` shows the platform objects are those of
today's passing kernel-check image.

Host checks:

- `apps/usb-returning/check.py`: 19 linked cases pass
  (`check-result.json`). The check now derives the expected number of
  platform mutexes from `include/a733_platform_locks.h`; it asserted two,
  and the current header has four locks.
- `apps/usb-returning/host-check.py`: 23 host cases pass
  (`host-check-result.json`) with the patched kernel runner, covering
  capture, disappearance, admission cutoff, transport failure and result
  consistency.
- `usb-package.py` on the 2026-10-18 board-two captures
  (`package-fixtures/`) reproduces that attempt's
  `config.json` byte for byte, and the actual constructors and all 14 rate
  predicates accept the resulting input with four platform mutexes.

## Commands

Set the variables in [the README](README.md#names-used-in-every-runbook).
The board is on the instance that rank 8 returned.

1. Turn the instruction cache off and survey:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/r9-cache-off 120 \
     "Board $PORT: returning instruction-cache disable before the native USB channel" \
     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
     -- $PY -B $V/common/cache-off.py $S/r9-cache-off
   $PY -B $V/common/survey.py $PORT $S/r9-survey "Board $PORT before the native USB channel"
   ```

2. Capture the 14 clock and reference words and the three entry words
   (ROM USB selector, DMA0 reset and gate). These are 4-byte FEL reads; no
   code runs:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/r9-capture 120 \
     "Board $PORT: read-only clock, reference and USB-entry words for the USB input" \
     "Seventeen 4-byte reads complete on the current ROM instance." \
     -- $PY -B $V/rank-9-usb/usb-capture.py $S/r9-capture
   ```

   Success: `{"port": "PORT", "complete": true, "words": 17}`.

3. Build and check the input offline:

   ```sh
   $PY -B $V/rank-9-usb/usb-package.py --bundle $V/rank-9-usb \
     --capture $S/r9-capture --ddr $S/ddr/session.json --survey $S/r9-survey/session.json \
     --warm-return $S/kernel/session.json --warm-return $S/timeout/session.json \
     --warm-return $S/r6-thermal/session.json \
     --warm-return $S/r7-prepare/runtime/session.json \
     --warm-return $S/r8-prepare/runtime/session.json \
     --output $S/r9-package
   ```

   It refuses unless the capture and survey cover the same instance, the
   configured AHB, NSI and MBUS rates are 200, 600 and 600 MHz, the ROM USB
   selector is 0 and DMA0 is held in reset with its gates off. It writes
   `input.bin`, runs the image's actual constructors and rate predicates on
   it, validates the DDR record, survey and five-link chain with the
   bundle's kernel runner, and writes `source-review.json`,
   `entry-prerequisites.json` and `command.json`, the complete queue
   command of step 5.

4. Review `$S/r9-package/source-review.json` and
   `$S/r9-package/entry-prerequisites.json`, in particular the PIO-only USB
   history of this powered lifetime, secondaries OFF, no other writer, the
   requested pin and rail profile, and the availability of power-removal
   recovery.

5. Run the channel once with the queue command that step 3 wrote:

   ```sh
   $PY -B $V/common/exec-command.py $S/r9-package/command.json
   ```

   `command.json` runs `common/bundle-run.py` with a 180 s queue timeout,
   the prerequisites `supervised-ram-recovery` (written for the current
   instance), `usb-return-source-review` and `usb-entry-ownership`, and the
   runner `host/tools/check-a733-usb-returning.py --fel ... --image
   probe.bin --ram-input input.bin --evidence $S/r9-usb --expect "COMPLETE
   native USB bounded observation" --return-timeout 5 --ddr-evidence
   $S/ddr/session.json --platform-survey $S/r9-survey/session.json` with the
   five `--warm-return` links. `exec-command.py` prints the command before
   it runs.

   Success text: `COMPLETE native USB bounded observation`, `PASS: kernel
   check, FEL recovery, automatic USB access.`, `PASS native USB complete
   OUT/IN observation, quiescence and responsive ROM return.` and
   `"result": "passed"`. Functional success also requires
   `native_usb.functional_success` true in `$S/r9-usb/session.json`; the
   runner exits nonzero without it.

## Stop conditions

- Any nonzero exit in steps 1, 2 or 3: stop. Step 3 is offline, so a
  refusal there leaves the board idle in the ROM; resolve the refused
  condition before any USB attempt.
- Step 5 nonzero: stop and keep everything. A host timeout of 180 s, a
  missing ROM return or a native failure leaves the board unreachable;
  request a power removal for this port at the top of the handoff.
  `USB observation incomplete` after a ROM return is a controlled negative:
  the board is reachable, but the test does not pass and is not repeated.
- The runner admits a native request only during the first 1.05 s after
  launch and ends observation at 1.65 s; a late enumeration sends no request.

## Without the patch

The five-link chain contains a forced-stall link and two native-runner
links, which the unpatched kernel runner refuses. Two alternatives work
with the unpatched runner in `bringup`:

- Initialize DDR again before step 1, then give step 3 only that DDR
  session and the new survey, with no `--warm-return`.
- Give step 3 `--signature-path` and no `--warm-return`. The command then
  runs `tools/check-a733-usb-returning.py` from `bringup` without DDR
  admission. The runner admits on the probe signature at `0x42000000`,
  which the rank 6 result wrote and ranks 7 and 8 restore.
