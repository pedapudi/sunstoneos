# Native GPU lifecycle backend

The native backend connects the GPU lifecycle service to restricted GPU MMIO,
the existing firmware-memory adapter, platform power admission, and GIC interrupt
delivery. It checks physical identity after power-on and before any firmware
register write. Instruction-model checks pass; physical GPU boot and execution
remain unqualified.

Healthy CLOSE uses firmware idle/off acknowledgements and states, local idle/reset
checks, and bounded native platform power-off before firmware-memory reclaim.
The [shutdown contract](gpu-isolation.md) distinguishes that sequence from
failed-device recovery. Any uncertain outcome retains backing and outstanding
ownership until independent recovery.

## Binding verifies mappings without peripheral access

Initialize the [firmware-memory adapter](gpu-memory.md) and
[power platform](gpu-platform.md), then call `a733_gpu_backend_bind` before
initializing the GPU service. The supplied service configuration must refer to
the same firmware and shared-buffer descriptors as the memory adapter. Its
initial clock field must be the legacy 400 MHz staging value. Successful
binding replaces it with the power controller's 1008 MHz core clock. Service
OPEN reconstructs the firmware system-init and runtime-config clock fields
from that value before publication and startup. A refused bind preserves the
configuration. Buffers, page lists, backend state, and control objects must
remain private and pinned.

Binding verifies EL1 execution, enabled MMU/data cache, the 24 MHz counter,
and readable/writable identity Device-nGnRnE mappings for every allowlisted
GPU and GIC register page. It checks the beginning and end of each 4 KiB page.
The native translation helper preserves PAR_EL1 and interrupt masks. Binding
performs no GPU/GIC access or power transition and preserves its outputs on refusal.

The board source places GPU registers at `0x01800000`, within a `0x8ffff`-byte
resource. This backend uses only pages at offsets `0`, `0x1000`, `0x3000`, and
`0xa000`. GPU IRQGPU is SPI 63, or GIC interrupt ID 95. The distributor is at
`0x03400000`. Its configured route, security group, level-high mode, priority,
and initial disabled state remain platform prerequisites. DVFS interrupt ID 96
is a separate resource.

Binding installs one common callback context. Queue, startup, control, shutdown,
power, and interrupt callbacks forward to their respective native components.
Optional free-list callbacks retain their original table and context through
forwarding wrappers. Every return value is preserved, including an uncertain
allocation result of minus one. Non-free-list events use the separate external
event callback and its context. Those contexts must outlive the service.

## Identity admission follows completed power-on

The asynchronous enable callback passes the absolute OPEN deadline to the real
power platform. Pending power admission remains pending. Only platform `ON`,
with a held token and delegated lease, permits the first GPU register read.
Identity reads also share the OPEN deadline; a late read cannot admit firmware.
Clock checks reject regression relative to both the backend and its private
platform/controller observations. Shared resource-manager state remains protected
by its own arbitration.

The first transaction reads 64 bits at GPU offset `0x20`. A nonzero branch field
selects the packed branch/version/unit/configuration identity. A zero branch
selects the published legacy path: 32-bit revision at `0x20`, followed by 32-bit
core ID at `0x18`. Each poll performs at most one identity transaction.

The resulting identity must be 36.56.104.183. A mismatch, invalid mapping,
expired deadline, or failed power operation retains ownership and latches an
error. Later lifecycle callbacks refuse access. Interrupt masking remains
available after a worker fault so the service can disable delivery.

## Register transactions follow the prepared controller step

Startup and shutdown accesses must match the current controller step's offset
and width. Writes must also match its value, including the preserved SLC bit
computed by startup. An independent allowlist restricts accepted registers and
values to the published reset, wrapper, SLC, bus, and context-disable plans.
The prepared steps remain private to the service worker.

