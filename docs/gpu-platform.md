# GPU power binding and shared platform locks

The GPU platform binding connects real resource admission to the bounded GPU
power controller. Its native backend performs ordered CCU and PCK accesses.
Static FreeRTOS mutexes serialize register changes and PMIC I2C transactions
across participating drivers. These components have instruction-model and
FreeRTOS emulator evidence. Physical GPU power transitions remain unqualified.

The binding supports the verified inherited baseline described in
[GPU resource ownership](gpu-resources.md): a 400 MHz parent clock, enabled
DCDC4 at 800 mV, initialized PMIC I2C, and disabled GPU-local clocks with reset
asserted. Cold PLL, PMIC, and I2C programming remain required work. Initialization
does not establish those settings or fabricate readiness.

## Admission precedes controller startup

One privileged task owns each `a733_gpu_platform` object. The platform registers
one exclusive client ID with the global resource manager. Begin records an
absolute microsecond deadline. Poll first requests all GPU dependencies from
that manager. A competing resource owner or manager operation returns pending;
the controller has not attempted acquisition and no platform token exists.

Successful admission verifies the inherited settings, enables supported shared
gates, and returns a resource token. The controller then receives a delegated
handle to that lease and advances its existing clock, reset, and domain plan.
Begin returns one for acceptance. Poll returns zero while pending, one when
ready, and minus one after a fault. These signatures match the GPU service's
asynchronous enable callbacks. A service context containing several backends
must forward those callbacks to its platform object.

Partial admission retains the manager token. A later controller failure also
retains the token and prevents further platform callbacks. That stable lease
continues to exclude conflicting changes, even when its manager record remains
`STABLE`. Resource-adapter failures mark the record `RETAINED` and disable
manager I/O. No automatic release or retry follows either failure.

Each poll performs one admission operation or one controller step. Admission
includes a synchronous PMIC snapshot and deadline-bounded lock waiting. The
existing I2C driver can exceed the outer deadline by one in-progress transaction,
with separate ten-millisecond transfer and STOP limits. Post-return checks
reject late success. Schedule polls through the worker; do not spin on pending.

## Power off retains ownership through manager release

Off has its own absolute deadline and separate begin/poll functions. Its caller
must first close job admission, complete firmware shutdown, mask interrupts,
and retain every allocation the device could access.

After the controller verifies clocks off, reset asserted, and GPU_TOP status
zero, its release callback returns the delegated handle to the platform.
Controller `OFF` therefore precedes platform `OFF`. The authoritative platform
token remains held until the manager verifies the electrical state and releases
the resource record. Manager contention returns pending. Deadline or callback
failure preserves any token whose release has not completed.

A successful release followed by a failed clock check leaves the platform
failed with token zero, reflecting the completed release. Shared gates and the
always-on rail remain enabled. Electrical power-off and resource accounting do
not establish firmware quiescence by themselves. The native backend invokes
these functions only after the complete firmware/local-idle/reset sequence in
[the healthy shutdown contract](gpu-isolation.md). Failed or uncertain GPU work
still requires independent recovery.

## All platform participants share the same mutexes

Create one static `a733_platform_locks` object before clients become runnable.
Use its callbacks for every CCU/PCK writer and every PMIC/I2C participant,
including the resource adapter and the native power adapter. A second lock
instance or direct register access would bypass arbitration. The binding does
not automatically retrofit unrelated drivers.

The mutexes provide priority inheritance and scheduler waits on the project's
AArch64 SRE port and patched two-core port. Admission requires an EL1 task,
running scheduler, enabled IRQs, and no active critical section or interrupt
handler. Recursive acquisition and nesting either platform mutex are refused.
Every holder must release or explicitly retain its mutex before acquiring another.

Waiting uses finite intervals of at most one scheduler tick and checks the
absolute deadline between waits. A final interval shorter than one tick permits
only immediate acquisition. Dispatch can occur after expiry; a post-acquisition
check returns the mutex without allowing device I/O. The callback is
deadline-bounded, with scheduler latency, rather than nonblocking.

An uncertain transaction keeps its mutex. The native rail adapter already
omits release after transport or restoration failure. Other clients can call
`a733_platform_lock_retain` while holding a mutex to refuse later admission
immediately. Retention preserves the current owner. Do not delete that task,
reinitialize the locks, or force an unlock; recovery requires independent reset.
Healthy register work can continue while the separate I2C mutex is retained.

## Native register operations have a restricted plan

Native power operations accept only the registers and write tuples used by the
[GPU power controller](gpu-power.md). Full writes configure the five GPU_TOP
delay registers. Masked updates select the GPU clock, control its local gate,
control external bus/reset bits, and request the GPU_TOP policy. Shared-gate
updates remain in the resource adapter. Unknown addresses, masks, and values
are refused before locking or MMIO.

All mutations hold the common register mutex. Masked updates read and write
within that same critical interval, preserving unrelated fields. Device barriers
order accesses and complete writes before unlock. The resource lease protects
the GPU-local fields while other drivers can change unrelated fields under the
same mutex. The register mutex is not a substitute for resource ownership.

Both native adapters require caller-established EL1 execution and identity
Device mappings. Their initialization checks the 24 MHz architectural counter;
it does not validate exception level, page attributes, peripheral readiness, or
watchdog qualification. Native callbacks are private privileged operations.

Construct the binding in this order, using zeroed static storage:

1. Initialize shared locks with `a733_gpu_platform_native_now` and a null clock
   context. The clock callback enables setup without circular adapter dependencies.
2. Initialize the native resource adapter with the shared lock callbacks, then
   initialize the global resource manager with that adapter's operations.
3. Initialize the native power adapter with the same lock callbacks.
4. Initialize the platform with the manager, its reserved client ID, and the
   native power operations. Connect its begin/poll functions to the GPU service.

Every initialization must succeed before clients use the objects. None of these
initializers reads a peripheral. The native clock callback must not support
device work until native initialization validates the counter frequency.

## Focused evidence covers the new bindings

The native instruction-model check executes the resource manager, resource
adapter, power controller, platform wrapper, and native power operations as
AArch64 code. It models I2C transfers, immediate domain acknowledgements, and
the clock write-key readback. It passes resource contention, on/off/reopen,
restricted-operation refusal, release contention, and retained transport and
controller failures. A separate focused entry verifies rejection when the final
wrapper clock read moves backward relative to the controller's latest reading.

One two-core FreeRTOS session passes a waiter blocked behind another task,
deadline refusal, recursive/nested and critical-section refusal, and retained
I2C ownership. Register locking remains usable after I2C retention. These checks
reuse the existing native rail-adapter and source-reference evidence. They do
not execute the physical I2C controller or qualify physical GPU acknowledgements.

Evidence, commands, source identities, and artifacts are under
`~/.cache/sunstoneos/evidence/gpu-platform-20261016/`. The new checks are
`checks/a733-gpu-platform-native.c`, `checks/a733-gpu-platform-emulate.py`, and
`checks/a733-platform-locks.c`. The emulator fixture supplies the inherited
baseline; it is not a board-ready executable.

Physical qualification requires current board access, recovery suitable for the
selected bounded test, Device mappings, a real shared-lock deployment, and an
established PLL/rail/I2C baseline. Watchdog expiry is not a prerequisite for
unrelated supervised tests. A coordinator can then build a bounded power
transition image with explicit token and register-state reporting. GPU firmware
boot, transaction isolation, execution, and physical IRQ delivery still require
their own integration. No board request is submitted from these emulator images.
