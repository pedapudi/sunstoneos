# GPU recognition with CPU operating-point control

Status: implemented and software-tested. This increment combines
supervised recorded GPU recognition with the existing CPU clock owner and two
A76 inference workers. It uses the existing `SUNSTONE_A733_OS_GPU_LINEAR` and
`SUNSTONE_A733_OS_CPU_CLOCK` options. No hardware run is authorized by this
design. Physical integrated GPU recognition remains unqualified.

## Existing ownership permits independent CPU and GPU clocks

The source baseline is `81029651`. The relevant native owners are
[`apps/os/cpu.c`](../../apps/os/cpu.c),
[`apps/os/thermal.c`](../../apps/os/thermal.c) and
[`apps/os/gpu.c`](../../apps/os/gpu.c).

| Resource | CPU clock owner | GPU owner | Consequence |
| --- | --- | --- | --- |
| PLL and mux | CPU-L, CPU-B and DSU words at 0x08871000, 0x08872000 and 0x08873000 | PLL_GPU0 and GPU-local clock/reset controls | CPU transitions do not reprogram the GPU core clock |
| Supply | DCDC3 for CPU-B; DCDC5 for CPU-L and DSU | DCDC4 at 960 mV | Existing rail leases can coexist with distinct owner identities |
| Shared PMIC | Requests through `providers.shared.pmic` | The same provider through the GPU supply consumer | The eight-slot request queue serializes complete transfers and preserves each request's owner and deadline |
| Shared clock sources | CPU clock leases add a stable oscillator reservation | GPU resource manager holds oscillator, PCK, fabric and local resources; its LOCAL set excludes the PERI0 PARENT lease | CPU changes neither the shared source configuration nor GPU fabric rates |
| Thermal policy | The thermal task lowers/restores CPU domains through the CPU driver | The recognizer closes/reopens a healthy GPU session at the existing layer boundary | One policy controls admission; neither owner releases uncertain work |

These are the actual paths in `drivers/clock/a733_cpu_clock.c`,
`drivers/clock/a733_clock_leases.c`, `drivers/gpu/a733_gpu_platform.c`,
`drivers/gpu/a733_gpu_resources.c` and `drivers/power/a733_pmic.c`.
CPU-L's clock lease also holds the DSU rate stable; the DSU lease holds both
cluster rates. Existing driver ordering raises voltage before frequency and
lowers frequency before any voltage decrease. No new shared-resource manager
or power sequence is needed for this combination.

The CPU driver requests the highest granted points in CPU-B, CPU-L, DSU order.
A normal high-clock session uses the existing TOP approval for the chip's speed
bin. The source tables and DSU voltage/rate rule remain those of
[the CPU top-point design](cpu-top-opp.md). GPU startup retains 1008 MHz at
960 mV, including the firmware's serialized rate. Read-only grants and lower
explicit grants retain their current diagnostic semantics; they are not the
normal high-clock operating mode.

## Startup and admission requirements

The GPU build replaces the ordinary recognition patch with
`recognition-dispatch.patch`. Both patches now contain the bounded
`os_cpu_clock_ready()` wait. GPU OPEN and timed inference follow resolution of
the initial CPU requests.

The GPU's pre-provider `session_owners_distinct()` checks the board and thermal
owners and CPU grant owner against its fixed firmware and session identities.
The combined profile rejects either alias before GPU provider construction.
Host admission rejects the same aliases before preparing a launch bundle.

The plain GPU host reader requires an 808-byte linked session. A CPU grant adds
an existing 96-byte record, making this profile 904 bytes. The exact ELF
contract, manifest CPU feature and session bytes must agree. No new session
version, command, GPU configuration version or approval schema is needed.

## Composition uses existing services

1. In the GPU recognition patch, use the existing one-second finite CPU-ready
   wait after input authentication and before the first GPU OPEN. Check the
   existing board lifetime on every delayed pass and enter the existing
   terminal failure path on expiry or revocation. The initial CPU requests
   therefore resolve before GPU power-up. There is no extra wait per GPU job.
2. Under `SUNSTONE_A733_OS_CPU_CLOCK`, include the sealed grant owner in the
   GPU's existing owner-collision check. Mirror its two fixed identity
   exclusions in GPU host admission using the existing parsed grant. This
   extends the current startup check; it does not add a generic identity
   registry or transfer ownership between clients.
3. Permit supervised recorded GPU plus CPU_CLOCK in the existing CMake and
   link-check conditions. Require the existing A76 admission and two-worker
   placement. Preserve PERF, eight-worker GPU, NPU and supervised
   capture/radio/headset exclusions for this increment. Resident GPU plus CPU
   grants remains outside this supervised composition.
