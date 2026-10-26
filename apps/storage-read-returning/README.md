# Returning microSD reads

Status: **DMA implemented and checked in software; physical DMA and SDR104
qualification pending.** The earlier programmed-I/O image completed 256
high-speed reads on port 3-3 (`board-session-20261025-30`). Its SDR104 run
stopped at CMD11 (`board-session-20261025-32`). The corrected switch and this
DMA image have native instruction-model evidence, not board evidence. See
[the DMA design](../../docs/designs/sd-dma.md) for the measured PIO baseline
and predicted DMA costs. Every DMA lifetime requires whole-board power
removal before another operation until the first physical qualification.

The default input selects SD profile 2 (high speed, four-bit, 3.3 V). A
completed read plan restores the SDC0 module/bus/reset fields and PF pins
before returning through the qualified monitor to the boot ROM. Profile 3
keeps its existing UHS-I protocol but cannot authorize return: its 1.8 V
restoration remains unqualified. Neither mode writes card data.

## What the image does

The image runs on the returning platform: the EL3 monitor, EL1 platform and
FreeRTOS kernel of the rank 9 native USB image (`rank-9-usb-v4`), with the
monitor and platform objects of the kernel check that passed on port 3-3 with
the vendor EL3 security writes and the MBUS restore (`build.py` lists each
object's origin). Before EL1 starts, that monitor restores MBUS
(`include/a733_el3_mbus.h`): when the word's gate, DFS, update, source and
divider fields hold the ROM's `0x84000000` (400 MHz) and MSI_LITE0 is
released, it sets them to boot0's `0x81000000` (600 MHz). It skips the
restore when the GPU or NPU is clocked, DMA0 is busy or CE has a task loaded
(codes 8 to 10). It writes `MBUS <before> <after> <code>` as the first line
of the result page only when the word changed, a skip had one of those
codes, or the restore failed. A word that already holds boot0's value, a
word with other field values, and an MSI_LITE0 that is not released all
skip the restore with code 0 and no line. The first returning run after a
cold DDR start is the first case: boot0 left MBUS at `0x81000000`, and only a
return through the ROM's forced-FEL entry sets `0x84000000` again
(lifetime A of 2026-10-25 read `0x81000000` before its launch and logged no
line). Two tasks run on core 0:

- The PMIC task polls the production PMIC provider until the SD task asks it
  to park.
- The SD task runs the production SD0 provider (`drivers/storage/`) with the
  production clock, pin and rail providers. It identifies the card in the
  input's profile and runs the input's read plan. For each read it records the
  time from submission to completion, command/first-byte/completion
  observations, cache-maintenance durations, and the CRC-32 of the data.
  Reads use IDMAC with a 128 KiB write-back buffer and 33 non-cacheable
  descriptors at `0x42100000`; every page is checked for writable identity
  translation before admission. CRC instructions run outside the timed read. It then asks the PMIC task to park and waits for it until the end
  deadline, checks that the park happened and that no PMIC request, rail
  borrow or register lock is outstanding, then requests terminal SD close.

**Normal close and uncertain exits.** Close accepts only profile 2 after a
successful read has completed and its result has been collected. The existing
core DONE path already proves descriptor ownership returned, command/data
completion, IDMAC reset/idle, interface/FIFO reset, DMA disabled and AFTER
cache maintenance. Close rechecks idle/error-free hardware; it never uses a
reset or clock gate to manufacture a drain proof.

Close stops the card clock with MASK_DATA0 and the vendor UPDATE sequence,
checks errors before acknowledging observed flags, then gates the module,
gates its bus and asserts reset in the pinned BSP's order. It restores the
initial module factors, releases PF0..6 through the pin provider, and releases
the SD rail/clock leases. Every phase shares one absolute deadline. A failed
phase retains all remaining owners and cannot be retried as a new lifetime.
The PMIC performed snapshots only: its supply settings, PL pins and clock
route remain unchanged. Its worker must be parked with no queued or borrowed
request, and EL3 verifies its bus is idle before return.

Private linker wrappers leave the qualified monitor object unchanged. Before
either task is scheduled, EL1 marks a receipt at `0x42101000` ARMED. This
non-cacheable page is separate from descriptors and the monitor log. Only a
completed plan and successful terminal close publish READY. The EL3 wrapper
accepts the normal success SMC only with READY, matching initial SD/PF/PMIC
pin and clock fields, and idle S_TWI0; it consumes the receipt and prints
`SD_RESTORED=1` before entering the qualified monitor's existing return path.
The linked vector branches all pass through this wrapper.

Every timer, exception, panic, incomplete plan or failed SD/provider
restoration before monitor delegation parks with ownership retained. It performs no speculative reset, pin
remap, ROM return or memory reuse. Even a timer after READY parks. Input,
provider-binding and return-admission refusals before task scheduling may
return status 1 because no task has touched SD or PMIC hardware. A malformed
or uninitialized receipt cannot use that exception. The current image is
still physically unqualified; these changes do not authorize a preserved
board lifetime without its existing recovery prerequisites. The inherited
monitor may replace status 0 with status 6 if its GIC restoration fails and
still reset to ROM. The SD restoration proof precedes that delegation; the
host requires an explicit final status 0 to report success.

**Input.** `tools/storage-read-returning.py input` writes the 2192-byte input
that the image reads at `0x43000000` (`sdrt.h`): the profile, fresh owner,
handoff and topology identifiers, the fabric words and rates, the PMIC and PF
pin configurations, and the read plan. The fabric words come from the
lifetime's own FEL read of CCU `0x500`, `0x580` and `0x588`
(`tools/board-bundles-next-read.py`) on the DDR start's node and ROM
identity. The tool applies the monitor's MBUS rule with the mask and values
of the monitor's header (fields equal to the ROM's value become boot0's),
then masks the words and computes their rates as the SD0 provider does. The
read cannot see the monitor's other conditions, MSI_LITE0 released and no
active bus master. When the monitor skips the restore for any reason, the
SD0 provider refuses the predicted MBUS word and the report says so
(`SD_REASON=3`, with the provider's stage and fault); the monitor's `MBUS`
line precedes the report only for codes 8 to 10. Input version 2 with
`--plan measurement` (the compatible default) selects a
240-read plan: 64 sequential and 64 random 4 KiB reads, 32 sequential and
32 random 32 KiB reads, eight sequential and eight random 128 KiB reads,
and 32 single sectors. Random starts align to the request size below sector
4,194,304. Sequential classes start at sector zero. The first 64 reads
repeat the accepted PIO run's sectors for CRC comparison. Profile 2 is the
input tool's default; profile 3 requires an explicit selection and cannot return.

For the first DMA read, use `input --profile 2 --plan first-read` with the
same approval, request, DDR, survey, fabric-read, configuration and output
arguments. This emits one read of sector 0, eight blocks (4096 bytes), in
the existing input format. It exercises multi-block DMA and automatic stop
without a measurement sweep. It refuses profile 3 and preserves all
lifetime admission checks. The existing native binary accepts both plans.

**Report.** The log lines of the result page:

- `CARD`: CID, CSD, OCR, RCA, capacity, SCR, the mode word (profile, bus
  width, CMD6 access mode, module divisor), the card clock, the UHS-I word
  (outcome, 1.8 V flag, capabilities, sample delay, tuning window) and the
  identification time;
- `R`: index, total elapsed microseconds, command time, first-byte time,
  completion time, clean and invalidate durations, poll count and CRC-32.
  The input supplies sector and block count. Compact fields saturate at
  their all-ones value; those reads are excluded from timing statistics;
- `SD_READS`, `SD_PLAN`, `SD_PMIC_PARKED`, `SD_CLOCK_OFF`, then
  `COMPLETE SD0 returning read`, or `INCOMPLETE SD0 returning read:` followed
  by `window` or `stopped`; a stop also prints `SD_REASON`, the provider's
  stage, fault, last command and result.

`SD_CLOCK_OFF=1` now means terminal close completed, including pin and
lease restoration; zero means it did not. Older frozen images used values
2 and 3 for weaker clock-off observations. Their captures remain historical
evidence, not proof of this return path.

`SD_RETURN_VERSION=1` identifies the new return contract. A complete decoded
log with this marker requires profile 2 and `SD_RESTORED=1`. Earlier version-1
PIO and version-2 DMA logs without that marker remain decodable. `SD_REASON`
1–7 retain their existing provider/read meanings; 9 means PMIC parking was
not proved and 10 means terminal close was refused or failed. These failures
park after reporting and cannot produce a controlled ROM return.

`tools/storage-read-returning.py decode` checks every `R` line against the
input's plan and reports per-class throughput and observed card/data phases.
It fits fixed request cost plus per-block cost over the sequential classes,
with and without measured cache maintenance. Censored values are excluded,
with explicit counts. The decoder also accepts version-1 PIO captures.
Successful first-read decoding requires `decode --reference PIO_RESULT`
with the accepted profile-2 PIO run's decoded `sd-result.json`. It requires
a complete successful reference, the same card CID and a matching sector-0,
eight-block, 4096-byte record and CRC. Failed runs remain decodable without
a reference. First-read reports contain no throughput fit.

For `--plan measurement`, `decode --reference PIO_RESULT` retains the
comparison of all 64 repeated reads. Measurement decoding without a
reference makes no reference-parity claim. Neither plan establishes
physical DMA recovery; whole-board power removal remains required after
the first DMA lifetime.

## Time window

The monitor arms its secure timer for 2 s before EL1 starts. The private
wrapper parks on expiry; it cannot extend the timer or use it as a drain
proof. The image's
deadlines, measured from the start of `kernel_main()`:

| Deadline | Time | Bounds |
| --- | --- | --- |
| Identification | 1.15 s | Power-up checks, CMD0, ACMD41, mode selection and, for profile 3, the voltage switch and 64-delay CMD19 sweep |
| Plan end | 1.55 s | No read starts after it; the report then says `window` and gives the reads completed |
| Provider lifetime | 1.75 s | Every SD0 operation, including terminal close |
| End | 1.8 s | PMIC parking and the report |

Identification polls for up to 1 ms per wake and yields to the PMIC task.
During each read the SD task runs above the PMIC task and polls to completion
or the provider's 100 ms deadline, with no tick sleep. The controller moves
all data; no CPU FIFO read occurs during DMA transfers. CMD13 follows only
an open-ended CMD18 ending at the card's last sector.

The 4.52 MiB plan's physical time remains unmeasured. The design predicts
about 21–23 MB/s for 128 KiB reads at high speed and 50–60 MB/s at SDR104,
subject to card streaming speed and cache costs. A slow card can exceed the
plan window; the report then gives only completed reads. A new lifetime
requires power removal and fresh initialization.

## What the forced-FEL return changes

The monitor returns by writing the reset vector base address (`0x08001004`)
as `0x20` and requesting an AArch32 warm reset (`platform/a733/monitor.S`,
`monitor_reset`). The CPU warm reset resets no peripheral. A static
reachability analysis of the ROM path from entry `0x20` through the FEL
dispatcher at `0x2514` into the nonsecure ROM's USB FEL loop found:

- **PF bank voltage selector:** untouched. No reachable code refers to
  `GPIO_POW_VAL_SET_CTL` (`0x02000070`), and the GPIO helper reached on this
  path skips `GPIO_POW_MOD_SEL` and `GPIO_POW_VAL` for the argument it
  receives.
- **SD controller:** untouched. The SMHC0 registers (`0x04020000`) and the
  SMHC0 clock, gate and reset words (`0x02002d00`, `0x02002d0c`) appear only
  in the ROM's storage-boot code, which this path does not reach, and nowhere
  in the nonsecure ROM.
- **PMIC:** untouched. No S_TWI0 address appears in either ROM image.
- **PF pin functions:** changed. The dispatcher's first call (`0x24d8`, at
  `0x2528`) rewrites `PF_CFG0` (`0x02000300`) as
  `(old & 0xff0f0f00) | 0x00303033`. That returns PF0, PF1, PF3 and PF5 to
  function 3, the JTAG route, and keeps PF2 (the SD clock), PF4 and PF6.
  The same write runs on every FEL entry: it produces the ROM JTAG route that
  every lifetime starts with and that the SD provider takes over
  (`a733_mmc_sd0_inherited_mux`).

The current profile-2 path restores SD0's initial clock/reset and PF pin
fields before this ROM path runs. It never reaches ROM from an armed timer
or exception. The card remains powered at 3.3 V; successful software close
is not yet physical evidence that another native lifetime is eligible.

Earlier frozen profile-3 images returned with the PF bank and card at 1.8 V.
Those captures and the ROM analysis remain historical evidence. The current
image parks for profile 3. Whole-board power removal remains its only
qualified way back to 3.3 V; a system reset may restore the selector while
the card remains at 1.8 V and must not be substituted.

The analysis is static. It does not cover the two indirect calls of the
nonsecure ROM, which can reach only code that names no PF, SMHC or PMIC
address, or the secure path at `0x11000`, which runs only for a ROM entry
argument the forced-FEL return does not pass.

## Build and check

From the repository root, with a central build configured from this tree and
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`:

```sh
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python apps/storage-read-returning/build.py \
  --platform-build /absolute/central-build --evidence /absolute/new/directory
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/sd-return-native.py \
  --image /absolute/new/directory/build/probe.elf --output /absolute/new/check
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/sd-return-host.py \
  --native /absolute/new/check --output /absolute/new/host-check
python3 checks/sd-return-close.py --output /absolute/new/provider-check
```

`build.py` refuses unless this tree's `platform/a733/monitor.c`, compiled
with the monitor's counter option, equals byte for byte the monitor object of
the kernel check that passed on port 3-3 with the EL3 security writes and the
MBUS restore (`board-bundles-20261025-mbus-qual-2`, run
`board-session-20261025-24/port-3-3/bb2-run-2`). It
writes `origins.json`, which compares every platform and provider object with
`rank-9-usb-v4`'s.

`checks/sd-return-native.py` checks the current linked task, close and monitor
guard with explicit MMIO/card/cache/service models. It observes delegation
to the unchanged qualified monitor entry; it does not execute a new ROM-return
sequence. `checks/sd-return-close.py`
checks actual provider restoration under ASan/UBSan. The unchanged qualified
monitor body and arithmetic fixtures are reused.

For earlier frozen images, `checks/storage-read-returning.py` runs the input copy,
provider construction, SD task, quiescence check and report under the SD card
and controller models of the supervised SD0 checks, for profiles 2 and 3, the
port 3-3 configuration, a window expiry, an absent card, two CRC faults and an
unparked PMIC task. `checks/storage-read-returning-host.py` checks the host
tool's signing, input and decoding. Neither establishes physical operation.

The return change adds 5,412 bytes of linked text against the same-platform
`6a945b30` baseline, 80 bytes of private monitor state and a 16-byte NC receipt.
BSS is unchanged. The text increase includes the existing pin, rail and clock
release implementations becoming reachable. Ordinary read polling and cache
maintenance are unchanged; close adds six bounded phases once per lifetime.
No physical close duration or throughput is claimed. A future authorized
runner must require `SD_RESTORED=0x0000000000000001` as its success expectation
and use the current pre-task-only controlled outcomes.

## Board configuration

`BOARD_CONFIG.json` holds `pmic_pins`, `expected_pins` and `desired_pins`;
the input tool refuses one that also carries fabric words. The expected PF
pins are the ROM JTAG route: function 3 on PF0, PF1, PF3 and PF5 and
function 15 on PF2, PF4 and PF6.

The fabric words are read in each lifetime because the SD0 provider checks
the live AHB, NSI and MBUS words (CCU `0x500`, `0x580`, `0x588`) against its
configuration under the masks `0x0300001f`, `0x8700001f` and `0x8700001f`. A
frozen value would turn any change of those clocks before the run into a
refusal. Of the image's code, only the EL3 monitor writes one of them: the
MBUS restore, which the input tool predicts. The SD0 adapter admits the three
words for reading only (`a733_mmc_platform_native_address_selected`), and no
other unit writes them. On port 3-3 a pre-launch read gives `0x03000002`,
`0xc3000000` and `0x84000000`; the image's configuration is then
`0x03000002`, `0x83000000` and `0x81000000` at 200, 600 and 600 MHz. Bit 30
of `0xc3000000` is outside the mask, and `0x81000000` is the restored MBUS
word. Every difference between the tool's prediction and the monitor's
decision ends the same way: the provider refuses the word before any SD
command.
