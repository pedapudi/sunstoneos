# RTC calendar and alarm while running from the internal RC oscillator

**Status: implemented and host-tested; board run pending.**

The board starts with the RTC selected to the internal RC oscillator (clock control 0x401c, source status 0, prescaler 0x0f, calibration clear, RC enabled in `+0x160`). The earlier read-only record measured one RTC second in about 1.1 s. The external-source open and the calendar writer in `drivers/rtc/a733_rtc_initialize.c` accept only the external 32.768 kHz source, so they refuse this state. This design adds an RTC mode that writes the calendar and arms one alarm without writing any clock-source register.

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
- One alarm interrupt arrives. Three RTC seconds take between 2.5 and 4.5 s of system-counter time (about 3.3 s at the measured 1.1 s per RTC second). The result states the measured RTC second; it does not assert 32.768 kHz accuracy.
- The owner finishes the alarm within 5 ms of the interrupt and the final calendar is at or after the alarm target.
- A second run of the same image reads, at its start, the calendar of the first run's end plus the real time between the runs divided by the measured RTC second, which shows that the calendar survives the return to the host.

Time budget: boot and admission about 1.2 s, wait for the CPU clock owner until about 9 s, application about 4 s (set 5 ms, alarm 3.3 s), against a 60 s session, a 55 s finish bound, a 30 s run bound and a 2 s progress window (the application reports progress at most 0.5 s apart). If the alarm interrupt never arrives, the owner cancels the alarm at 8 s and the record shows a timeout, so an overrun is a decoded result, not a watchdog reset.
