# Publishing stable PLL ownership

The common clock registry can atomically convert exclusive PLL_NPU or DISPLL1
ownership to shared ownership. NPU startup can adopt a prepared PLL controller's
reservation through this operation. Physical boot authority, safe RAM clocks,
remote firmware exclusion and recovery remain prerequisites supplied by the
platform provider; token publication does not establish them.

## Registry transition

`a733_clock_publish_stable` accepts an exact live token and a nonempty subset of
PLL_NPU and DISPLL1 that the token holds exclusively. Under the registry's
nonblocking lock, it validates the token, requested scopes, fresh identity
availability and deadline. It then changes only the selected scopes to shared
ownership and issues a new token identity. Owner and all other reservations
remain unchanged. The old identity immediately becomes invalid.

Every callback and fallible check precedes the commit. The replacement token is
written to the caller's output as part of that commit. There is no release or
reacquisition interval. Refusal preserves the original reservation and output;
a failed clock observation also faults the registry. Registry, input token and
output token storage must not overlap. A coordinator must keep the output in
persistent storage if any later handoff step can fail.

The operation does not inspect hardware or record a claimed electrical profile.
The privileged PLL coordinator must have verified its profile and kept consumers
excluded before publication. A display coordinator can publish DISPLL1 while
retaining exclusive VO0 in the same token. It must install the fresh token in
its live consumer records before allowing access; historical records cannot be
used as live reservations.

## NPU startup adoption

A PLL controller can bind its actual clock registry before its first poll.
Before scheduling, NPU startup can bind that exact controller as its permanent
source. Binding verifies registry identity, owner, handoff identity and fabric
configuration. The registry and startup use the same physical-counter object.
The PLL provider must use that object's monotonic time service as well; arbitrary
callback code cannot be certified merely by storing its function pointer.

Source polling and startup polling must be serialized by the designated
bootstrap task while scheduling is active. Cold boot may instead complete PLL
preparation before binding. The state fields are ordinary C data; concurrent
observation or mutation is outside this interface. PLL completion becomes
observable only after its final release and deadline checks succeed.

The source controller, configuration, callbacks, context and whole-platform
reservation must remain live for startup and service lifetime. Neither object
may be copied or moved. Self-address checks reject copied controller/startup
objects, and a source may have only one successor. Startup snapshots the source
configuration and callback identities to detect later changes.

The designated privileged bootstrap task waits for the bound PLL controller to
complete. Startup then authenticates its retained reservation and atomically
publishes shared PLL_NPU ownership while preserving exclusive local NPU and all
parent/fabric scopes. The registry writes the fresh token directly to startup's
persistent storage. Infallible assignments update the source and platform token
records and mark the transfer before the next clock observation. A subsequent
deadline failure therefore retains a reachable live token.

The whole-platform reservation remains in the source controller. It still
protects RAM/PHY/MCU and consumer exclusions established by the independent
provider. No source resource is released by adoption. Later startup polls check
the source's identity, configuration and provider metadata, plus the exact live
adopted token. Mutation, a failed source or registry failure stops further
platform access. Metadata-lock contention leaves startup pending.

Adoption skips only the conflicting fresh clock acquisition. The existing
DCDC2 observations, rail settling, PCK reservation, local clock/reset/domain
sequence and deferred service activation still apply. An unbound startup uses
its existing inherited-clock acquisition path. Concrete pre-DRAM boot authority
and physical qualification remain required for disabled-PLL enable.

## Thermal ownership additions

The registry also reserves exclusive THS controller/bus/reset/interrupt
ownership and shared or exclusive GPADC24M clock ownership. THS depends on
GPADC24M. GPADC24M conservatively reserves REF and OSC because its source can be
SYS24M or DCXO. A clock change requires exclusion of every ADC and thermal
consumer. Analog supply ownership remains separate.

FABRIC includes APB1 selector/divisor/global-gate control at CCU offset `0x518`.
Every writer requires exclusive FABRIC; a consumer takes shared FABRIC plus
separate selected-parent reservations and verifies the live rate. Software
scope membership does not prove remote-firmware exclusion. These relationships
come from the pinned board clock source and manual sections 4.1.3.4, 7.1.2 and
7.1.4. The thermal provider owns its register, frequency and analog checks.

## Validation boundary

Focused host checks cover publication, preserved child ownership, competing
admission, stale identities, overlap refusal and all terminal refusal classes.
They also cover the added thermal parent dependency and local shared refusal.
The composed two-core model uses the actual PLL controller, common registry,
physical-counter service, FreeRTOS mutexes, NPU startup and deferred service.
Registers and the whole-platform provider remain modeled.

Four new composed cases cover successful adoption through activation, source
mutation before publication, a deadline first observed after publication and
provider mutation after adoption. Existing PLL sequence, rail, service and
convolution results remain reusable for their unchanged behavior. No physical
PLL operation or numerical NPU execution is established by these checks.
