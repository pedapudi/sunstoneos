# Design: adopting UHS-I SDR104 in the OS (stage 4)

Status: design only. No adoption code is written. It builds on [the recovery design](sd-uhs-recovery.md), whose board results are:

- **3-3 card (Phison SD32G, 2020):** re-entered at 1.8 V after a watchdog reset by moving the bank to 1.8 V first (CMD0, CMD8, ACMD41, no CMD11); S18A is
  **clear** at 1.8 V; the CMD6 group 1 support field read at 1.8 V is `0x801f` (SDR12 to SDR104 and DDR50); SDR25 reads at 50 MHz, 21.4 MB/s per request;
  the close restores the bank to 3.3 V and the lifetime returns normally. The first SDR104 trial stopped in the core before the card was identified at SDR104 (see the recovery note);
  SDR104 is therefore not yet shown to work on this board, and the speeds below are what the card supports, not what the OS achieves.
- **3-1 card (Phison SD64G, 2026):** seen at 3.3 V only. It answers S18A **set** at 3.3 V. Its 1.8 V behaviour is expected to match 3-3's, and is unmeasured.
- **After a watchdog reset the board returns to FEL with the card at 1.8 V** (lifetimes 1 and 2 on 3-3 ended that way, with the card at 1.8 V at the time).

Two facts about the platform are used below. The platform baseline requires the PF bank at 3.3 V and adaptive mode at the start of a lifetime; the lifetimes that
began after a normal or a failed close all met it, but none began after a reset that left the bank at 1.8 V, so whether a watchdog reset restores the bank
selector is unmeasured. And the card cannot be returned to 3.3 V by software on these boards (the card supply is not separately switchable, and the rail
documents disagree about DC1SW2), so a card switched to 1.8 V stays there until board power is removed.

## 1. The session-start resolution algorithm

One algorithm for every session that touches the card, for the cases of both boards. It is the algorithm of the recovery note with the measured answers
filled in, and with the one case it could not settle marked for a measurement.

The state the algorithm must handle at the start, by what a previous session may have left:

| Card | Bank (selector) | How it arises | Example |
| --- | --- | --- | --- |
| 3.3 V | 3.3 V | after power removal, or a session that never switched | 3-1 today |
| 1.8 V | 3.3 V | after a lifetime that switched and closed, or after a reset that restores the selector | 3-3 now |
| 1.8 V | 1.8 V | after a reset that leaves the selector (unmeasured) | not yet seen |
| mid-switch or in transfer | either | a reset during the switch or a read | a cut |

Steps:

1. **Read the bank.** Selector and `GPIO_POW_VAL` agree on 3.3 V (the platform baseline today): go to step 2. On 1.8 V (the unmeasured case): go to step 4 with the bank
   already there.
2. **Probe at 3.3 V.** Clock stopped, then CMD0, CMD8 at 400 kHz.
   - **An R7 answer:** the card is at 3.3 V, or it is at 1.8 V and a 3.3 V host can read it (the unknown K1 of the recovery note). ACMD41 with S18R then decides: **S18A set means
     the card signals 3.3 V** (3-1: measured), so the normal switch follows (CMD11 and the 10 ms sequence). **S18A clear** is a card that declines UHS or one already at 1.8 V: for a card
     that the OS itself switched, the OS knows (below); otherwise treat it as a card that declines and stay at 3.3 V high speed, because sending nothing is safe and CMD11 would be
     refused by a 1.8 V card anyway.
   - **No answer:** the card is at 1.8 V, absent or hung; go to step 4.
