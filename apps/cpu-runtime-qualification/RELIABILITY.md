# Bounded scheduling and payload checks on all eight CPUs

Status: the CPU reliability application builds, passes its OS link check and
passes offline return-image preparation. Its scheduler model passes seven
cases. Image `02-reliability` ran on 3-3 and failed with the CPU clock
driver in RETAINED at `return-cpu.c:159`. Evidence is under
`~/.cache/sunstoneos/evidence/cpu-reliability-1031/start-1/`. Another worker owns
the defect. Physical startup repeatability, sustained operation, full restoration,
recurring local interrupts and idle/wake remain unqualified.

The application extends the existing finite CPU workload through the general
[application kit](../../include/sunstone_app.h). It uses the existing FreeRTOS
scheduler, CPU clock owner, thermal service, watchdog and returning lifetime.
It introduces no startup, interrupt-controller, clock or power sequence.
The [multicore reference comparison](../../docs/reference-backsolve-cpu.md)
and [return contract](../os/NORMAL-RETURN.md) describe those retained sequences.

One task runs on each CPU, including both Cortex-A76 cores. Each task completes
4,096 rounds. A round publishes 512 bytes to its next neighbor, checks the
saved floating-point register `d8` across `vTaskDelay(1)`, and verifies its
previous neighbor's payload. The existing assembly check also preserves and
compares integer register `x19`. It does not test every floating-point register.

Each CPU owns one payload slot. A release publication follows its ordinary
payload stores; its neighbor acquires that publication before reading them.
The neighbor releases a consumption acknowledgment after its final load.
The producer acquires that acknowledgment before reusing the slot.
Task notifications wake the relevant consumer or producer. Atomic predicates
handle notifications that arrive early or combine several events.

Every task writes its own result fields. Its final release acknowledgment
publishes the completed result and ends its payload writes. The worker then
notifies the main task and suspends. The existing highest-priority terminal
tasks and all-core capture barrier exclude subsequent kernel writes before
cache cleanup. A completion flag alone does not establish kernel quiescence.

The workload starts only after the CPU owner reports the existing vendor
bin-4 points: CPU-L at 1,794 MHz/1,000 mV and CPU-B at 2,002 MHz/1,050 mV.
The native clock record must still report those points at completion.
Thermal throttling ends the diagnostic through its first-failure path.
The clock owner retains its existing DSU policy and thermal protection.
No voltage table, grant or transition implementation changes.

At 1,000 ticks per second, the workload spans at least 4,096 tick increments,
nominally 4.096 seconds. The first increment can consume a partial tick.
The application bounds its work to 12 seconds from initialization, using
wrap-safe tick subtraction. The OS retains its independent counter-based
session deadline, five-second return reserve and host-armed watchdog.
The prepared image declares a 60-second session and watchdog mode `0xb1`.
A stopped scheduler tick therefore cannot extend the hardware lifetime.

The round budget is 32,768 floating-point checks and 16 MiB each of payload
writes and reads. Static payload storage occupies 5 KiB. Eight reserved worker
stacks occupy 64 KiB; the main task uses its separate 8 KiB manifest stack.
There is no workload allocation or new shared lock. Runtime duration and
throughput remain unmeasured on hardware.

The 224-byte result contains per-core rounds, unsigned payload sums and wait
counts, plus the core mask and elapsed ticks. The decoder independently
recomputes every expected sum. Wait counts may be zero and do not establish
interrupt counts. The image rejects unexpected console output explicitly;
its result travels through `ss_finish`.

Build from the repository root:

```sh
cpu_build="$HOME/.cache/sunstoneos/build/cpu-reliability-1031"
"$HOME/.cache/sunstoneos/bin/cfg.sh" "$PWD" "$cpu_build/reliability" \
  -DMIMUS_DIR= -DSUNSTONE_A733_OS=ON \
  -DSUNSTONE_APPLICATION=cpu-reliability \
  -DSUNSTONE_APPLICATION_cpu-reliability_ROOT="$PWD/apps/cpu-runtime-qualification"
cmake --build "$cpu_build/reliability" --target sunstone-a733-os -j8
```

Use a new build directory when reproducing these commands. No mimus sources
are needed. Artifact hashes, full coordinator commands and stage-specific
failure expectations are in the build directory's `README.md`.
Further launches remain held pending resolution of the recorded failure
and current runner admission.

The host check uses the real patched FreeRTOS kernel, task creation,
notifications, delays, interrupt frames and existing floating-point assembly.
OS admission, clock/thermal records, architectural registers and peripherals
are fixtures. Eight rounds exercise the same loop as the board image.
It stops at `ss_finish`; existing CPU-return checks cover the terminal barrier.

```sh
"$HOME/.cache/sunstoneos/bin/cfg.sh" "$PWD" "$cpu_build/scheduler-model" \
  -S "$PWD/checks/cpu-reliability-model" -DMIMUS_DIR=
cmake --build "$cpu_build/scheduler-model" --target a733-smp-cached-diagnostic -j8
"$HOME/.cache/sunstoneos/venv/bin/python3" -B checks/cpu-reliability.py \
  "$cpu_build/scheduler-model/a733-smp-cached-diagnostic.elf" \
  --out "$cpu_build/scheduler-check-reviewed"
```

Passing cases cover normal execution, delayed suspension, completion before
notification, payload corruption, a missing producer, wrong operating points
and thermal throttling. Both delayed-worker cases retain the worker at
completion and verify the immutable result. Decoder cases reject malformed
headers, too few ticks, incomplete rounds and wrong payload sums.
No model result establishes physical cache coherence or IRQ delivery timing.

The longer recognition failure remains unlocated. Its saved session is
`~/.cache/sunstoneos/evidence/sol-cpu-robustness-1031/recognition-return/session.json`.
It records no ROM return and no recovered native result. The separate
hello-compute clock race has a first-failure report and a corrected successful
return under `dev-runs-1031/3-1-cpu-return-211702/`. That control result cannot
explain the older recognition failure.

The build directory also contains a new longer-recording application image.
Its external mimus source derives from the pinned revision and changes only
the recording declarations in `inputs.json` and `manifest.json`.
Models and arithmetic sources remain unchanged. Recognition-specific host
helpers stay with that external application artifact. The proposed run tests
current first-failure observability and exact reference parity; it does not
assume the older failure had the control image's cause.
