# GPU power control has host evidence

The A733 GPU power controller prepares the GPU at 400 MHz and requests power
for its top-level domain. It supports bounded startup, shutdown, and reopening.
Host checks and an AArch64 compilation pass. Physical execution, regulator
control, shared-clock management, and integration with the FreeRTOS GPU service
remain outstanding.

The implementation is [a733_gpu_power.c](../drivers/gpu/a733_gpu_power.c), with
its callback contract in [a733_gpu_power.h](../include/a733_gpu_power.h).
A privileged owner polls the controller. One poll runs the resource and
register steps back to back, with monotonic clock reads. It waits on the clock
for the one-microsecond clock-settling delay. The board clock reports whole
microseconds rounded down, so the delay ends only when the clock passes the
sample taken after the clock-gate write by more than one microsecond. An
unsatisfied power-domain acknowledgement is re-read for up to 50 µs
(`A733_GPU_SPIN_US`, the bound shared with the startup and shutdown
controllers) from its first read before the poll returns to the owner. The
lease callback and earlier register steps in the same poll therefore do not
consume that budget. A wait whose clock shows no advance for 1,024 consecutive
samples (`A733_GPU_SPIN_STALLED_READS`) returns to the owner without advancing
the step, and the next poll restarts the delay. Clock settling and domain
acknowledgement share one absolute deadline. An expired deadline, clock
regression, callback failure, or unexpected register value stops further I/O.
The controller performs no automatic rollback after a failure.

## Shared resources require an explicit owner

The resource manager must acquire the following before register preparation:

- Exclusive ownership of the GPU local clock, external reset, and GPU top-level
  power domain.
- A stable 400 MHz peripheral-clock output and an established 800 mV DCDC4 rail.
- Shared leases for the oscillator, power-controller clock, and GPU AHB and
  memory-bus gates.

The board operating-point table specifies 400 MHz at 800 mV. This configuration
is a bring-up setting; it is not a measured frequency or a performance target.
The manager must complete regulator settling and parent-clock preparation before
its acquisition callback returns. It must arbitrate concurrent users and prevent
changes while this controller holds its leases. The board marks DCDC4 as always
on, so releasing the GPU lease must preserve that rail.

The controller checks that the shared bus gates and power-controller clock are
enabled. It refuses an inherited GPU whose core clock, bus clock, or external
reset indicates possible execution. It does not attempt to take over a running
GPU. Resource acquisition can have effects before that refusal; the failed
controller retains its diagnostics and does not release resources automatically.

Register operations use these address spaces:

| Register bank | Physical base | Fields used |
| --- | --- | --- |
| Main clock controller | `0x02002000` | AHB gate `0x5c0[7]`, memory-bus gate `0x5e0[16]`, GPU clock `0xb20`, bus gate/reset `0xb24[0,16]` |
| Always-on clock controller | `0x07010000` | Power-controller gate `0x1ac[0]` |
| Power controller | `0x07060000` | GPU domain 5, policy `0x5000`, status `0x5008`, sequencing delays |

Shared gates are read-only in this controller. The platform's register-update
callback must preserve unrelated fields atomically under the shared register
lock. GPU clock writes include the update key in bit 27; that key reads as zero.
No parent PLL or PMIC register is written by the controller.

## Startup and shutdown preserve explicit limits

Startup programs the source-defined power-controller delays, requests GPU domain
state 8, and waits for that exact status. It selects the 400 MHz peripheral
parent with divisor one, enables the core and bus clocks, waits at least one
microsecond, and releases external reset. Relevant writes receive readback
checks. The selected clock requires only 80 ns for 32 cycles, so the one
microsecond delay covers the reference driver's settling requirement.

Shutdown requires stopped firmware, closed job admission, and masked GPU
interrupts. It asserts external reset, disables the local bus and core clocks,
requests domain state zero, and waits for that exact status. It then releases
shared-resource leases through the resource manager. The controller allows no
new operation after a failed transition.

A successful power-off result does not establish that outstanding memory
transactions have drained. This controller cannot supply the isolation callback
required by [GPU shutdown](gpu-stop.md). Device backing must remain reserved
until a separate platform mechanism proves memory isolation. Shared-resource
release also does not authorize freeing device buffers.

