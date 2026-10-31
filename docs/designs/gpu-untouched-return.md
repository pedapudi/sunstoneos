# Return a GPU construction refusal without claiming GPU execution

The returning recognition profile requires a validated GPU job before its
successful GPU retirement. A pre-scheduler GPU constructor refusal instead leaves
GPU OPEN disabled and allows CPU recognition. Its subsequent zero-job retirement
currently retains the image. This path can explain a host nonreturn without any
GPU power-up, but it does not identify the cause of the failed physical attempt.

A distinct return outcome covers only a recorded pre-scheduler construction
refusal. The refusal reason is written by that constructor's failure exit, after
suspending any partially created worker. The marker is immutable afterward.
Returning evidence requires no GPU start, open, epoch, ticket, pending job or
runtime failure, and no placed or completed GPU work. A repeated constructor
cannot overwrite the marker.

An uninitialized platform or backend must remain zeroed. An initialized platform
must remain NEW, with no resource token, PMIC request, rail lease, power request
or counted power-domain row. An initialized backend must remain BOUND to those
exact local objects, with no runtime clock, identity, error or compute mode.
The service remains COLD without a request or reply. A worker must be absent or
both marked suspended before scheduler startup and still reported suspended by
FreeRTOS. An indefinitely blocked worker does not satisfy that check. Partial
software construction may retain DEVICE-tagged RAM; it does not release that
ledger or manufacture a hardware-drain proof. The returning host later restores
its saved RAM after CPU cleanup.

The copied record keeps its 64-byte extent. Its distinct version 2 encodes zero
jobs, zero hardware-completion flags and the constructor refusal reason. It
asserts no firmware OFF, isolation, voltage or GPU numerical result. A linked
capability marker explicitly admits this format; older bundles retain the
strict version 1 successful-retirement decoder. The host reuses ordinary CPU
terminal proof, DRAM restoration and the mandatory rail helper. GPU qualification
is false for this outcome even if the CPU transcript matches the reference.

An accepted OPEN or a started worker without the exact refusal proof retains
existing failure handling. The new outcome cannot convert a partially activated
GPU, pending request, missing acknowledgment or uncertain hardware state into a
successful cleanup receipt.

The source comparison follows `a733_gpu_mimus_start` through native platform and
PMIC bindings, firmware/session memory construction, backend binding and service
queue/task creation. These operations construct RAM state, translate or publish
memory and read architectural counters. Device power, register writes and DMA
require runtime OPEN. In particular, service initialization invokes no interrupt
callback; its worker is suspended before the scheduler if final binding refuses.
The proof does not apply to a successfully constructed worker that has started.

The only new task-state query runs during the zero-job refusal return. The GPU
return target enables the existing FreeRTOS getter; other targets keep their
configuration. The getter uses task critical sections, whose port implementation
supports EL0 callers through SVC, and checks the suspended and event lists under
the scheduler lock. Recognition and job hot paths add no operations. Constructor
refusals add their reason store; return adds bounded metadata checks and one
getter when a worker exists. Code-size evidence belongs with the rebuilt image.

The staged software-tested image has SHA-256
`c9e8b9087d28c5fb3c9ee7ce034bd1b2c13b4e0367dea4b7bd9d36790df100a6`.
Its linked task-state getter occupies 364 bytes and the new proof occupies
1,828 bytes. Constructor code grows by 608 bytes, hook retirement by 40 bytes,
the capability marker by four bytes, and private GPU storage by sixteen bytes
including alignment. These symbol measurements exclude unrelated dependency
changes and whole-image layout. Physical use of this outcome remains staged.
