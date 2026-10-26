# Resident CPU and accelerator performance service

Status: proposed software integration; physical cold-boot and runtime operating
point qualification remain prerequisites. The target is recorded resident OS
with eight recognition workers, CPU clock grants, performance commands and the
selected ShuffleNet NPU application. The GPU performance owner composes with
this profile when its reviewed implementation lands.

## Reuse the resident owners and command path

The thermal task already runs `os_cpu_clock_step` and `os_perf_step`; USB0
operation 8 publishes requests and copies the existing 192-byte report.
`apps/os/cpu.c` selects the performance boot sequence instead of the ordinary
startup-only CPU requests. Recognition waits for `os_cpu_clock_ready`.
The service defaults to HIGH; explicit LOW selects the vendor low points.
No task, service queue, session field or card extent is required.

Two build/host exclusions currently block this composition: the PERF+RESIDENT
CMake guard and the card producer's `perf_modes` rejection. Removing them alone
would leave a real NPU lifecycle defect. `apps/os/npu.c` bounds its finite work
at 60 seconds and passes that bound to the startup provider. The stopped
bootstrap remains available to performance requests, but the provider's
`now`, register-lock entry, lock completion and unlock checks use the expired
startup deadline. Retunes therefore stop working after that finite workload.

## Separate finite work from stopped provider operations

Keep the application's work deadline, startup and normal-restart deadline,
thermal stop bound and immutable completion report. Local stop retains the
NPU row, clock, rail, PCK and memory ownership for the resident lifetime.
Those tokens do not expire merely because the application's work ends.

Add one deadline-taking stopped-retune entry to the existing startup provider:
`a733_npu_startup_retune_until(startup, mhz, deadline_us)`. The existing retune
entry remains a wrapper using its original startup deadline. The new entry
uses a command-local deadline throughout its time, lock and completion checks;
it never changes the original configuration or extends an in-flight command.
The existing register sequence, five PLL writes and bounded lock observation
remain the hardware implementation.

The caller must be the bound bootstrap task in STOPPED with the register
window closed. Before even same-rate success, verify fresh monotonic time,
retained clock/rail/PCK tokens, the registered domain-4 row and the stopped
hardware state. Preserve `last_now` across operations. Expired or invalid
commands refuse before writes. Lost ownership or uncertainty after a write
retains the provider and ends the board lifetime. This entry permits no
restart, new inference, resource release or fault recovery.

The thermal owner passes the engine's existing absolute RATE deadline through
`os_npu_perf_retune`. The deadline is published before the request's release
store and consumed after its acquire load. Keep the request occupied until
the matching answer is published; clearing it on pickup must not permit the
producer to overwrite the deadline. There is one producer and at most one
outstanding command. The bootstrap checks board liveness, live stopped proof
and the fixed deadline before invoking the provider. An engine timeout ends
the board lifetime and cannot authorize another command.

## Bind runtime authority to each cold boot

The existing cold policy binds the board SID, actual image and monitor,
canonical session, recognition capability and CPU maxima. Its digest remains
the CPU grant's approval identity. Stage 2 stamps fresh board/grant epochs and
the slot generation before authenticating the complete session. The existing
904- and 1,176-byte CPU and CPU+NPU session forms remain adequate.

Add an explicit performance section to that policy. It binds the linked boot
mode and operation 8, the owned blocks, vendor high/low points and the CPU
change budget per boot. Runtime-transition evidence must cover thermal
interruption, retained failure and watchdog recovery. A startup-only policy
cannot satisfy this section, and supervised per-epoch approval cannot replace
cold authority. The producer freezes these references with the existing
policy and validates the actual linked service capability.

NPU cold authority must bind the actual boot rate and authorize stopped PLL
retunes between 492 and 852 MHz at 800 mV on a supported bin. The finite NPU
work record still binds the selected catalogue, owner, pool and 60-second
budget. GPU composition additionally binds its linked high/low capability and
cold combined-PLL conditions; a fixed-point GPU record does not establish
runtime-transition authority. Model fixtures remain explicitly nondeployable.

## Keep finite grants and thermal precedence

The CPU grant remains at most 64 transitions per boot. Each requested CPU
domain change consumes a change; admission reserves two for thermal restores.
The policy distinguishes this authority budget from operation deadlines and
resident service lifetime. Budget exhaustion refuses further CPU-changing
switches. It does not reset the budget, renew authority or end reporting.
Thermal lowering of owned CPU points remains available; a restore without
remaining authority leaves the lowered point in place.

The sole thermal owner admits switches only with fresh NORMAL readings.
THROTTLE cancels the remaining plan after the outstanding bounded owner action
resolves; CRITICAL ends the board lifetime. GPU thermal handling remains with
its existing owner. No automatic rollback writes follow an uncertain failure.
An NPU retune keeps its module gated and resets asserted throughout.

USB switches already require the current board epoch. The cold-entry epoch
prevents an old command from authorizing writes after reboot. The thermal
service's existing heartbeat covers performance-owner progress; its fixed
transition deadlines detect a stalled NPU bootstrap even after the finite
client's normal completion exempts that client from heartbeat requirements.
An uncertain transition publishes failure, calls `board_fail` through the
thermal owner, stops watchdog renewal and retains resources for qualified
cold recovery. Idle service operation requires no artificial NPU heartbeat.

## Ownership, costs and focused validation

The resident integration owns `card.py`, resident-specific admission and host
fixtures, the PERF+RESIDENT guard, and the NPU stopped-retune deadline path.
The GPU integration owns `perf.c/.h`, the shared mode engine, GPU transition
providers and GPU capability guards. It will apply the agreed deadline
argument at the NPU call site; shared changes are integrated sequentially.

The runtime addition is one deadline publication per NPU retune and constant
ownership/time checks per command. It adds no work per inference, frame or
idle tick, and no new task, buffer copy or repeated model authentication.
Record actual linked code/data costs and preserve fixed CODE, pool and
32 KiB monitor bounds. No stage-2 sequence or session-layout change is planned.

Focused checks cover a stopped retune after 60 seconds, stale deadlines,
same-rate ownership checks, mailbox exclusion and retained post-write failure.
Native resident checks exercise operation 8's epoch and report behavior,
finite CPU budget refusal, thermal precedence and terminal watchdog health.
The actual card producer must bind runtime authority and reject wrong boot
mode, points, image/SID, budget or startup-only evidence. Reuse the unchanged
card generation/stamping and arithmetic evidence. Build the complete intended
resident composition once the GPU source is ready; no hardware access or
inference replay is part of this work.
