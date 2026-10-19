This finite operation selects the existing external RTC source without setting a calendar target.

The new EL3 wrapper records actual CurrentEL, SCTLR_EL3 and SCR_EL3 before the accepted monitor masks SPIs. It reads privileged controls only after checking EL3. EL3 with stage 1 translation disabled accesses Secure physical space; the wrapper also requires cache-disabled EL3 and a two-security-state, affinity-routed GIC. It captures IRQ228's original enabled, pending and active state through that Secure view. The task requires this retained admission before any RTC write. The hook never assigns an interrupt group or changes SPI configuration. Its record is outside EL1-cleared BSS and is never written by EL1.

One FreeRTOS task validates the existing source/calendar, then calls the unchanged production initializer with SOURCE only and day/HMS zero. Its callback permits only keyed RTC control and exact source-switch W1C writes. Calendar, RC prescaler/calibration, DCXO and wake writes are prohibited. The operation has a 1.5-second deadline under the unchanged two-second returning monitor. The existing initializer may wait approximately one second. Successful external selection persists; there is no automatic RC rollback.

`RTC_SOURCE_OUTCOME` is 0 for completed initialization, 1 for harmless refusal before a hardware write, and 2 for uncertain mutation. Outcome 0 requires initialization stage 6, strict external source/status checks and a valid coherent inherited calendar. It does not prove external oscillation, calendar advancement, UTC correctness or accuracy. Outcome 1 may return normally. Any hardware write followed by failure, or a callback invariant violation, exits nonzero and retains ownership. The common marker `COMPLETE native RTC source result` is emitted only for normal completion/refusal. The accepted runner also requires terminal stage 5/status 0 and responsive ROM return.

Build and run the focused offline cases:

```
python3 apps/rtc-source-returning/build.py --operation source --evidence ~/.cache/sunstoneos/evidence/rtc-source-returning-20261018
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python apps/rtc-source-returning/check.py --operation source --evidence ~/.cache/sunstoneos/evidence/rtc-source-returning-20261018
```

The cases execute the linked device admission hook, task, counter and production driver/initializer. They cover original Secure IRQ state, retained admission across BSS clearing, pre-write refusals, successful source-only writes, partial mutation and failure to select external. Original monitor/scheduler/time/register effects are modeled. Existing monitor, read-only and calendar arithmetic evidence is reused.

The coordinator must refresh identity, DDR continuity and launcher/cache survey, and admit source-write permissions, fitted source, stable supplies, exclusion of all affected osc32k consumers and remote writers, RAM/GIC/timer ownership and recovery. NS reads do not establish peripheral write permissions. The copied PMIC host tree provides baseline context only; this is not an executable physical package until the coordinator freezes current runner/import/runtime-hashed dependencies and binds FEL, survey and continuity records. No ownership or physical-review declaration is supplied here.

Run the alarm operation only after source outcome 0/stage 6/source control and status are accepted and the returned state is independently admitted. Stop after the first transport failure or disappearance. Uncertain mutation requires the approved recovery; normal ROM return alone does not authorize warm reuse. No image/result DRAM restoration is claimed.
