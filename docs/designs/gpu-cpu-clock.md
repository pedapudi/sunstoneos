# GPU recognition with CPU operating-point control

Status: **host-tested**. Supervised recorded GPU recognition with the CPU clock owner and two A76 inference workers is implemented across the OS tree (`apps/os/gpu.c`, `apps/os/cpu.c`) and the mimus checkout (`recognition-dispatch.patch`, `prepare.py`), and passes its software checks. It uses `SUNSTONE_A733_OS_GPU` and `SUNSTONE_A733_OS_CPU_CLOCK`. Physical integrated GPU recognition is **open**; no hardware run is authorized by this design.

## Independent CPU and GPU clocks

The CPU clock owner ([apps/os/cpu.c](../../apps/os/cpu.c)), the thermal owner ([apps/os/thermal.c](../../apps/os/thermal.c)) and the GPU owner ([apps/os/gpu.c](../../apps/os/gpu.c)) use disjoint resources:

| Resource | CPU clock owner | GPU owner | Consequence |
| --- | --- | --- | --- |
| PLL and mux | CPU-L, CPU-B and DSU words at 0x08871000, 0x08872000 and 0x08873000 | PLL_GPU0 and GPU-local clock and reset controls | CPU transitions do not reprogram the GPU core clock |
| Supply | DCDC3 for CPU-B; DCDC5 for CPU-L and DSU | DCDC4 at 960 mV | Rail leases coexist with distinct owner identities |
| Shared PMIC | Requests through `providers.shared.pmic` | The same provider through the GPU supply consumer | The eight-slot request queue serializes complete transfers and keeps each request's owner and deadline |
| Shared clock sources | Leases add a stable oscillator reservation | The resource manager holds oscillator, PCK, fabric and local resources; its LOCAL set excludes the PERI0 PARENT lease | CPU changes alter neither the shared source configuration nor GPU fabric rates |
| Thermal policy | The thermal task lowers and restores CPU domains through the CPU driver | The recognizer closes and reopens a healthy GPU session at the layer boundary | One policy controls admission; neither owner releases uncertain work |

The paths are `drivers/clock/a733_cpu_clock.c`, `drivers/clock/a733_clock_leases.c`, `drivers/gpu/a733_gpu_platform.c`, `drivers/gpu/a733_gpu_resources.c` and `drivers/power/a733_pmic.c`. CPU-L's clock lease also holds the DSU rate stable, and the DSU lease holds both cluster rates. Drivers raise voltage before frequency and lower frequency before any voltage decrease, so no new shared-resource manager or power sequence is needed.

The CPU driver requests the highest granted points in CPU-B, CPU-L, DSU order. A normal high-clock session uses the TOP approval for the chip's speed bin; the tables and the DSU voltage and rate rule are those of [the CPU top-point design](cpu-top-opp.md). GPU startup holds 1008 MHz at 960 mV, including the firmware's serialized rate. Read-only grants and lower explicit grants are diagnostic modes.

## Startup and admission

- The GPU recognition patch (`recognition-dispatch.patch` in the mimus checkout, as the ordinary recognition patch) contains the bounded `os_cpu_clock_ready()` wait. After input authentication and before the first GPU OPEN it waits at most 1000 one-tick delays, checking the board lifetime on every delayed pass and entering the terminal failure path on expiry or revocation. GPU OPEN and timed inference therefore follow resolution of the initial CPU requests, and no wait is added per GPU job.
- The GPU's pre-provider `session_owners_distinct()` checks the board owner, the thermal owner and the CPU grant owner against the fixed firmware and session identities (`OS_GPU_FIRMWARE_OWNER`, `OS_GPU_SESSION_OWNER`). Either alias is rejected before provider construction, and host admission rejects the same aliases before it prepares a launch bundle.
- The plain GPU host reader requires an 808-byte linked session; a CPU grant adds a 96-byte record, so this profile is 904 bytes. The ELF contract, the manifest CPU feature and the session bytes must agree (`apps/os/prepare.py` derives the expected size as `OS_BYTES + GRANT_BYTES` only for the CPU_CLOCK feature).
- CMake and the link check permit supervised recorded GPU plus CPU_CLOCK with A76 admission and the two-worker placement. The performance, eight-worker ([gpu-eight-workers.md](gpu-eight-workers.md)), NPU ([os-gpu-npu.md](os-gpu-npu.md)) and supervised capture, radio and headset extensions are separate compositions. Resident GPU with CPU grants uses [resident-gpu.md](resident-gpu.md).

