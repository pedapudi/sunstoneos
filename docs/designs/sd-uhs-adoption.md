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
| 3-3, card at 1.8 V | **3.3 V bank** | **not recorded**: no run has put a 3.3 V host in front of a card at 1.8 V | none |

So the one case the rule needs, a card at 1.8 V seen from a 3.3 V host, has never been measured, and the recorded answers cannot choose between "silent" and "S18A clear". What the records do support: a card that answers at 3.3 V with S18A set is at 3.3 V
(3-1, and 3-3 before its first switch); S18A clear has only been seen from a card at 1.8 V with the bank at 1.8 V. The rule below is therefore the simplest one that is correct under either outcome of the missing measurement, and one read-only run of the
existing mode 0 on 3-3 (card at 1.8 V, bank at 3.3 V, CMD0, CMD8, ACMD41 with S18R, no CMD11) records the missing row; its result simplifies the rule if the card is silent or never answers S18A clear.

**The measurement that settles step 2.** Whether a 3.3 V host reads a 1.8 V card is unmeasured. One read-only run on 3-3 with the card at 1.8 V and the bank at 3.3 V (the existing mode 0: CMD0,
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
