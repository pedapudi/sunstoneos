# microSD UHS-I SDR104 qualification and read throughput

Status: **not frozen**. The coordinator freezes the image, sessions and
authority records after review. [The UHS-I design](../designs/sd-uhs.md) gives
the sequence, its sources and the fallback rules. One lifetime has run: lifetime
A on port 3-3 on 2026-10-25, through the supervised image, which stopped before
any SD command ("How the result reaches the host").

On boards whose USB-C cable is their only power supply, the run uses the
returning SD image (`apps/storage-read-returning/`, "The returning run"
below): it reads the card, returns to the boot ROM and leaves its result in
DRAM for FEL. The supervised image's result path needs a USB reconnect after
every run, which on those boards removes power.

The test reads the card twice on one board, from the same image and card,
with whole-board power removal before, between and after:

- **Lifetime A, before:** SD profile 2, four-bit high speed at 50 MHz and
  3.3 V. No SD command has yet run on either board, so this lifetime is also
  the first physical qualification of the high-speed profile.
- **Lifetime B, after:** SD profile 3, UHS-I SDR104 at 1.8 V.

Both lifetimes are read-only on every card area, including boot0, the boot
package and the stage-2 slots. Only CMD17, CMD18 and CMD13 touch media, and
only to read. Goal section 4 therefore needs no host backup or data-partition
confinement for this test.

Run it on port 3-3 first: that board reported adaptive PF withstand mode with
sensed and selected 3.3 V, which the switch requires. Run lifetime B last in
its connection window, because it leaves the card at 1.8 V until power is
removed.

## Inputs

```sh
PORT=3-3
PY=~/.cache/sunstoneos/venv/bin/python
FEL=~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel
SR=/home/sunil/git/sunstoneos-worktrees/sd-uhs-1025   # or the merged bringup tree
S=~/.cache/sunstoneos/evidence/board-session-<date>/port-$PORT
export PYTHONDONTWRITEBYTECODE=1
```

- **Image:** `sunstone-a733-storage-read-single-core.elf`, from the
  standalone project `apps/storage-read-single-core` (target
  `sunstone-a733-storage-read-single-core`). The ROM keeps USB0 after FEL, and
  only the single-core SD build admits USB0 start mode 2, takeover of the
  ROM's device controller (`apps/board/admission.c`); the supervised SD image
  admits only mode 1, a cold controller. Its `storage_read_sd_modes` marker
  is version 2 with profile mask 15, and its records are version 4.
- **Prepared set:** `~/.cache/sunstoneos/evidence/board-bundles-20261025-14/`
  holds the image, the software evidence, the session specification, the
  draft records, one approval per lifetime (`approval-A.json`,
  `approval-B.json`), the host tools pinned as an archive of one revision
  (`host-src/`), `tools/sign-approval.py` and `tools/bind-lifetime.py`. After
  each lifetime's cold DDR start and survey, the coordinator signs that
  lifetime's approval, and `bind-lifetime.py` binds the bundle; it refuses an
  approval signed before the DDR session or for another one, and a survey
  that failed or ran on another port, node or ROM identity. Its `runbook.md`
  gives every command.
- **Session templates:** the existing 880-byte storage-read session for this
  port, with offset 860 (`sd_profile`) set to 2 for A and 3 for B,
  `identify_us` 5,000,000, `read_us` 1,000,000, the wide PF0–PF5 route and a
  one-sector native application range at sector 0.
