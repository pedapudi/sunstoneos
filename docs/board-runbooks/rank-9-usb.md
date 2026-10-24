# Rank 9: native USB device channel

Status: **ready** once the coordinator accepts
[the admission patch](README.md#admission-after-the-forced-stall-return)
and reviews the two records that step 4 writes. Runs last on each board.
Bundle for the next run: `~/.cache/sunstoneos/evidence/board-bundles-20261023/rank-9-usb-v4/`.
`rank-9-usb-v3/` passed on port 3-3 on 2026-10-24. `rank-9-usb-v4/` runs on
the current platform, which adds the 12 GiB EL1 map, so its run is a new
board test. Do not use `rank-9-usb/` or `rank-9-usb-v2/`, whose image
refused USB takeover on 2026-10-23.

The image runs the production PMIC and USB providers as two FreeRTOS tasks.
It takes over USB0 from the boot ROM, enumerates a vendor interface as
`sunstoneos returning USB`, answers one 4-byte OUT request with a 40-byte IN
response, parks the PMIC, detaches, and returns to the ROM through the
monitor. [The application README](../../apps/usb-returning/README.md) gives
its contract. A failure of the native stack or of the handback leaves the
board unreachable until its power is removed.

## The 2026-10-23 refusal and the 2026-10-24 pass

`rank-9-usb-v2/` ran once on port 3-3 instance `013` and returned to the
ROM with monitor status 1 (`board-session-20261023/port-3-3/r9-v2-usb/`).
The USB provider passed the ROM admission check, which already permitted
the inherited DEVCTL SESSION bit, and masked the ROM's interrupt enables.
It then refused at the soft-disconnect (stage `0x16`): its 32-bit store of
`0x00029900` to `0x04100040` completed, and the readback still had SoftConn
(PCTL bit 6) set. No USB exchange began and the PMIC was healthy.

`rank-9-usb-v3/` removes SoftConn with one 8-bit store of PCTL, the access
width of the pinned BSP, TinyUSB and the matched ROM. It passed on port 3-3
on 2026-10-24 (`board-session-20261023/port-3-3/r9-v3-usb/`): SoftConn
cleared, the OUT/IN exchange completed with received 1 and sent 40, the
USB task quiesced and the ROM return was responsive, with
`native_usb.functional_success` true.
[USB0 takeover from the boot ROM's device session](../designs/usb-session-takeover.md)
gives the decoding, the rule, the sources and what remains unproven.

## Changes in `rank-9-usb-v4/`

`rank-9-usb-v4/` is built with `apps/usb-returning/build.py
--platform-build` from the current tree (commit `288de66d`). Against
`rank-9-usb-v3/` it differs in two platform objects and the four USB units:

- `platform/a733/platform.c` adds the 12 GiB EL1 map and one-time local
  pool preparation, and `platform/a733/cpu_coherence.S` changed with the
  cached-prepare check. No returning image has run this platform on a
  board.
- The PCTL byte store now lives only in the native adapter
  `a733_usb_platform_native_write`; it is the same `strb` to the same
  register that `rank-9-usb-v3/` ran.
- The provider starts the 150 ms detach interval at the first read with
  SoftConn clear. While the bit still reads set it re-reads without
  writing, bounded by the provider deadline; a bit that never clears ends
  with a time failure at stage `0x1d` and no reset write.
- The admission rule no longer refuses LSDEV and FSDEV; HOSTREQ, HOSTMODE,
  VEND0 DMA request selection and the sixteen DMA start bits still refuse.

| File | SHA-256 |
| --- | --- |
| `probe.bin` (112,328 bytes) | `5ecacbe49cb5ee97a503f71dafa3e2f0038cf41341773719f5275792bb61bf9d` |
| `build/probe.elf` | `e845183b6bc1b7dea1f76a46f20fa4253c0a8fcde11c42cd88b47920d0615a4d` |

Its host tools, capture script, input builder and rate admission runner are
byte-identical to `rank-9-usb-v3/`. Offline checks of `rank-9-usb-v4/`:
27 linked cases (`check-result.json`; the modeled takeover takes 166 polls
at one per 1 ms tick), 24 host cases, 13 native callback and first-fault
cases (`first-fault-check.json`, with the first-fault check of
`primary.patch`), five input-builder fixtures and nine rate admission
cases. The fixture `package-fixtures/port-3-3-r9-v3-package/` rebuilds the
input of the 2026-10-24 run from its capture, survey and the `ddr-2` DDR
record; its `config.json` is byte-identical to that run's.

The rest of this runbook describes `rank-9-usb-v3/`. For `rank-9-usb-v4/`
set `R=$V/rank-9-usb-v4` and use directory names starting with `r9-v4-`,
because port 3-3 already holds the `r9-v3-` records.

## Bundle `rank-9-usb-v3/`

| File | SHA-256 |
| --- | --- |
| `probe.bin` (111,424 bytes) | `fe0db87004a1bd7d9e1a518abb20aa29e95c96658b01bf4d7e31ce0a7234cf53` |
| `build/probe.elf` | `be4fbaa2bf374d45396b3f3d460969d87cde83750082c84d535027f8e2523528` |
| `host/tools/check-a733-usb-returning.py`, `check-a733-kernel.py`, `usb-access.py`, `check-a733-smp.py` | the runner closure, with the kernel-runner part of `common/primary.patch` applied; byte-identical to `rank-9-usb-v2/` |
| `usb-capture.py` | `7a488867ce1fb76c378ede6c0aee0c031adaf74778810b620424fdd6eb8f3b19`: read-only capture of the 17 words the input needs |
| `usb-package.py` | `425bd2774b1ad7d42b6afb1488b9a3a98b6c9657d7ad2767c999d45ea50a5501`: offline input builder and checks |
| `rate-admission/run.py` | `9a8b85c14f8cc66e9b320396b0bee8f4ee3b6caa65e453e21df0476c29e7204d`: the rate admission cases below |

`manifest.json` in the bundle records the SHA-256 of every file and lists
the files that differ from `rank-9-usb-v2/`. The capture script, the input
builder, the rate admission runner and the host tools are byte-identical to
`rank-9-usb-v2/`.

The image is built with the same `apps/usb-returning/build.py
--platform-build` builder as `rank-9-usb-v2/`, from the same platform
revision, `dfa5174`, with the corrected USB files overlaid
(`input-identities.json` records the source export as commit `c7dcafb`).
Every reused object is byte-identical to `rank-9-usb-v2/`: monitor,
platform, kernel, PMIC, clock, rail, pin, the native USB adapter and the
five accepted TinyUSB objects. Only the objects of
`apps/usb-returning/main.c`, `platform/a733/native_usb.c` and
`drivers/usb/a733_usb_platform.c` differ. `platform-equivalence.json` shows
that the central build of that revision reproduces the kernel-check image
that passed ranks 4 and 5 on both boards
(`574245e291d8ec9a2a52a405ab45ce48fe5550f323a46158b007123dbf96bc54`). The
current tree's `platform/a733/platform.c` adds the 12 GiB EL1 map and local
pool preparation, which no returning image has run on a board; this bundle
keeps those out so that the run tests only the USB correction.

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

- `apps/usb-returning/check.py`: 28 linked cases pass
  (`check-result.json`). Nineteen are the cases of `rank-9-usb-v2/`. Nine
  run the provider's actual ROM takeover and native callbacks from the
  2026-10-23 port 3-3 input, clock capture and observed word `0x00029940`,
  with a register model in which a 32-bit store to `0x04100040` leaves
  PCTL unchanged and an 8-bit store replaces it. On the `rank-9-usb-v2/`
  image that model reproduces the board's fault record field for field.
  On this image the takeover reaches PREPARED with one PCTL byte store. A
  SoftConn bit that never clears refuses at stage `0x18` before the reset
  write. HOSTREQ, HOSTMODE, FSDEV, VEND0 DMA request selection, a missing
  B-device role and an active DMA channel each refuse before any write.
- `apps/usb-returning/host-check.py`: 24 host cases pass
  (`host-check-result.json`) with the patched kernel runner. The added case
  requires that the 2026-10-23 session record never classifies as
  functional success.
- `usb-package.py` on the 2026-10-18 capture of board two, port 3-3
  instance `064` (`package-fixtures/board-two-064-capture/`), with that
  lifetime's DDR record and survey: it accepts AHB, NSI and MBUS at 200, 600
  and 600 MHz (`package-fixtures/usb-fixture-admitted-package/`, and
  `usb-fixture-signature-package/` for the signature path). Its
  `config.json` is byte-identical to that of the USB attempt on that
  instance (`usb-first-fault-20261018/physical-064/`). The input
  generation differs from `rank-9-usb-v2/` because it hashes `probe.bin`.
- `usb-package.py` on the 2026-10-23 port 3-3 capture of instance `013`
  (`board-session-20261023/port-3-3/r9-capture/`), with its DDR record, the
  `r9-survey` record and the three-link chain kernel check, forced stall,
  thermal: it accepts 200, 600 and 400 MHz and the kernel runner admits the
  chain (`package-fixtures/port-3-3-013-package/`, and
  `port-3-3-013-signature-package/`). These records are fixtures; that
  instance no longer exists.
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
and `R=$V/rank-9-usb-v3`. After the 2026-10-23 refusal both ports were
power-cycled and passed a fresh cold DDR start, recorded in `$S/ddr-2/`;
set `D=$S/ddr-2/session.json`. The board is on the instance that the last
successful run created: the DDR start itself if nothing has run since, or
the last returning image of ranks 6 to 8. Every directory below starts with
`r9-v3-`, because port 3-3 already holds the `r9-` and `r9-v2-` records of
2026-10-23 and every runner refuses an existing directory.

1. Turn the instruction cache off and survey:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/r9-v3-cache-off 120 \
     "Board $PORT: returning instruction-cache disable before the native USB channel" \
     "The helper turns the instruction cache off and the 40 SRAM bytes at 0x73000 are restored." \
     -- $PY -B $V/common/cache-off.py $S/r9-v3-cache-off
   $PY -B $V/common/survey.py $PORT $S/r9-v3-survey "Board $PORT before the native USB channel"
   ```

2. Capture the 14 clock and reference words and the three entry words
   (ROM USB selector, DMA0 reset and gate). These are 4-byte FEL reads; no
   code runs:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/r9-v3-capture 120 \
     "Board $PORT: read-only clock, reference and USB-entry words for the USB input" \
     "Seventeen 4-byte reads complete on the current ROM instance." \
     -- $PY -B $R/usb-capture.py $S/r9-v3-capture
   ```

   Success: `{"port": "PORT", "complete": true, "words": 17}`.

3. Build and check the input offline. Give one `--warm-return` for each
   successful returning run since the DDR start, in order. With nothing run
   since the DDR start the chain is empty:

   ```sh
   $PY -B $R/usb-package.py --bundle $R \
     --capture $S/r9-v3-capture --ddr $D --survey $S/r9-v3-survey/session.json \
     --output $S/r9-v3-package
   ```

   After ranks 6, 7 and 8 ran on that DDR start, from directories `R6`,
   `R7` and `R8`:

   ```sh
   $PY -B $R/usb-package.py --bundle $R \
     --capture $S/r9-v3-capture --ddr $D --survey $S/r9-v3-survey/session.json \
     --warm-return $S/R6/session.json \
     --warm-return $S/R7/runtime/session.json \
     --warm-return $S/R8/runtime/session.json \
     --output $S/r9-v3-package
   ```

   The builder refuses unless the capture and survey cover the same
   instance, the image's USB constructor and stable predicate accept the
   captured fabric configuration, the ROM USB selector is 0 and DMA0 is
   held in reset with its gates off. It writes `input.bin`, records the
   decoded rates in `config-provenance.json` and `input-validation.json`,
   validates the DDR record, survey and warm-return chain with the bundle's
   kernel runner, and writes `source-review.json`,
   `entry-prerequisites.json` and `command.json`, the complete queue
   command of step 5. Its run directory is `$S/r9-v3-usb`.

4. Review `$S/r9-v3-package/source-review.json` and
   `$S/r9-v3-package/entry-prerequisites.json`, in particular the PIO-only
   USB history of this powered lifetime, secondaries OFF, no other writer,
   the requested pin and rail profile, and the availability of power-removal
   recovery. `$S/r9-v3-package/config-provenance.json` states the configured
   rates.

5. Run the channel once with the queue command that step 3 wrote:

   ```sh
   $PY -B $V/common/exec-command.py $S/r9-v3-package/command.json
   ```

   `command.json` runs `common/bundle-run.py` with a 180 s queue timeout,
   the prerequisites `supervised-ram-recovery` (written for the current
   instance), `usb-return-source-review` and `usb-entry-ownership`, and the
   runner `$R/host/tools/check-a733-usb-returning.py --fel ... --image
   $R/probe.bin --ram-input input.bin --evidence $S/r9-v3-usb --expect
   "COMPLETE native USB bounded observation" --return-timeout 5
   --ddr-evidence $D --platform-survey $S/r9-v3-survey/session.json` with
   the `--warm-return` links of step 3. `exec-command.py` prints the command
   before it runs.

   Success text: `COMPLETE native USB bounded observation`, `PASS: kernel
   check, FEL recovery, automatic USB access.`, `PASS native USB complete
   OUT/IN observation, quiescence and responsive ROM return.` and
   `"result": "passed"`. Functional success also requires
   `native_usb.functional_success` true in `$S/r9-v3-usb/session.json`; the
   runner exits nonzero without it.

   A refusal at USB takeover returns to the ROM with status 1 and prints
   `INCOMPLETE USB: USB preparation` with the provider's first fault.
   With `rank-9-usb-v3/`, `USB_FAULT_STAGE=0x18` with `USB_FAULT_MASK=0x640`
   and bit 6 set in `USB_FAULT_OBSERVED` means the PCTL byte store did not
   remove SoftConn within 150 ms. With `rank-9-usb-v4/` the same condition
   almost always ends as `INCOMPLETE USB: startup deadline/provider` with
   `USB_STAGE=0x1d`, `USB_FAULT_KIND=0` and `USB_FAULT_OBSERVED=0`: the USB
   task's own deadline check uses the provider's deadline and runs first.
   Stage `0x1d` alone shows that SoftConn never read clear. No reset is
   written. The design document lists the remaining causes.

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

A warm-return chain that contains a forced-stall link or a native-runner
link (ranks 7 and 8) needs the patched runner; the unpatched kernel runner
refuses both kinds. A chain of kernel-mode returns only, or an empty chain,
needs no patch. Two alternatives work with the unpatched runner
in `bringup`:

- Initialize DDR again before step 1, then give step 3 only that DDR
  session and the new survey, with no `--warm-return`.
- Give step 3 `--signature-path` and no `--warm-return`. The command then
  runs `tools/check-a733-usb-returning.py` from `bringup` without DDR
  admission. The runner admits on the probe signature at `0x42000000`,
  which the rank 6 result wrote and ranks 7 and 8 restore.