3. **(the normal switch)** as in the recovery note; on any failure the next session starts at step 1 and the table above gives the state.
4. **Move the bank to 1.8 V** with the clock stopped, wait for `GPIO_POW_VAL`, hold the clock off 10 ms (the platform's `ENTER_1V8`), then CMD0, CMD8, ACMD41 with S18R, ignoring S18A (3-3: clear
   at 1.8 V), CMD2 onward (the core's re-entry mode). No CMD11. Silence here, with the bank returned to 3.3 V, is the one terminal outcome: card absent or hung, needing power removal.
5. **Speed.** At 1.8 V the CMD6 group 1 support field is meaningful: select SDR104 when `0x8` is set and the tuning sweep finds a window of at least 12 delays, otherwise SDR25 at 50 MHz.

**What the cards answered, from the recorded entries (`docs/evidence/storage.jsonl`).**

| Card | Host side | Recorded answer | Entry |
| --- | --- | --- | --- |
| 3-1, Phison SD64G, never switched | 3.3 V bank, CMD0, CMD8, ACMD41 with S18R | R7 answered; **S18A set** (OCR `0xc1ff8000`); CMD6 group 1 support `0x8003` (functions 0 and 1; functions 2 to 4 read as 0 at 3.3 V) | `1793639891731890732-2700d966` |
| 3-3, Phison SD32G, at 3.3 V (before its first switch) | 3.3 V bank | identified and switched to 1.8 V; S18A was set (the switch followed) but the entry did not keep the OCR | `1793648082135831049-05f70d50` (lifetime 1) |
| 3-3, card at 1.8 V | **1.8 V bank**, CMD0, CMD8, ACMD41 with S18R, no CMD11 | R7 and ACMD41 answered; **S18A clear**; CMD6 group 1 support `0x801f` (SDR12 to SDR104 and DDR50) | `1793649204965774597-d1ed3e86`, `1793649661736969334-9742cb48`, `1793650532228649180-1fb49397` and the experiment `1793652351260245353-286b63df` |
| 3-3, card at 1.8 V | **3.3 V bank** | CMD0, CMD8 and ACMD41 answered (OCR `0xc0ff8000`: ready, CCS, **S18A clear**); identified at 3.3 V high speed, 32 reads at 21.4 MB/s, CRCs equal to the earlier runs; **CMD6 group 1 support `0x801f`** (SDR12 to SDR104 and DDR50) | `1793663873502474507-76c206d4` |

So a 3.3 V host does read a card at 1.8 V (the run on 3-3 measured it), and the card answers S18A **clear**, exactly as it does to a 1.8 V host; S18A alone cannot tell a card at 1.8 V from a card that declines UHS. What does tell them apart is
the CMD6 group 1 support field: a card at 3.3 V reports functions 2 to 4 as 0 (3-1: `0x8003`, and the specification's Table 4-11 text), a card at 1.8 V reports them (3-3: `0x801f`, read through a 3.3 V host and through a 1.8 V host alike).
The rule these records support, with no 1.8 V probe: **at 3.3 V, S18A set means switch with CMD11; S18A clear with SDR50, SDR104 or DDR50 in the group 1 support field means the card is already at 1.8 V, so move the bank to 1.8 V and re-enter;
S18A clear with only functions 0 and 1 is a card without UHS-I and stays at 3.3 V high speed; no answer to CMD8 at 3.3 V means re-enter at 1.8 V** (not observed from this card, kept because a different card could be silent and the ROM
treatment of a silent card is the same). The re-entry costs one more identification (about 0.3 s) once per power cycle.

**The measurement that settled step 2 (done; its design follows).** Whether a 3.3 V host reads a 1.8 V card is unmeasured. One read-only run on 3-3 with the card at 1.8 V and the bank at 3.3 V (the existing mode 0: CMD0,
CMD8, ACMD41 with S18R, no CMD11) answers it: an answer, with its S18A value, or a stop at identification. If the card is silent at 3.3 V (K1 false), step 2 needs no discriminator: silence means 1.8 V
or absent, and the OS goes straight to step 4. If it answers with S18A clear, the discriminator above applies. The run is safe (it only reads and never sends CMD11); it is not yet submitted.

**A marker.** A byte that survives a watchdog reset and is lost with board power would let the OS skip step 2: "the card was switched". The vendor boot flow uses the AXP8191 `BUFFER0`
(`0x04`) for the reboot mode, so it is not taken; the RTC words after the breadcrumb block are not cleared by a reset but the runner clears the breadcrumb block at each launch, and whether the
words beyond it survive a power removal is unmeasured. The algorithm does not depend on a marker: it only saves the probe when the marker is trusted, and is wrong never, because step 4's
first command is CMD0 at 1.8 V.

## 2. When the OS switches

- **At session start, once, and never back.** The switch (CMD11 and the PF selector) happens in the first identification of a session whose card is at 3.3 V and reports S18A; after it the OS
  stays at 1.8 V for the card's life (until board power is removed). A running session never changes the signalling level.
- **Which images.** An image that is loaded over FEL (development and the current OS images) takes SDR104 by default once the trials pass, behind a build or session setting so that a
  session can stay at high speed. An image that boots from the card (boot0 and the stage-2 loader read the package at 3.3 V) must never leave the card at 1.8 V: it stays at 3.3 V high speed
  and never sends CMD11. The two cannot share a card without a supply switch, so a card that has been used by an SDR104 session needs power removal before an autonomous boot.
- **What the large loader gains.** It reads at 21.7 MB/s now (6.0 ms per 128 KiB); a loader at 150 MHz is bounded by 75 MB/s raw and the card. The gain is the trial's sustained rate over 21.7.
- **Clock and approvals.** The runner's policy treats new clock points as needing the coordinator's approval; 150 MHz on the SD module is one, and the first OS adoption needs it.

## 3. What a watchdog reset leaves behind

| Left behind | Consequence | Handling |
| --- | --- | --- |
| The card at 1.8 V, in any state: idle, ready, transfer, SDR104 timing, mid-read | CMD0 at 1.8 V returns it to idle and to SDR12 timing (specification 3.9.4); it stays at 1.8 V | step 4 |
| The PF selector: 3.3 V if the reset restores it (the expected reset value), 1.8 V if not (unmeasured) | the baseline must accept either | step 1 reads it; the platform baseline gains the 1.8 V case |
| The SD controller, module clock and bus: reset by the watchdog | none | the platform's own setup |
| A read in flight | the card may still be sending data | CMD0 ends it; the next identification does not depend on it |
| The ROM | returns to FEL without trouble with the card at 1.8 V (measured on 3-3) | none |
| A card with a stuck switch (CMD11 answered, clock stopped, bank not moved) | holds CMD and DAT low until the clock stops | the pending-switch row of the recovery note's table |

The card is never left in a state that needs a human, except the terminal case of step 4, which needs two failures at once (a card that refuses CMD0 at 1.8 V and no response at 3.3 V).

## 4. The write path (CMD25) at SDR104

Nothing about writes at 1.8 V has been run. What is known and what is needed:

- **Known (code).** The core programs the read threshold for SDR104 reads (`THLD` block size and bit 0) and the write threshold for CMD25 (`THLD` block size and bit 2) per command, so the two do not
  conflict. The native adapter admits a `THLD` write in the UHS-I profile, and CMD25 only for a verified bounds object in a writer build (`A733_MMC_BOUNDED_WRITER`); the two have not been combined in one
  host model. The write path's DAT0 busy timer and programming-status check do not depend on the bus speed.
- **Derived: the drive and sample phases of every mode.** The board kernel sets them per clock range in `sunxi_mmc_set_clk_dly` (`bsp/drivers/mmc/sunxi-mmc-v5p3x.c:161-290`) from the defaults of
  `sunxi_mmc_clk_dly_v5p3x` (`:820-882`), which a device-tree node can override with `sunxi-dly-<range>`. The board's `&sdc0` node (`sun60i-a733-orangepi-zero3w.dts`, `sdc0`) sets none, so the defaults apply.
  The command drive phase is bit 16 and the data drive phase bit 17 of `0x140` (`:82-83`), set for 180 degrees and clear for 90; the sample phases are bits 5:4 (command) and 9:8 (data) of `0x5c` (`:116-117`).
  The sample-delay and delay-chain registers (`SAMP_DL`, `DS_DL`) are not written by the vendor's per-range code (the lines are commented out, `:260-270`); the tuning writes `SAMP_DL` through `sunxi_mmc_set_samp_dl_raw`.

  | Range (clock) | Our modes | Command drive | Data drive | Data sample | Command sample |
  | --- | --- | --- | --- | --- | --- |
  | 400 kHz | identification | 180 | 90 | 0 | 0 |
  | up to 26 MHz | default speed, SDR12 | 180 | 90 | 0 | 0 |
  | up to 52 MHz | high speed, SDR25 | 180 | 180 | 1 | 1 |
  | up to 104 MHz | SDR50 | 180 | 90 | 0 | 0 (tuned delay) |
  | up to 208 MHz | SDR104 | 180 | 90 | 0 | 0 (tuned delay) |

  The same registers serve reads and writes: there is no separate write-side phase. For a write the data drive phase is what matters (the host-to-card data), and the CRC status the card returns is sampled with the same sample
  setting as read data, so the tuned delay applies to it. **A difference found by this derivation:** the SDR104 setting in the core up to `e6dfa2a6` drove data at 180 degrees (`3 << 16`), and so did the 400 kHz setup;
  the vendor drives data at 90 for 400 kHz, 26 MHz, 104 MHz and 208 MHz, and at 180 only for the 52 MHz range. Reads do not depend on the data drive phase, which is why the SDR104 reads worked, but a write does.
  The core now writes the vendor's values (`uhs_registers`, the cold setup) and `checks/sunxi-mmc-uhs.py` pins the table above by parsing the pinned vendor source and node, and `checks/sunxi-mmc-uhs.c` compares every mode's register
  values with it, so a change to either fails the check. Nothing is unresolved in the sources for the write at 150 MHz; the vendor's tuning of the write path (`execute_tuning` covers read data only) is the same.
- **Qualification path.** (1) Extend `checks/sunxi-mmc-write-dma.c` to a 1.8 V card at SDR25 and then SDR104 with the sample-delay and drive-phase registers modeled from the vendor driver; (2) read the
  vendor `sunxi-mmc-v5p3x.c` for the write-side timing registers at SDR104 and record the register sequence the way `docs/designs/mmc-multiblock-write.md` does for 3.3 V; (3) a board write session at
  SDR25 on 1.8 V under the per-session backup procedure; (4) only then SDR104 writes, in the same way. Every board write session keeps the existing blob, backup and approval-record rules.

## Work items for adoption (none started)

1. Platform: accept the bank at 1.8 V in the baseline; a runtime SDR104-to-SDR25 mode change; the OS owner (`apps/os/large_sd.c`) using the UHS-I profile with `start_1v8` decided by step 1.
2. The resolution algorithm in the platform and the owner, with its host models (the recovery model already runs the table).
3. The measurement of step 2 (mode 0 on 3-3), then the same set of runs on 3-1, which has never been at 1.8 V.
4. The write-path qualification above.
5. The runner: approval of the 150 MHz point and of CMD11 for OS images, and the image classification for UHS-I.

## Prediction for the measurement of a 3.3 V host in front of a card at 1.8 V (written before the run)

Run: the read-only probe in mode 0 on 3-3 (`--uhs-mode 0`, 32 reads of 128 KiB from sector 0), image built from `work/sd-adopt-1102` at this commit. The card is at 1.8 V from the earlier lifetimes (it has not lost power) and the bank is at 3.3 V (the
last close restored it), the state the boot ROM meets on every reset. The probe sends CMD0, CMD8 and ACMD41 with S18R and never CMD11. A silent card is a stop at identification and the lifetime ends in a watchdog reset, as the stops of the sweep
trials did; the board returns to FEL by itself. The run does not change the card's state. It is also the first board run of the 400 kHz setup with the vendor's data drive phase (90 degrees, bit 17 clear; the core drove 180 before).

| Outcome | Expected | What it decides |
| --- | --- | --- |
| The card is silent at CMD8 | most likely (about two in three): a card whose signalling is 1.8 V puts 1.8 V high levels on CMD, below a 3.3 V host's input threshold (0.625 of 3.3 V = 2.06 V) | stop word reason 3, core protocol 1 (CMD8), core error 6 (controller), flags: response timeout set and command done clear. Rule: silence at 3.3 V means 1.8 V or absent, so re-enter at 1.8 V (no S18A-clear case exists) |
| CMD8 answers, ACMD41 answers with S18A clear | about one in four | identification completes at 3.3 V high speed, 32 reads at about 21 MB/s, OCR bit 24 clear, normal return. Rule: S18A clear at 3.3 V cannot tell a non-UHS card from a card at 1.8 V, so the 1.8 V probe of this note is needed |
| Garbled answers (CRC errors, partial) | unlikely (about one in ten) | stop at identification with response CRC error; treated like silence |

Not predicted to happen: S18A set from a card at 1.8 V; any change of the card's state; a lifetime that cannot return.

### Result (entry `1793663873502474507-76c206d4`, 3-3, image `15c9d9cd`)

The second outcome of the prediction: CMD8 and ACMD41 answered, OCR `0xc0ff8000` (S18A clear), identification at 3.3 V high speed took 0.299 s, 32 reads of 128 KiB ran at 21.43 MB/s median (the first eight slow, as at 1.8 V), the CRCs match, the bank stayed at 3.3 V,
the card stayed at 1.8 V, and the lifetime returned normally. The CMD6 support field was `0x801f`. This was also the first board run of the 400 kHz setup with data drive 90 degrees: identification and reads are unchanged. A card at 1.8 V read by a 3.3 V host is
out of its signalling specification and happened to work here; the OS therefore re-enters at 1.8 V as soon as the rule above says the card is there, and does not read it at 3.3 V.

## Prediction for the forced step-down restart on 3-3 (written before the run)

Run: the read-only probe in mode 4 on 3-3: re-entry at 1.8 V and SDR104 as in the earlier runs, then four phases of 32 reads of 128 KiB from 4 MiB (the region that was always fast), with the platform's restart between them, as the recovery policy does after
errors: before phase 1 the same ceiling (a new tuning), before phase 2 the ceiling SDR50, before phase 3 the ceiling SDR25. Read only; the card is at 1.8 V already and the bank is at 3.3 V, as in the previous lifetime. The restart has never run on a board; it is the
first board run of the SDR50 mode and of the corrected data drive phase (90 degrees) in the tuned modes, for reads.

| Item | Prediction |
| --- | --- |
| Identification | re-entry at 1.8 V, S18A clear, SDR104 at 150 MHz; a sweep window of 40 to 50 delays starting near 17; chosen delay 40 plus or minus 6 |
| Phase 0, SDR104 | median 2.4 to 2.8 ms per request (47 to 52 MB/s); no request over 5 ms in this region apart from the first few of the lifetime |
| Restart 1, same ceiling | no CMD11, no PF write, bank stays at 1.8 V; SDR104 again; a new window within 8 delays of the first width; mode steps 1 (reason retune); phase 1 as phase 0 within 10 percent |
| Restart 2, ceiling SDR50 | the card reports SDR50 (group 1 support `0x801f`), so outcome SDR50 at 100 MHz with a window of at least 12 delays (most likely 30 to 50); median 3.3 to 4.2 ms (31 to 40 MB/s); mode steps 2 |
| Restart 3, ceiling SDR25 | 50 MHz without tuning; median 6.0 to 6.2 ms (21.4 MB/s, as every earlier 50 MHz run); mode steps 3 |
| Whole run | 128 reads, no error; the log holds (3 to 3 retune), (3 to 2 errors), (2 to 1 errors); normal close with the bank back at 3.3 V; normal return |
| Risk | the restart is new on hardware. About one chance in three that it fails the first time. A stop reads: reason 17 (the platform refused the restart), or reason 6 or 7 with a core protocol in the identification steps (the cold setup after the restart did not accept the controller), or no stop word and a watchdog reset (a wait in the restart that never ended: reset bits, clock update or module change) |

A clear difference between phase 0 and phase 1 at the same ceiling would mean that the restart leaves the controller in a different state from a cold start. SDR50 slower than SDR25 would point at its phases or its sample delay.

### First submission of the restart probe (entry `1793666413836584828-d698c21f`): stopped at the first restart

Stop word `0x48873f00`, site 17: the platform faulted at the register write of `core_write` (source line 290 of `a733_mmc_platform.c`, which refuses a write when the core guard, the IDMAC value rule, the chain walk or the adapter denies it) while identifying
again after the restart before phase 1, in platform stage IDENTIFY; the card was at 1.8 V and reported group 1 support `0x1f`. The word did not say which register. The host checks that exercise the restart (the platform model, and the lifetimes model with every write
offered to the real adapter filter) pass, and the IDMAC value rule accepts every write of the restart for a configuration with and without a descriptor area; what differs on the board is that this probe configures descriptor DMA, which the host models of the restart do not.
The next submission is the same run with the stop word extended for site 17: the first register the adapter refused, the core's setup step and its restart step, so a second stop names the access (apps/sd-backup-probe/input.py, `stop` with site 17).

### Second submission (entry `1793666839062917650-d5d01b71`): the cause

Stop word `0x48872607`, site 17: the first register refused was SMHC offset `0x18`, the command register, in the core's identification (setup step 6). The native adapter's CMD6 rule (`drivers/storage/a733_mmc_platform_native.c`) admitted function 1 and, in the UHS-I profile,
function 3 only; the restart under the SDR50 ceiling sends the function 2 switch (`0x00fffff2`, `0x80fffff2`), which the adapter refused. That is the restart of phase 2; the report of the first run, which had the same stop, is lost, so its stop at the same place is consistent with this
and not proved. The host models did not cover it because their restarts used the ceiling SDR104 only, and no check had put a card with SDR50 through the adapter filter. The adapter now admits the function 2 switch in the UHS-I profile, and the lifetimes model walks a card with SDR50
through the ceilings SDR104, SDR50 and SDR25 with every write offered to the real filter: it fails with the old rule (the restart under SDR50 is refused) and passes with the new one. The prediction above is unchanged.

### Result of the third submission (entry `1793667245855934208-55d19b3c`, 3-3, image `ea572001`)

The run completed: 128 reads without error, the lifetime closed with the bank back at 3.3 V, and the board returned normally. Every restart re-entered at 1.8 V with the bank untouched and reached READY. The first run to pass; the first two stopped at the function 2 switch (above).

| Phase | Mode | Window | Delay | Mean per 128 KiB request | Predicted | Mode steps |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | SDR104, 150 MHz | 16 to 63 (48) | 40 | 2.445 ms (53.6 MB/s) | 2.4 to 2.8 ms: met | 0 |
| 1, same ceiling (new tuning) | SDR104, 150 MHz | 16 to 63 (48) | 40 | 2.445 ms | within 10 percent of phase 0: met | 1 (retune) |
| 2, ceiling SDR50 | SDR50, 100 MHz | 0 to 63 (all 64 delays pass) | 32 | 3.342 ms (39.2 MB/s) | 3.3 to 4.2 ms: met, at the fast end | 2 |
| 3, ceiling SDR25 | SDR25, 50 MHz | none (not tuned) | 0 | 6.059 ms (21.6 MB/s) | 6.0 to 6.2 ms: met | 3 |

One prediction missed: the SDR50 window was predicted at 30 to 50 delays and every one of the 64 passed, so the sample point is not constrained at 100 MHz on this card. No request in the 4 MiB region was slow in any phase (the largest was 6.065 ms at SDR25), so the earlier slow
requests are not in this region, whatever the mode. This is the first board run of SDR50 and of the corrected data drive phase (90 degrees) in the tuned modes: both read correctly. The data drive phase for writes is still untested on a board.

## Prediction for the read-only switch to 1.8 V on 3-1 (written before the run; not submitted until the coordinator has seen it)

Run: the read-only probe in mode 5 on 3-1 (Phison SD64G, 2026, U3, V30), image built from `work/sd-adopt-1102`, with one phase of 64 reads of 128 KiB from 4 MiB. The card has only ever been at 3.3 V (S18A set, group 1 support `0x8003`). The probe identifies it at 3.3 V,
sends CMD11, moves the PF bank to 1.8 V, identifies at 1.8 V, runs the mode ladder with tuning, reads, and closes with the bank back at 3.3 V; no write. The card then stays at 1.8 V until board power is removed (as 3-3's does), which the OS handles by its rule
(S18A clear with SDR50 or SDR104 in the group 1 support field: close, open again with the bank at 1.8 V). Nothing in the run writes to the card.

| Item | Prediction |
| --- | --- |
| Identification at 3.3 V | S18A set (OCR bit 24), CMD11 accepted, DAT low during the switch and high within 10 ms of the restart, the bank reports 1.8 V; as on 3-3's lifetime 1 |
| CMD6 group 1 support at 1.8 V | `0x801f` (SDR12 to SDR104 and DDR50), as 3-3; at least bit 3 (SDR104) set. A card without SDR104 would show `0x8007` or `0x8003` and the ladder would end at SDR50 or SDR25 |
| Mode | SDR104 at 150 MHz with a tuning window of 30 to 60 delays (3-3: 48); the chosen delay near the middle of the window. If no window of 12 delays: SDR50 at 100 MHz, then SDR25 |
| Throughput | 128 KiB per request in 1.9 to 2.8 ms (47 to 68 MB/s); 64 reads without error; no slow requests expected in this region |
| Close | bank back at 3.3 V, clocks and pins restored, normal return; the card at 1.8 V afterwards |
| Risk | the card has never been at 1.8 V. About one chance in five that CMD11 or the sweep behaves unlike 3-3's card (a stop at the switch, or no window). Any stop returns the board to FEL by watchdog reset; the card stays at 3.3 V if CMD11 was not completed, or at 1.8 V if it was. No power cycle is needed for either |

## Prediction for the capture of 3-3's partition layout (written before the run)

Run: the read-only probe in mode 0 (3.3 V host, high speed) on 3-3 with the card at 1.8 V, capturing sector 0 and eight sectors at 2048, 8192 and 32768 (the usual partition starts), the first step of the write procedure (the host backup and a fresh capture). It reads the MBR and the boot sector at the start the MBR names;
predicted: sector 0 is an MBR with one FAT32 entry (type 0x0B or 0x0C) starting at 8192 or 32768 (the 3-1 card starts at 32768), the boot sector at that start has 512 bytes per sector, a power-of-two cluster size and two FATs, and the card reads as it did in the earlier mode 0 run (OCR `0xc0ff8000`, support `0x801f`, normal return, the card unchanged at 1.8 V).

### Result of the layout capture and the plan for the 3-3 write session (entry `1793677614855993008-f7a06712`)

The capture returned normally. 3-3's card: CID `2750485344333247120000029d014c47` (Phison SD32G, 2020), 61,067,264 sectors, MBR with one FAT32 entry (type 0x0c) at sector 2048 of 61,065,216 sectors; boot sector (`mkfs.fat`): 512 bytes per sector, 32 sectors per cluster (16 KiB), 32 reserved sectors, two FATs of 14,912 sectors,
root cluster 2, FSInfo at sector 1, label `32 GB DRIVE`; FSInfo: 1,907,354 free clusters, next-free hint 2 (an empty volume). So FAT1 starts at 2080, FAT2 at 16,992, the data area at 31,904 (cluster 2 is the root directory, sectors 31,904 to 31,935). The allocation unit is 4 MiB, so the file's data starts at the next AU boundary, sector 32,768 (cluster 29).

The plan, as the 3-1 plan is built (a 512 MiB region file written by mode 0), on this geometry: write ranges `[2049,1]` (FSInfo), `[2080,258]` and `[16992,258]` (the first 258 FAT sectors of each copy: the file's 32,768 clusters, 29 to 32,796, need FAT indexes 0 to 256), `[31904,64]` (the root cluster and the new directory's cluster) and `[32768,1048576]` (data, exempt from the backup); backup ranges `[0,1]`, `[2048,8]`, `[2080,258]`, `[16992,258]`, `[31904,64]`, 589 sectors in all, which two captures of at most 512 sectors cover. Two read-only capture runs of this card follow (A: sector 0, 2048 to 2055 and the first FAT's 258 sectors; B: the second FAT's 258 sectors and the 64 root and directory sectors); the same prediction as for the first capture applies: normal return, the card unchanged, FATs and root cluster as `mkfs.fat` left them.

## The first bounded write on 3-3 stopped (entry `1793683567666621650-3e3c6631`), and what the next session does

**What the record shows.** A watchdog reset. The failure record names `apps/os/sd_owner.c:90` on core 2 with value `0x80229122`: stage 34 (`hw_write_piece`: `drive_platform` returned something other than COMPLETE), platform result 4 (RETAINED), fault flag set, no deadline expired
(`fault_why` 0), no register refused by the adapter (`refused` 0), platform fault line 553 of `a733_mmc_platform.c`: the exit taken when the controller core is in FAULT. So the core faulted during a write at SDR104 and 150 MHz. The report was lost with the reset (DRAM is not kept), and the record did not carry
the core's error, its interrupt status, the piece or the count of pieces written. That is what the next run adds (below).

**The card afterwards** (the read-only captures of the restore-point sectors, A entry `1793683923163699289-3c296ae6` and B entry `1793683932704369296-a2880fff`, 589 sectors in all): 35 sectors differ from the restore point.
- FAT 1 sector 0 and FAT 2 sector 0: entry 1 went from `0x0fffffff` to `0x07ffffff` (the volume dirty flag, set when the session mounted for writing) and entry 3 is now `0x0fffffff`, the end-of-chain mark of cluster 3, the new directory `/sunstone-test`. The two copies are equal.
- Root cluster sector 31904: a directory entry `SUNSTO~1` (a directory, cluster 3) with its long-name entries, added to the root.
- Directory cluster 3, all 32 sectors (31936 to 31967): the `.` and `..` entries and zeros, the new directory's initial content.
- The boot area and FSInfo (1,907,354 free, hint 2) are unchanged; no FAT entry beyond cluster 3 is set.
So the session created the directory completely (FAT copies, root entry, directory cluster) and stopped in or before the first flush of the file, which was never published. The volume is dirty.

**What mode 0 does next.** It mounts for writing; the service recovers a dirty volume at mount (`fwr_mount`: it finds interrupted entries, completes or truncates them, corrects FSInfo and clears the flag; proven on 3-1). It then makes `/sunstone-test` (an existing directory is accepted) and opens the region file with `FWR_O_CREATE`, which **fails when the path
exists** (status 7, `E_OPEN`, detail the error). A file the failed session published would therefore stop the next mode 0; the capture shows no cluster chain beyond cluster 3 in FAT 1's first 7 entries, so the file was not published. The mode 0 expectation stays as it was (status 0, 536,870,912 bytes), and status 7 would mean a file existed.

**The write path against the vendor, register by register** (board kernel `sunxi-mmc-v5p3x.c`, `sunxi-mmc.c`, pinned in `checks/sunxi-mmc-uhs.py`):

| Item | Vendor | Ours | Same |
| --- | --- | --- | --- |
| Command and data drive phase at 150 MHz | the sdc0 node sets `execute_tuning_in_kernel`, and the in-kernel tuning leaves the host in the v4p6x method (`sunxi_mmc_execute_tuning` exits v5p3x and initialises v4p6x; nothing restores v5p3x), whose `sunxi_mmc_set_clk_dly` drives command and data at 180 for SDR50 and SDR104 (`mmc-sdc3/sunxi-mmc-v4p5x.c`, pinned). The v5p3x 104M and 208M rows (data 90) apply only until the tuning runs | command 180, data 180 in `uhs_registers` since the fix below; the first three write sessions and the format run drove data at 90 | **no, until the fix** |
| Sample phases, sample delay, timing mode | phases 0 and 0; the tuned `SAMP_DL` (set by `sunxi_mmc_set_samp_dl_raw`); old sample mode | the same, the delay from the sweep (41 on the read run) | yes |
| Write threshold | `WR_THLD_ENB` with the block size when `blksz <= tdtl` (the TX trigger level in bytes, 992 here) (`:647-681`) | block size 512 in 27:16, bit 2 set, bit 0 cleared (`command_step`, `P_WRITE_DMA`) | yes in effect: the vendor's read branch clears the read enable for a write |
| FIFO watermarks and burst size | `dma_tl` the same for read and write | `FTRGL` `0x200700f8` for both | yes |
| Descriptor chain | the same chain for both directions | the same 4 KiB chain, the platform walks it for the write direction | yes |
| Busy after a write | `card_busy` on DAT0 with a bounded wait after the data over | DAT0 busy timer, then CMD13 until the card is ready (`P_WRITE_DMA_STATUS`) | equivalent |
| Write-specific tuning or delay | none: `execute_tuning` samples read data only (`:2415-2640`) | none | yes |
| Data timeout | the host default | `TMOUT` 0xffffffff after the sweep | differs (longer); a write that waits on the card's CRC status or on busy is not cut short by it |

No register-level difference that explains a write fault at 150 MHz which reads do not see. The vendor drives and samples the write's CRC status with the settings that the read sweep tunes, as we do. What the sources do not give is whether this card needs another sample delay for the CRC
status token than for read data; neither the vendor nor we tune it. The next session therefore does not assume the write works at 150 MHz: it steps down on errors, and records why.

**Writes now step down like reads.** A write that faults with a controller, card or timeout error (a CRC status error, a busy or data timeout, an end-bit error among them) is a recoverable result of the platform, not a retention: the first error since the last completed request identifies the card again at the same ceiling (a new tuning), the second in a row steps down one mode (SDR104, SDR50, SDR25,
SDR12 at 1.8 V), and the owner writes the same piece again, the same sectors with the same data, inside the same admitted bounds. A stop at the lowest mode, or an unrecoverable error (a lost register, a platform deadline), retains the platform as before, with the record below.
Every step is in the mode log, with the core's error, protocol step and interrupt status of the fault that caused it, and the report carries the first two faults (`os_sd_mode` word 4, decoded by `apps/storage-write-test/host.py` as `sd_mode.faults`), so a session that steps down and completes still says why SDR104 failed. The host check (`checks/a733-mmc-uhs.c`, `write_recovery`) shows a write CRC error at SDR104 stepping down to SDR50 and the retry completing, the walk down to SDR12, a stop there faulting cleanly with the core's error, interrupt status and protocol step intact, and an I/O error retaining; a mutation that makes writes not recoverable fails it.

**The failure record of a write.** The record's value word carries the stage in bits 5:0, the core's error in 9:6, its protocol step in 15:10 and the low 16 bits of the controller's raw interrupt status at the fault in 31:16; its aux field carries the pieces written so far in 12:0 and the bus timing in force in 14:13 (3 SDR104 ... 0 default). The piece's first sector follows from the
piece count and the stream (the geometry check's write log maps piece numbers to sectors). `os_sd_write_pieces` and `os_sd_write_sectors` count what completed; `os_sd_mode` holds the mode and the steps.

## Prediction for the bounded write session on 3-3, second attempt (written before the run)

Run: mode 0 as before, on the image of the commit that carries this section, with the write recovery. Everything in the first prediction holds except the failure handling.
- **If SDR104 writes now pass** (the first failure was transient or the retune cures it): status 0, 536,870,912 bytes, 8 to 30 MB/s, mode record SDR104 with at most a restart or two (restarts 0 to 2, mode steps 0 to 2, the log naming retune), the volume clean.
- **If the write needs a lower mode** (the likelier case, given the first stop): the first write error tunes again at SDR104, the second steps to SDR50, then SDR25, until a mode writes; the session completes with status 0 and the file whole; the mode record shows the mode it finished in (SDR50 or SDR25) with 2 to 6 restarts and the steps in the log with reason 4 (errors), and `sd_mode.faults` names the core's error code and interrupt status of the first two faults (a CRC status failure is expected to show the data CRC error bit, 0x80, with or without the end-bit error, 0x8000). The rate is the card's.
- **Mount first:** the dirty volume is recovered at mount (recoveries 1, recovered clusters 0 or 1, the directory kept); the directory exists; the file is created fresh. A status 7 (`E_OPEN`) means a file existed after all.
- **If every mode fails:** the platform is retained, the watchdog resets, and the record now names the core's error, protocol step and interrupt status, the pieces written and the timing at the stop; nothing outside the plan can have been written.

## Prediction for the format and the bounded write on the scratch card 3-3 (written before the runs)

3-3's card holds nothing of value, so its writes are admitted on its CID and its partition (2048 + 61,065,216 sectors) alone, with one record for the card, and a failed session is undone by reformatting the partition (mode 9 of `storage-write-test`), not by restoring sectors. Two runs of the same image, in this order; the card's state after the earlier sessions is not known, which is why the first run formats.

**Run 1, mode 9 (format).** The open is as in the read run: the card at 1.8 V is found, the owner reopens with the bank at 1.8 V and enters SDR104 at 150 MHz. The enable checks the card's CID against the blob's and that every write lies inside the partition; nothing is read back for a hash. The format zeroes both FATs (14,912 sectors each) and the root cluster (32 sectors), writes FAT entries 0 to 2, the FSInfo sector and the boot sector's label: 29,856 sectors zeroed, `written_bytes` 15,286,272, about 120 write calls of up to 256 sectors, status 0, no file, no mount.
- This is also the first write at SDR104 whose result is read: 15 MB in under 2 seconds if the mode holds. If a write faults, the first fault tunes again and the second steps down; either way the run completes with status 0 and `sd_mode.faults` names the core's error and interrupt status of the first two faults (the question the first write session left open). Mode records: SDR104 with 0 to 2 restarts, or a lower mode with steps and reason 4.
- A refusal at the enable (stage 22 to 25, status 1) means the CID or the partition differs from the blob's; nothing is written then.

**Run 2, mode 0 (create the 512 MiB region file).** Mount finds a clean volume (recoveries 0), makes `/sunstone-test` and creates the file as in the second-attempt prediction above: status 0, 536,870,912 bytes, 8 to 30 MB/s, the mode of run 1 or a lower one, the volume clean. A status 7 (`E_OPEN`) would mean the format left the old file visible.

**Restore.** After any failed or interrupted session on this card: mode 9 again, then the next mode 0. The host check `checks/storage-write-guard.py` runs the format over an image that holds the finished file: `fsck.fat -n` clean afterwards, only the FATs, the root cluster, the FSInfo and the boot sector written, the file gone, and the region file created again whole.

## The format run (entry `1793687494152436674-9dcd2cb2`): the data drive phase of a tuned mode

**Record.** Value `0x801c8da6`, `sd_owner.c:94`, aux 1. Stage 38 (`recover_fail`, the restart's poll loop), detail `first_fault_detail`: core error 6 (`SUNXI_MMC_CONTROLLER`), protocol step 35 (`P_WRITE_DMA`, the CMD25 data phase), RINTSTS low 16 bits `0x801c`: COMMAND_DONE (2), DATA_OVER (3), TX_DATA_REQUEST (4) and END_BIT_ERROR (15). The manual (section 11.3.8, SMHC_RINTSTS) defines bit 15 while transmitting as "the host controller does not receive CRC status token"; a negative token would set bit 7 (DATA_CRC_ERROR), a data timeout bit 9, the busy clear bit 13. So the first write at SDR104 ended with no CRC status token seen, after the controller had sent its data. Aux 1 is the interrupt state that `irq_suspend` leaves in `os_sd_diag[2]`, not the platform result (the guard in `recover_fail` kept it); the restart's own result was not recorded and is now.

**Cause.** The driver took the drive phases of a tuned mode from the v5p3x rows (data 90 at 104 and 208 MHz). The vendor never runs a tuned SDR50 or SDR104 card in v5p3x: its in-kernel tuning exits v5p3x for v4p6x, whose clock-delay rule drives data at 180 for both modes. Reads do not use the data drive phase, so the read runs passed; every write has driven data at 90 since the phases were changed. The card sampled the data half a bit off, and the CRC status token never came back.

**Fix.** `uhs_registers` drives command and data at 180 for SDR50 and SDR104 (the 400 kHz setup and the untuned modes keep the v5p3x rows). The Python check now asserts the v4p6x rule from the pinned source, and the core check asserts both drive bits for every tuned mode.

**Prediction for the next write run.** The format (mode 9) completes at SDR104 with status 0, `written_bytes` 15,286,272, no faults in `sd_mode.faults`, 29,856 sectors in about 120 calls. If a fault remains, its RINTSTS names the next candidate and the restart's platform result is in aux.

## The data drive phase did not change the fault (entry `1793687913540627897-0dcbebfc`), and the restart after a write fault

**The record.** The same value as the first format run, `0x801c8da6`: write fault in `P_WRITE_DMA` (core error 6), RINTSTS `0x801c`, no CRC status token (bit 15). Aux 372 is now the restart's result: RETAINED (4) at platform stage IDENTIFY (14) with the fault flag set. So driving data at 180 did not change the first write fault, and the restart that follows it fails. The two are separate problems.

**The restart after a write fault (fixed, in two parts).**
- **The cause on the board (second submission of this fault, entry `1793688426368337098-abb94372`, identical record, aux 372 again).** The write ran in interrupt mode. The owner masks INTID 193 and leaves interrupt mode before the restart, but the controller's status that raised the interrupt is still asserted, so the level interrupt stays pending in the distributor. The platform's running check (`running`, called on every poll before the core is touched) accepts a masked line only if it is also not pending (`masked`, "preserve an unexplained inherited pending interrupt"), so the first poll of the restart failed at platform stage IDENTIFY, before the restart's own reset could clear the status. The host model reproduces the board's aux value exactly (RETAINED 4, stage 14, fault, aux 372, fault line 538 in `running`) once its distributor keeps the line pending until the controller's interrupt mask or status is written; it did not before, because the model's distributor never reported a pending line. The restart now tolerates the pending bit of the fault during the core's reset phases (`core.restart` nonzero) and requires it clear afterwards. The mutation `restart-pending-fatal` restores the old check and is killed.
- **A second, independent defect (also fixed).** A card left programming by an aborted CMD25 holds DAT0 low (specification 4.3) until CMD0 resets it (4.2.1), and a clock update waits for DAT0 unless the clock control masks it. The restart masked its first update only; the cold setup that followed updated the clock unmasked, which would have stalled on a busy card. The vendor masks DAT0 in every update (`sunxi_mmc_oclk_onoff` passes `ignore_dat0` 1), and so does the core now. The model keeps a card busy after an aborted write until CMD0 (`prog_busy`); the mutation `setup-clock-update-unmasked` is killed.
- CMD12 first is not needed: CMD0 sets every card to idle in any state (4.2.1) and ends the programming. It does not change the signalling voltage, so a card at 1.8 V stays there, and the identification treats S18A clear with the re-entry flag as the 1.8 V card and sends no CMD11 (`P_SD_OCR`, `h->reenter`). The host keeps the bank at 1.8 V; the restart returns the module to the identification clock and resets the controller before CMD0.
- Linux: `mmc_sd_hw_reset` is `mmc_power_cycle` (power off, 1 ms, power up) followed by `mmc_sd_init_card` with the old card, which negotiates 1.8 V again. The board can do that (`vmmc-supply = <&reg_dc1sw2>` for sdc0, a rail the platform already holds), but it drops the card's 1.8 V state and costs a full re-negotiation; CMD0 is enough here.

**Prediction for the next format run.** The first write at SDR104 faults as before; the restart now completes (CMD0 ends the programming); the second consecutive fault steps down; the format completes at SDR50 or SDR25 with status 0, `written_bytes` 15,286,272, restarts 2 or 3, mode steps 2 or 3, and `sd_mode.faults` holding the two faults (error 6, protocol 35, RINTSTS `0x801c`).

**The register state of a tuned SDR104 write, the vendor's and ours.** The vendor's state is the v4p6x method that its in-kernel tuning leaves (`mmc-sdc3/sunxi-mmc-v4p5x.c`, hash pinned in `checks/sunxi-mmc-uhs.py`) over the v5p3x host (`sunxi-mmc-v5p3x.c`, `sunxi-mmc.c`); lines are those files'.

| Register | Vendor | Ours | Same |
| --- | --- | --- | --- |
| DRV_DL 0x140 phases | command 180, data 180 (`v4p5x:419-431`, SDR50 and SDR104 set `dat_drv_ph` 1 at `:308-313`) | 180, 180 (`uhs_registers`, since `9d54a1fc`) | yes |
| SAMP_DL 0x144 | `SW_EN | kernel tuning delay` (`:425-429`) | `0x80 | tuned delay` | yes |
| DS_DL 0x148 | `(DS_DL & ~0x3f) | ds_dly (0) | SW_EN` (`:431-435`); the manual calls it the data strobe delay (HS400) | never written (reset value `0x2000`, software enable clear) | **no** (HS400 field; unlikely to matter) |
| NTSR 0x5c | 1x mode (bit 31 clear, `:811-814`); other bits at reset | bit 31, 9:8, 5:4, 0 cleared | yes |
| CSDC 0x54 | CRC_DET_PARA 3 (`:820-833`) | 3 | yes |
| EDSD 0x10c | HS400 bit 31 clear | clear | yes |
| THLD 0x100 (write) | block size 512 in 27:16, write enable (bit 2) set, read enable clear (`thld_ctl :866-907`) | the same (`P_WRITE_DMA`) | yes |
| SFC 0x104 | not written (`sfc_dis` only for SMHC v4p5p1 and v4p9; this is v5.3) | bypass bit cleared, which is the reset value | yes |
| FTRGL 0x40 | `dma_tl` of sdc0: `0x200700f8` (`sunxi-mmc.c:3630`) | `0x200700f8` | yes |
| TMOUT 0x08, WIDTH 0x0c | `0xffffffff`, 4-bit | the same | yes |
| A12A 0x58, IMASK 0x30, CMDR | 0, errors + auto-command-done, CMD25 with data, write, response CRC, wait-pre-over, auto stop | the same | yes |
| GCTRL, DMAC 0x80, DLBA, IDST, IDIE | resets before each DMA, DMA enable, `FIX_BURST | IDMA_ON` (`0x82`), IDIE for reads only | the same | yes |
| DBGC 0x50 | `0xdeb` at host init (`sunxi-mmc.c:722`), debug counters | never written | no (debug) |
| CLKCR 0x04 | enable, `CCLK_CTRL` low-power gating from `sunxi-power-save-mode`, `MASK_DATA0` in every update | enable only, no gating; the mask is now in every update | gating differs |
| Pad drive strength (SDC0 pins) | command and data 10 mA (code 0), clock 30 mA (code 2), pull-up, 1.8 V power source in the `mmc_1v8` state (`sun60iw2p1.dtsi:997-1030`) | 20 mA (code 1) on all six pins, pull-up (`large_sd.c:181`) | **no** |

**What differs.** In the SMHC registers only the data-strobe delay and the debug register, neither used by SDR104 writes. The one state that differs and is plausible for a fault that appears only when the host drives the data lines (reads pass, and so do commands) is the pad drive: the vendor drives the data and command pins at 10 mA and the clock at 30 mA, we drive all at 20 mA. A faster clock edge and slower data edges move the data against the clock at the card, which is what the 90 and 180 degree phases are tuned around. That is a candidate, not a finding: the board has not been run with the vendor's strengths, and the 50 MHz writes that pass on 3-1 used ours. The step-down makes it a performance question instead of a correctness one; it should be tried on 3-3 after this run.

## Every board write we have, and where a write fault stops

The records of all writes to a card on a board, by card and bus state. A CMD25 of up to 256 sectors (128 KiB) is carried by one descriptor per 4 KiB (up to 32, `SUNXI_MMC_DMA_SEGMENT`; 33 slots), from the application's flush region for data and from the owner's private buffer for metadata pieces.

| Card | Bus | Writes | Sectors per CMD25 | Outcome | RINTSTS | Entries |
| --- | --- | --- | --- | --- | --- | --- |
| 3-1 (CID `6d4a7f01…`) | 3.3 V, high speed, 50 MHz, 4-bit | file creation: 385,875,968 bytes in 128 KiB pieces; append of 150,994,944 bytes (1,164 write calls: 1,152 data pieces, 12 metadata); the interruption test (about 40 MiB written, cut deliberately, recovered); mount recovery (3 metadata writes, 1,536 bytes) | 1 to 32 (FAT, directory, FSInfo, recovery) and 256 (data) | all completed; 14.7 MB/s flushed; the 512 MiB file reads back with 0 mismatches | none | `1793623788842205312-60954030`, `1793632378497932347-1618d4a8`, `1793635810117434075-86ece323`, `1793636210564944400-a065537e` |
| 3-3 (CID `27504853…`) | 1.8 V, SDR104, 150 MHz | session 1: the dirty flag in both FATs (1 sector each), the new directory cluster (32 sectors), the FAT entry and the root entry (1 sector each); then the first data flush | 1 to 32 completed; the next write (a flush of 256 sectors) faulted | stage 34, RETAINED, platform fault line 553 (core in FAULT) | not recorded | `1793683567666621650-3e3c6631` |
| 3-3 | 1.8 V, SDR104 | session 3 (mount recovery, then a write): a recoverable write error, then the restart refused | not recorded | stage 37 | not recorded | `1793685607294001067-6c6ebb1d` |
| 3-3 | 1.8 V, SDR104 then SDR50 | the format (mode 9), whose pieces are 256 sectors (a tail of 64, the root cluster 32, FSInfo and boot 1): four runs | 256 | a write faults at SDR104, again after the retune, at SDR50, again after the retune, then the budget ended | `0x801c`: command done, data over, TX request, end-bit error (no CRC status token); error 6, `P_WRITE_DMA` | `…c7085caf`, `…0dcbebfc`, `…abb94372`, `…4e190fcd` |

**What separates pass from fail.** Not the length alone: 256-sector writes complete on 3-1, 1,164 times in one session. The writes that failed are all on 3-3 at 1.8 V; no write of more than 32 sectors has ever completed on 3-3, at any mode, and no write has been made to 3-3 at 3.3 V (the card has been at 1.8 V since the first read). Within 3-3 the only variable that separates a completed write from a failed one is the length (1 to 32 sectors passed, 256 never). Card and bus voltage are confounded with each other and with SDR104 and SDR50 being the only modes tried; length is confounded with the data phase being the first write of its kind on that card. The format run below separates length from bus speed from the same card.

**Where a write fault stops (recorded from this run on).** At a data-phase error the core reads SMHC_TBC0 (0x48, bytes between card and FIFO) and SMHC_TBC1 (0x4c, between memory and FIFO) (manual 11.3.8.19 and 20) and the current host descriptor (0x90, vendor `sunxi-mmc.h` REG_CHDA; the manual does not list it), and keeps the descriptor index. The mode log carries the card-side blocks and the descriptor per fault. The failure record of stage 34 carries (detail, 26 bits): the status bits 15:1 in 14:0, the bus timing in 16:15, the descriptor reached in 22:17 (62 unreadable, 63 outside the list), the error in 25:23; (aux, 15 bits): the card-side 512-byte blocks of the last fault in 8:0 and, in 14:9, the first fault's count in 4 KiB units. The pieces written and the protocol step are no longer in it. The retry budget is the ladder (`A733_MMC_RECOVERIES`, 8: two recoveries per mode through SDR104, SDR50, SDR25, and the refusal at SDR12), bounded by the lifetime deadline; a host case walks the ladder and requires the same count.

**Prediction for the format run with the full ladder.**
- **Length-dependent fault:** every mode fails, SDR104 down to SDR12, with the same RINTSTS `0x801c` and a card-side block count that repeats (the first fault's count in aux 14:9 equals the last fault's blocks divided by 8); the session ends in a watchdog reset with a stage-34 record.
- **Speed-dependent fault:** SDR25 or SDR12 completes the format with status 0 and written_bytes 15,286,272, the mode record shows the mode it finished in, and `sd_mode.faults` holds the earlier faults.

## Results of the full ladder, the 440 MiB write, its verify, and the pad drive

- **Format with the ladder (entry `1793689119969310811-abcfb6bc`, image `3e308c9c`).** SDR104 and SDR50 fault (error 6, `P_WRITE_DMA`, RINTSTS `0x801c`); SDR25 completes: status 0, 15,286,272 bytes, 4 restarts. The fault depends on the bus speed.
- **Mode 0 (entry `1793689196470444588-1be26413`).** The same ladder on the first large flush, then SDR25 for the rest of the session (the stepped-down mode is the core's ceiling, so no later piece climbs back). The write ended at the time guard after a clean sync and close: status 11, 440,401,920 of 536,870,912 bytes in 47.9 s, flush rate 9.8 MB/s (this card programs at about 10 MB/s at 50 MHz; the bus is not the limit). A write that ends at the guard this way is a pass (`host.guard_stop`).
- **Verify (entry `1793689311355151182-d43861f7`).** Mode 2 read the 440,401,920-byte file back at SDR104, 150 MHz, tuned (window 18 to 63, delay 41), 46.8 MB/s: status 0, 0 mismatches, no mode step. Reads at 150 MHz are sound on the card that cannot write at 100 MHz.
- **Pad drive (entry `1793689497237363008-de0d289f`, image `83bfe0ba`).** With the board kernel's strengths (clock 30 mA, command and data 10 mA) the format runs the same ladder and ends at SDR25. The two faults are again `0x801c`, and now the report says where: **1 block** transferred card side (SMHC_TBC0 / 512) in both, descriptor 63 (the current-descriptor register is not readable as a list address). The transfer stops after the first block: the CRC status token of the first block is not seen at 100 and 150 MHz, at a fixed position, at either pad drive. The pad drive is not the cause and is left at 20 mA, the strength the 3.3 V writes on 3-1 used.
- **What this leaves.** Reads sample the card's output with the sweep's delay; the CRC status token that follows a written block is also the card's output, sampled by the same circuit, but nothing tunes it for writes. The vendor sets CSDC to 3 for every mode other than HS400, as we do. A token that does not arrive from the first block at 100 and 150 MHz and does at 50 MHz is a timing question between the card's output and the host's sample point for that token; the sample delay chosen for reads (window 18 to 63, delay 41) may not suit it. That is the next thing to look at, behind the working step-down.

## Where the controller samples the CRC status token, and the one change to try

**Which register sets the sample point.** None is specific to the token. The token is the card's output on DAT0 after each written block (RINTSTS bit 15 while transmitting reports that it was not seen; manual 11.3.8, SMHC_RINTSTS). It enters through the same input circuit as read data and responses, whose sample point is SMHC_SAMP_DL (0x144): software enable in bit 7 and the delay in 5:0, "the relative delay between clock line and command line, data lines" (manual 11.3.8.37). The other registers near it do not move the point in the mode we run: SMHC_CSDC.CRC_DET_PARA (0x54[3:0]) selects the HS400 value (6) or the value for every other mode (3), and the other values are reserved (11.3.8.21); SMHC_NTSR's sample phases (9:8 data, 5:4 command) and its "clear the data lines' input phase before the CRC status" bit (22, default 1) act in the new timing mode (bit 31 set), while the tuned modes run in the old mode (bit 31 clear). THLD bit 1 (BCIG) enables the busy-clear interrupt, not the token.

**What the vendor writes.** CSDC 3 at 100 and 150 MHz in both methods (`sunxi-mmc-v5p3x.c:610-630`, `mmc-sdc3/sunxi-mmc-v4p5x.c:820-833`); ours is 3. In v5p3x's new mode the sample phase is 0 for the 104M and 208M rows with no software delay (the delay write is commented out, `v5p3x.c:260-270`). In v4p6x, which the in-kernel tuning leaves in force, SAMP_DL is the software enable with the swept delay (`sunxi_mmc_set_clk_dly`, `v4p5x.c:425-429`): the middle of the longest passing window, `start + width / 2` (`sunxi-mmc.c` `get_best_sdly`), 41 for our 18 to 63 and 32 for the 100 MHz window of all 64. Ours is the same. The difference is in the failure path: with `SUNXI_SC_EN_RETRY` (sdc0 sets `ctl-spec-caps` 0x428) a data request that fails is waited out, the host reset, and sent again with SAMP_DL set to `sunxi_samp_dl % 64` and that value raised by 2 per retry, up to 32 retries (`sunxi_mmc_judge_retry_v4p6x`, `v4p5x.c:1086-1090`; the retry loop at `sunxi-mmc.c:1700-1740`). The counter starts at 0, so the first retry of a failed write runs at delay 0. The vendor drivers cover a write whose token is not seen by walking the read-tuned delay away from its tuned value, which implies the token's best point is not the read window's.

**Does the read delay apply to the token, and what is ours.** It does: one register covers every input. Ours is 41 at 150 MHz (the middle of a window that passes at 18 to 63 for read data) and 32 at 100 MHz (every delay passes for reads). Delay 0 is outside the 150 MHz read window, so it is a value the sweep rejects for data; it is the value 50 MHz runs at, where the write completes.

**The one change.** Run the data phase of a tuned card's write with SAMP_DL software delay 0 and give the tuned delay back when the data and the automatic stop are done: `SUNXI_MMC_WRITE_SAMPLE_DELAY` (0) in `include/sunxi_mmc.h`, written at the CMD25 in `command_step` and restored in `P_WRITE_DMA` (`drivers/storage/sunxi_mmc.c`). It is the value of the vendor's first write retry and of the one mode that writes on 3-3. A legacy or SDR25 card is untouched; the status commands (CMD13) run with the tuned delay. The CMD25 response is sampled with delay 0 too; the vendor's retry does the same.

**The run.** The format on 3-3 (mode 9) with this change, starting at SDR104 as always, on the image built from the commit that carries this section; the ladder stays, so a fault still returns a report.
- **Confirmed** if the format completes at SDR104 at 150 MHz: status 0, written_bytes 15,286,272, 0 restarts, no `sd_mode.faults`. The token is sampled correctly with delay 0; the next step is to find the token's window (the vendor's +2 walk) and to run mode 0.
- **Refuted** if the first fault is again RINTSTS `0x801c` after 1 block, the ladder runs to SDR25, and the format ends there as before. The token does not depend on the sample delay at 150 MHz; the next candidates are the sample mode (new against old, the passing 50 MHz writes use the new mode with a 180 degree phase) and BCIG.
- **Unclear** if the fault changes (for example a response error, RINTSTS bit 6 or 8, at the CMD25), which would say that delay 0 is wrong for the command response and the write delay must be applied after the response.