- **Authority:** the existing topology record for negotiated profiles
  (`four_bit_data_route_authorized`, `read_only_bus_negotiation_authorized`,
  `peri0_400m_stable_owned`, `sd_profile`). Profile 3 also requires
  `uhs_1v8_signalling_approved` (the user's 2026-10-25 approval),
  `pf_bank_voltage_writer_exclusive`, `pf_adaptive_withstand_observed` and
  `board_power_removal_after_lifetime`, each `true`.

Prepare each bundle with `$PY $SR/apps/storage-read/prepare.py --image …
--manifest … --admission … --template … --output … --clang … --lld …`, as for
the earlier storage-read bundles.

## Commands

1. Remove and restore board power on `$PORT` (unplug the USB-C supply; a host
   reconnect alone does not reset the card). Bring the board to FEL and DDR as
   in the [runbook index](README.md).
2. Lifetime A:

   ```sh
   $PY $SR/tools/check-a733-storage-read-supervised.py --bundle <bundle-A> \
     --fel $FEL --evidence $S/sd-uhs-a-hs --database <queue> --claim <id> \
     --owner coordinator --supervised-manual-reconnect --throughput 0:4194304
   ```

   The plan reads 128 sequential 4 KiB requests from sector 0, 96 4 KiB
   requests at fixed pseudo-random offsets below sector 4,194,304 (2 GiB) and
   32 single sectors. The record's `storage_read_throughput` holds MB/s per
   pattern and the per-block and per-request costs.

   Through the board queue, `common/bundle-run.py` takes the timeout and
   the three `--artifact` paths from
   `$PY $SR/tools/storage-read-queue-plan.py --bundle <bundle-A> --format shell`.
   The checker refuses a claim whose submission does not cover the bundle's
   `prepared.json`, `image.elf` and `launch.bin`. The timeout is the sum of the
   checker's own bounds: every FEL request bounded by `fel_timeout`, the
   59-second capture window, the refusal path and 60 seconds of setup. It is
   638 s for the bundles of 2026-10-25. After the capture window every outcome
   but a clean refusal holds the port lock and claim, so the timeout ends that
   hold. `checks/storage-read-queue-plan.py` builds the queue row in a scratch
   database and requires the checker's claim check to refuse it without the
   artifacts and accept it with them.
3. Remove board power, restore it, and bring the board to FEL and DDR again.
4. Lifetime B: the same command with `<bundle-B>` and `$S/sd-uhs-b-sdr104`.
5. Remove board power. This step is mandatory even after a failure: the card
   stays at 1.8 V until it loses power, and the boot ROM's microSD boot and
   every later SD lifetime expect 3.3 V. Between lifetime B and this power
   removal, do not reset the board in any way (watchdog, FEL reset or ROM
   return followed by a new launch) and launch no other image on `$PORT`: a
   reset returns `GPIO_POW_VAL_SET_CTL` to its 3.3 V reset value while the
   card keeps 1.8 V signalling, so the bank would drive 3.3 V into the
   card.

Each command runs once. A retained or failed lifetime A stops the sequence
before B. A retained lifetime B is recorded and not retried; its recovery is
step 5.

## How the result reaches the host

The single-core image does not return to the ROM. After `exe`, it takes over
the ROM's USB0 device controller (start mode 2): one soft disconnect, a device
reset, and enumeration as a new device, product `sunstoneos native SD0 read`,
serial `sunstoneos-a733-storage-read`, `bcdDevice` 0x0100. The checker waits
up to 59 s for that instance, sends the read requests and stores each reply.
If the original FEL instance is still present and unchanged at the end of the
window, it makes one FEL read of the launcher's report at `0x52d00`, which
answers only if the launcher refused before leaving the ROM. Every other
outcome, success included, quarantines the port and holds it.
`--supervised-manual-reconnect` names that recovery: the operator removes
whole-board power before the port or the card is used again. On boards whose
USB-C cable is their only supply, that is a power removal and DRAM is lost.
A successful capture survives it, because the replies are on the host first.
A stop before enumeration does not: the image's `a733_smp_stop` writes its
status to DRAM at `0x42000000` and waits in WFE. FEL cannot read that address
while the CPU is outside the ROM, and the power removal erases it.

Lifetime A on 2026-10-25
(`board-session-20261025-14/port-3-3/sd-uhs-a-hs-2/`) ran `exe` 12.2 s after
the version request. For 59 s the original instance `003:051` stayed present
with the same identity, so the image never made its USB0 disconnect. The
report read then failed in `usb_bulk_send` with a timeout: the ROM's
endpoints were still configured, but no code serviced them, so the launcher
had not refused and the CPU had left the ROM. The image stopped after entry
and before the takeover's disconnect.

Its session supplied fabric words that the USB0 provider refuses, which
explains the stop. Lifetime A's session template
(`board-session-20261025-14/port-3-3/sd-bundle-A/session-template.bin`,
offsets 488 and 500) holds `0x03000002`, `0xc3000000` and `0x81000000` at
200, 600 and 600 MHz, copied from an unreviewed draft. Port 3-3 reads
`0x03000002`, `0xc3000000` and `0x84000000` at CCU `0x500`, `0x580` and
`0x588` (`board-session-20261023/port-3-3/r9-v4-capture`). Every provider's
fabric check masks the second word with `0x8700001f`
(`drivers/usb/a733_usb_platform.c`, `a733_usb_platform_init`;
`drivers/storage/a733_mmc_platform.c`, `a733_mmc_platform_scopes`), and bit
30 of `0xc3000000` falls outside that mask, so `a733_usb_platform_init`
returns 0. The board application calls it through `a733_native_usb_bind`
before the scheduler starts and, on that failure, calls
`platform_panic("board bind", 6)` (`apps/board/main.c`), which stops the CPU
in the monitor with the status in DRAM. The third word is also wrong: the
board's MBUS parent is 400 MHz. The correct configuration is `0x03000002`,
`0x83000000` and `0x84000000` at 200, 600 and 400 MHz, which the rank 9
input used when it passed on port 3-3. This decoding comes from source and
the session bytes; the image's own stop record was erased by the power
removal.

## The returning run

The returning image (`apps/storage-read-returning/README.md`) identifies the
card in the input's profile, runs the same read plan, records each read's
time and CRC-32, parks the PMIC service and exits; the monitor returns to the
ROM within its 2 s window. A stop also returns, with the provider's stage and
fault in the log. `tools/storage-read-returning.py` signs the coordinator's
approval for the lifetime, writes the image's input and decodes the result
into the same throughput summary that the supervised checker computes.

The return leaves the PF bank voltage selector, the SD controller and its
clock untouched, and resets the PF0, PF1, PF3 and PF5 functions to the ROM
JTAG route; the README gives the ROM analysis. Lifetime B therefore also uses
the returning image: the card and the PF bank stay at 1.8 V until the
whole-board power removal that ends the lifetime, and nothing else runs on the
board before it.

Each lifetime runs the cold DDR start, the cache-off helper and the survey,
then:

1. The coordinator signs the lifetime's approval:
   `$PY $SR/tools/storage-read-returning.py sign --approval APPROVAL --ddr $S/DDR --survey $S/SURVEY --signer coordinator`.
2. `$PY $SR/tools/storage-read-returning.py input --approval APPROVAL --request REQUEST --ddr $S/DDR --survey $S/SURVEY --config CONFIG --profile 2 --output $S/INPUT.bin`
   (profile 3 for lifetime B). It refuses an approval not signed for this
   DDR start and survey.
3. The kernel runner, through the board queue:
   `$PY $SR/tools/check-a733-kernel.py --fel $FEL --image IMAGE --mode kernel --ram-input $S/INPUT.bin --expect "COMPLETE SD0 returning read" --controlled CONTROLLED --return-timeout 5 --ddr-evidence $S/DDR/session.json --platform-survey $S/SURVEY/session.json --evidence $S/RUN`.
4. `$PY $SR/tools/storage-read-returning.py decode --run $S/RUN --input $S/INPUT.bin --output $S/RUN/sd-result.json`.

A prepared set gives these commands with every path filled in.


## Predictions

| Quantity | Lifetime A | Lifetime B |
| --- | --- | --- |
| Capability query, `uhs_caps` (UHS word bits 5–6) | Not issued | 3: the 1.8 V switch and PERI0_300M. Port 3-3 read the manual-default PERI0 gates `0x8fff0fff`, status `0x0fff0000` and PLL `0xff126310` in four reads on 2026-10-25 (`board-session-20261025-3/port-3-3/bb9-read/`, `board-session-20261025-7/port-3-3/ts-read-0` to `-3`): every PERI0 branch, including 300 MHz, is enabled at 2.4 GHz. If an earlier test in the window has gated PERI0 bit 4, 5, 9 or 11, the value is 1 and the card runs at 100 MHz |
| ACMD41 | S18R not requested | S18R requested; S18A set if the card is UHS-I |
| CMD11 and switch | Not issued | R1 without errors; DAT3 low 1 ms later; PF value 2 becomes 3 within 10 ms of the one selector write; DAT3 high and `RINTSTS` bit 10 set within 1–11 ms of the restarted clock |
| Outcome | Not applicable | `SDR104` (1); `DECLINED` (3) if the card is not UHS-I, leaving high speed at 3.3 V |
| Mode word | width 4, timing 1, divisor 4, 50 MHz | width 4, timing 3, divisor byte `0x81` (300 MHz / 1), 150 MHz; with `uhs_caps` 1, divisor byte 2 (400 MHz / 2), 100 MHz |
| Tuning window | Not applicable | One longest passing run of at least 12 delays, likely 20–40; the chosen delay is its midpoint. The vendor's untuned default at up to 150 MHz is delay 5 (`sunxi-mmc-v4p5x.c:1258`), which suggests that low delays pass. A run shorter than 12 gives outcome `NO_WINDOW` (5), SDR25 at 50 MHz |
| Identification | Under 0.2 s | 0.4–0.6 s, mostly about 520 CMD19 |
| Sequential 4 KiB reads | 8.5–13.0 MB/s | 8.5–15.6 MB/s, at least lifetime A's |
| Random 4 KiB reads | 6.0–7.9 MB/s | 6.0–8.9 MB/s, at least lifetime A's |
| Per-block cost | 20.8–34 µs (bus floor 20.8 µs) | 14–34 µs (bus floor 6.9 µs) |
| Sector digests | Reference | Identical to lifetime A for every planned read (CRC-32 per read in the returning run) |

The clock rates describe register configuration and are not measured. A
lifetime-B per-block cost equal to lifetime A's means programmed I/O bounds
both; the next change is the controller's descriptor DMA, under its own design.

## Stop conditions and records

- Any retained result: keep the claim and evidence, remove board power and
  record the decoded record's `error`, `protocol`, `command`,
  `controller_status`, `interrupts` and UHS word.
- `SIGNAL` (error 9) after CMD11 means the switch began and failed; the
  specification requires a card power cycle, which step 5 provides.
- A lifetime-B throughput below lifetime A's, or any digest difference, blocks
  the merge until explained.

Record per lifetime: the decoded UHS word (outcome, `signal_1v8`, `uhs_caps`,
`sample_delay`, `tuning_start`, `tuning_width`), the mode word and card clock,
`storage_read_throughput` and the per-read SHA-256 list.
