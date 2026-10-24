# Rank 9: native USB device channel

Status: **ready** once the coordinator accepts
[the admission patch](README.md#admission-after-the-forced-stall-return)
and reviews the two records that step 4 writes. Runs last on each board.
Bundle: `~/.cache/sunstoneos/evidence/board-bundles-20261023/rank-9-usb-v2/`.
It replaces `rank-9-usb/`, whose input builder refuses every board that has
returned to the boot ROM since DDR initialization; do not use `rank-9-usb/`.

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
| `usb-capture.py` | `7a488867ce1fb76c378ede6c0aee0c031adaf74778810b620424fdd6eb8f3b19`: read-only capture of the 17 words the input needs |
| `usb-package.py` | `425bd2774b1ad7d42b6afb1488b9a3a98b6c9657d7ad2767c999d45ea50a5501`: offline input builder and checks |
| `rate-admission/run.py` | `9a8b85c14f8cc66e9b320396b0bee8f4ee3b6caa65e453e21df0476c29e7204d`: the rate admission cases below |

`manifest.json` in the bundle records the SHA-256 of every file and lists
the files that differ from `rank-9-usb/`. The image, its ELF, the host
tools, the capture script and every compiled input are byte-identical to
`rank-9-usb/`; only `usb-package.py` and the records that the checks below
regenerate differ.

No frozen image of the current provider code existed: the 2026-10-18
images predate later provider changes, including the growth of
`struct a733_platform_locks` from two locks to four, and
`apps/usb-returning/build.py`
refused their accepted provider objects for the same changed headers as rank
6. The bundle is built with the new `--platform-build` option: platform and
providers come from the tree as for rank 6, and only the five
accepted TinyUSB objects (`dcd_sunxi_musb`, `tusb`, `tusb_fifo`, `usbd`,
`vendor_device`) are reused, after their header closures match the tree.
`platform-equivalence.json` shows the platform objects are those of the
kernel-check image that passed ranks 4 and 5.

## Fabric rates the image accepts

The USB input carries three configured fabric words, for the AHB, NSI and
MBUS clocks at CCU offsets `0x500`, `0x580` and `0x588`, and the rate in
hertz that each word programs. Two functions of the USB provider in
`drivers/usb/a733_usb_platform.c` decide whether the image accepts them:

- The constructor `a733_usb_platform_init` accepts a word when it has no bits
  outside the field mask, the NSI and MBUS gates (bit 31) are set, the
  source field selects a nonzero entry of the provider's parent table
  `parent_mhz`, and that parent rate divided by the divider field is a whole
  number of hertz equal to the stated rate. A parent other than the 24 MHz
  oscillator requires the PERI0 PLL.
- The stable predicate, which runs before the provider writes USB
  registers, requires each captured fabric word to equal the configured one
  under its mask. For each PERI0 parent in use it requires the PERI0
  multiplier between 53 and 105, the PERI0 equation for that parent to hold,
  and that output's gates set at CCU `+0x1908` and `+0x1988`.

The parent table admits these sources:

| Clock | Source 0 | Source 1 | Source 2 | Source 3 | Source 4 |
| --- | --- | --- | --- | --- | --- |
| AHB | 24 MHz | none | none | 600 MHz | none |
| NSI | 24 MHz | none | 800 MHz | 600 MHz | 480 MHz |
| MBUS | 24 MHz | 600 MHz | none | 480 MHz | 400 MHz |

The image therefore accepts MBUS at 400 MHz (`0x84000000`), the value that
every return to the boot ROM writes
([rank 7 diagnosis](rank-7-diagnosis.md#mbus-ran-at-400-mhz-in-both-lifetimes)),
as well as the 600 MHz that boot0 programs.
`apps/usb-returning/check.py` already runs its linked cases with MBUS at
400 MHz.

`usb-package.py` holds no list of admitted rates. It reads `parent_mhz`
from the frozen ELF, decodes the captured words with it, and runs the
image's constructor and stable predicate on the resulting input and the
captured words. It accepts a configuration if and only if both functions
accept it. A refusal names the
function that refused, the decoded rates and, for the stable predicate, the
provider's first failure record (kind, address, mask, expected and observed
value).

## Host checks

All checks run offline with the bundle's files:

- `apps/usb-returning/check.py`: 19 linked cases pass
  (`check-result.json`, identical to that of `rank-9-usb/`). The check
  derives the expected number of platform mutexes from
  `include/a733_platform_locks.h`; the header has four locks.
- `apps/usb-returning/host-check.py`: 23 host cases pass
  (`host-check-result.json`, identical to that of `rank-9-usb/`) with the
  patched kernel runner, covering capture, disappearance, admission cutoff,
  transport failure and result consistency.
- `usb-package.py` on the 2026-10-18 capture of board two, port 3-3
  instance `064` (`package-fixtures/board-two-064-capture/`), with that
  lifetime's DDR record and survey: it accepts AHB, NSI and MBUS at 200, 600
  and 600 MHz. It reproduces byte for byte the `config.json` of the USB
  attempt on that instance (`usb-first-fault-20261018/physical-064/`) and
  the `input.bin` that the `rank-9-usb/` builder wrote
  (`package-fixtures/usb-fixture-admitted-package/`, and
  `usb-fixture-signature-package/` for the signature path).
- `usb-package.py` on the 2026-10-23 port 3-3 capture of instance `013`
  (`board-session-20261023/port-3-3/r9-capture/`), with its DDR record, the
  `r9-survey` record and the three-link chain kernel check, forced stall,
  thermal: it accepts 200, 600 and 400 MHz, the constructor decodes parents
  of 600, 600 and 400 MHz, and the kernel runner admits the chain
  (`package-fixtures/port-3-3-013-package/`, and
  `port-3-3-013-signature-package/`). The `rank-9-usb/` builder refuses the
  same capture with `Unexpected configured fabric rates`. Without the
  thermal link, or with no link, the kernel runner refuses with
  `DDR/survey evidence does not admit the selected live board instance.`
- `rate-admission/run.py` on the port 3-3 capture: nine cases pass
  (`rate-admission/port-3-3-013/summary.json`). Each replaces captured words
  and runs `usb-package.py`:

  | Case | Replaced words | Builder |
  | --- | --- | --- |
  | Captured | none (MBUS 400 MHz) | accepts |
  | MBUS 600 MHz | `0x588` = `0x81000000` | accepts |
  | MBUS 300 MHz | `0x588` = `0x81000001` | accepts |
  | MBUS source 2 | `0x588` = `0x82000000` | constructor refuses: no parent |
  | MBUS 133.33 MHz | `0x588` = `0x84000002` | constructor refuses: inexact divider |
  | MBUS gate closed | `0x588` = `0x04000000` | constructor refuses |
  | MBUS 400 MHz, PERI0 400 MHz output gate clear | `0x1908` = `0x8fff0ffd` | stable predicate refuses at `0x02003908` |
  | MBUS 600 MHz, same PERI0 word | `0x588` = `0x81000000`, `0x1908` = `0x8fff0ffd` | accepts |
  | PERI0 multiplier outside 53 to 105 | `0x0a0` = `0xff003310` | stable predicate refuses, kind 10 |

## Commands

Set the variables in [the README](README.md#names-used-in-every-runbook)
and `R=$V/rank-9-usb-v2`. The board is on the instance that the last
returning image created. That image is rank 8's workload on a board where
ranks 6 to 8 ran, and rank 6's thermal application on a board where ranks 7
and 8 have not run.

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
     -- $PY -B $R/usb-capture.py $S/r9-capture
   ```

   Success: `{"port": "PORT", "complete": true, "words": 17}`.

   Port 3-3 has already run steps 1 and 2 on instance `013`, identity
   `[7,1506,1096972,0,0]` (`r9-cache-off`, `r9-survey`, `r9-capture`). While
   the board stays on that instance, go to step 3 with those records. If the
   instance has changed, stop: the warm-return chain below no longer reaches
   it.

3. Build and check the input offline. The output directory is
   `r9-v2-package`, because port 3-3 already holds the empty `r9-package`
   that the `rank-9-usb/` builder left when it refused. On a board where
   ranks 7 and 8 ran, the chain has five links:

   ```sh
   $PY -B $R/usb-package.py --bundle $R \
     --capture $S/r9-capture --ddr $S/ddr/session.json --survey $S/r9-survey/session.json \
     --warm-return $S/kernel/session.json --warm-return $S/timeout/session.json \
     --warm-return $S/r6-thermal/session.json \
     --warm-return $S/r7-prepare/runtime/session.json \
     --warm-return $S/r8-prepare/runtime/session.json \
     --output $S/r9-v2-package
   ```

   On a board where ranks 7 and 8 have not run, which is port 3-3 on
   2026-10-23, the chain is DDR (`003/004`) → kernel check (`003/009`) →
   forced stall (`003/011`) → thermal (`003/013`):

   ```sh
   $PY -B $R/usb-package.py --bundle $R \
     --capture $S/r9-capture --ddr $S/ddr/session.json --survey $S/r9-survey/session.json \
     --warm-return $S/kernel/session.json --warm-return $S/timeout/session.json \
     --warm-return $S/r6-thermal/session.json \
     --output $S/r9-v2-package
   ```

   The builder refuses unless the capture and survey cover the same
   instance, the image's USB constructor and stable predicate accept the
   captured fabric configuration, the ROM USB selector is 0 and DMA0 is
   held in reset with its gates off. It writes `input.bin`, records the
   decoded rates in `config-provenance.json` and `input-validation.json`,
   validates the DDR record, survey and warm-return chain with the bundle's
   kernel runner, and writes `source-review.json`,
   `entry-prerequisites.json` and `command.json`, the complete queue
   command of step 5. Its run directory is `$S/r9-v2-usb`.

4. Review `$S/r9-v2-package/source-review.json` and
   `$S/r9-v2-package/entry-prerequisites.json`, in particular the PIO-only
   USB history of this powered lifetime, secondaries OFF, no other writer,
   the requested pin and rail profile, and the availability of power-removal
   recovery. `$S/r9-v2-package/config-provenance.json` states the configured
   rates; on port 3-3 instance `013` they are 200, 600 and 400 MHz.

5. Run the channel once with the queue command that step 3 wrote:

   ```sh
   $PY -B $V/common/exec-command.py $S/r9-v2-package/command.json
   ```

   `command.json` runs `common/bundle-run.py` with a 180 s queue timeout,
   the prerequisites `supervised-ram-recovery` (written for the current
   instance), `usb-return-source-review` and `usb-entry-ownership`, and the
   runner `$R/host/tools/check-a733-usb-returning.py --fel ... --image
   $R/probe.bin --ram-input input.bin --evidence $S/r9-v2-usb --expect
   "COMPLETE native USB bounded observation" --return-timeout 5
   --ddr-evidence $S/ddr/session.json --platform-survey
   $S/r9-survey/session.json` with the `--warm-return` links of step 3.
   `exec-command.py` prints the command before it runs.

   Success text: `COMPLETE native USB bounded observation`, `PASS: kernel
   check, FEL recovery, automatic USB access.`, `PASS native USB complete
   OUT/IN observation, quiescence and responsive ROM return.` and
   `"result": "passed"`. Functional success also requires
   `native_usb.functional_success` true in `$S/r9-v2-usb/session.json`; the
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

Every warm-return chain above contains a forced-stall link, and the
five-link chain also contains two native-runner links; the unpatched kernel
runner refuses both kinds. Two alternatives work with the unpatched runner
in `bringup`:

- Initialize DDR again before step 1, then give step 3 only that DDR
  session and the new survey, with no `--warm-return`.
- Give step 3 `--signature-path` and no `--warm-return`. The command then
  runs `tools/check-a733-usb-returning.py` from `bringup` without DDR
  admission. The runner admits on the probe signature at `0x42000000`,
  which the rank 6 result wrote and ranks 7 and 8 restore.
