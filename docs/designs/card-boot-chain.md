# Every card boot stage needs a measured watchdog recovery

**Status: host-tested.** The boot0 and stage-2 rows have a board record of
their recovery; the OS row does not. The runner therefore refuses every
ordinary boot-once run and admits only a run that measures the OS row's
arming (see "Measuring the OS row"). The disarm before each re-arm in the
first stage and stage 2, the stage-2 progress words and the diagnostic
stage-2 build are source and host checks only; they reach the card only with
a coordinator-approved storage write.

A card boot under test control ([card-fel-recovery.md](card-fel-recovery.md))
returns to FEL only when WDT0 resets the board after any stage hangs or
stops. Register readback cannot establish that. On 2026-11-13 the first
boot-once run on port 3-3 (`1794619098327285673-8466a823`) passed every
readback that the host checks model, and the board then presented no USB
device for 124 seconds and afterwards. The chain therefore admits a stage
only when the board has shown the WDT0 arming that covers it expire, in the
same state.

## The chain and the arming that covers each stage

After the runner's WDT0 reset, the ROM loads the card first stage, whose
gate code finds the boot-once value in RTC GP2 and takes its cold path. The
stages after it, and the arming that resets each one if it hangs, are:

| Stage | Covered by | Board record of that expiry after boot0 |
| --- | --- | --- |
| boot0: DRAM training, PMIC writes, package load, monitor entry | The gate's 16-second arming (mode `0xb1`) from mode 0 | `1794643114289036712-ab8b9952` |
| Stage 2: card open, slot headers and slot load | The same arming, restarted after each card read | `1794643114289036712-ab8b9952` |
| The OS, from its entry to its stop | Stage 2's 4-second arming (mode `0x41`) from mode 0 | None; it has expired only in FEL sessions |

Every arming expires after boot0 has trained DRAM and written the PMIC.

The OS's stop parks the CPU and leaves the reset to WDT0, so even a correct
run returns only through an expiry.

## Two conditions lack a board measurement

- **An arming over an enabled WDT0.** After a watchdog reset WDT0 still reads
  the mode armed before the reset (card-fel-recovery step E). Before this
  change, the gate wrote mode `0xb1` over that retained `0x41`, and stage 2
  wrote `0x41` over the gate's `0xb1`, each without a disarm. Every expiry
  that the board has shown followed an arming from mode 0: the runner's
  `0x41` at each session reset and the rail-restore helper's `0xb1`. The gate
  and stage 2 now write mode 0 first; the gap is a few register writes.

  Two FEL-only runs on port 3-1 then wrote `0xb1` over a running `0x41`
  with no disarm, and both reset the board 16 seconds later plus the reset
  time: the runner's own writes (`1794643227744371298-6101ee0d`, 18.137 s)
  and the gate's warm path executed from SRAM
  (`1794643284421551583-789b13b5`, 18.169 s). A mode write over a counter
  that was armed from mode 0 in the same power epoch restarts it at the new
  interval. `REARM_MEASURED` in `tools/boot_chain.py` cites both runs. Two
  conditions of the 2026-11-13 run stay unmeasured and are listed in
  `REARM_OPEN`:

  | Condition | Where the old code met it |
  | --- | --- |
  | A re-arm over a mode that survived its own expiry reset | The gate's cold path after the runner's `0x41` reset |
  | Output and configuration writes over a running counter | The gate's cold path, before its mode write |

  The disarm-first code meets neither, so no stage depends on them. A stage
  that re-arms stays unproven, and its refusal names both conditions.
- **Expiry of mode `0x41` after boot0's PMIC writes.** boot0 trains DRAM and
  writes the AXP8191 rails DCDC2, DCDC4, DCDC6, DCDC7 and ELDO1 before any of
  these armings can expire, and no FEL-only run can reach that state. The
  port 3-3 replug record shows the gate's `0xb1` from mode 0 expiring there
  and returning the board to FEL (see "Records for the next miss"). The
  `0x41` arming that covers the OS has expired only in FEL sessions. The OS
  row stays open until a board record shows that expiry; the coordinator has
  recorded no acceptance.

## The model and its checks

`tools/boot_chain.py` holds the shared WDT0 model and the stage table:

- `armings()` classifies each keyed enabling write to WDT0 mode as an arming
  from mode 0 or as a re-arm over an enabled watchdog.
- `MEASURED` maps an arming, its mode and the state it expired in to the
  evidence entries of that expiry: two in FEL-launched sessions and one
  after boot0.
- `OPEN_RISKS` names the expiry after boot0's PMIC writes for armings that
  the board has not shown expire there, and `ACCEPTED` is empty. An
  acceptance grants a stage only an arming that the board has shown expire in
  a FEL session.
- `unproven()` returns one sentence per stage without a measured or accepted
  recovery. `boot_once_slot()` in `tools/board_images.py` refuses a
  boot-once run while it returns anything, except a measuring run that
  `measurable()` admits.

The table cannot drift from the code:

- `checks/card-fel-recovery.py` runs the gate's cold path from the boot-once
  state with WDT0 at `0x41`, `0xb1` and 1, and requires its arming to equal
  the boot0 and stage-2 rows.
- `checks/stage2-session.py` runs stage 2's linked entry from the gate's cold
  state and requires its arming to equal the OS row.
- `checks/card-boot-chain.py` covers the model, the cited evidence entries,
  the effect of an acceptance, the measuring rule and the runner's refusal.

## Measuring the OS row

The OS row can be measured only on the card, because its arming expires
after boot0. The diagnostic stage-2 build `a733-stage2-armed-stop` (CMake
definition `A733_STAGE2_STOP_ARMED`) runs the normal loader up to the OS
entry and then:

- in a boot-once boot (the gate record carries the ONCE flag `0x1000`), arms
  WDT0 `0x41` from mode 0 as the normal build does, writes LAST step `0x0f`
  and parks on `wfe` without entering the OS. The reset about 4 seconds later
  is the expiry of the OS row's arming after boot0;
- in any other cold boot, such as a power-up, finishes with status 8 and
  detail `0xb1` before arming and parks under the gate's 16-second `0xb1`,
  the arming that the replug record shows returning the board to FEL.

`--boot-once-measure os` admits a boot-once run for this build. It requires
`--approved-by coordinator`, and `measurable()` in `tools/boot_chain.py`
admits it only when the named stage is the last of the chain, is the one
stage without a recovery, and arms from mode 0 with a mode that has expired
in a FEL session. The pass is the gate's warm record `0x0c024041`, the marker
`0x16aa16aa`, the stage-2 tag `0x53320001`, LAST `0x0c030f00` and FAULT 0.
The wait drops the session term: 4 + 16 + 30 + 4 + 10 = 64 seconds from the
runner's arming. The runner cannot read which stage 2 the card holds, so the
run is valid only after the storage write that installs the diagnostic
build. A passing run is the board record that `MEASURED` needs for the OS
row; a later run can then enter the OS.

## Stage-2 progress words

Stage 2 writes its progress to the RTC general-purpose words
`0x07090100..0x0709011c` (`boot/a733/stage2/marks.h`). A WDT0 reset keeps
them and a power loss clears them, so the runner's first contact after a
stopped card boot reads where stage 2 stopped. Stage 2 writes no other RTC
word through them: GP2 (`0x07090108`) is cleared only by the gate-entry
code, and the cold-boot marker `0x070901f0` is never written. The stage-2
instruction models (`checks/standalone-boot-loader.py`,
`checks/remote-writers-model.py` and `checks/stage2-session.py`) allow an
RTC write only to those words other than GP2, through
`checks/stage2_marks.py`, and each has negative cases for GP2, the marker,
a word past `0x1c`, an unaligned address and a narrow write.

| Word | Offset | Content |
| --- | --- | --- |
| TAG | `0x100` | `0x53320001`, layout version 1, written first |
| LAST | `0x104` | `0x0c03SSDD`: the step reached in `SS`, a detail in `DD` |
| GP2 | `0x108` | The gate record; not written through these words |
| SNAPSHOT | `0x10c` | The SD admission snapshot, or the CPU owner's refusal |
| FAULT | `0x110` | `0x0c04PPSS` from a finish, or `0x0c0e00VV` from an exception |
| ESR | `0x114` | The finish detail, or the low half of `ESR_EL3` |
| ELR | `0x118` | The SD driver's last error, or the low half of `ELR_EL3` |
| FAR | `0x11c` | The SD controller's interrupt status, or the low half of `FAR_EL3` |

