# Design: adopting UHS-I SDR104 in the OS

Status: **host-tested; read-only paths qualified on the board (port 3-3).** The OS owner (`apps/os/large_sd.c`) implements the algorithm below and has not run on a board; writes at 1.8 V have not run. The design builds on [the recovery design](sd-uhs-recovery.md), whose board results are:

- **Port 3-3 card (Phison SD32G, 2020):** re-entered at 1.8 V after a watchdog reset by moving the bank to 1.8 V first (CMD0, CMD8, ACMD41, no CMD11). S18A is clear at 1.8 V; the CMD6 group 1 support field read at 1.8 V is `0x801f` (SDR12 to SDR104 and DDR50). SDR25 reads at 50 MHz run at 21.4 MB/s per request, and SDR104 at 150 MHz runs at 47.6 MB/s per request and 27.4 MB/s sustained (see [the UHS-I design](sd-uhs.md)). The close restores the bank to 3.3 V and the lifetime returns normally.
- **Port 3-1 card (Phison SD64G, 2026):** seen at 3.3 V only. It answers S18A set at 3.3 V. Its 1.8 V behaviour is expected to match 3-3's and is unmeasured.
- **After a watchdog reset the board returns to FEL with the card at 1.8 V** (measured on 3-3).

Two platform facts apply. The platform baseline requires the PF bank at 3.3 V and adaptive mode at the start of a lifetime; every lifetime so far began after a normal or a failed close and met it, but none began after a reset that left the bank at 1.8 V, so whether a watchdog reset restores the bank selector is unmeasured. And software cannot return the card to 3.3 V on these boards (the card supply is not separately switchable, and the rail documents disagree about DC1SW2), so a card switched to 1.8 V stays there until board power is removed.

## The session-start resolution algorithm

One algorithm for every session that touches the card, for the cases of both boards. It is the algorithm of the recovery design with the measured answers
filled in, and with the one case it could not settle marked for a measurement.

The state the algorithm must handle at the start, by what a previous session may have left:

| Card | Bank (selector) | How it arises | Example |
| --- | --- | --- | --- |
| 3.3 V | 3.3 V | after power removal, or a session that never switched | port 3-1 |
| 1.8 V | 3.3 V | after a lifetime that switched and closed, or after a reset that restores the selector | port 3-3 after its lifetimes |
| 1.8 V | 1.8 V | after a reset that leaves the selector (unmeasured) | not yet seen |
| mid-switch or in transfer | either | a reset during the switch or a read | a cut |

Steps:

1. **Read the bank.** Selector and `GPIO_POW_VAL` agree on 3.3 V (the platform baseline): go to step 2. On 1.8 V (the unmeasured case): go to step 4 with the bank
   already there.
2. **Probe at 3.3 V.** Clock stopped, then CMD0, CMD8 at 400 kHz.
   - **An R7 answer:** the card is at 3.3 V, or it is at 1.8 V and a 3.3 V host can read it (the open question of the recovery design). ACMD41 with S18R then decides: **S18A set means
     the card signals 3.3 V** (3-1: measured), so the normal switch follows (CMD11 and the 10 ms sequence). **S18A clear** is a card that declines UHS or one already at 1.8 V: for a card
     that the OS itself switched, the OS knows (below); otherwise treat it as a card that declines and stay at 3.3 V high speed, because sending nothing is safe and CMD11 would be
     refused by a 1.8 V card anyway.
   - **No answer:** the card is at 1.8 V, absent or hung; go to step 4.