Readiness means the CPU owner's startup requests have resolved. It does not claim that a thermally refused request reached its target; the CLOCK report records the applied rates. A retained CPU transition fails the board before GPU OPEN, and a pre-write refusal preserves the CPU owner's safe behavior.

## Scheduling and thermal transitions

The PMIC task runs at priority 3 on A55 core 0. The thermal task runs at priority 4 with the OS's default A55 affinity and delays one tick after each bounded pass. CPU clock work runs in that task, with at most a 200 µs burst while a PLL transition is parked. The recognizer and the second inference worker run at priority 3 on A76 cores 6 and 7, and the GPU worker at priority 4 on core 6 with IRQ 95 on that core.

A CPU-B mux transition slows the recognizer and the GPU worker temporarily; it does not stop the GPU or alter the architectural-counter frequency. The caller deadlines of [gpu-recognition-wait.md](gpu-recognition-wait.md) bound observation independently, and they are not renewed to hide a slow CPU transition. The CPU driver bounds update, lock and return from the oscillator.

During THROTTLE, CPU lowering may overlap an admitted GPU batch. The hook finishes the batch under its ownership rules and reconciles thermal state before the next layer. A healthy GPU close can overlap CPU lowering, and a reopen can overlap CPU restoration, because the rail leases differ and the PMIC serializes requests. CRITICAL or a provider fault fails the board. GPU timeout, late reply and uncertain close retain the context, mappings and device ownership, and CPU uncertainty retains its clock and rail leases. No forced reset, forced lease release or CPU fallback resolves uncertain GPU work.

## Cost

The startup wait adds one ready-state load when already ready, or at most 1000 one-tick delays. It is confined to authenticated startup, before any GPU request exists. Owner validation adds two integer comparisons once at GPU construction. There are no new tasks, queues, pool allocations, barriers, translations or workload-buffer copies, and CPU transitions keep their request counts and 120 ms deadlines.

## Qualified figures

| Figure | Value | Record |
| --- | --- | --- |
| Startup cases (immediate, delayed, expired, revoked readiness; two aliases; one distinct owner) | 7 pass | `gpu-cpu-clock-software` |
| Modeled power interleaving | CPU-B 2002 MHz with GPU ON; CPU-L 1794 MHz during GPU OFF; DSU 1274 MHz with reopen | `gpu-cpu-clock-software` |
| Host preparation and admission cases | 14 | `gpu-cpu-clock-software` |
| Linked contract | 904 B (plain GPU 808 B) | `gpu-cpu-clock-software` |
| Executable size against the plain GPU image | 1,040,992 to 1,055,712 B | `gpu-cpu-clock-software` |

Records: [gpu-cpu-clock.jsonl](../evidence/gpu-cpu-clock.jsonl). The fixtures model MMIO, queues and scheduling, and their entry facts are synthetic.

## Limitations and open work

- GPU performance commands need an acknowledged idle and hold boundary; [gpu-performance.md](gpu-performance.md) implements them and CPU_CLOCK admission alone does not imply them.
- Concurrent GPU and NPU execution needs distinct device identities, session composition, service progress and combined failure retention ([os-gpu-npu.md](os-gpu-npu.md)); shared-PMIC evidence does not establish complete OS coexistence.
- Open: run the combined image on a board and qualify the CPU and GPU clock interleaving under thermal load.
