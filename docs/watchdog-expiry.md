# Prepare protected watchdog enable and expiry trials

**Deferred.** No watchdog reset trial is scheduled. The required physical boot
connection is unavailable, and USB power-control investigation has stopped.
This preserved procedure does not block unrelated software development or
separately reviewed supervised RAM-only tests.

The SRAM watchdog diagnostic implements separate enable/disarm and expiry
requests. Its instruction-model and host-protocol checks pass. Neither phase
has executed on the physical board. The existing configuration/restoration
trial remains valid and does not need repeating.

The coordinator must first confirm a stable BOOT-to-GND bridge and obtain a
successful [physical input inspection](../tools/check-fel-input.py). Keep the
bridge attached through both phases and any reset. The [board wiring record](board-wiring.md)
identifies these pads. A low sample proves the observed input level; it does not
establish that the connection will remain low across reset.

## Execute one explicitly selected phase

Build the isolated image from its recorded sources:

```sh
bash tools/build-watchdog-expiry.sh
```

The build uses Clang 21 and the cached linker. Its default output directory is
`~/.cache/sunstoneos/build/watchdog-expiry`; `SUNSTONE_WATCHDOG_BUILD` can select
another directory. The image is 1,388 bytes with no static data or BSS. Its
SHA-256 is
`fe0735093d01cdf115cc9e605c2e18f64d2bd65042526d66c33348148e388993`.
The physical harness accepts only these reviewed bytes, the pinned FEL tool,
and the captured physical ROM identified in [watchdog.md](watchdog.md).

The coordinator runs enable/disarm first, after reviewing a queue proposal:

```sh
python3 tools/check-watchdog-expiry.py --phase arm-disarm \
  --confirm-bridge-stable \
  --input-evidence /absolute/path/fel-input/session.json \
  --fel "$HOME/.cache/sunstoneos/build/sunxi-a733/sunxi-fel" \
  --rom "$HOME/.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.bin" \
  --evidence /absolute/path/watchdog-arm-disarm
```

This phase prepares ROM redirection, selects a four-second watchdog interval,
enables and feeds the timer, then immediately disables it and restores saved
registers. The harness checks the diagnostic result, all saved registers, and
SRAM restoration. A failed disable can still cause expiry, so the bridge is
required for this phase too.

Expiry requires the successful enable/disarm session record for the same image
and live USB instance. Invoke it separately:

```sh
python3 tools/check-watchdog-expiry.py --phase expiry \
  --confirm-bridge-stable \
  --input-evidence /absolute/path/fel-input/session.json \
  --arm-disarm-evidence /absolute/path/watchdog-arm-disarm/session.json \
  --fel "$HOME/.cache/sunstoneos/build/sunxi-a733/sunxi-fel" \
  --rom "$HOME/.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.bin" \
  --evidence /absolute/path/watchdog-expiry
```

The diagnostic enables the same interval and enters an infinite branch with
asynchronous exceptions, IRQ, and FIQ masked. It performs no subsequent feed or
software reset. The independent watchdog must end execution. The host waits
at most 20 seconds for a distinct, accessible ROM instance on the same physical
port. No requests target the old instance during that wait.

The runner verifies the A733 response and relevant physical ROM bytes after
reconnection. It records host elapsed time and returned register values. Its
acceptance window is 2–20 seconds from the execution request, including ROM
initialization and USB enumeration. This broad window detects an immediate
unexpected restart or absent return; it is not a watchdog clock measurement.

A successful observation establishes protected reset and live ROM return under
these test conditions. The diagnostic stage is in SRAM, which ROM clears.
There is no retained final-stage marker or qualified reset-cause register, so
this result alone cannot attribute the reset conclusively to watchdog expiry.
It also does not qualify recovery while a peripheral bus transaction is stalled.
Treat DDR as uninitialized after the reset.

## Preserve ownership and host backups

The runner acquires `/tmp/mimus-board.lock`, checks USB host health, requires the
port's reconnect access rule, and refuses quarantined instances. It compares
the reset prefix and boot-input routine with the physical ROM capture before
execution. Both phases reserve the shared USB re-enumeration budget because a failed
disable can cause a reset. Successful enable/disarm records zero resets.
Workers submit proposals; only the coordinator executes either phase.

