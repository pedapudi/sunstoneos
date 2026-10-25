# microSD UHS-I SDR104 qualification and read throughput

Status: **not frozen**. The coordinator freezes the image, sessions and
authority records after review. Nothing here has run. [The UHS-I
design](../designs/sd-uhs.md) gives the sequence, its sources and the
fallback rules.

The test runs the supervised read-only SD service twice on one board, from the
same image and card, with whole-board power removal before, between and after:

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
| Sector digests | Reference | Identical to lifetime A for every planned read |

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
