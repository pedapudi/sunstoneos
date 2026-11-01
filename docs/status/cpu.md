# CPU, interrupts and scheduling

**Status: implemented.**

Eight-core FreeRTOS startup, GIC-600 handling,
cache publication, timer and inter-core interrupts, task context restoration,
CPU clock ownership and bounded normal return have software evidence.
Qualification is limited to the frozen workloads below.

**Board-qualified scope:** The returning workload runs 32 floating-point rounds
per core across six Cortex-A55 and two Cortex-A76 cores. It demonstrates
background scheduling, inter-core interrupts, per-core timer delivery, seven
independently observed secondary OFF states and restoration of saved RAM and
control registers. One eight-worker CPU-only mimus input matches its 90-frame
reference at the bin-4 top operating points. A corrected reliability workload
completes 4096 rounds in 4096 ticks on all eight pinned CPUs, with independently
matching payload digests, seven secondaries off and WDT0 disarmed. Three
consecutive returning audio, memory and audio controls on port 3-1 normalize
all seven secondary policies from dynamic OFF to static OFF, retain matching
RTC summaries and return normally without a replug.

Evidence paths are relative to `~/.cache/sunstoneos/evidence/`:

- `processor-completion-20261024/port-3-1/current-cpu-root-review/runtime/session.json`
  records the current fault-capture workload and 18 MiB restoration.
- `processor-completion-20261024/port-3-1/cpu-local-timer/runtime/session.json`
  adds one timer interrupt per secondary and 32 CPU0 ticks. CPU0 owns the
  global FreeRTOS tick.
- `secondary-coherence-probe-20261018/physical-113/` records 224 ordered
  payload transfers and atomic increments across the seven secondaries.
- `sol-resume-20261028/` and [application evidence](../mimus-evidence.md)
  identify the exact CPU recognition input and restored return.
- `dev-runs-1031/3-1-cpu-return-211702/run/session.json` records the corrected
  hello-compute return with seven secondaries off.

The corrected reliability result is queue entry `1793513595977482947-679bf1ea`
under `~/.cache/sunstoneos/board-queue/entries/`. It binds the 224-byte returned
result to image SHA-256
`ad25bc86d87130038dfafddae2a63250c694e2db7f375d4d01371967d978c4cf`
and native fix `f2131e76`. This is one successful run; three-start repeatability
and full restoration qualification remain open.

The retained failed launch records RETAINED at `return-cpu.c:159` in
`cpu-reliability-1031/start-1/`. The later diagnostic identifies CPU-B N=77
readback after UPDATE clears while requesting N=39. Native adapters now complete
PLL writes, and the driver waits boot0's 20 microseconds between N and UPDATE.
The two preceding queue diagnostic entries retain the first setter and raw PLL
word. [The clock record](../cpu-clock.md) states the correction's scope.
The first hello input finishes while the owner is REQUEST; the return check
requires FOLLOW and refuses at
`dev-runs-1031/3-1-cpu-return-204119/run/watchdog-report.json`.
The corrected hello result establishes only that control's scope.

Remaining work includes repeated cold startup, sustained scheduling, broader
coherence and per-core IRQ workloads, induced-fault recovery, idle/wake and
thermal transitions. The longer-recognition nonreturn has no recovered fault
location. Do not repeat it without a concrete correction or narrower hypothesis.
Eight-core cached images keep noncacheable table walks until the documented
core-6 walk measurement qualifies cacheable walks. WFI idle behavior has only
instruction-model evidence; retention and cluster power-down are incomplete.

The [startup](../cpu-start.md), [coherence](../cpu-coherence.md),
[SMP runtime](../smp-runtime.md), [CPU clocks](../cpu-clock.md) and
[reliability application](../../apps/cpu-runtime-qualification/RELIABILITY.md)
records define source and test scope. PMU empty/fixed regions, table-walk counts
and WFI counters have a single-core physical result in
`board-session-20261023/port-3-3/set5-bb4-run/`; broader event qualification remains.

## Returning probe entry

The shared returning-probe runtime normalizes acknowledged secondary OFF policies
to static OFF and disables each secondary's GIC wake. PWSR completion precedes
PWPR verification, under one 100 ms deadline and a finite sample cap. The inherited
policy appears in an RTC breadcrumb. CPU0, cluster, shared domains, rails and
clocks receive no normalization writes. Static OFF remains held until the ROM
reset request; later multicore consumers require the reviewed held-policy release path.
The [entry design](../designs/probe-entry-normalization.md) defines that contract.

Three independent reviewers approve the source and linked memory-test and audio
images. Forty-four distinct shared-runtime cases, sixteen focused audio cases,
the memory-test recovery checks and the ISP prefetch sanitizer case pass.
The full integration gate passes all central targets, twelve OS variants,
the core checks and 88 indexed host checks. The hello and mimus baseline images
also build. The check index verifies 805 checks, including 93 pure-host checks;
693 checks requiring external inputs remain pending in the full gate.

Raw evidence is under `~/.cache/sunstoneos/build/probe-shared-1101/`: reviews in
`reviews/`, retained-summary cases in `probe-breadcrumb-check/`, audio cases
in `audio-breadcrumb-focused/`, and the gate in `gates/gate-shared-probe-full/`.
The diagnostic summary update also passes the selected gate in
`gates/gate-policy-breadcrumb/`. The final audio and memory-test image hashes are
`3baaace90784c0bcd0c75929cbac03d4de1ca3c06b1338e7d745c1184b031227`
and `61d3d9582ec9846510ac2537d35df6938c7e0c01e60aae388c4583fc5ca2f4bf`.
The unchanged audio and scan-only memory images pass three consecutive queued
runs on port 3-1 with the ENODEV/ENOENT discovery fix from `6636cae8`:

- Audio control: `1793563765178868625-47ea9df5`.
- Scan-only memory probe: `1793563856677556459-2fc4fcf6`.
- Audio control: `1793563904446986983-31e882d1`.

All three results are `returned`, with no fault and WDT0 disarmed. Each RTC
summary has ESR `0xfe`, ELR `0x50505531` and FAR `0xfe`: every secondary inherits
policy `0x100` and completes static-OFF normalization. LAST is RETURN_ROM.
Audio matches its full 128-byte expectation. The memory payload matches the
`MRECORD1` substring; its complete record checksum, contract and pass status
also validate. No seed image is needed because the first run inherits dynamic
OFF. The queue ledger shows no intervening port-3-1 job; no replug is issued.
The decoded records and ledger excerpt are in `proof-fixed/`.

The second and third images inherit dynamic OFF again. Every queued image has
an intervening system-watchdog reset and DDR restoration. The exact reset or
software mechanism restoring dynamic OFF is unproved, so this evidence qualifies
repeated entry normalization, not static-policy continuity across preparation.
Camera reset and peripheral gate normalization remain with the camera stream.