The native backend performs one aligned volatile 32-bit or 64-bit transaction
per accepted operation. Full-system device barriers order accesses and complete
writes before return. It rechecks the addressed page's identity Device mapping.
Unknown offsets, unsupported widths, changed write values, and output aliases
into backend/control/device backing are refused before MMIO. Output storage
must have writable Normal memory mappings. The queue receiver may copy one
88-byte firmware event into its own `last_event` field at its current consumer
offset. That exact destination is the sole permitted output within service state.

Control reads only the 32-bit MIPS interrupt status at `0x8a8` and acknowledges
it by writing one to `0x8b0`. Queue submission writes zero to the 32-bit MTS
schedule register at `0xb00`, after the existing queue publication barriers.
Those operations require published memory and an active control phase.

GIC callbacks use the existing bounded SPI adapter under the service's critical
sections. They perform no task-mutex wait. The backend's error field is atomic
because interrupt masking can run on another core. Firmware register and memory
callbacks remain serialized by the single privileged service worker.

## Reclaim follows complete orderly shutdown

After fresh firmware idle/off acknowledgements, the backend acquires the
independent firmware state and requires forced idle or OFF as appropriate.
The stop controller completes its pre-reset idle checks and reset readback.
Native teardown then starts once and advances one platform off-poll per worker
step under the original CLOSE deadline. Pending power-off keeps backing pinned.

Only platform OFF with no token or delegated lease permits firmware-memory
reclaim. A failure or expired deadline prevents further callbacks. An already
completed token release remains recorded even when a later clock check fails.
The optional synchronous override must return one and satisfy those same final
state checks. Compute/free-list allocation parents remain owned, preventing
another OPEN. Physical orderly shutdown remains unqualified.

The mimus native owner can reopen its retained allocations after the complete
healthy stopped proof; [gpu-mimus-service.md](gpu-mimus-service.md) defines its
context reconstruction and epoch rules. The optional nonblocking `live`
callback requires exactly 1 before normal backend access. The OS binds
`board_live`, so expiration of a mutable board lifetime permanently latches
REVOKED even if that lifetime is later renewed. This adds two architectural
counter reads to each admitted OS backend check. Fault-safe GIC masking is
still allowed; it does not establish GPU shutdown.

## Focused evidence covers register and callback composition

The identity reference check compiles the published decoding function and
verifies both identity formats and their access widths. Existing startup and
shutdown reference traces are reused after checking their source hashes.

The native instruction-model check executes every recorded register transaction
through the new backend and checks its offset, width, direction, and written
value. It also executes native firmware-memory and GIC adapters. Cases cover
post-power identity admission, legacy identity, mismatches, deadline expiry,
mapping refusal, callback output alias refusal, retained faults, successful
reclaim/reopen, and isolation refusal before reclaim. Free-list forwarding
preserves its original context and all six operation return values. Focused
checks also execute the real firmware-event receiver through native memory,
verify consumer acknowledgement, and refuse nearby or partial service aliases.
A separate clock check rejects regression after private power callbacks.

The fixture supplies saved controller plans and synthetic reserved RAM. It
models stage-1 translation results, counter reads, ordinary RAM copies, and the
already-checked power provider. It does not execute service scheduling, complete
firmware preparation, physical I2C, or GPU firmware. Existing memory, power,
interrupt-service, and controller evidence covers those unchanged components
within their documented limits.

Commands, source identities, logs, and artifacts are under
`~/.cache/sunstoneos/evidence/gpu-backend-20261016/`. The checks are
`checks/a733-gpu-backend-reference.py`, `checks/a733-gpu-backend.c`, and
`checks/a733-gpu-backend-emulate.py`. The instruction-model executable is not a
board qualification image.

Physical use still requires qualified independent recovery, inherited clock/rail
and I2C settings, reserved RAM and pinned mappings, a verified firmware hash,
configured interrupt routing, and the complete orderly shutdown sequence. A coordinator
must integrate these prerequisites before submitting a bounded firmware-boot
qualification request. No physical request has been submitted for this backend.