The steps, each written before the code it names runs:

| Step | Code | Detail |
| --- | --- | --- |
| `01` | Gate entry | 0 no gate record, 1 record consumed, 2 GP2 clear did not read back |
| `02` | Counter check | 0 |
| `03` | Remote-writer exclusion | 0 |
| `04` | CPU owner and cache clean | 0 |
| `05` | Card open | 0 |
| `06`, `07` | Slot A and slot B headers | 0 |
| `08` | Slot load | Slot index |
| `09` | Entry policy and entry state | Slot index |
| `0a` | Card stop | Slot index |
| `0b` | Session framing | 0 |
| `0c` | SID evidence | 0 |
| `0d` | WDT0 `0x41` arming started | 0 |
| `0e` | `0x41` written and restarted | 0 |
| `0f` | Armed, before the OS entry | 0 |

SD admission writes SNAPSHOT once, with boot0's state as it found it: the
SMHC0 module clock fields (bits 31, 26:24, 12:8 and 4:0), the PF bank
withstand mode (6:5), the bank selector (7), the detected bank level
(14:13), the SMHC0 bus gate and reset (15), PF6's pin function (30:27) and,
in 23:16, the check that refused it:

| Index | Refused check |
| --- | --- |
| 0 | None: admitted |
| 1 to 10 | Parent clock, interrupts masked, bus gate and reset, module clock, PF pin functions, PF pulls, PF bank voltage, command pending, card busy, internal DMA |
| 11 to 14 | PF6 writes, module clock writes, settle time, card absent |

A refusal by the CPU owner writes `0x0c05AAWW` to SNAPSHOT instead. `AA` is
1 once a power-policy write was made. `WW` names the step: 0 the entry
checks, 1 CLIDR, 2 the cache geometry, `0x10` plus the core number the
preflight, `0x20` plus the core number the static OFF write and wait,
`0x30` the final recheck and `0x40` the cache clean.

A finish writes FAULT `0x0c0400SS` with its status in `SS` before it stops
the card, and returns to the ROM's FEL entry at `0x20` when `PP` stays 0.
When the card does not stop, or a CPU power-policy write was made, stage 2
parks instead, and `PP` is 11 or 12. An exception writes FAULT with its
vector number and the three syndrome registers, then parks.
`tools/a733-crumbs.py` decodes the words when the tag is `0x53320001`.

## Records for the next miss

The runner reads all eight RTC general-purpose words
`0x07090100..0x0709011c` and the WDT0 interrupt status `0x02050004` at every
first contact, before any write ([board-runner.md](../board-runner.md)).
After a WDT0 reset, or a replug that keeps VDD_RTC above 0.7 V, GP2 then
shows whether stage 2 consumed the gate's cold record, and the stage-2
progress words show where it stopped.

The user's replug of 2026-11-13 23:48 did not keep VDD_RTC. Port 3-1's
first contact afterwards read the marker as 0 and WDT0 output at its reset
value `0x1f`. Port 3-3's first contact
(`1794643114289036712-ab8b9952`) read:

- GP2 `0x0c0240b1`: a warm record, with WDT0 mode `0xb1` at gate entry and no
  cold flag;
- the marker `0x16aa16aa` and the other seven RTC words 0;
- WDT0 status 1, configuration 1 and output 2.

WDT0 resets at power-up, so the gate armed `0xb1` after the replug. Only
its cold path does that, so the marker read 0 and the card cold-booted.
Stage 2 then cleared the cold record. It never armed `0x41`, because the
reset found mode `0xb1`. The gate's 16-second arming, made from mode 0,
expired after boot0's PMIC writes and returned the board to FEL. The kernel
log shows one enumeration of port 3-3, at 23:48:47, 27 seconds after port
3-1's. This is the first board record of an expiry after boot0. On
2026-11-14 the coordinator added it to `MEASURED` as the arming of mode
`0xb1` from mode 0 expiring after boot0, which grants the boot0 and stage-2
rows.
