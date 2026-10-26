# DRAM training captures on ports 3-1 and 3-3

Status: **frozen for both boards; two port 3-3 lifetimes have run.** On
2026-10-25 a four-P-state lifetime with the v3 set
(`board-session-20261025-21`) and a P-state-0-only lifetime with the v5 set
(`board-session-20261025-25`) completed all six stages in both captures,
and every checked prediction held. The P-state-0-only training took
4,441 ms from `DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE`, against 5,098 ms
for the four-P-state one: the 2040 MHz run costs about 4.4 s whether it
runs alone or last. Port 3-1 has not run. These
are the captures of [the training-restore design](../designs/dram-training-restore.md#captures-identify-which-registers-training-sets).
Each lifetime trains DRAM with the unchanged FEL DDR step and then captures
twice: the parameter block that boot0 leaves in SRAM, the SID chip ID and
thermal calibration, and the DRAM controller and PHY registers.
`tools/analyse-ddr-captures.py` then classifies every register as constant,
board, lifetime or status. These are the four-P-state captures, the first
of the three capture kinds in the restore design. The other two, after a
P-state-0-only training and after an initialization-only run, are frozen in
the same set and wait for the design's second review
([their section](#p-state-0-only-and-initialization-only-captures)). The v4
set below replaces v3 and v4; all three hold the same reader and stage
tables for the four-P-state captures, so their captures are analysed
together. The sets
`~/.cache/sunstoneos/evidence/dram-fast-boot-20261025`,
`dram-training-capture-20261025`, `dram-training-capture-v2-20261025` and
`dram-training-capture-v4-20261025` are superseded and are not used.

The run writes no storage and changes no training input. After training,
each capture writes SRAM at `0x71000` (the cache-off helper) and `0x85000`
(the reader), DRAM at `0x43000000`–`0x43258000` (tables and results), and
two PHY registers: `MicroContMuxSel` (`0x0aaa0000`) and, in stages 4 to 6,
`UcclkHclkEnables` (`0x0aa80100`), which the reader reads only after
clearing `MicroContMuxSel`. It restores both to the values it read; the
first lifetime read `UcclkHclkEnables` as 0. boot0 makes the first write on every boot, when it reads two PHY
registers after training (`0x5767a`–`0x5768a`), and in its training-save
routine (`0x576e4`); NXP's published driver for this PHY makes both.

## Frozen inputs

`F=~/.cache/sunstoneos/evidence/dram-training-capture-v5-20261025`

| Input | Port 3-1 | Port 3-3 |
| --- | --- | --- |
| Capture set | `$F/port-3-1` | `$F/port-3-3` |
| Build manifest `capture-build/manifest.json` | `6cb19cbf…` | `6cb19cbf…` |
| Reader `ddr-capture.bin`, 388 bytes at `0x85000` | `0da3194f…` | `0da3194f…` |
| SRAM capture build `capture-sram/manifest.json`, reader `f9aa11e8…` | `bd2c4a6e…` | `bd2c4a6e…` |
| P-state-0-only DDR build `ddr-pstate0/manifest.json`, wrapper `efe58d3b…` | `d3f61d2f…` | `d3f61d2f…` |
| Initialization-only DDR build `ddr-init-only/manifest.json`, wrapper `31c5435e…` | `fbcc95f7…` | `fbcc95f7…` |

Both sets come from `work/dram-fast-boot-1025` at `84de8552`, whose tree is
`$F/source`, and hold byte-identical builds. They share these inputs:

| Input | Identity |
| --- | --- |
| Runner `$F/source/tools/run-ddr-capture.py` | `812a2227…` |
| Classifier `$F/source/tools/analyse-ddr-captures.py` | `581a9f3d…` |
| DDR runner `$F/source/tools/run-ddr.py`, with `--allow-untrained` | `ea7a597a…` |
| DDR build `~/.cache/sunstoneos/build/a733-ddr/manifest.json`, wrapper `a401ce2b…` | `4f20beed…` |
| `sunxi-fel` | `4a54cdc1…` |

Each set's `freeze.json` holds the full hashes and the check results. On
each build `checks/ddr-capture.py` passed: every stage reads each listed
register once, in table order, and writes only the two controls; a wrong
magic, a bad width and a zero count refuse. `checks/ddr-capture-host.py`
passed: the runner binds to the DDR mailbox before any execution, honours
`--stages` and `--allow-untrained`, and stops at the first incomplete
stage. `checks/ddr-wrapper.py` passed on both variant wrappers: each copies
the selected parameters and patches only `dram_tpr13` bit 2 and, for the
initialization-only variant, `dram_tpr10` bits 28:16 to `0x0001`.
`checks/ddr-host-retention.py` passed with the two new cases.
`checks/ddr-capture-analysis.py` passed on synthetic captures.

| Stage | Content | Registers | Output bytes |
| --- | --- | --- | --- |
| 1 | SID chip ID `0x03006200`–`0x0300620c` and thermal calibration `0x03006244`–`0x0300624c` | 7 | 28 |
| 2 | boot0's save window: two registers per lane, nine lanes, four byte-lane blocks | 72 | 144 |
| 3 | Controller words at `0x0a020000`, `0x0a021800`, `0x0a021d00`, `0x0a030000`, `0x0a110000` and `0x0a510000` | 1,842 | 7,368 |
| 4 | PHY blocks over boot0's extents: APB-only, DRTUB, master, two `0x30000` blocks, four byte lanes, ACSM, initialization engine, four P-state copies of `0xa0000` | 32,007 | 64,040 |
| 5 | Training firmware message block at `0x0a9b0000` | 1,233 | 2,468 |
| 6 | PHY registers `0x0000`–`0xbfff` (`0x0a900000`–`0x0a917fff`), where the published PHY keeps address/command delays; boot0 never accesses them, so the stage runs last | 49,152 | 98,304 |

## Commands for one lifetime

Set the variables of [the README](README.md#names-used-in-every-runbook)
with `PORT` set to the board, then:

```sh
F=~/.cache/sunstoneos/evidence/dram-training-capture-v5-20261025
C=$F/port-$PORT
S=<a new session directory for a fresh lifetime of this board>
T="<temperature note: room temperature, minutes since power-on, any load or heating>"
```

0. The board is in a fresh lifetime after a power removal, and no runner
   has used it since. The session access rule for the port is installed
   (`tools/usb-access.py check-session --port $PORT`).
1. Cold DDR, unchanged:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/dc-ddr 300 \
     "Board $PORT: cold DDR before the DRAM training captures" \
     "boot0 trains LPDDR5 and reports stage 5 with 12,288 MiB; both 4 KiB pages read back." \
     -- $PY -B $F/source/tools/run-ddr.py --fel $FEL \
        --build ~/.cache/sunstoneos/build/a733-ddr --evidence $S/dc-ddr
   ```

2. First capture, directly after step 1:

   ```sh
   $PY -B $V/common/bundle-run.py $PORT $S/dc-capture-1 300 \
     "Board $PORT: DRAM parameter block, chip ID and controller and PHY register capture" \
     "The mailbox equals dc-ddr's; stages 1 to 6 complete with MicroContMuxSel read as 1." \
     --artifact $C/freeze.json --artifact $C/capture-build/manifest.json \
     -- $PY -B $F/source/tools/run-ddr-capture.py --fel $FEL --build $C/capture-build \
        --ddr-evidence $S/dc-ddr --evidence $S/dc-capture-1 --temperature-note "$T"
   ```

3. Second capture: step 2 again with `dc-capture-2`. Registers that differ
   between the two are status registers.

Each command runs once. A nonzero exit stops that board's session: the
runner keeps the port lock, quarantines the instance and waits for the
coordinator. A stage that stops the bus leaves the ROM unable to answer,
and the board then needs a power removal; earlier stages are already on the
host. Stage 6 carries that risk most, because boot0 never reads its range;
if it stops a board, later captures of that board add `--stages 5` to the
runner and the analysis uses stages 1 to 5. The other board continues independently.

## Schedule

A register that training sets to one of two equally likely values looks
constant over n lifetimes with probability 2 × 0.5^n: 1/16 for five
lifetimes on one board and 1/512 over ten lifetimes when both boards behave alike. The
diff between the P-state-0-only and initialization-only captures in the
restore design catches registers that training sets to a repeating value,
so the lifetime count only has to find the varying ones and estimate each
one's spread.

The restore uses boot0's P-state-0-only path, so that kind is the reference
and carries most lifetimes. A four-P-state and a P-state-0-only lifetime
differ in about 13,500 registers that boot0 programs differently for the
two kinds (the `0xa0000` block, the address/command sequencer, a group of
master registers and the multi-P-state message-block fields), so lifetime
variation is measured within one kind only.

| Kind, per board | Room temperature | Cold | Warm | Run so far on 3-3 | On 3-1 |
| --- | --- | --- | --- | --- | --- |
| P-state 0 only (`p0-`) | 5 | 3 | 3 | 1 | 0 |
| Initialization only (`in-`) | 2 | — | — | 0 | 0 |
| Four P-states (`dc-`) | 2 | — | — | 1 | 0 |

So port 3-3 still needs four room-temperature, three cold and three warm
P-state-0-only lifetimes, two initialization-only lifetimes and one more
four-P-state lifetime; port 3-1 needs the full column. A cold lifetime
starts after at least one hour unpowered in the coolest available place, a
warm one within a minute of removing power after a sustained CPU and memory
load; `T` records either. The four-P-state lifetimes serve the status class
and the vendor path's own variation; two per board are enough for that.

The analysis groups captures by lifetime and chip ID and records each
capture's note. A temperature reading needs a later THS read in the same
lifetime; until one exists the note is the only temperature record.

After each batch, on the host, once per kind for the lifetime and status
classes, and once over the reference kinds for the restore set. The
analysis touches no board, so it uses the classifier from
`work/dram-fast-boot-1025` at `fb2dfebf` or later (`A`); the frozen copy in
`$F/source` predates the training-kind options.

```sh
A=/home/sunil/git/sunstoneos-worktrees/dram-fast-boot-1025/tools/analyse-ddr-captures.py
$PY -B $A <every p0-capture-N directory> --output <new report path>.json
$PY -B $A <every p0- and in-capture-N directory> \
  --trained-kind pstate0 --baseline-kind init-only --output <new report path>.json
```

The tool reads each capture's training kind from its DDR record. Parameter
words *n* in this runbook are the parameter block's words, which
`session.json` lists as `parameter_block` and which start at offset `0x400`
of `bootparam-buffer.bin`: words 32–51 are that file's 32-bit words 288–307.

## Predictions and pass criteria

| Item | Prediction | Source |
| --- | --- | --- |
| `dc-ddr/firmware.log` | Results 7 for P-states 1, 2, 3 and 0; 12,288 MiB; `DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE` 5,083–5,103 ms; `select dram para5` | 58 earlier logs |
| Each capture's `session.json` | `complete` true; six stages with `done` true and `progress` equal to the last entry; `port` equal to `PORT` | Reader and runner checks |
| `muxsel` in every PHY stage | 1 | boot0's last PHY read after training sets it to 1 (`0x5768a`) |
| `ucclk` | Recorded; no prediction | Not written by boot0 after training |
| Stage 1 chip ID | Equal in every capture of a board, different between the boards | SID |
| `parameter_block` words 0–31 | Equal to `dc-ddr/dram-parameters.bin` and to earlier captures, `dram_tpr13` = `0x10065` | boot0 copies 128 bytes into a zeroed buffer |
| `parameter_block` words 32–51 | Nonzero and equal in the two captures of a lifetime; words 52–127 zero | boot0's save routine `0x576e4` runs every boot, because stored set 5 has `dram_tpr10` bit 29 set (the first port 3-3 lifetime showed this; the prediction before it said zero) |
| Stage 2 values | Each halfword below `0x80` | 7-bit fields that boot0's save routine masks |
| Classifier, after three lifetimes per board | Controller words mostly constant; PHY byte-lane and message-block registers include lifetime-class values; status registers few and confined to the APB-only, DRTUB and message-block areas | The PHY's training writes lane registers; the mailbox and handshake registers change on their own |
| Classifier, both boards | Board-class registers include stage 1 and part of the byte-lane registers | Chip ID and per-chip training |

A board's session passes when every command exits 0 and every prediction
of the first six rows holds. The classifier rows are expectations that the
analysis tests, not pass criteria: the result defines the restore set
whatever it is. A stage-2 halfword at or above `0x80` means the window holds
other fields than the design assumes; record it and continue.

## P-state-0-only and initialization-only captures

**Gate: closed for both runs.** These change boot0's training input. The
second independent review (2026-10-25) closed the gate for the
P-state-0-only run, and its re-check at `a1caa6f3` closed it for the
initialization-only run once its capture moved out of untrained DRAM into
the SRAM build, after confirming that `0x4a000`–`0x5f9a4` is unused by the
ROM, `sunxi-fel` and the capture after the wrapper returns. Each run uses a fresh lifetime;
the commands differ from the four-P-state ones only as listed.

- **P-state 0 only** (three lifetimes per board): step 1 uses
  `--build $C/ddr-pstate0` and the record name `p0-ddr`; steps 2 and 3
  name `p0-capture-1` and `p0-capture-2` and pass `--ddr-evidence $S/p0-ddr`.
  *Predictions:*
  - one `Training result is = 7` line, then `DRAM Pstate 0 training,
    frequency is 2040 Mhz`, and no P-state 1 to 3 lines;
  - 12,288 MiB and `DRAM simple test OK.`;
  - `DRAM BOOT DRIVE INFO` to `Actual DRAM SIZE` about 4,424 ms
    (2 + 4,417 + 5 from the first lifetime), within 4,400–4,650 ms;
  - `dram_tpr13` reported as `0x10061`, `dram_tpr10` as `0x925f0000`;
  - message-block training-step value `0x125f` at `0x0a9b0010` and
    parameter words 32–51 nonzero;
  - both 4 KiB pages read back and six stages complete in each capture.

  A P-state 0 run much shorter than 4.4 s is a finding about boot0's
  multi-P-state runs and must be recorded. A firmware failure returns 0
  from `0x56132`, the wrapper reports 0 MiB and `run-ddr.py` quarantines
  the board.
- **Initialization only** (two lifetimes per board, after the re-check):
  step 1 uses `--build $C/ddr-init-only --allow-untrained` and the record
  name `in-ddr`; steps 2 and 3 use the SRAM build, name `in-capture-1` and
  `in-capture-2`, and add `--allow-untrained` with
  `--build $C/capture-sram --ddr-evidence $S/in-ddr`. The SRAM build keeps
  its tables at `0x4a000` and results at `0x4b000`–`0x5f9a4`, in boot0's
  body area, which is free after the wrapper returns, and omits stage 6.
  *Predictions:* the wrapper returns to FEL with stage 5; if it does not
  return within the host's 30 s limit, the session stops with quarantine. With firmware result 7, the log shows
  `Actual DRAM SIZE =12288 M`, `dram_tpr13 = 10061` and most likely
  `DRAM simple test FAIL… read 10 times again`; `dram_mib` is 0 and
  `dram_tpr10` reads `0x80010000` (bit 29 cleared at `0x49c9a`). With any
  other result the log shows `DRAM initial error : 1 !` and `dram_tpr10`
  stays `0xa0010000`. Parameter words 32–51 hold values read from
  untrained DRAM and are not used. Five stages complete in each capture.
  The lifetime ends there: the next use of the board needs a power removal.

The restore set is the PHY registers that differ between the P-state-0-only
and initialization-only captures of a board, minus the status class.

## After the runs

Each `stageN-*.bin` file holds the stage's registers in table order; the
`entries` list of the build manifest gives each range's address, width and
byte offset. The classifier's `restore_candidates` list and `varying` map
are the input to the restore set, which the restore design's review
freezes.