Before modifying SRAM or registers, the harness saves SRAM to host files and
records register values. It flushes those backups and the execution-intent
record before issuing the single execution request. The recorded request
contains a nonzero random nonce and its complement. The SRAM entry accepts
only the diagnostic magic, a recognized command, and a matching nonce pair.
The nonce identifies the request; it is not an authentication mechanism.

Both phases refuse an enabled watchdog, active CPU redirect, or active alternate
ROM dispatcher. The host checks the BOOT input immediately before execution,
and the SRAM diagnostic samples it four times before arming. The diagnostic
installs the ROM entry before activating its redirect flag. It verifies the
watchdog reset policy before enabling the timer.

Enable/disarm restores the original watchdog mode, configuration, reset output,
bootloader RTC flag, and CPU redirect entry. If disabling the watchdog fails,
the diagnostic retains recovery state. It does not clear the redirect while
timer state remains uncertain. The SRAM wrapper preserves the FEL caller's
registers, stack, and CPSR when the diagnostic returns normally.

After a distinct ROM instance appears, the expiry harness first records the
returned state. It refuses restoration if the watchdog remains enabled, the
alternate dispatcher changed, or the CPU redirect has an unexpected owner.
For recognized state, it clears and verifies the redirect flag before restoring
the entry, watchdog policy, and saved bootloader RTC flag. It then compares all
saved registers. Reset retention remains a physical observation, not an assumed
property of these registers.

The harness saves the fresh ROM instance's SRAM before using its helpers and
restores those bytes afterward. It never writes the prior instance's SRAM over
a newly initialized ROM session. Neither phase accesses DDR or storage, and
neither uses an additional RTC word as a result marker.

Any failed transaction, disappearance outside the planned reconnect wait,
failed acceptance condition, or unknown restoration state stops the run. The
runner saves the failure and quarantines an instance whose state may have
changed. It sends no automatic cleanup requests after a transport failure and
never retries execution or reset. Inspect the saved state and actual process
handles before releasing a queue claim. Host timeouts do not prove the watchdog
or target process stopped.

## Focused checks cover instructions and host request boundaries

The changed behavior is timer enable/disarm and an intentional wait for expiry.
The credible risks are enabling without protected recovery, losing restoration
state, or sending further requests after an uncertain reset. The smallest
checks execute the linked diagnostic in an instruction model and exercise the
host protocol through injected transport callbacks.

```sh
"$HOME/.cache/sunstoneos/venv/bin/python" checks/watchdog-expiry.py \
  "$HOME/.cache/sunstoneos/build/watchdog-expiry/probe.bin" \
  "$HOME/.cache/sunstoneos/build/watchdog-expiry/probe.elf"
python3 checks/watchdog-expiry-host.py
```

The instruction model passed fifteen cases covering enable/disarm, the masked
expiry wait, invalid requests, existing owners, high boot input, ignored writes,
and failed restoration. It verifies caller state and stack guards for returning
paths. It models keyed MMIO behavior but does not model a physical watchdog
clock, reset, register retention, USB, or a stalled bus.

The host check passed both phase paths, upload corruption, and a failure at each
of the seven requests across those paths. It checks that failures produce no
later request, execution occurs once, and a durable-intent callback precedes it.
Separate cases check redirect restoration ordering, ownership refusal, and
same-instance prerequisite evidence. Two additional cases execute the main
runner with a fake ROM transport and malformed diagnostic results. They verify
reset-budget reservation, saved failure state, quarantine, and no later request.
These checks inject transport behavior;
they do not qualify the physical FEL transaction implementation or host failure
handling during a real reset.

Evidence, build inputs, and hashes are retained under
`~/.cache/sunstoneos/evidence/watchdog-expiry-20261016/`. The initial instruction
model incorrectly required a feed write to imply successful enabling even when
the test ignored the enable write. Correcting that model assertion required one
model rerun; the image was unchanged. Existing configuration, kernel, mimus,
NPU, GPU, and performance checks were not repeated.
