# Resident CPU and accelerator performance service

Status: implemented and checked in the recorded resident OS with eight
recognition workers, admitted Cortex-A76 cores, CPU clock grants, GPU and
ShuffleNet model 3. The performance service defaults to HIGH. Physical cold-boot,
live operating-point transitions and recovery remain unqualified.

## Reuse the resident owners and command path

The thermal task already runs `os_cpu_clock_step` and `os_perf_step`; USB0
operation 8 publishes requests and copies the existing 192-byte report.
`apps/os/cpu.c` selects the performance boot sequence instead of the ordinary
startup-only CPU requests. Recognition waits for `os_cpu_clock_ready`.
The service defaults to HIGH; explicit LOW selects the vendor low points.
No task, service queue, session field or card extent is required.

The resident build admits the performance service. Card preparation requires
runtime authority in addition to cold-start authority. The NPU application
still bounds its finite work at 60 seconds; its stopped provider accepts fresh
command deadlines after that work ends. The provider retains the ownership
needed to serve those commands for the resident lifetime.

## Separate finite work from stopped provider operations

The application keeps its work deadline, startup and normal-restart deadline,
thermal stop bound and immutable completion report. Local stop retains the
NPU row, clock, rail, PCK and memory ownership for the resident lifetime.
Those tokens do not expire merely because the application's work ends.

The existing startup provider exposes the stopped-retune entry
`a733_npu_startup_retune_until(startup, mhz, deadline_us)`. The existing retune
entry remains a wrapper using its original startup deadline. The new entry
uses a command-local deadline throughout its time, lock and completion checks;
it never changes the original configuration or extends an in-flight command.
The existing register sequence, five PLL writes and bounded lock observation
remain the hardware implementation.

The caller must be the bound bootstrap task in STOPPED with the register
window closed. Before even same-rate success, the provider verifies fresh monotonic time,
retained clock/rail/PCK tokens, the registered domain-4 row and the stopped
hardware state. It preserves `last_now` across operations. Expired or invalid
commands refuse before writes. Lost ownership or uncertainty after a write
retains the provider and ends the board lifetime. This entry permits no
restart, new inference, resource release or fault recovery.

The thermal owner passes the engine's existing absolute RATE deadline through
`os_npu_perf_retune`. The deadline is published before the request's release
store and consumed after its acquire load. The request remains occupied until
the matching answer is published, preventing the producer from overwriting
the deadline during execution. There is one producer and at most one
outstanding command. The bootstrap checks board liveness, live stopped proof
and the fixed deadline before invoking the provider. An engine timeout ends
the board lifetime and cannot authorize another command.

## Bind runtime authority to each cold boot

The existing cold policy binds the board SID, actual image and monitor,
canonical session, recognition capability and CPU maxima. Its digest remains
the CPU grant's approval identity. Stage 2 stamps fresh board/grant epochs and
the slot generation before authenticating the complete session. The existing
904- and 1,176-byte CPU and CPU+NPU session forms remain adequate.

An explicit performance section in that policy binds the linked boot
mode and operation 8, the owned blocks, vendor high/low points and the CPU
change budget per boot. Runtime-transition evidence must cover thermal
interruption, retained failure and watchdog recovery. A startup-only policy
cannot satisfy this section, and supervised per-epoch approval cannot replace
cold authority. The producer freezes these references with the existing
policy and validates the actual linked service capability. The producer reads
all points as `[MHz, mV]` pairs from the unique linked mode table. It checks the GPU capability
against that row. GPU runtime evidence must also grant exclusive DCDC4 control
at 800–960 mV, using the GPU performance provider's existing authority condition.
Combined GPU/NPU runtime transitions require a separate
`concurrent-gpu-npu-runtime-cross-pll-qualified` condition bound to those points.
Cold startup qualification at one fixed pair cannot satisfy that condition.

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

## Bounds and software validation

The complete resident image adds 28,512 allocated CODE bytes and 1,536 BSS
bytes relative to the accepted fixed-point resident CPU/GPU/NPU image.
Initialized data remains 1,118,048 bytes, including the NPU payloads in
non-executable RAM. CODE has 732,848 bytes free within its existing 2 MiB
mapping. The write-back device pool still ends at `0x41b27000`, leaving
888,832 bytes before the uncached pool. The reused monitor is 32,456 bytes
within its fixed 32 KiB limit. The session remains 1,176 bytes with four card
extents. No task, stack, heap allocation or model copy was added.

The stopped NPU command adds an eight-byte deadline mailbox and constant
ownership/time checks. A modeled 852-to-492 MHz retune after 70 seconds
performs the existing five writes, 65 register reads and 11 validated clock
reads in 236 modeled microseconds. Same-rate success performs no writes,
seven register reads and five validated clock reads. These counts include
ownership checks; modeled elapsed time does not establish physical latency.
Inference and idle ticks incur no new retune work.

Thirteen selected provider cases cover late lifetime, same-rate ownership,
expired commands, busy locks and retained failure, including expiry during
PLL completion. The actual OS mailbox fixture covers queued/executing
exclusion and matching answers. The final resident card producer passes
21 focused authority/admission cases. Six linked native cases cover resident
admission and operation 8 after 70 seconds, including stale epochs and an
occupied mailbox. Those native cases model the thermal task's publication;
they do not claim a completed hardware transition.

The accepted GPU service, shared-engine and CPU/NPU owner checks supply the
transition, exhausted-budget and thermal-interruption evidence. Their reviewed
sources are integrated unchanged. Cold monitor, card generation/stamping and
recognition arithmetic evidence are reused without replay. All generated card
records are explicitly modeled and nondeployable. Physical runtime switching,
thermal recovery and cold retirement still require the bound evidence described
above.

## Reproduce the stopped-retune mailbox check

From the repository root, use an existing configured NPU OS build for its
FreeRTOS and application include paths. The host fixture includes the actual
OS NPU source and models only scheduler calls and the provider answer.
`BUILD` names that build directory; `OUTPUT` names a new executable outside
the repository. This check does not build or run an inference image.

```sh
python3 - "$BUILD" "$OUTPUT" <<'PYTHON'
import json, shlex, subprocess, sys
from pathlib import Path
sys.path.insert(0, 'checks')
from harness import CLANG, SANITIZE, environment
root = Path.cwd()
rows = json.loads((Path(sys.argv[1]) / 'compile_commands.json').read_text())
row = next(r for r in rows if r['file'].endswith('/apps/os/npu.c'))
source_root = str(Path(row['file']).parents[2])
includes = [a.replace(source_root, str(root)) for a in shlex.split(row['command'])
            if a.startswith('-I') and 'newlib' not in a]
flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections']
defines = ['SUNSTONE_BOARD_SUPERVISED=1', 'SUNSTONE_BOARD_RESIDENT=1',
           'SUNSTONE_A733_OS_NPU=1', 'SUNSTONE_A733_OS_PERF=1', 'OS_PERF_BOOT_MODE=1']
subprocess.run([str(CLANG), *flags, *SANITIZE, *includes,
                *['-D' + d for d in defines], 'checks/os-npu-perf-mailbox.c',
                '-o', sys.argv[2]], check=True, env=environment())
subprocess.run([str(Path(sys.argv[2]).resolve())], check=True, env=environment())
PYTHON
```