3. **(the normal switch)** as in the recovery design; on any failure the next session starts at step 1 and the table above gives the state.
4. **Move the bank to 1.8 V** with the clock stopped, wait for `GPIO_POW_VAL`, hold the clock off 10 ms (the platform's `ENTER_1V8`), then CMD0, CMD8, ACMD41 with S18R, ignoring S18A (3-3: clear
   at 1.8 V), CMD2 onward (the core's re-entry mode). No CMD11. Silence here, with the bank returned to 3.3 V, is the one terminal outcome: card absent or hung, needing power removal.
5. **Speed.** At 1.8 V the CMD6 group 1 support field is meaningful: select SDR104 when `0x8` is set and the tuning sweep finds a window of at least 12 delays, otherwise SDR25 at 50 MHz.

**What the cards answer (records in `docs/evidence/storage.jsonl`).** A 3.3 V host reads a card at 1.8 V, and the card answers S18A clear, as it does to a 1.8 V host, so S18A alone cannot tell a card at 1.8 V from a card that declines UHS. The CMD6 group 1 support field tells them apart: a card at 3.3 V reports functions 2 to 4 as 0 (port 3-1: `0x8003`), a card at 1.8 V reports them (port 3-3: `0x801f`, through either host voltage). The rule, with no 1.8 V probe: at 3.3 V, S18A set means switch with CMD11; S18A clear with SDR50, SDR104 or DDR50 in the group 1 support field means the card is at 1.8 V, so move the bank to 1.8 V and re-enter; S18A clear with only functions 0 and 1 is a card without UHS-I and stays at 3.3 V high speed; no answer to CMD8 at 3.3 V means re-enter at 1.8 V. Re-entry costs one more identification (about 0.3 s) once per power cycle. The card at 1.8 V read by a 3.3 V host is outside its signalling specification, so the OS re-enters at 1.8 V and does not read it at 3.3 V.

**Restart and step-down.** A failed read identifies again (a new tuning), then steps the mode down through the ceilings SDR104, SDR50 and SDR25 with no CMD11 and no PF write. The native adapter admits the CMD6 function 1, 2 and 3 switches of the UHS-I profile; the lifetimes model walks a card with SDR50 through all three ceilings with every write offered to the real filter.

**A marker.** A byte that survives a watchdog reset and is lost with board power would let the OS skip step 2: "the card was switched". The vendor boot flow uses the AXP8191 `BUFFER0`
(`0x04`) for the reboot mode, so it is not taken; the RTC words after the breadcrumb block are not cleared by a reset but the runner clears the breadcrumb block at each launch, and whether the
words beyond it survive a power removal is unmeasured. The algorithm does not depend on a marker: it only saves the probe when the marker is trusted, and is wrong never, because step 4's
first command is CMD0 at 1.8 V.

## When the OS switches

- **At session start, once, and never back.** The switch (CMD11 and the PF selector) happens in the first identification of a session whose card is at 3.3 V and reports S18A; after it the OS
  stays at 1.8 V for the card's life (until board power is removed). A running session never changes the signalling level.
- **Which images.** An image that is loaded over FEL (development and OS images) takes SDR104 by default once adoption passes its board runs, behind a build or session setting so that a
  session can stay at high speed. An image that boots from the card (boot0 and the stage-2 loader read the package at 3.3 V) must never leave the card at 1.8 V: it stays at 3.3 V high speed
  and never sends CMD11. The two cannot share a card without a supply switch, so a card that has been used by an SDR104 session needs power removal before an autonomous boot.
- **What the large loader gains.** It reads at 21.7 MB/s at 50 MHz (6.0 ms per 128 KiB); a loader at 150 MHz is bounded by 75 MB/s raw and the card. The gain is the sustained rate of the SDR104 reads over 21.7.
- **Clock and approvals.** The runner's policy treats new clock points as needing the coordinator's approval; 150 MHz on the SD module is one, and the first OS adoption needs it.

## What a watchdog reset leaves behind

| Left behind | Consequence | Handling |
| --- | --- | --- |
| The card at 1.8 V, in any state: idle, ready, transfer, SDR104 timing, mid-read | CMD0 at 1.8 V returns it to idle and to SDR12 timing (specification 3.9.4); it stays at 1.8 V | step 4 |
| The PF selector: 3.3 V if the reset restores it (the expected reset value), 1.8 V if not (unmeasured) | the baseline must accept either | step 1 reads it; the platform baseline must accept the 1.8 V case |
| The SD controller, module clock and bus: reset by the watchdog | none | the platform's own setup |
| A read in flight | the card may still be sending data | CMD0 ends it; the next identification does not depend on it |
| The ROM | returns to FEL without trouble with the card at 1.8 V (measured on 3-3) | none |
| A card with a stuck switch (CMD11 answered, clock stopped, bank not moved) | holds CMD and DAT low until the clock stops | the pending-switch row of the recovery design's table |

The card is never left in a state that needs a human, except the terminal case of step 4, which needs two failures at once (a card that refuses CMD0 at 1.8 V and no response at 3.3 V).

## The write path (CMD25) at SDR104

No write at 1.8 V has run. What is known and what is needed:

- **Known (code).** The core programs the read threshold for SDR104 reads (`THLD` block size and bit 0) and the write threshold for CMD25 (`THLD` block size and bit 2) per command, so the two do not
  conflict. The native adapter admits a `THLD` write in the UHS-I profile, and CMD25 only for a verified bounds object in a writer build (`A733_MMC_BOUNDED_WRITER`); the two have not been combined in one
  host model. The write path's DAT0 busy timer and programming-status check do not depend on the bus speed.
- **Drive and sample phases (derived).** The board kernel sets them per clock range in `sunxi_mmc_set_clk_dly` (`bsp/drivers/mmc/sunxi-mmc-v5p3x.c`) from the defaults of `sunxi_mmc_clk_dly_v5p3x`; the `&sdc0` node sets none. The command drive phase is bit 16 and the data drive phase bit 17 of `0x140`; the sample phases are bits 5:4 (command) and 9:8 (data) of `0x5c`. `SAMP_DL` and `DS_DL` are written only by tuning. The core writes these values (`uhs_registers`, the cold setup), `checks/sunxi-mmc-uhs.py` pins the table by parsing the vendor source and node, and `checks/sunxi-mmc-uhs.c` compares every mode's register values with it.

  | Range (clock) | Our modes | Command drive | Data drive | Data sample | Command sample |
  | --- | --- | --- | --- | --- | --- |
  | 400 kHz | identification | 180 | 90 | 0 | 0 |
  | up to 26 MHz | default speed, SDR12 | 180 | 90 | 0 | 0 |
  | up to 52 MHz | high speed, SDR25 | 180 | 180 | 1 | 1 |
  | up to 104 MHz | SDR50 | 180 | 90 | 0 | 0 (tuned delay) |
  | up to 208 MHz | SDR104 | 180 | 90 | 0 | 0 (tuned delay) |

  The same registers serve reads and writes. The CRC status of a write is sampled with the read-data setting, so the tuned delay applies. Reads do not depend on the data drive phase; a write does. Reads with data drive 90 degrees at 400 kHz, SDR50 and SDR104 pass on port 3-3; writes are untested on a board.
- **Policy until qualified.** Writes run at SDR25 (50 MHz) on a card at 1.8 V, or at 3.3 V high speed on a card that was never switched. The core has no runtime mode change, so an SDR104 session that wants to write needs a step in its state machine (clock stop, CMD6 check and set, module clock change, restart) and the strict CMD25 model run at 1.8 V and 50 MHz.
- **Qualification path.** (1) Extend `checks/sunxi-mmc-write-dma.c` to a 1.8 V card at SDR25 and then SDR104 with the sample-delay and drive-phase registers modeled from the vendor driver; (2) read the
  vendor `sunxi-mmc-v5p3x.c` for the write-side timing registers at SDR104 and record the register sequence the way `docs/designs/mmc-multiblock-write.md` does for 3.3 V; (3) a board write session at
  SDR25 on 1.8 V under the per-session backup procedure; (4) only then SDR104 writes, in the same way. Every board write session keeps the existing blob, backup and approval-record rules.

## Open work

1. Run the OS owner on a board (read run and the bounded write at 1.8 V on port 3-3, which needs the write session backup procedure; the 3-3 layout and backup plan is in record `sd-uhs-adoption-3-3-layout-capture`).
2. Run the read-only switch to 1.8 V on port 3-1, which has never been at 1.8 V (prediction in record `sd-uhs-adoption-3-1-switch`, not run).
3. A runtime SDR104-to-SDR25 mode change and the strict CMD25 model at 1.8 V.
4. The runner: approval of the 150 MHz point and of CMD11 for OS images, and the image classification for UHS-I.
5. Two image-level SD checks (`storage-read-native.py`, `sd-return-native.py`) are stale models of the current image and are waived in `checks/gate-inputs.json`; rewrite them against the image.
