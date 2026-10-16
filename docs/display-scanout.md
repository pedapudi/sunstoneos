# Native display scanout coordination

The scanout coordinator runs the existing A733 PHY, DSI video, LCD1, and display
engine controllers as one lifecycle. It admits one immutable XRGB8888 framebuffer
for the implemented UCH3 → DE1 → LCD1 → DSI1 path. Its orchestration is
implemented and checked in a host model. Physical scanout remains unqualified.

The interface is `include/display_scanout.h`; the implementation is
`drivers/display/display_scanout.c`. One privileged FreeRTOS owner supplies
the platform provider, a board identity, a selected panel identity, timing,
and a framebuffer descriptor. The coordinator allocates no memory and creates
no task. The caller schedules bounded polling and explicit observation.

## Resource and framebuffer tokens bind the complete request

The provider returns two persistent tokens from its real ownership registries.
Nonzero identifiers supplied by a caller cannot establish ownership. The
provider authenticates each token against its registry, generations, physical
configuration, and the complete request.

The resource token records owner, board, panel, token generation, recovery
generation, and exact reference, host, HS, and LS clock rates. Its scopes cover:

- The entire display engine, both outputs, all channels, shared selectors,
  line-buffer partition, urgency, offline/writeback controls, and register queues.
- The complete VO0 shared selector words at offsets `0x04`, `0x0c`, and `0x20`,
  excluding every LCD, LVDS, and DSI writer that can affect them.
- LCD1 and the DSI1/COMBOPHY1/display-PLL1 group, including the selected host/PHY
  shared reset and stable clock parents.
- The selected panel's pins, power, reset, and command-controller ownership.
- The memory route, address translation/IOMMU policy, fabric clocks, and
  independently qualified recovery mechanism.

DSI1 uses CCU clock/reset words `0x1588` and `0x158c`. The corresponding DSI0
words are `0x1580` and `0x1584`. Host/PHY shared-reset exclusion applies to the
selected instance. It does not imply that both DSI instances use one reset word.

The framebuffer token records owner, frame identity, CPU address, device
address, mapped span, pitch, allocation generation, mapping generation, cache
visibility generation, and immutable device-read state. The provider must
authenticate allocator ownership, translation, aliases, cache-line isolation,
fetch-burst padding, and completed cache publication. Those obligations also
exclude overlap with coordinator, callback, or other live CPU storage.

The coordinator requires the returned extents to equal the requested extents.
It checks CPU address alignment and range, then uses the existing
[DE planner](display-engine.md) to validate the device range, pitch, padded
image span, and exact display timing. It never derives a device address from
a CPU pointer. The provider pins the mapping and cache state for the complete
token lifetime, including faults.

Acquisition may complete, remain pending, or fail. Before a reservation or
physical effect, the provider publishes the complete token. A pending call
with a zero token has no effects. Once published, a token remains immutable
and does not expire. If a later acquisition returns a changed token, the
coordinator retains both the original identity and the unexpected output for
recovery. It does not silently replace the reservation.

## Preparation establishes the inactive pipeline

`display_scanout_init()` copies the request and provider and validates geometry
without callbacks. `display_scanout_prepare()` starts one operation with an
absolute deadline covering every preparation step:

1. Acquire the combined resources and immutable framebuffer mapping.
2. Run the actual [PHY controller](dsi-phy.md) against an authenticated inherited,
   enabled, locked PLL with the requested factors.
3. Prepare the selected panel through its bounded command-controller callback.
4. Run the actual [DSI video preparation](dsi-video.md) while the pixel producer
   remains inactive.
5. Prepare [LCD1](tcon-lcd.md), then prepare the [DE framebuffer](display-engine.md).

Resource acquisition must establish the COMBOPHY1 display-PLL clock selector
while the PHY is inactive, before PHY preparation. The coordinator cannot
substitute PLL lock observations for a verified cold-start sequence.

The panel callback owns the DSI1 command engine under the same resource lease.
It must call the real command controller for the selected panel's LP commands
and implement the panel's power, reset, and timing requirements. Completion
requires an idle command engine and panel readiness for video. PHY readiness
alone cannot satisfy that contract. No unspecified panel or generic display-on
sequence is assumed.

Every leaf controller receives admission adapters from the coordinator. These
adapters check the actual predecessor controller states and then authenticate
the resource and framebuffer tokens through the platform provider. Caller
booleans cannot substitute for those controller states or registry checks.
Successful preparation reaches `READY` with the producer inactive.

