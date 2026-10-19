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

## Native ownership binds clocks, retained memory and controller access

The native owner API in `include/a733_display_native.h` composes the existing
media-clock and display-PLL ownership handoff with a concrete framebuffer
provider. `a733_display_native_init()` accepts the actual completed media-clock,
display-PLL and display IOMMU controllers. Preparation publishes the existing
common clock token through `a733_display_handoff`; it creates no second clock
registry. The original producers and their PCK lease remain retained.

The caller supplies two distinct DEVICE-owned allocations from the existing
memory pool: an immutable XRGB8888 frame and its 32 KiB IOMMU tables. The
request describes the complete frame allocation. The binding authenticates
allocation generations and exact extents, the display master's active READ-only
IOMMU span, every table word, and each CPU page's uncached translation. It
checks the actual tables and allocator before publication and each admission.
The image owner must exclude CPU aliases and writers throughout the session.
A fault retains both allocations; there is no return to CPU ownership.

Ordered native controller accesses use the common platform register mutex and
verify Device mappings. The binding admits the existing DE, LCD1, VO0 selector,
DSI1, PHY1 and selected host CCU windows. An uncertain access retains the mutex.
One designated privileged FreeRTOS task calls prepare, poll and request through
the native API. The private build fragment `cmake/a733-dsi-native.cmake` adds
these sources to an existing admitted native image.

The image must supply authenticated initial isolation and drain, stable supplies,
remote-writer exclusion, analog-profile provenance and a selected panel's real
power, reset and command-controller sequence. Neither a selected panel nor a
qualified cold PLL profile is provided. The standalone application below requires
these physical inputs. Register reads and numeric generations cannot establish
them. The shared retained IOMMU provider must keep all other masters
excluded for the complete session.

The native binding supplies no DE drain or output-quiescence proof. A STOP
request disables DSI and DE, then faults at the independent DE-drain phase with
LCD1, clocks, framebuffer and tables retained. It cannot reach successful stop,
release memory, or authorize another session. Physical signaling, panel reception,
and safe transaction/output drain remain unqualified. No MIPI display is attached
and the panel remains undecided.

## Native provider validation uses the actual producers

`checks/display-native.py` reuses the media-clock/PLL producer fixture and links
the actual allocator, IOMMU tables, handoff, coordinator and leaf controllers.
Its ASan/UBSan model passed preparation and start, refusal of an invalid CPU
mapping before publication, and refusal of a changed live IOMMU mapping before
DE arming. Unsupported drain preserved active LCD1 and retained both memory
allocations. Wrong-task admission had no effects. An uncertain native write
retained the common mutex; terminal polling made no further MMIO accesses.
Native register accesses, CPU translation, mutexes and the panel are modeled;
these results do not establish physical operation.

The provider core and native MMIO adapter compiled for freestanding AArch64.
The adapter used the existing FreeRTOS headers and configured cached-port
headers. The exact commands, dependencies, results and objects are preserved in
`~/.cache/sunstoneos/evidence/display-native-20261018/final/`; the immutable
snapshot is its sibling `snapshot/`. Existing leaf and ownership-publication
evidence was reused. No hardware access or performance measurement occurred.

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

## Standalone retained DSI application

`apps/dsi-scanout` builds `sunstone-a733-dsi-scanout` without selecting a panel.
The image composes the actual PMIC, rail and clock leases, pin manager,
media-clock and PLL producers, display IOMMU and native scanout provider. Its
single privileged owner creates an immutable XRGB8888 color-bar framebuffer,
starts the DE1/LCD1/DSI1 route, and copies one controller observation to USB0.
The observation does not establish visible pixels, panel readiness or electrical
correctness. No panel is selected or attached, and physical qualification is
false.

The private target uses the existing extended memory provider unchanged. Entry
must reserve and exclude aliases and remote writers from the complete 768 MiB
write-back pool `[0x48000000,0x78000000)` and 16 MiB uncached pool
`[0x78000000,0x79000000)`. The runtime validates both full pools before releasing
the scheduler. DSI uses the uncached pool for 32 KiB of aligned IOMMU tables and
one page-rounded framebuffer including explicit, qualified fetch padding. A
1280×720 image with 5120-byte pitch occupies 3,686,400 bytes before padding;
1920×1080 with 7680-byte pitch occupies 8,294,400 bytes. Both fit the 16 MiB pool.
Narrower valid modes remain available within the existing planners' constraints;
this path does not admit modes beyond their limits.

`profile.py` encodes a caller-supplied value-only panel JSON and a bound 592-byte
board prefix into a sealed 4096-byte session. No command sequence or electrical
values are supplied by default. The profile includes exact timing and rates,
seven qualified initial PLL words and their generations, supplies, pin states,
fetch padding, and bounded GPIO, delay and LP packet steps. DSI1 uses the pinned
real-silicon PD10–PD19 mux4 route with four lanes. Optional power, reset and
backlight pins are explicit. The actual timing, PLL, pin and packet validators
run before providers. Up to 48 steps and 1536 payload bytes are admitted; LP
packets run before video, and GPIO/delay steps may continue after video. A panel
that requires unsupported reads or another initialization mechanism needs a
further implementation.

`prepare.py` binds the selected image, complete session, full memory reservation,
initial isolation, electrical/PLL record and selected-panel record. The board
prefix must already bind the initial qualification and recovery digest, memory
and wiring authority; the value encoder does not manufacture those facts. Each
DSI fact record must identify the exact profile values and image, include frozen
source evidence, and state that physical access established the required facts.
The panel record also requires a model and datasheet. Model-only records are
not launch authority. The private 288-byte supervised boot record carries the
existing extended-memory layout and separate full-session and entry digests.
Its retained copy and sealed session survive ordinary BSS clearing. The private
linker bounds the heap, including the exception stack, below the uploaded
session. Shared CPU, GIC and memory-provider sources are unchanged.

After video starts, the owner completes the explicit post-video steps, obtains a
native controller observation and scans display IOMMU faults. It publishes a
256-byte immutable result with session/frame hashes, geometry, generations,
controller state and raw observations. It continues bounded controller and
IOMMU observations while the original result remains immutable. A deadline,
uncertain access or fault closes admission and retains memory and resources.
There is no successful STOP, release, retry or panel power-down claim: the
existing provider still lacks a DE drain proof.

The USB0 response has a 128-byte header with magic `0x31525344`, version 1,
status, copied byte count, epoch/generation, session and payload SHA256 hashes,
and zero reserved bytes. Status 1 appends exactly 256 bytes; pending or failure
has no payload. `observe.py` validates identity, dimensions, allocation extent,
frame hash, panel identity and completed controller/IOMMU state. `capture.py`
preserves that copied observation under the existing supervised claim and
frozen-bundle workflow. It never sends desktop events or storage commands.

Offline application evidence is under
`~/.cache/sunstoneos/evidence/dsi-scanout-application-20261019/`. Actual producer
composition passes at 720p and 1080p with explicit model-only electrical and panel
inputs; mapping mismatch, IOMMU fault and expiry retain both allocations. The
linked private serializer, admission and seal execute with modeled boundaries;
real host decoding and durable capture validate the copied observations. The
selected SRAM launcher cases execute only its private admission and handoff
boundary. Root separately reviewed linked startup geometry and retained
lifetimes; shared cache/table setup, GIC, secondary startup and devices were not
executed in these checks. Existing provider/planner evidence remains applicable.
A selected panel, qualified analog profile, actual rail and bank voltages,
connector/cable verification, initial isolation, fetch-padding proof and
supervised physical recovery remain prerequisites for a physical run.
