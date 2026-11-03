# Design: adopting UHS-I SDR104 in the OS

Status: **qualified on the board for reads and writes at SDR104, 150 MHz (scratch card, port 3-3): 512 MiB written and read back with no mismatch.** The OS owner (`apps/os/large_sd.c`) implements the algorithm below. The design builds on [the recovery design](sd-uhs-recovery.md), whose board results are:

- **Port 3-3 card (Phison SD32G, 2020):** re-entered at 1.8 V after a watchdog reset by moving the bank to 1.8 V first (CMD0, CMD8, ACMD41, no CMD11). S18A is clear at 1.8 V; the CMD6 group 1 support field read at 1.8 V is `0x801f` (SDR12 to SDR104 and DDR50). SDR25 reads at 50 MHz run at 21.4 MB/s per request, and SDR104 at 150 MHz runs at 47.6 MB/s per request and 27.4 MB/s sustained (see [the UHS-I design](sd-uhs.md)). The close restores the bank to 3.3 V and the lifetime returns normally.
- **Port 3-1 card (Phison SD64G, 2026):** seen at 3.3 V only. It answers S18A set at 3.3 V. Its 1.8 V behaviour is expected to match 3-3's and is unmeasured.
- **After a watchdog reset the board returns to FEL with the card at 1.8 V** (measured on 3-3).

Two platform facts apply. The platform baseline requires the PF bank at 3.3 V and adaptive mode at the start of a lifetime. Every lifetime so far began after a normal or a failed close and met it, but none began after a reset that left the bank at 1.8 V, so whether a watchdog reset restores the bank selector is unmeasured. And software cannot return the card to 3.3 V on these boards (the card supply is not separately switchable, and the rail documents disagree about DC1SW2), so a card switched to 1.8 V stays there until board power is removed.

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
     that the OS itself switched, the OS knows (below). Otherwise treat it as a card that declines and stay at 3.3 V high speed, because sending nothing is safe and CMD11 would be
     refused by a 1.8 V card anyway.
   - **No answer:** the card is at 1.8 V, absent or hung; go to step 4.
