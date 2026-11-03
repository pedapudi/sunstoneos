# Resident CPU and accelerator performance service

Status: **implemented**; **host-tested** in the recorded resident OS with eight recognition workers, admitted Cortex-A76 cores, CPU clock grants, GPU and ShuffleNet model 3. The performance service defaults to HIGH. Physical cold boot, live operating-point transitions and recovery are **open**. The cold-policy performance section described below is **open**; its binding in the card producers is not confirmed in the tree. Host results are in [the records](../evidence/resident-performance.jsonl).

## Reuse of the resident owners and command path

The thermal task runs `os_cpu_clock_step` and `os_perf_step`; USB0 operation 8 publishes requests and copies the 192-byte report ([performance-modes.md](performance-modes.md)). `apps/os/cpu.c` selects the performance boot sequence instead of the startup-only CPU requests, and recognition waits for `os_cpu_clock_ready`. The service defaults to HIGH; an explicit LOW selects the vendor low points. No task, service queue, session field or card extent is required.

The resident build admits the performance service. Card preparation requires runtime authority in addition to cold-start authority. The NPU application bounds its finite work at 60 seconds; its stopped provider accepts fresh command deadlines after that work ends and retains the ownership needed to serve those commands for the resident lifetime.

## Finite work and stopped provider operations

The application keeps its work deadline, startup and normal-restart deadline, thermal stop bound and immutable completion report. Local stop retains the NPU row, clock, rail, PCK and memory ownership for the resident lifetime; those tokens do not expire when the application's work ends.

The startup provider exposes the stopped-retune entry `a733_npu_startup_retune_until(startup, mhz, deadline_us)`. The older retune entry is a wrapper that uses the original startup deadline. The `_until` entry uses a command-local deadline throughout its time, lock and completion checks; it never changes the original configuration or extends an in-flight command. The register sequence is five PLL writes and a bounded lock observation.

The caller must be the bound bootstrap task in STOPPED with the register window closed. Before even a same-rate success, the provider verifies fresh monotonic time, retained clock, rail and PCK tokens, the registered domain-4 row and the stopped hardware state. It preserves `last_now` across operations. Expired or invalid commands refuse before writes. Lost ownership or uncertainty after a write retains the provider and ends the board lifetime. The entry permits no restart, new inference, resource release or fault recovery.

The thermal owner passes the engine's absolute RATE deadline through `os_npu_perf_retune`. The deadline is published before the request's release store and consumed after its acquire load. The request stays occupied until the matching answer is published, so the producer cannot overwrite the deadline during execution; there is one producer and at most one outstanding command. The bootstrap checks board liveness, live stopped proof and the fixed deadline before invoking the provider. An engine timeout ends the board lifetime and cannot authorize another command.

## Runtime authority bound to each cold boot

The cold policy binds the board SID, actual image and monitor, canonical session, recognition capability and CPU maxima. Its digest is the CPU grant's approval identity. Stage 2 stamps fresh board and grant epochs and the slot generation before authenticating the complete session. The 904- and 1,176-byte CPU and CPU+NPU session forms are adequate.

A performance section in that policy binds the linked boot mode and operation 8, the owned blocks, the vendor high and low points and the CPU change budget per boot. Runtime-transition evidence must cover thermal interruption, retained failure and watchdog recovery. A startup-only policy cannot satisfy this section, and supervised per-epoch approval cannot replace cold authority. The producer freezes these references with the policy and validates the actual linked service capability. It reads all points as `[MHz, mV]` pairs from the unique linked mode table and checks the GPU capability against that row. GPU runtime evidence must grant exclusive DCDC4 control at 800 to 960 mV, using the GPU performance provider's authority condition. Combined GPU/NPU runtime transitions need a separate `concurrent-gpu-npu-runtime-cross-pll-qualified` condition bound to those points; cold startup qualification at one fixed pair cannot satisfy it.

NPU cold authority binds the actual boot rate and authorizes stopped PLL retunes between 492 and 852 MHz at 800 mV on a supported bin. The finite NPU work record binds the selected catalogue, owner, pool and 60-second budget. GPU composition additionally binds its linked high/low capability and cold combined-PLL conditions; a fixed-point GPU record does not establish runtime-transition authority. Model fixtures are nondeployable.

## Finite grants and thermal precedence

The CPU grant allows at most 64 transitions per boot. Each requested CPU domain change consumes a change, and admission reserves two for thermal restores. This authority budget is distinct from operation deadlines and the service's lifetime. Budget exhaustion refuses further CPU-changing switches; it does not reset the budget, renew authority or end reporting. Thermal lowering of owned CPU points stays available; a restore without remaining authority leaves the lowered point in place.

The thermal owner admits switches only with fresh NORMAL readings. THROTTLE cancels the remaining plan after the outstanding bounded owner action resolves; CRITICAL ends the board lifetime. GPU thermal handling stays with the GPU owner. No automatic rollback writes follow an uncertain failure. An NPU retune keeps its module gated and resets asserted throughout.

USB switches require the current board epoch, and the cold-entry epoch prevents an old command from authorizing writes after reboot. The thermal service's heartbeat covers performance-owner progress; its fixed transition deadlines detect a stalled NPU bootstrap even after the finite client's normal completion exempts that client from heartbeat requirements. An uncertain transition publishes failure, calls `board_fail` through the thermal owner, stops watchdog renewal and retains resources for qualified cold recovery. Idle service operation needs no artificial NPU heartbeat.

## Qualified figures

All figures are host or modeled-native results (record `resident-performance-host-validation`).

| Figure | Value |
| --- | --- |
| Resident image growth over the fixed-point CPU/GPU/NPU image | 28,512 CODE bytes, 1,536 BSS bytes |
| CODE free within the 2 MiB mapping | 732,848 bytes |
| Device pool headroom before the uncached pool | 888,832 bytes |
| Reused monitor | 32,456 bytes of 32 KiB |
| Session | 1,176 bytes, four card extents |
| Modeled 852-to-492 MHz retune after 70 s | 5 writes, 65 register reads, 11 clock reads, 236 µs |
| Same-rate success | 0 writes, 7 register reads, 5 clock reads |
| Cases | 13 provider, 21 producer authority/admission, 6 native, plus the OS mailbox fixture |

No task, stack, heap allocation or model copy is added, and inference and idle ticks incur no new retune work. `checks/os-npu-perf-mailbox.c` includes the actual `apps/os/npu.c` and models only scheduler calls and the provider answer; the record gives the compile recipe.

## Open work

1. Physical runtime switching, thermal recovery and cold retirement with the bound evidence above.
2. Bind the performance section of the cold policy in the card producers.