4. In `apps/os/prepare.py`, derive the expected GPU contract size as
   `OS_BYTES + GRANT_BYTES` only for a boolean CPU_CLOCK feature. Require that
   size in both the actual linked contract and manifest. Continue through the
   existing session, CPU grant, board B1 and exact-image approval validation,
   plus every existing GPU evidence requirement. Frozen-entry validation
   already calls these functions and must exercise the same combined case.

Readiness means the CPU owner's startup requests have resolved. It does not
claim that a thermally refused request reached its target. The existing CLOCK
report records the applied rates; no new success flag will conceal a refusal.
A retained CPU transition fails the board before GPU OPEN or any later normal
work. A pre-write refusal preserves the existing CPU owner's safe behavior.

## Scheduling and thermal transitions

The PMIC task runs at priority 3 on A55 core 0. The thermal task runs at priority
4 with the OS's default A55 affinity and delays one tick after each bounded
pass. CPU clock work runs in that thermal task, with at most a 200 microsecond
burst while a PLL transition is parked. The recognizer and second inference
worker remain at priority 3 on A76 cores 6 and 7. The GPU worker remains at
priority 4 on core 6 with IRQ 95 on that core. No placement or priority change
is required for the two-worker increment.

A CPU-B mux transition temporarily slows both the recognizer and GPU worker;
it does not stop the GPU or alter the architectural-counter frequency. The
accepted GPU caller deadlines continue to bound observation, independently of
that scheduling delay. They must not be renewed to hide a slow CPU transition.
The CPU driver already bounds update, lock and return from the oscillator.

During THROTTLE, CPU lowering may overlap an admitted GPU batch. The GPU hook
finishes that batch under its existing ownership rules and reconciles thermal
state before the next layer. A healthy close can overlap CPU lowering; reopen
can overlap CPU restoration. Their different rail leases remain valid while
the common PMIC serializes requests. CRITICAL or a provider fault fails the
board. GPU timeout, late reply and uncertain close retain the context, mappings
and device ownership; CPU uncertainty retains its clock and rail leases. No
forced reset, forced lease release or CPU fallback resolves uncertain GPU work.

## Costs and focused validation

The new startup wait adds one ready-state load when already ready, or at most
1000 one-tick delays with the existing lifetime/progress checks. It is confined
to authenticated startup, before any GPU request exists. Owner validation adds
two integer comparisons once at GPU construction. There are no new tasks,
queues, pool allocations, barriers, translations or workload-buffer copies.
The steady GPU submit/reply path remains the accepted bounded-caller path.
CPU transitions retain their existing request counts and 120 ms deadlines.

Measure combined ELF executable/static storage against the accepted GPU image;
attribute the existing CPU driver and thermal-report storage separately from
the small composition changes. The existing 4 MiB heap, device pools, stacks,
model inputs, arena and GPU view must remain disjoint. Count startup waits and
provider requests in the focused fixtures; do not add runtime instrumentation
or replay model inference.

The smallest useful checks are:

- Run the actual generated GPU recognizer up to OPEN with readiness immediate,
  delayed, expired and revoked. Verify OPEN and timed inference remain absent
  until readiness, and no owner return follows terminal failure.
- Exercise the actual GPU startup identity check with each fixed-owner alias
  and one distinct CPU owner; refusals must precede provider or pool work.
- Extend the existing shared-provider fixture for CPU DCDC3/5 activity alongside
  GPU DCDC4 retention and externally quiesced power transitions. Cover one successful
  interleaving and the affected PMIC/transition failure retention paths using
  actual CPU/GPU power controllers and provider code, with MMIO modeled. Reuse
  the accepted caller close/reopen proof: power transitions do not establish
  firmware drain or a new full OS thermal lifecycle. This closes
  the new coexistence boundary without repeating prior GPU completion cases.
- Build one actual supervised two-worker GPU+CPU_CLOCK image with the accepted
  toolchain and mimus pin. Check the 904-byte linked contract, placement,
  firmware, memory limits and unchanged model/placement identities. Cover the
  combined nominal TOP/read-only templates, missing or mismatched grant/entry,
  owner aliases, and attempted contract/CPU-feature relabeling through actual
  preparation and frozen validation. Reuse the accepted plain-GPU artifact for
  its 808-byte compatibility case and check only the remaining affected
  configuration refusals.

Accepted GPU evidence is in
`~/.cache/sunstoneos/evidence/resume-gpu-wait-20261026/`, including the frozen
image, native caller/lifecycle checks and host admission. Existing CPU native
owner and clock checks are `checks/os-cpu-clock-owner.c`,
`checks/os-cpu-clock-native.py` and `checks/cpu-clock-check.py`. Shared-provider
coverage is `checks/accelerator-shared-owner.c`. No inference benchmark or
physical test is part of this increment.