## Start and observation use the live controller states

`START` arms the DE, starts LCD1, and then starts the DSI video engine. LCD1
admission requires DE armed and DSI prepared. DSI admission requires DE armed,
LCD1 active, completed panel preparation, and the same live resource token.

`ACTIVE` records successful composed controller startup. It does not prove
panel reception, correct electrical signaling, or visible pixels. The owner
must explicitly request `OBSERVE` to run the DE, LCD1, and DSI observation paths.
Observation preserves ownership and does not acknowledge hardware flags.

Each leaf retains its existing deadline, readback, and fault behavior. The
coordinator performs one provider or leaf step per poll, with additional time
callbacks. It uses one monotonic clock and one deadline for the complete
requested operation. Pending panel and completion callbacks are reauthenticated
before their next step. The owner and provider must remain serialized and
non-reentrant.

## Stop requires independent transaction and output completion

`STOP` follows this order:

1. Stop the DSI instruction engine while DE and LCD1 continue providing pixels.
2. Disable the DE producer while LCD1 and the required clocks remain available.
3. Wait for independent evidence that DE memory transactions have drained.
4. Stop LCD1 after the platform provider authenticates that drain proof.
5. Wait for independent external-output and panel quiescence evidence.

The drain proof must cover fetches, prefetch, writeback, offline composition,
and register queues, with new transactions excluded. A cleared enable bit or
frame-end observation cannot establish these conditions. The output proof must
cover the external path and panel while command-engine exclusion remains held.

Each proof has a provider-owned identity and generation. It binds the resource
and framebuffer token generations and records completion after its phase began.
The coordinator rejects missing, stale, future-dated, or mismatched proof
metadata. The provider remains responsible for authenticating the underlying
physical evidence; valid metadata alone does not prove drain.

Successful stop reaches `STOPPED_RETAINED`. The framebuffer mapping, tokens,
PHY state, and clocks remain owned. There is no release or restart API. Release
requires verified PHY/PLL shutdown, panel and shared-resource restoration,
transaction isolation and drain, and ordered return of memory/resource leases.
Those operations are not implemented by the existing leaf controllers.

Any failure reaches `FAULT_RETAINED`, preserving the failing phase and leaf
diagnostics. No rollback, retry, automatic stop, reset, buffer retirement, or
lease release follows a failure. Later polls of a terminal state make no
callbacks. The platform must retain reservations even if the client times out.

## Production bindings remain explicit dependencies

The coordinator requires actual providers; it does not supply an alternate
clock registry or fabricate hardware completion. The shared CCU/reset binding
must acquire the DE, VO0, LCD1, DSI1, PHY, PLL-parent, and fabric domains through
the common registry and register mutex. Panel pins and rails require the
selected panel's concrete platform configuration.

The existing DMA-controller memory policy cannot establish DE ownership or
IOVA reachability. A DE memory binding must authenticate the actual IOMMU
master route, mapping and allocator generations, aliases, cache visibility,
and burst bounds. An independent DE transaction-drain mechanism remains
unresolved. Cold display-PLL provisioning and electrical panel qualification
also remain required before physical use.

## Validation exercises the composed implementations once

`checks/display-scanout.py` builds one ASan/UBSan model linking the actual PHY,
LP command, DSI video, LCD1, and DE implementations. One lifecycle runs
preparation, start, observation, and stop with pending acquisitions and drain
proofs. The model panel executes a native DCS NOP command; this is a controller
composition fixture, not a production panel initialization sequence.

Fault cases restore checkpoints from that successful lifecycle. They cover
resource-token mutation, mismatched frame extents, authority denial, uncertain
writes, post-write deadlines, reentry, DSI stop failure, wrong drain generation,
drain failure, and an indefinitely pending drain. They verify retained tokens
and that LCD1 stays active until independent DE drain succeeds.

The composed model and freestanding AArch64 compile passed in one batch. The
coordinator object is 4709 bytes. Direct stack frames are 624 bytes for
initialization, 48 for polling, and 176 for leaf initialization. Task sizing
must also include the nested controller planners and provider callbacks.

Exact executed inputs, executable, commands, native object, stack report, and
results are preserved under
`~/.cache/sunstoneos/evidence/display-scanout-20261016/initial-snapshot/`.
Existing leaf source-comparison evidence was reused. No physical hardware or
performance benchmark was run.
