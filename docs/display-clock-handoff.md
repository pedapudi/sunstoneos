# Display PLL ownership transfers atomically into scanout

The native display handoff binds completed media-clock preparation and display
PLL1 (DISPLL1)
setup to the existing PHY, DSI, LCD and display-engine scanout coordinator.
It publishes stable PLL ownership through the common clock registry. It keeps
the display output and engine reserved throughout the transfer.

The implementation is [a733_display_handoff.c](../drivers/display/a733_display_handoff.c),
with its interface in [a733_display_handoff.h](../include/a733_display_handoff.h).
It implements clock ownership and controller composition. Physical operation
still requires qualified initial PLL analog state, board authority, framebuffer
admission and a selected panel.

## Bind completed producers before preparing scanout

The caller first completes native media-clock preparation for the display engine
(DE) and display-output domain (VO0), then
completes DISPLL1 setup using that exact common token. The PLL setup must use the
same handoff identity and requested output rates. The media token must reserve
DE_SYS, VO0 and DISPLL1 exclusively.

Binding performs no callbacks or hardware access. It validates both completed
objects, copies their configurations and provider identities, and claims each
for one consumer. Object addresses remain fixed for their lifetime. Copied
producers, a copied binding and a second consumer are refused.

One designated task serializes producer completion, binding and all subsequent
scanout operations. The source observers require completion after final time
checks and mutex release. They do not establish current electrical readiness.
All clocks used by the registry, producers and consumer must observe one
coherent monotonic history, including the registry publication sample.

The caller uses the handoff prepare, request and poll functions. Its embedded
scanout object is read-only to callers. Directly polling that object would
bypass native token checks and fault retention.

## Publish directly into persistent consumer storage

Resource acquisition authenticates the board request, inactive-consumer epoch,
initial-profile provenance and recovery generation through the platform provider.
The binding independently checks the exact common token and borrowed
power-controller clock (PCK) lease.
Pending admission or registry contention yields without admitting a consumer.

The registry converts DISPLL1 from exclusive to shared ownership and issues a
fresh token. It preserves the owner and every other scope, including exclusive
VO0 and DE_SYS. Publication writes directly into the binding's persistent token;
there is no release-and-reacquire interval.

Before another callback can run, the serialized coordinator updates the media
controller's token, shared scopes and exclusive scopes. It marks both producers
as transferred and publishes the scanout resource identity from the fresh token.
The PLL configuration retains its original token as historical initialization
evidence. That stale token cannot authenticate with the registry and never
authorizes consumer access.

Every consumer poll checks the current token, PCK lease and producer identities.
The binding also checks source configuration and provider snapshots around
delegated operations. It retains the active reservation if those inputs change.
Unrelated high clock-scope bits survive publication because the registry and
binding use the common 64-bit token type.

## Failures retain the reachable reservation

A refusal before publication retains the original exclusive token. A deadline
failure after publication retains the fresh token and the refreshed media
reference. The scanout resource identity remains reachable even when its
acquisition fails a subsequent time check.

If registry fault retention returns BUSY, handoff polling remains pending and
performs only metadata retention attempts. No provider or device callback runs
after the fault. Terminal failure retains all clock, frame, panel and backing
owners. This component provides no release, retry, rollback or automatic
shutdown operation.

## Platform providers still establish physical admission

The handoff supplies native clock resource acquisition. Its remaining provider
hooks must establish these independent conditions:

- Qualified provenance for every initial PLL word, stable rails and reference,
  inactive output consumers, remote-writer exclusion and independent recovery.
- Allocator ownership, immutable framebuffer extent, valid address translation,
  cache visibility, alias exclusion and required bus-burst guard coverage.
- A selected panel's rails, pins, reset sequence and commands through the actual
  DSI command controller. Command-engine completion alone is not panel acceptance.
- Independent display-engine transaction drain and external output quiescence
  before shutdown or future resource release.

All register hooks use the same common mutex as media and PLL setup, preserve
unrelated fields and retain authority after uncertain effects. A software token
does not establish any of these physical conditions by itself.

## Focused validation

[The composed check](../checks/display-clock-handoff.c) executes actual native
media and PLL preparation once. It restores that completed model at the same
addresses for subsequent handoff cases. Existing scanout register and LP-command
fixture helpers are reused; their old test entry point is not executed.

The selected cases exercise publication into actual media state followed by
PHY, panel-command, DSI, LCD and DE preparation and start. They also cover copied
and duplicate consumers, pending admission, registry contention, source mutation
before and after publication, and a late deadline with BUSY fault retention.

The focused sanitizer composition and native dependency compilation passed.
The runner archives its source and header closure before execution, runs the
focused composition with address and undefined-behavior sanitizers, and compiles
the native dependency closure. Existing register-sequence evidence remains
applicable to unchanged controller behavior. The model's analog profile, panel,
memory and physical-admission callbacks are fixtures; they do not qualify the
board or supply production providers.
