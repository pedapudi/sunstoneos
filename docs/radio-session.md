# One native task owns radio startup and socket service

The native radio session connects the actual radio platform lifetime to SDIO
setup, silicon identity, firmware loading, runtime messages, Wi-Fi management,
packet service and FreeRTOS+TCP sockets. One native FreeRTOS composition passed
with modeled MMIO and explicit firmware/association checkpoints. Physical radio
operation remains unqualified.

This document is for platform and driver developers. The session supplies
coordination and ownership. The existing controller, firmware and protocol
workers implement their respective hardware and wire formats.

## Startup retains one resource and protocol graph

The [session API](../include/a733_radio_session.h) requires an initialized
[radio platform](radio-platform.md) that has not started acquiring resources.
It copies the immutable profile and retains its owner task, expected silicon
identity, firmware spans and runtime callback context. Firmware bytes remain
immutable through verification, use, fault and external recovery.

One recorded FreeRTOS task advances every startup stage:

1. Poll the actual platform until it has acquired clocks, rails and pins,
   observed its supplies, enabled the module and enumerated its SDIO card.
2. Borrow that actual card once and initialize the existing transport.
3. Perform function setup and the authorized bootstrap write, then read silicon
   identity through the real transport. The firmware engine checks that observed
   identity against the configured expectation before accepting ownership.
4. Verify the pinned firmware artifacts and perform the existing bounded upload.
   A start confirmation permits runtime attachment; it does not establish
   association, Bluetooth readiness or a secure network.
5. Attach the actual runtime and exclusive receive dispatcher, initialize Wi-Fi
   management and attach packet service.

The session calls `a733_radio_platform_live` directly before advancing any
borrowed protocol work. There is no configurable readiness callback or omitted
production guard. The same task must satisfy both the session's task identity
and the platform adapter's scheduler and execution-context checks.

Every constructor consumes fresh private storage. A failed partial construction
cannot be retried over existing workers. Session, platform, managers and firmware
spans must be independent. Trusted callback backing must also remain separate
and live; callbacks are bounded and non-reentrant. Admission and copied outputs
also use the actual PMIC provider’s exclusion callback to protect its adapter,
lock and counter backing.

## Runtime renewal pauses work until both owners accept the bound

The platform has a fixed lifetime deadline. Startup uses a separate absolute
bootstrap deadline within that lifetime. Runtime uses bounded leases that may
extend only within the same fixed lifetime.

Before attempting renewal, the session authenticates the current platform bound.
The transport then verifies its old deadline and its actual drained boundary:
no pending controller request, retained aggregate, packet transmission or
unfinished binding action may cross renewal. Only that API can extend the
transport deadline.

After transport renewal succeeds, the session obtains the same bound from the
actual platform guard. A busy guard pauses all protocol polling and command
admission until the platform accepts it. Failure retains the entire graph.
The transition never replaces the borrowed card generation or revives an
expired active operation.

Lease duration and renewal margin are immutable session settings. A transfer or
consumer that prevents a drained boundary until expiry causes retained failure.
The session does not extend deadlines merely to preserve apparent progress.

## Socket queues use the same polling task

`sunstone_net_init_owned` creates the existing bounded request and reply queues
on the running radio task. It does not create a second radio worker.
`sunstone_net_activate` starts the actual IP stack on that owner, and
`sunstone_net_poll_owned` performs one existing network and socket-service
iteration on the recorded task.

Application tasks continue to submit copied requests and receive copied replies
through `sunstone_net_submit` and `sunstone_net_receive`. Socket handles, event
copies and packet ownership retain their existing semantics. The IP task may
queue copied output packets; it never polls the radio or consumes dispatcher
events. The pre-scheduler network initializer preserves its existing behavior
for users of that separate entry point.

The socket-service structure includes an owned-polling mode field. Rebuild its
consumers together with the service; it has no persistent or wire ABI.

The session permits network activation only through its guarded owner path.
The configuration must name its actual transport, dispatcher and owner task.
Packet authorization and usable entropy providers remain prerequisites of the
existing network adapter. A session reporting NETWORK means socket service has
been activated; current link readiness remains a separate network query.

## Association and security require explicit admission

Management readiness permits separately authorized scan, association and
disconnection requests. The session preserves the lower workers' channel,
beacon, connection and open-port authority requirements. These owner APIs do
not grant application tasks direct radio access.

WPA2 attachment uses the actual RSN implementation and requires an initialized
security profile with its observed AP information, key material and production
entropy callback. Initializing that profile consumes its private lifetime;
a failed subsequent attachment retains the session rather than replacing keys.
The runtime binding challenge likewise requires a successful real platform-ID
callback. The session supplies neither an identity stub nor an entropy fallback.

The hardware entropy source has not completed production security qualification.
Deterministic fixture entropy establishes only software control flow. It does
not qualify WPA2, TCP sequence unpredictability or a secure physical network.

## Faults close admission and preserve outstanding work

A failed actual platform guard stops the host and protocol workers, closes
network admission and retains resource tokens, firmware, receive records,
packets, requests and sockets. The owned socket loop may continue delivering
copied link-loss replies and notifying the IP stack that the interface is down.
That failed-network path performs no further radio MMIO.

No session teardown, module reset, resource release or reinitialization API
exists. Explicit socket disposal retains its existing stack completion contract;
it does not release the radio platform. Native shutdown requires a separate
protocol that drains every borrower and proves controller/module quiescence.
Independent hardware recovery remains required for stalled MMIO.

## The focused composition reuses existing protocol evidence

The [native fixture](../checks/a733-radio-session.c) runs actual platform,
PMIC, rail, pin, clock, SDIO and card workers against modeled registers. It uses
real FreeRTOS tasks and the actual TCP stack and socket queues.

The fixture executes new session startup through transport setup, identity and
firmware claim. It then supplies the existing completed-upload checkpoint to
those lower workers. Their current card, transport owner and borrowed platform
generation remain unchanged. Existing firmware authentication and upload cases
are not replayed.

After actual runtime, dispatcher and Wi-Fi attachment, the fixture supplies the
existing management and association checkpoint to the Wi-Fi worker. It does
not write session readiness or replace dispatcher ownership. The session then
attaches packet service, renews the actual runtime/platform bounds, starts its
owned socket queues and exchanges one TCP payload. Registry contention between
transport renewal and platform authentication must pause all protocol progress
until both bounds agree. A subsequent actual clock
lease failure retained the active packet and socket while returning link
loss through the copied reply queue. Provider-backing alias admission and
wrong-task polling were also refused.

The focused run passed once. Its preserved executable has SHA-256
`9d3b52249a9a102697aca8c344530d53b087d876b24f484e220582ea6bf3165b`.
Inputs, build configuration, copied executable and output are retained under
`~/.cache/sunstoneos/evidence/radio-session-20261016/initial/`.
The input manifest includes the compile dependency closure. The evidence runner
also preserves firmware, source references, linker and emulator identities.

Prior component evidence remains in these directories beneath
`~/.cache/sunstoneos/evidence/`:

- `radio-platform-20261016/initial/`: actual shared-resource and card composition.
- `radio-firmware-20261016/`: artifact authentication and upload protocol.
- `radio-association-20261016/`: management/association transitions and retention.
- `radio-network-20261016/`: native socket behavior and packet ownership.
- `radio-dispatch-20261016/`: exclusive receive ownership and copied events.

The new composition cannot establish physical RF operation or complete
uninterrupted boot-to-network execution on the board. Its checkpoint assumptions,
modeled entropy and physical admission requirements remain explicit.