3. **(the normal switch)** as in the recovery design; on any failure the next session starts at step 1 and the table above gives the state.
4. **Move the bank to 1.8 V** with the clock stopped, wait for `GPIO_POW_VAL`, hold the clock off 10 ms (the platform's `ENTER_1V8`), then CMD0, CMD8, ACMD41 with S18R, ignoring S18A (3-3: clear
   at 1.8 V), CMD2 onward (the core's re-entry mode). No CMD11. Silence here, with the bank returned to 3.3 V, is the one terminal outcome: card absent or hung, needing power removal.
5. **Speed.** At 1.8 V the CMD6 group 1 support field is meaningful: select SDR104 when `0x8` is set and the tuning sweep finds a window of at least 12 delays, otherwise SDR25 at 50 MHz.

**What the cards answer (records in `docs/evidence/storage.jsonl`).** A 3.3 V host reads a card at 1.8 V, and the card answers S18A clear, as it does to a 1.8 V host. S18A alone therefore cannot tell a card at 1.8 V from a card that declines UHS. The CMD6 group 1 support field tells them apart: a card at 3.3 V reports functions 2 to 4 as 0 (port 3-1: `0x8003`), a card at 1.8 V reports them (port 3-3: `0x801f`, through either host voltage). The rule, with no 1.8 V probe: at 3.3 V, S18A set means switch with CMD11. S18A clear with SDR50, SDR104 or DDR50 in the group 1 support field means the card is at 1.8 V, so move the bank to 1.8 V and re-enter. S18A clear with only functions 0 and 1 is a card without UHS-I and stays at 3.3 V high speed. No answer to CMD8 at 3.3 V means re-enter at 1.8 V. Re-entry costs one more identification (about 0.3 s) once per power cycle. The card at 1.8 V read by a 3.3 V host is outside its signalling specification, so the OS re-enters at 1.8 V and does not read it at 3.3 V.

**Restart and step-down.** A failed read identifies again (a new tuning), then steps the mode down through the ceilings SDR104, SDR50 and SDR25 with no CMD11 and no PF write. The native adapter admits the CMD6 function 1, 2 and 3 switches of the UHS-I profile; the lifetimes model walks a card with SDR50 through all three ceilings with every write offered to the real filter.

**A marker.** A byte that survives a watchdog reset and is lost with board power would let the OS skip step 2: "the card was switched". The vendor boot flow uses the AXP8191 `BUFFER0`
(`0x04`) for the reboot mode, so it is not taken. The RTC words after the breadcrumb block are not cleared by a reset but the runner clears the breadcrumb block at each launch, and whether the
words beyond it survive a power removal is unmeasured. The algorithm does not depend on a marker: it only saves the probe when the marker is trusted, and is wrong never, because step 4's
first command is CMD0 at 1.8 V.

## When the OS switches

- **At session start, once, and never back.** The switch (CMD11 and the PF selector) happens in the first identification of a session whose card is at 3.3 V and reports S18A. After it the OS
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

## The write path (CMD25) at 1.8 V

- **Registers.** The core programs the read threshold for reads (`THLD` block size and bit 0) and the write threshold for CMD25 (`THLD` block size 512 in 27:16, bit 2 set, bit 0 clear) per command. The native adapter admits a `THLD` write in the UHS-I profile, and CMD25 only for a verified bounds object in a writer build (`A733_MMC_BOUNDED_WRITER`). The DAT0 busy timer and programming-status check do not depend on the bus speed.
- **Drive and sample phases.** The board kernel sets them per clock range in `sunxi_mmc_set_clk_dly` (`bsp/drivers/mmc/sunxi-mmc-v5p3x.c`); the `&sdc0` node sets none. The sdc0 node enables the in-kernel tuning, which leaves the host in the v4p6x method (`mmc-sdc3/sunxi-mmc-v4p5x.c`): its rule drives command and data at 180 degrees for SDR50 and SDR104. The core writes these values in `uhs_registers`; `checks/sunxi-mmc-uhs.py` pins the table from the vendor source and `checks/sunxi-mmc-uhs.c` compares every mode's registers with it. Command drive is bit 16 and data drive bit 17 of `0x140`; the sample phases are bits 5:4 (command) and 9:8 (data) of `0x5c`; the tuned delay is written to `SAMP_DL`.

  | Mode | Command drive | Data drive | Data sample | Command sample |
  | --- | --- | --- | --- | --- |
  | identification (400 kHz), default speed, SDR12 | 180 | 90 | 0 | 0 |
  | high speed, SDR25 | 180 | 180 | 1 | 1 |
  | SDR50, SDR104 | 180 | 180 | 0 | 0 (tuned delay) |

  The same registers serve reads and writes. Reads do not depend on the data drive phase; a write does.
- **Data phase of a tuned write: the new timing mode.** The sample circuit of the old timing mode (`NTSR` bit 31 clear, the read sweep's delay) does not see the card's CRC status token at 100 and 150 MHz. The writes that complete, SDR25 at 1.8 V and high speed at 3.3 V on port 3-1, run in the new timing mode, and so does a tuned write: at the CMD25 the core sets `NTSR` bit 31 with the data and command sample phase `SUNXI_MMC_WRITE_NTSR_PHASE` (0, the board kernel's v5p3x row for 104 and 208 MHz) and writes `SAMP_DL` as software delay 0 (`0x80`); when the data and the automatic stop are done (the card's response, state `P_WRITE_DMA`) it writes the tuned old mode and the sweep's delay back, so the status commands and the reads that follow run as tuned. A fault leaves the new mode in place; the restart's setup and sweep rewrite both registers. `checks/sunxi-mmc-write-dma.c` requires the two writes of each register in order, and none for a legacy or SDR25 card; `checks/a733-mmc-uhs.c` requires the old mode and the tuned delay back after a restart and after a step-down.
- **Write-busy timeout.** The controller times the card's busy period after written data (`SMHC_WR_BUSY_TMOUT`, default 0xffffff card clocks: 112 ms at 150 MHz, 336 ms at 50 MHz) and raises `RINTSTS` bit 18. The board kernel's error mask does not contain it. During a DMA write the core clears the bit, counts it (`result.busy_timeouts`, summed in the owner and reported in bits 47:40 of `os_sd_mode` word 2, `busy_timeouts` in `host.py`) and waits for the card on DAT0 as before; the request deadline (2 s per piece in the OS) turns a card that never releases DAT0 into a recoverable timeout fault. In every other command bit 18 is still a terminal error.
- **Registers compared with the vendor's tuned SDR104 write state.** `DRV_DL`, `SAMP_DL`, `NTSR`, `CSDC` (CRC_DET_PARA 3), `EDSD`, `THLD`, `FTRGL` (`0x200700f8`), `TMOUT`, `WIDTH`, the command register, the DMA registers and the descriptor chain match. The data strobe delay `DS_DL` (an HS400 field) and the debug register are not written; clock gating differs; every clock update masks DAT0, as the vendor's does. The pad drive differs (vendor: command and data 10 mA, clock 30 mA; ours 20 mA on all six pins); running with the vendor's strengths changed nothing, so ours stays.
- **Step-down on write faults.** A write that faults with a controller, card or timeout error is a recoverable result: the first error since the last completed request identifies the card again at the same ceiling (a new tuning); the second in a row steps down one mode (SDR104, SDR50, SDR25, SDR12 at 1.8 V); the owner writes the same piece again with the same data inside the same admitted bounds. An unrecoverable error (a lost register, a platform deadline) retains the platform. The ladder is `A733_MMC_RECOVERIES` (8: two per mode), bounded by the lifetime deadline. The stepped-down mode is the core's ceiling for the rest of the session. `checks/a733-mmc-uhs.c` (`write_recovery`) walks the ladder, and mutations that make writes unrecoverable fail it.
- **Restart after a write fault.** CMD0 ends a programming state and returns the card to idle in any state without changing the signalling level; the identification treats S18A clear with the re-entry flag as the 1.8 V card and sends no CMD11. The restart tolerates the level interrupt of the fault still pending in the distributor during the core's reset phases, and requires it clear afterwards. A card left programming by an aborted CMD25 holds DAT0 low, so every clock update masks DAT0. The host model keeps a card busy after an aborted write until CMD0, and mutations that restore either old rule fail the check.
- **Failure record.** Every fault records the source line of the check that raised it (`result.fault_line`), the status of two reads of `RINTSTS` in full (bit 18 is outside the low 16 bits) and, for a data-phase fault, the IDMAC status and the piece's size class. Stage 34 (a write out of retries): the stage in bits 5:0 of the value word; the detail field holds the status bits 15:1 in 14:0, the bus timing in 16:15, the descriptor reached in 22:17 (62 unreadable, 63 outside the list) and the core's error in 25:23; the aux holds the card-side 512-byte blocks in 8:0, the IDMAC error bits in 12:9 and the size class in 14:13. Stages 37 to 40 (a recovery that did not finish): the value word holds the first fault, `RINTSTS` bits 15:2 in 13:0, bit 18 in 14 and the fault line in 25:15; the aux holds the platform's result and fault flag, the `board_live` refusal bits, the lifetime flag, the first fault's size class and IDMAC bits, and whether the loop ended on a platform result (`os_sd_recover_aux`). The first stop's aux is kept. A stop with a non-zero status that left no other record writes a failure note from the maintenance hook (`apps/board/stop_note.h`); a clean exit leaves none. The core reads `SMHC_TBC0` (`0x48`), `SMHC_TBC1` (`0x4c`) and the current host descriptor (`0x90`) at a data-phase error. `os_sd_mode` holds the mode and the steps, and the report carries the first two faults (`sd_mode.faults` in `apps/storage-write-test/host.py`).
- **Measured.** Writes at 3.3 V, 50 MHz on port 3-1 complete (256-sector pieces). On port 3-3 at 1.8 V, SDR104 at 150 MHz: the format (15,286,272 bytes) and the 512 MiB file complete with no restart and no mode step, 13.2 to 13.5 MB/s flushed (the card's programming rate; 15 write-busy timeouts waited out in the 512 MiB), and the file reads back with 0 mismatches at 46.5 MB/s. Before the new-mode data phase every write at 100 and 150 MHz faulted with `RINTSTS` `0x801c` (no CRC status token) after 1 block, and the ladder ended at SDR25. The records are in `docs/evidence/storage.jsonl` (`sd-uhs-adoption-write-table`, `1793690518271950065-2379bd2f`, `1793691763257856580-edf2f59c`, `1793692002289431339-7dfb06bb`, `1793692075400545011-09988217`, `1793692393910115727-f58db0e9`).

## Open work

1. Run the read-only switch to 1.8 V on port 3-1, which has never been at 1.8 V (prediction in record `sd-uhs-adoption-3-1-switch`, not run).
2. The runner: approval of the 150 MHz point and of CMD11 for OS images, and the image classification for UHS-I.
3. Five image-level SD checks (`storage-read-native.py`, `storage-read-binding.py`, `sd-return-native.py`, `sd-return-host.py`, `sd-return-continuity.py`) are stale models of the current image and are waived in `checks/gate-inputs.json`; rewrite them against the image.
