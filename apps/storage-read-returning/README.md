# Returning microSD reads

Status: **implemented and host-checked; not run on a board.** The image reads
the microSD card on SDC0 in SD profile 2 (high speed, 3.3 V) or profile 3
(UHS-I SDR104, 1.8 V), returns to the boot ROM, and leaves its result in DRAM
for the host to read over FEL. It is the result path for boards whose USB-C
cable is their only power supply, where the supervised SD0 service's recovery,
a manual USB reconnect, removes power and erases any result still in DRAM.

## What the image does

The image runs on the returning platform: the EL3 monitor, EL1 platform and
FreeRTOS kernel of the rank 9 native USB image (`rank-9-usb-v4`), with the
monitor and platform objects of the kernel check that passed on port 3-3 with
the vendor EL3 security writes (`build.py` lists each object's origin). Two
tasks run on core 0:

- The PMIC task polls the production PMIC provider until the SD task asks it
  to park.
- The SD task runs the production SD0 provider (`drivers/storage/`) with the
  production clock, pin and rail providers. It identifies the card in the
  input's profile and runs the input's read plan. For each read it records the
  time from the first poll after submission to completion, and the CRC-32 of
  the data. It then parks the PMIC task, checks that no PMIC request, rail
  borrow or register lock is outstanding, writes its report and exits.

The image never touches USB0. The monitor returns to the ROM after
`platform_exit()`, and the host reads the 16 KiB result page at `0x42000000`
over FEL.

**Input.** `tools/storage-read-returning.py input` writes the 2192-byte input
that the image reads at `0x43000000` (`sdrt.h`): the profile, fresh owner,
handoff and topology identifiers, the fabric words and rates, the PMIC and PF
pin configurations, and the read plan. The plan is the supervised service's
throughput plan, `throughput_plan(0, 4194304)` in
`tools/check-a733-storage-read-supervised.py`: 128 sequential eight-sector
reads from sector 0, 96 eight-sector reads at fixed pseudo-random offsets
below sector 4,194,304, and 32 single sectors.

**Report.** The log lines of the result page:

- `CARD`: CID, CSD, OCR, RCA, capacity, SCR, the mode word (profile, bus
  width, CMD6 access mode, module divisor), the card clock, the UHS-I word
  (outcome, 1.8 V flag, capabilities, sample delay, tuning window) and the
  identification time;
- `R`: one line per read, with its index, first sector, block count, time in
  microseconds and CRC-32;
- `D0` to `D7`: sector 0 in hex;
- `SD_READS`, `SD_PLAN`, `SD_PMIC_PARKED`, then `COMPLETE SD0 returning read`,
  or `INCOMPLETE SD0 returning read:` followed by `window` or `stopped`; a
  stop also prints the provider's stage, fault, last command and result.

`tools/storage-read-returning.py decode` checks every `R` line against the
input's plan and computes the supervised checker's throughput summary
(`throughput_summary`) from the same per-read times.

## Time window

The monitor arms its secure timer for 2 s before EL1 starts and returns to
the ROM with status 2 when it expires; the image cannot extend it. The image's
deadlines, measured from the start of `kernel_main()`:

| Deadline | Time | Bounds |
| --- | --- | --- |
| Identification | 1.15 s | Power-up checks, CMD0, ACMD41, mode selection and, for profile 3, the voltage switch and 64-delay CMD19 sweep |
| Plan end | 1.55 s | No read starts after it; the report then says `window` and gives the reads completed |
| Provider lifetime | 1.65 s | Every SD0 provider operation, including the last read |
| End | 1.8 s | PMIC parking and the report |

The SD task polls the provider for up to 1 ms per scheduler wake during
identification and during each read, and submits, completes and takes each
read and starts the next without waiting for a tick. One poll per 1 ms tick
would not fit the 1280 tuning commands of profile 3, or four ticks per read
the 256 reads, in the window.

The plan fits the window when the card answers promptly. At high speed a
4 KiB read moves its data in about 0.2 ms, so the 256 reads take tens of
milliseconds. The SD specification allows up to 100 ms of read latency per
read, so a slow card can exceed the plan end. The report then gives the reads
that completed, and the remaining reads need another lifetime; the image
cannot continue the plan after a return, because a new launch would have to
initialize the card again with CMD0.

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

After a return, therefore, the card stays powered at the signalling voltage
of its lifetime, the PF bank selector still matches it, and the controller
keeps running the card clock. The command and data lines carry the JTAG
functions with the pulls the provider set; with no debugger attached the TAP
stays in reset and its TDO output stays released, so the command line stays
high and the card, in its transfer state, receives no command.

## Lifetime B uses the returning path

For profile 3 the return leaves the PF bank at 1.8 V, the card at 1.8 V
signalling and the controller untouched, so no step drives 3.3 V into a card
that signals at 1.8 V. The only way back to 3.3 V signalling is the
whole-board power removal that ends lifetime B, which the SD specification
allows and the runbook already requires. Until then the board runs nothing
else: no further launch, no FEL reset and no watchdog, because a system reset
returns the selector to 3.3 V while the card keeps 1.8 V.

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
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/storage-read-returning.py \
  --image /absolute/new/directory/build/probe.elf --board-config BOARD_CONFIG.json --output /absolute/new/check
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python checks/storage-read-returning-host.py \
  --output /absolute/new/host-check
```

`build.py` refuses unless this tree's `platform/a733/monitor.c`, compiled
with the monitor's counter option, equals byte for byte the monitor object of
the kernel check that passed on port 3-3 (`board-bundles-20261025-10`). It
writes `origins.json`, which compares every platform and provider object with
`rank-9-usb-v4`'s.

`checks/storage-read-returning.py` runs the linked image's input copy,
provider construction, SD task, quiescence check and report under the SD card
and controller models of the supervised SD0 checks, for profiles 2 and 3, the
port 3-3 configuration, a window expiry, an absent card, two CRC faults and an
unparked PMIC task. `checks/storage-read-returning-host.py` checks the host
tool's signing, input and decoding. Neither establishes physical operation.

## Board configuration

`BOARD_CONFIG.json` holds `fabric`, `fabric_hz`, `pmic_pins`,
`expected_pins` and `desired_pins`. For port 3-3 the fabric words are
`0x03000002`, `0x83000000` and `0x84000000` at 200, 600 and 400 MHz, the
words port 3-3 reads at CCU `0x500`, `0x580` and `0x588`
(`board-session-20261023/port-3-3/r9-v4-capture`), with bit 30 of `0x580`
removed: the board reads `0xc3000000` there, and every provider's fabric
check masks that bit out. These are the words of the rank 9 input that passed
on port 3-3. The expected PF pins are the ROM JTAG route: function 3 on PF0,
PF1, PF3 and PF5 and function 15 on PF2, PF4 and PF6.