The `lease_attempted` and `lease_held` diagnostics describe callback progress.
A failed acquisition or release can leave partial ownership. A successful
release followed by an expired deadline records failure with `lease_held` clear.
Platform recovery must reconcile that state before reuse.

## Focused checks and source identities

The changed behavior is clock, reset, and power-domain sequencing. The relevant
risks are accessing a live device, changing shared fields, releasing resources
prematurely, accepting a transient domain state, or continuing after failure.
One host model and one reference comparison cover those risks without accessing
the board or running performance measurements.

The [native check](../checks/a733-gpu-power.c) runs under AddressSanitizer and
UndefinedBehaviorSanitizer. Successful startup, shutdown, and reopening preserve
unrelated fields and shared gates. Reset release follows the clock gate by at
least the settling delay. A second clock model counts one 24 MHz tick per
clock read and callback and reports whole microseconds rounded down; under it,
reset release followed the clock gate by 44 counter ticks, at least the 24 that
one microsecond requires. One poll completes startup when the domain
acknowledges within 50 µs, including when the register writes before the
acknowledgement take 60 µs; a stalled acknowledgement returns after at least
50 µs of re-reads and completes on a later poll. With a stopped clock, the
settling delay and the acknowledgement wait each return without advancing and
complete once the clock runs. The check covers 102 retained faults, including every
callback failure on a successful startup/shutdown path, stalled transitions,
invalid prerequisites, a partially applied write, and a late resource release.

The [reference check](../checks/a733-gpu-power-reference.py) compiles the original
power-domain initialization and transition functions with a register model. All
emitted power-controller writes match the native trace. It derives GPU mux,
divisor, update key, gate, and reset expectations from the clock definitions.
It also verifies register bases, the selected operating point, the board's GPU
supply assignment, and the always-on rail requirement.

References come from the [pinned board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f):

- `bsp/drivers/pm_domain/pck600_domains.c` and
  `bsp/include/dt-bindings/power/sun60iw2-power.h` define domain identifiers,
  sequencing delays, policy requests, and acknowledgement fields.
- `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c` and `ccu-sun60iw2-r.c` define clocks,
  gates, reset, the divisor table, and the update key.
- `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` and
  `sun60i-a733-orangepi-zero3w.dts` define addresses, supply wiring, and operating
  points.
- `bsp/drivers/gpu/imagination/pvr_power.c` enables parent, core, and bus clocks,
  waits one microsecond, and deasserts reset. Its shutdown reverses reset and
  clock enablement.

Downloaded files retain their original license notices. The native controller
uses independently written state transitions and source-derived register facts.
The reference checker preserves the original power-domain source notice in its
compiled extraction. Its off transition requires status zero; the original
helper accepts any status other than eight as off.

Raw evidence is `~/.cache/sunstoneos/evidence/gpu-power-20261016/`. It contains
native and reference register traces, compiled artifacts, source hashes,
commands, and results. Source files and their provenance manifest are cached in
`~/.cache/sunstoneos/sources/gpu-power/`. An initial reference-shim compilation
failed because its disabled logging macro left a local variable unused. The
corrected shim passed; native controller evidence did not require repeating.
The later native check adds partial-write and late-release coverage. Its
successful register trace is unchanged, so the reference result remains valid.

## Physical qualification requires recovery and resource managers

No physical test executable or queue submission exists for this controller yet.
Before a board trial, implement shared leases and ordered register callbacks,
qualify independent watchdog recovery, and establish a stopped GPU. Preserve
all memory backing throughout the trial.

The first proposed board test should read the initial clock, reset, domain,
and rail settings under the coordinator's device lock. If the GPU is safely
stopped, run one bounded startup at the selected operating point, read GPU
identity, and run one shutdown. Capture every transition and verify ROM return.
Stop on the first transport failure, unexpected state, or deadline. A successful
trial would qualify power sequencing and identity access only; firmware boot,
workload execution, memory isolation, and interrupt delivery need their own
missing evidence.