## Measured software results

The combined A76 image builds with the source and model identities in
`apps/mimus/source.json` and `apps/mimus-board/inputs.json`. Its SHA-256 is
`e1c78f6daaa80928ab3f81a4f83e3f8f4ab5a12787599ddc66bb52be171a1cae`.
Evidence is under
`~/.cache/sunstoneos/evidence/resume-gpu-cpu-clock-20261026/`:

- `startup.json`: seven cases execute the linked recognizer or GPU startup.
  Immediate readiness uses one ready read and no delay; delayed readiness uses
  three one-tick delays. Expiry stops after 1000 delays, and revocation after
  two. Neither terminal path reaches OPEN. Both CPU-owner aliases refuse
  before provider construction; the distinct owner reaches that boundary.
- `shared2/result.json` and `shared4/result.json`: actual CPU/GPU controllers
  interleave over the shared provider model. CPU-B reaches 2002 MHz alongside
  GPU ON; CPU-L reaches 1794 MHz during externally quiesced GPU OFF, then DSU
  reaches 1274 MHz alongside reopen. PMIC arbitration loss and CPU PLL lock
  failure preserve both owners. Earlier fixture-only timing/assertion repairs
  do not affect the accepted nominal branch. These checks establish modeled
  power interleaving, with external quiescence assumed.
- `host/result.json`: fourteen cases cover actual preparation, native session
  admission and frozen-entry validation. Read-only and TOP grants pass; wrong
  approval bindings, missing approval, single-cluster TOP, GPU-owner aliases,
  CPU-feature relabeling and an 808-byte combined contract refuse. Entry facts
  are synthetic and cannot authorize a physical launch.
- `guards/result.json`: performance commands and missing A76 admission refuse
  configuration. `plain-gpu-compatibility.json` verifies current admission of
  the accepted unmodified 808-byte GPU image.
- `layout.json`: the 904-byte contract, A76 pair, GPU firmware, 1008 MHz/960 mV
  rate and placement table match the linked image. The 4 MiB heap, two 2 MiB
  pools, static stack sizes and fixed model/arena/GPU regions retain their
  bounds. The native link checker verifies their disjoint extents.

`cost.json` compares allocated sections and linked input-section contributions
against the accepted plain GPU image. Executable bytes rise from 1,040,992 to
1,055,712; read-only bytes rise by 1,312 and mutable sections by 32,768.
The composition itself adds 64 executable bytes in the recognizer and 24 in
GPU startup, with no mutable state. Enabling the existing CPU owner accounts
for 636 bytes of owner state and 33,816 bytes of thermal/report state. The
linked CPU driver adds 11,464 executable and 1,223 read-only bytes. Existing
CPU reporting, thermal logic, admission and provider operations supply the
remaining CPU-enabled code. Independent intervening radio startup changes
add 132 executable bytes to this baseline comparison. Output-section padding
explains the difference between summed input contributions and allocated
section totals. No steady GPU job operation, task, allocation or workload
buffer copy is added by this composition.

## Subsequent compositions and file ownership

Eight-worker GPU execution remains a required follow-on. The generic engine
and OS eight-worker placement already exist, with A55 inference at priority 2
below the PMIC and thermal services. The GPU-specific recognizer patch, owner
assertions and host exclusions still encode a pair. Extending them requires
checking that only the recognizer calls the exact-sum hook while generic CPU
ranges use all workers, and that GPU completion and service tasks make
progress. It does not require a new scheduler.

PERF is a different ownership extension. The current planner can describe GPU
hold/rate/voltage actions, but `apps/os/perf.c` only implements accelerator
hold/retune for the NPU and treats GPU state as read-only. GPU performance
commands need an acknowledged idle/hold boundary, a concrete retune owner and
firmware clock updates before work resumes. CPU_CLOCK admission alone must
not imply that those operations exist.

Concurrent GPU/NPU execution also remains required. It needs distinct device
client/owner identities, session composition, service progress and combined
failure retention. Existing shared-PMIC evidence helps with that work but does
not establish complete OS coexistence.

The implementation changes the GPU recognition patch, GPU startup identity check,
supervised GPU CMake/link exclusions, GPU host admission and focused checks.
The resident NPU owner holds the separate resident CPU/NPU CMake conditions,
NPU OS files and card admission. This design changes no thermal service,
performance-mode service, resident loader or NPU path. Independent design
approval preceded implementation; final review binds the selected artifacts.
