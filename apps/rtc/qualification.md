# RTC alarm notifications require a live operation

The RTC driver accepts a deferred interrupt notification only while its alarm
is armed, owned, unacknowledged and free of an earlier IRQ failure. Successful
finish clears the alarm ownership flag after restoring and verifying the saved
disabled target. Failed operations retain their reservations and refuse later
IRQ calls before invoking any callback.

The caller remains one serialized owner task. It masks IRQ228 and confirms
that the GIC interrupt is inactive before each operation, as required by
the public driver contract. This change adds no ISR concurrency support.

The pinned vendor handler in `rtc-sunxi.c:sunxi_rtc_alarm0_isr` reads alarm
status, acknowledges a pending alarm and reports it to Linux. Its lifetime
depends on Linux interrupt registration. The FreeRTOS adapter additionally
checks its finite operation's lifetime before accessing those registers.
[The source record](sources.json) pins the GPL reference and its hash.
The source remains a development reference; the adapted driver runs on the CPU.

Run the focused host check with the project virtual environment:

```sh
~/.cache/sunstoneos/venv/bin/python3 -B checks/rtc-alarm-ownership.py \
  --output ~/.cache/sunstoneos/build/buses-rtc-1031/rtc-ownership-check
```

The check executes the production driver with address and undefined-behavior
sanitizers. It tests stale notifications after firing and cancellation, duplicate
notifications before finish, and failures at every callback in arm, IRQ and
finish. A refused notification must leave the callback count, alarm enable,
IRQ enable and pending event unchanged. The existing alarm and calendar checks
cover preserved behavior. The driver also compiles for freestanding AArch64.

These checks model register effects and monotonic time. Physical IRQ delivery,
oscillator accuracy, wakeup and retention remain unqualified. The returning RTC
images require the coordinator's current watchdog and launch admission before
they can supply physical evidence.
