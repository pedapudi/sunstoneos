The returning RTC image performs two coherent calendar reads using the production driver.

One FreeRTOS task adopts an existing valid external 32768 Hz source and calendar. Before accessing RTC registers, it verifies that R_CCU gate/reset-release bits 0 and 16 are already set. It also requires interrupt 228 disabled, inactive and unpending. It refuses missing prerequisites without enabling the bus, releasing reset or acknowledging events. Its write callback rejects every RTC store.

The task waits at least 1.1 seconds between readings and accepts one or two calendar seconds of advancement, including midnight rollover. Counter initialization requires the existing 24 MHz architectural counter. A 1.5-second client deadline starts before gate, interrupt and driver work. The task yields during its wait, preserving scheduler interrupts and the accepted monitor's unchanged two-second recovery deadline. It reports source state, saved alarm target, decoded calendar readings and counter timestamps through the existing RAM log, then calls platform_exit. It does not initialize native USB.

Build and check from the repository root:

```
python3 apps/rtc-returning/build.py --evidence ~/.cache/sunstoneos/evidence/rtc-returning-20261018
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python apps/rtc-returning/check.py --evidence ~/.cache/sunstoneos/evidence/rtc-returning-20261018
```

The build compiles only this client and drivers/rtc/a733_rtc.c. It reuses fifteen accepted PMIC-probe objects: twelve returning platform objects and the counter, task and queue objects. It preserves baseline source evidence and host tools, compiler dependency snapshots, object identities and invocation records. The focused check executes the linked task-construction path, client, counter conversion and RTC driver. Scheduler calls, architectural observations and register values are modeled. The check covers progression by one or two seconds, midnight rollover, gate/interrupt/source/calendar/event refusals, the client deadline, stalled calendar progression and source changes. Every case requires no peripheral writes; direct execution of the linked write callback also verifies refusal.

Physical execution remains pending coordinator admission. Refresh the selected board's identity and launcher/cache survey, and establish DDR continuity, secondaries OFF, exclusive RTC/R_CCU/source/calendar ownership and the existing monitor's RAM/GIC/timer ownership. The gate preflight establishes accessibility before any RTC register read. Source/calendar validity is determined by the actual driver; failure must preserve the report and must not trigger automatic initialization.

Use the unchanged accepted kernel runner with the frozen probe.bin and expected text `PASS native RTC coherent read progression`. Preserve normal monitor cleanup, a responsive new ROM instance and the copied log before releasing ownership. Stop at the first transport failure or disappearance. Physical review and queue admission must be supplied by the coordinator. This image qualifies only coherent calendar reads and observed advancement; alarm delivery, calendar writes, accuracy, retention and the complete queued RTC service remain separate work.
