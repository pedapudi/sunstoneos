# Every card boot stage needs a measured watchdog recovery

**Status: failed on the board.** The port 3-3 card holds the disarm-first
first stage and the diagnostic stage 2 (writes D1 and D2 of 2026-11-14). The
boot-once run that measures the OS row then did not return (see "Card boots
of this first stage"). No further card boot runs until the cause is known
from data; the FEL-only run in "Separating stage 2's arming from boot0" is
implemented and not yet run on the board. The boot0 and stage-2 rows have a board record of their recovery
after a power-up; the OS row has none.

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

## Card boots of this first stage

Three card boots have started on port 3-3 since the first stage was
installed. The boots that started from a WDT0 expiry reset did not return to
FEL; the boot that started from a power-up did.

| Boot | Start of the cold path | WDT0 arming before boot0 | Stage 2 | Returned to FEL |
| --- | --- | --- | --- | --- |
| Boot-once, 2026-11-13 (`1794619098327285673-8466a823`) | The runner's `0x41` expiry reset, boot-once record | The old gate: `0xb1` written over the retained `0x41` | Normal build | No |
| Replug, 2026-11-13 (`1794643114289036712-ab8b9952`) | Power-up | The old gate: `0xb1` from the power-up state | Normal build, no boot-once record, never arms `0x41` | Yes, through the `0xb1` expiry |
| Boot-once with `--boot-once-measure os`, 2026-11-14 (`1794675338482809309-27c7ed0f`) | The runner's `0x41` expiry reset, boot-once record | The disarm-first gate: mode 0, then `0xb1` | Armed-stop build | No |

The run of 2026-11-14 wrote the boot-once value, read it back with the
marker set, and armed WDT0 `0x41`; the board left USB at that reset and no
FEL device appeared within the 64-second deadline or 71 seconds after the
reset. No word was read after the reset, so the step stage 2 reached is
unknown. Neither stage 2's `0x41` nor the gate's `0xb1` reset the board.
Because the gate now writes mode 0 before arming, the re-arm over a retained
mode no longer separates the failed boots from the boot that returned; the
start of the cold path does. Every FEL-only expiry after an expiry reset has
reset the board, including the disarm-first gate's warm path on port 3-1
(`1794675168534838087-526c0fd2`, 18.158 s); none of them ran boot0.

Before that run, the first contact read WDT0 configuration `0x101`, where
the read-only capture before the writes (`1794675109428657635-3a8d28e4`) read
configuration 1. The runner wrote `0x101` itself: it arms the rail helper
with configuration `0x101` (the DCXO/750 clock, `tools/board.py`, `rails`),
and its disarm writes only the mode. The two runs that wrote the card ran that
helper last; the capture ran no helper. The boot-once run of 2026-11-13 and
the FEL-only run on port 3-1 also read `0x101` at first contact, and the
port 3-1 run returned. Each arming writes configuration 1 and reads it back,
and the gate's cold path writes it again, so all three card boots counted
WDT0 on the RTC 32 kHz clock.

## Separating stage 2's arming from boot0

boot0 takes the same path after a WDT0 expiry reset as after a power-up
([boot0 and a watchdog reset](../vendor-firmware/boot-rom-and-boot0.md#boot0-takes-the-same-path-after-a-watchdog-reset-as-after-a-power-up)).
It never accesses WDT0. The words it reads match across the card boots on
port 3-3:

- GP2 holds a gate record in every boot, which matches neither of boot0's
  two values;
- the DCXO word reads `0x983fb0f7` at every contact on record;
- the PMIC settings are the same on every boot.

`LOSC_CTRL` bit 0 had not been recorded in any of these epochs. Only the RTC
application selects the crystal; the gate, stage 2 and the rail helper do not
write `LOSC_CTRL`. boot0 therefore most likely left the RTC on its RC
oscillator in every boot.

| Boot | Port | Epoch started by | boot0 ran | Arming live when the reset was due | Written | Returned |
| --- | --- | --- | --- | --- | --- | --- |
| Boot-once, 2026-11-13 | 3-3 | `0x41` expiry | Yes | Stage 2's `0x41` over the gate's counting `0xb1`, no disarm | After boot0 | No |
| Replug, 2026-11-13 | 3-3 | Power-up | Yes | The gate's `0xb1` | Before boot0 | Yes |
| Runner re-arm (`1794643227744371298-6101ee0d`) | 3-1 | FEL session | No | `0xb1` over a counting `0x41`, no disarm | FEL | Yes, 18.137 s |
| Gate warm path (`1794675168534838087-526c0fd2`) | 3-1 | `0x41` expiry | No | The gate's `0xb1` after a disarm | FEL | Yes, 18.158 s |
| Boot-once with `--boot-once-measure os`, 2026-11-14 | 3-3 | `0x41` expiry | Yes | Stage 2's `0x41` after it disarmed the gate's counting `0xb1` | After boot0 | No |

In both boots that did not return, stage 2 wrote the arming that was live
when the reset was due, after boot0 had run in an epoch started by an expiry.
Every return had its live arming written before boot0, or ran no boot0. The
records leave two causes open:

- **S:** on port 3-3, stage 2's arming sequence over the gate's counting
  arming does not expire. All FEL-only re-arm measurements were made on
  port 3-1.
- **B:** state that boot0 leaves in an epoch started by an expiry stops a later
  arming. The only candidate in boot0 is its `LOSC_CTRL` store, and that store
  can change the clock only when bit 0 reads set.

A reset that fires but never reaches FEL would need the gate's refusal path:
SCTLR with the MMU or data cache enabled, or a nonzero ROM redirect word. That
path parks the CPU. Neither boot0 nor stage 2 writes a redirect word, and
every first contact now records them.

### The FEL-only run that separates S from B

`--rearm-expiry stage2` on port 3-3 ([board runner](../board-runner.md#wdt0-re-arm-expiry-test))
makes no card write and no clock-source write. It arms WDT0 only in modes and
configurations the runner already uses:

1. The first contact reads the RTC words, WDT0, `0x07090160`, `LOSC_CTRL`,
   `LOSC_AUTO_SWT_STA` and the redirect words. The runner refuses the run
   before any WDT0 write if `LOSC_CTRL` bit 0 reads set or a redirect word is
   nonzero.
2. The runner's `0x41` expiry resets the board through the gate's warm path,
   which starts an epoch after an expiry as the boot-once run did.
3. The replay makes the gate's cold-path arming. Three seconds later it makes
   stage 2's `arm()` accesses. The runner then waits for the `0x41` reset.

Prediction: the contact reads `LOSC_CTRL` `0x401c` (the reset value `0x4010`
with boot0's gain bits), status 0 and zero redirect words. The replay reports
status 1 (armed). The board resets about 4.5 s after the second execute
request. That figure is the 4-second interval at the RC rate measured on port
3-1, where `0xb1` took 18.14 to 18.17 s for 16 s nominal, a factor of 1.135.

| Observation | Conclusion | Next step |
| --- | --- | --- |
| Contact reads `LOSC_CTRL` bit 0 set, or a redirect word nonzero | B is possible (boot0's keyed store switches the WDT0 clock source), or the gate could refuse | The runner refuses before step 2; revise the run |
| Replay status other than 1 | The replayed `arm()` refused at the recorded access | Compare the recorded values with stage 2's admission |
| No reset within 14 s of the stage-2 arming; the runner disarms WDT0 on the same instance | S holds on port 3-3 and boot0 is not required | Change stage 2's arming; test the change FEL-only first |
| Reset 4 to 9 s after the stage-2 arming | S is refuted on port 3-3; the failure needs boot0's run | Card boot whose stage 2 leaves the gate's `0xb1` alone, proposed only after the contact rules out B |

Time budget, worked from the measured and nominal figures:

| Stage | Seconds | Source |
| --- | --- | --- |
| First contact | 1 | One read-only transaction |
| Runner `0x41` to the warm-path FEL instance | 4.5 + 2 | RC rate; gate return and enumeration as in the card writes of 2026-11-14 |
| Second contact, replay upload and gate arming | 1 | Three transactions |
| Gap before the stage-2 arming | 3 | `REARM_STAGE2_GAP_S`, shorter than the gate's 18 s `0xb1` |
| Stage-2 `0x41` to the next FEL instance | 4.5 + 2 | RC rate; gate return |
| Return report | 1 | One transaction |
| Total | 19 | |

The step-2 wait ends at 14 s, the stage-3 wait at 14 s after the stage-2
arming (interval plus 10 s), and the gate's `0xb1`, live during the gap,
expires about 18 s after the gate arming. A `0x41` that never fires is
therefore caught at 14 s while the board is still in FEL. No step runs boot0;
each reset passes through the gate's warm path to FEL, which returned on port
3-3 in the card writes of 2026-11-14.

