# DRAM training capture on port 3-3

Status: **built, host-checked and frozen; not run.** This is run 1 of the
board plan in [the DRAM fast-boot design](../designs/dram-fast-boot.md#five-fel-runs-test-the-options-before-any-card-write).
It trains DRAM with the unchanged FEL DDR step and then reads, in the same
lifetime, the parameter block that boot0 leaves in SRAM and the DRAM
controller and PHY registers. Its results decide whether a trained-register
restore is worth building.

The run writes no storage and changes no training input. After training,
the capture writes SRAM at `0x71000` (the cache-off helper) and `0x85000`
(the reader), DRAM at `0x43000000`–`0x431c1000` (tables and results), and
two PHY registers: `MicroContMuxSel` (`0x0aaa0000`) and, in stages 3 and 4,
`UcclkHclkEnables` (`0x0aa80100`). The reader restores both to the values
it read. boot0 makes the first write on every boot, when it reads two PHY
registers after training (`0x5767a`–`0x5768a`), and in its training-save
routine (`0x576e4`); NXP's published driver for this PHY makes both.

## Frozen inputs

`F=~/.cache/sunstoneos/evidence/dram-fast-boot-20261025`

| Input | Identity |
| --- | --- |
| Source tree `$F/source` | `work/dram-fast-boot-1025` at `1874fb45` |
| Capture build `$F/capture-build/manifest.json` | `6838a7fa…` |
| Reader `ddr-capture.bin`, 388 bytes at `0x85000` | `b8513eff…` |
| Runner `$F/source/tools/run-ddr-capture.py` | `7381060e…` |
| DDR runner `$F/source/tools/run-ddr.py`, equal to `bringup` | `783ac662…` |
| DDR build `~/.cache/sunstoneos/build/a733-ddr/manifest.json`, wrapper `a401ce2b…` | `4f20beed…` |
| `sunxi-fel` | `4a54cdc1…` |

`$F/freeze.json` holds the full hashes. On this build, `checks/ddr-capture.py`
passed: every stage reads each listed register once, in table order, and
writes only the two controls; a wrong magic, a bad width and a zero count
refuse. `checks/ddr-capture-host.py` passed: the runner binds to the DDR
mailbox before any execution and stops at the first incomplete stage.
Their outputs are `$F/check-ddr-capture.txt` and
`$F/check-ddr-capture-host.txt`.

| Stage | Content | Registers | Output bytes |
| --- | --- | --- | --- |
| 1 | boot0's save window: two registers per lane, nine lanes, four byte-lane blocks | 72 | 144 |
| 2 | Controller words at `0x0a020000`, `0x0a021800`, `0x0a021d00`, `0x0a030000`, `0x0a110000` and `0x0a510000` | 1,842 | 7,368 |
| 3 | PHY blocks over boot0's extents: APB-only, DRTUB, master, two `0x30000` blocks, four byte lanes, ACSM, initialization engine, four P-state copies of `0xa0000` | 32,007 | 64,040 |
| 4 | Training firmware message block at `0x0a9b0000` | 1,233 | 2,468 |

## Commands

Set the variables of [the README](README.md#names-used-in-every-runbook)
with `PORT=3-3`, then:

```sh
F=~/.cache/sunstoneos/evidence/dram-fast-boot-20261025
S=<a new session directory for a fresh port 3-3 lifetime>
```

0. The board is in a fresh lifetime after a power removal, and no runner
   has used it since. The session access rule for port 3-3 is installed
   (`tools/usb-access.py check-session --port 3-3`).
1. Cold DDR, unchanged:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/dc-ddr 300 \
     "Board $PORT: cold DDR before the DRAM training capture" \
     "boot0 trains LPDDR5 and reports stage 5 with 12,288 MiB; both 4 KiB pages read back." \
     -- $PY -B $F/source/tools/run-ddr.py --fel $FEL \
        --build ~/.cache/sunstoneos/build/a733-ddr --evidence $S/dc-ddr
   ```

2. Capture, once, directly after step 1:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/dc-capture 300 \
     "Board $PORT: DRAM parameter block and controller and PHY register capture" \
     "The mailbox equals dc-ddr's; stages 1 to 4 complete with MicroContMuxSel read as 1." \
     --artifact $F/freeze.json --artifact $F/capture-build/manifest.json \
     -- $PY -B $F/source/tools/run-ddr-capture.py --fel $FEL \
        --build $F/capture-build --ddr-evidence $S/dc-ddr --evidence $S/dc-capture
   ```

Each command runs once. A nonzero exit stops the session: the runner keeps
the port lock, quarantines the instance and waits for the coordinator. A
stage that stops the bus leaves the ROM unable to answer, and the board then
needs a power removal; stages 1 and 2 are already on the host by then.

## Predictions and pass criteria

| Item | Prediction | Source |
| --- | --- | --- |
| `dc-ddr/firmware.log` | Results 7 for P-states 1, 2, 3 and 0; 12,288 MiB; `DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE` 5,083–5,103 ms | 58 earlier logs |
| `dc-capture/session.json` | `complete` true; four stages with `done` true and `progress` equal to the last entry | Reader and runner checks |
| `muxsel` in every stage | 1 | boot0's last PHY read after training sets it to 1 (`0x5768a`) |
| `ucclk` | Recorded; no prediction | Not written by boot0 after training |
| `parameter_block` words 0–31 | Equal to `dc-ddr/dram-parameters.bin` and to earlier captures, `dram_tpr13` = `0x10065` | boot0 copies 128 bytes into a zeroed buffer |
| `parameter_block` words 32–127 | Zero | `dram_tpr10` bit 29 is clear, so `0x576e4` does not run |
| Stage 1 values | Each halfword below `0x80` | 7-bit fields that boot0's save routine masks |

The run passes when both commands exit 0 and every prediction above holds.
A stage-1 halfword at or above `0x80` means the window holds other fields
than the design assumes; record it and continue with the analysis.

## After the run

Each `stageN-*.bin` file holds the stage's registers in table order; the
`entries` list of `$F/capture-build/manifest.json` gives each range's
address, width and byte offset. The next board step is a second capture in
another lifetime (run 2 of the plan), so that registers which differ between
two trainings can be separated from fixed configuration.
