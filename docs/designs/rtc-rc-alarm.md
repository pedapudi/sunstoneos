# RTC calendar and alarm while running from the internal RC oscillator

**Status: implemented; the RC source, calendar set, alarm and retention hold on the board; the read after the alarm is corrected and not yet run on the board.**

The board starts with the RTC selected to the internal RC oscillator (clock control 0x401c, source status 0, prescaler 0x0f, calibration clear, RC enabled in `+0x160`). The earlier read-only record measured one RTC second in about 1.1 s; two runs 70 s apart show 59 RTC seconds, so about 1.19 s. The rate is uncalibrated and not asserted. The external-source open and the calendar writer in `drivers/rtc/a733_rtc_initialize.c` accept only the external 32.768 kHz source, so they refuse this state. This design adds an RTC mode that writes the calendar and arms one alarm without writing any clock-source register.

## Vendor sequence and the diff of our code

The pinned `rtc-sunxi.c` settime and alarm paths and the manual (section 9.1.7) give the sequence.

| Step | Vendor | Service |
| --- | --- | --- |
| Set time | Wait until the calendar-write busy bits (control bits 8 and 7) clear, write the time register, wait, write the day register, wait; each wait ends with a 2 ms settle. | `a733_rtc_set_time` does the same: HMS, then DAY, each with the busy wait and the settle, through a pause that holds no lock. It then reads the calendar coherently and requires it to be neither before the request nor later than the request plus the elapsed time. |
| Source | The vendor probe selects and gates the oscillator. | No source write. The RC mode accepts the inherited normal-RC state only if control, status, prescaler, calibration and the RC-enable bit are as observed and stay stable for the life of the open. Only the calendar and alarm registers (`+0x10`, `+0x14`, `+0x20` to `+0x30`) are writable through the service. |
| Alarm | Program the alarm day and time, enable the alarm and its interrupt; in the handler read the status, clear the enable, write the status to acknowledge. | The same in the driver. The handler of the OS only masks INTID 228 and wakes the owner task, which confirms the line masked, calls `a733_rtc_irq`, clears the distributor pending state, restores the saved alarm registers (`a733_rtc_finish`) and wakes the application. |

The service reads the calendar through the driver's coherent read (two matching reads of the day and time registers).

## Prediction for the board run

The application `apps/rtc-alarm` opens the RTC, reads the calendar the RTC holds (the retention datum), sets 2026-11-12 04:00:00 (day 20770), reads it back, arms an alarm three RTC seconds ahead, receives it, reads the calendar again and closes.

- The source is the normal RC (source 2) with control 0x401c.
- The calendar read after the set is 04:00:00 to 04:00:02.
- One alarm interrupt arrives. Three RTC seconds take between 2.5 and 6 s of system-counter time. The board shows the interrupt arriving when the calendar reaches the target second plus one, so the wait is between 3 and 4 RTC seconds, 4.0 and 4.7 s in the two runs. The result states the measured RTC second; it does not assert 32.768 kHz accuracy.
- The owner finishes the alarm within 5 ms of the interrupt and the final calendar is at or after the alarm target.
- A second run of the same image reads, at its start, the calendar of the first run's end plus the real time between the runs divided by the measured RTC second, which shows that the calendar survives the return to the host.

Time budget: boot and admission about 1.2 s, wait for the CPU clock owner until about 9 s, application about 4 s (set 5 ms, alarm 3.3 s), against a 60 s session, a 55 s finish bound, a 30 s run bound and a 2 s progress window (the application reports progress at most 0.5 s apart). If the alarm interrupt never arrives, the owner cancels the alarm at 8 s and the record shows a timeout, so an overrun is a decoded result, not a watchdog reset.

## The board fits a 32.768 kHz crystal; proposal to select it

The V1.2 schematic, sheet 8 ("08_CORE1"), connects crystal Y2 (32.768 kHz, 12 pF load, 20 ppm) between the SoC pins X32KIN (W37) and X32KOUT (V37), with 12 pF C0G capacitors C151 and C158. The pinned device tree feeds the RTC and the other blocks from `rtc_ccu` `CLK_OSC32K` as the low-speed clock (`losc`). The board therefore fits the crystal, and the RC source it runs from is the boot default, not a board limitation. A crystal source would give 20 ppm instead of the measured error of about 16%.

