# Every card boot stage needs a measured watchdog recovery

**Status: host-tested.** The runner refuses every boot-once run, because no
stage after the card first stage has a measured recovery. The disarm before
each re-arm in the first stage and stage 2 is source and host checks only; it
reaches the card only with a coordinator-approved storage write.

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

| Stage | Covered by | Expires after boot0's PMIC writes |
| --- | --- | --- |
| boot0: DRAM training, PMIC writes, package load, monitor entry | The gate's 16-second arming (mode `0xb1`) | Yes |
| Stage 2: card open, slot headers and slot load | The same arming, restarted after each card read | Yes |
| The OS, from its entry to its stop | Stage 2's 4-second arming (mode `0x41`) | Yes |

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
- **Expiry after boot0's PMIC writes.** boot0 trains DRAM and writes the
  AXP8191 rails DCDC2, DCDC4, DCDC6, DCDC7 and ELDO1 before any of these
  armings can expire. No board run has let WDT0 expire in that state, and no
  FEL-only run can reach it. Vendor U-Boot reboots through WDT0 after the
  same boot0, which is a reference, not a measurement. This risk stays open
  until a board record shows the expiry or the coordinator records an
  acceptance.

## The model and its checks

`tools/boot_chain.py` holds the shared WDT0 model and the stage table:

- `armings()` classifies each keyed enabling write to WDT0 mode as an arming
  from mode 0 or as a re-arm over an enabled watchdog.
- `MEASURED` lists the armings the board has shown expire in a FEL-launched
  session, each with its evidence entry.
- `OPEN_RISKS` names the expiry after boot0's PMIC writes, and `ACCEPTED`
  is empty. An acceptance grants a stage only an arming that the board has
  shown expire in a FEL session.
- `unproven()` returns one sentence per stage without a measured or accepted
  recovery. `boot_once_slot()` in `tools/board_images.py` refuses a
  boot-once run while it returns anything.

The table cannot drift from the code:

- `checks/card-fel-recovery.py` runs the gate's cold path from the boot-once
  state with WDT0 at `0x41`, `0xb1` and 1, and requires its arming to equal
  the boot0 and stage-2 rows.
- `checks/stage2-session.py` runs stage 2's linked entry from the gate's cold
  state and requires its arming to equal the OS row.
- `checks/card-boot-chain.py` covers the model, the cited evidence entries,
  the effect of an acceptance, and the runner's refusal.

## Records for the next miss

The runner reads all eight RTC general-purpose words
`0x07090100..0x0709011c` and the WDT0 interrupt status `0x02050004` at every
first contact, before any write ([board-runner.md](../board-runner.md)).
After a replug that keeps VDD_RTC above 0.7 V, GP2 then shows whether stage 2
consumed the gate's cold record, and the LAST word shows whether the OS ran.
Stage 2 keeps its progress in DRAM, which a power loss erases; a stage-2
progress word in the RTC domain needs a new package on the card.