Proposed design (needs the coordinator's approval before any run, because it writes the clock-source selection): reuse `source()` of `drivers/rtc/a733_rtc_initialize.c`, which the manual's section 9.1.7 sequence already defines: select the external oscillator (control bit 0) with the gate (bit 4), poll the oscillator health (status bit 2 clear, four consecutive samples), then accept the external state `(control & 0x193) == 0x11, (status & 7) == 1`. After the switch the existing `a733_rtc_open` and `a733_rtc_set_time` paths apply unchanged. The run reads the calendar before and after, which the switch must preserve, then measures one RTC second against the system counter over ten seconds. The failure case (crystal not oscillating) leaves the RC selected and reports the health bits. The register writes are the control word at offset 0 only, with the key field.

## Crystal selection: vendor sequence and the diff of our code

The manual (section 9.1.7.1 and 9.1.7.2) and `ccu-sun60iw2-rtc.c` define the sequence that `source()` in `drivers/rtc/a733_rtc_initialize.c` follows. `a733_rtc_select_crystal` applies it to an instance that is already open on the RC.

| Step | Vendor and manual | Service |
| --- | --- | --- |
| 1 | Auto-switch field (control bits 15:14) = 01 and the crystal gate (bit 4) set. | Both written with the key `0x16aa`, each with a readback of mask `0xc01f`. The board already holds both (control 0x401c), so the values do not change. |
| 2 | The vendor polls the oscillator status; the manual defines health as status bit 2 clear. | Four consecutive samples with bit 2 clear, 3 us apart, within 1 s. |
| 3 | Select the external source (bits 1:0 = 01). | `control & 3 = 1`, written with the key. |
| 4 | Clear the source-switch pending bit (status bit 1). | Written only if set. |
| 5 | Settle: `control & 0x193 == 0x11` and `status & 7 == 1`. | Polled at 1 ms for at most 1 s. |
| Not healthy or not settled | Not specified. | The control word is written back to its value on entry, the RC stays selected, and the record carries the samples, the longest healthy run and both control and status words (result 2). |

The service writes only `+0x00` and `+0x04`, and only while the owner runs the selection.

### Prediction for the board run

`apps/rtc-crystal` opens on the RC, times five RTC seconds, selects the crystal, and times ten RTC seconds, then repeats the calendar set, three-second alarm and read.

- The open reports source 2 (control 0x401c); five RC seconds take 1.0 to 1.4 s each (about 1.19 s).
- The selection ends with result 1: control & 0x193 == 0x11, status & 7 == 1, a healthy run of 4 samples. It issues two key writes for the gate fields, one source write, and a status write only if the pending bit is set.
- The calendar read before and after the switch differs by 0 or 1 s.
- Ten RTC seconds on the crystal take 10.000000 s of system counter time within 50 ppm. Resolution: each edge is found by 20 us sleeps plus wake and read latency, under 50 us per edge, so the difference of the two edges is known to under 100 us, 10 ppm over ten seconds. The reference is the 24 MHz system counter, not a calibrated standard, so the 50 ppm allows for its own tolerance and for the crystal's 20 ppm.
- If the crystal fails to start, the record shows result 2 with the health bits, and the RTC stays on the RC.

Time budget: boot and the wait for the CPU clock owner about 9 s, the RC measurement about 7 s (a 1.4 s edge search and five seconds), the selection under 10 ms, the crystal measurement about 11 s, the alarm about 4 s: about 31 s of application against a 45 s run bound, a 60 s session and a 55 s finish bound. The application reports progress between steps, at most 11 s apart; the progress window is 2 s, so the long measurements run inside the owner while the application waits in `ss_rtc_wait` (a supervised wait) and reports progress every 0.5 s through the alarm loop only; the measurement waits use the same wait call.
