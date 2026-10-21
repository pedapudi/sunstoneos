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

Port authorization can admit ordinary data frames before
`a733_radio_session_start_network` attaches the socket service; an access point
may send a broadcast immediately after the port opens. While the session is in
its management or bootstrap stage, each poll discards one such frame after
dispatcher polling and counts it in `discarded_data`. A held frame would
otherwise stop the transport receive parser. The supplicant could then never
reach its authorized state, and network attachment would never become ready.
EAPOL frames remain owned by the supplicant. Reconnect discards ordinary frames
in the same way while the network is offline.

The hardware entropy source has not completed production security qualification.
Deterministic fixture entropy establishes only software control flow. It does
not qualify WPA2, TCP sequence unpredictability or a secure physical network.

## Explicit healthy reconnect preserves the existing service

`a733_radio_session_reconnect` requests one bounded reconnect to the same access
point with fixed IPv4 settings or DHCP. The recorded owner task supplies the target,
fresh security configuration or explicit OPEN authority, disconnect reason and
absolute deadline inside the current runtime lease. The session must still be
healthy and associated. Clients must first complete every socket CLOSE and
consume every reply; listener, accept and DNS handoffs must also be finished.
Admission refuses unresolved transitions, expired deadlines and live client work. It never clears a terminal failure or reloads firmware.

The owner closes socket submission and network output, then waits for the IP
task to complete the pinned stack's down processing. That task appends a private
marker to its FIFO queue and acknowledges the requested offline epoch only when
it receives that marker. The receipt covers earlier receive and output events,
including an endpoint that was already down. Offline gates suppress subsequent
output and address publication. The owner then discards only unsent queued
packets; the retained data worker and its sequence counter survive.

For WPA2, retirement completes controlled-port closure and every key deletion
while the old station is still associated. The existing transport detaches the
retired workers only at its drained boundary. A narrowly admitted rearm requires
confirmed retirement, an empty key ledger, no RSN object, timers or actions, and
successful detach. Fresh configuration constructs new RSN state for the next
association generation. No faulted or uncertain worker can be rearmed.

For DHCP, the IP task stops the old DHCP socket and timer during down processing,
then parks the state machine before acknowledging the offline epoch. A queued
old timer event cannot restart acquisition. The old lease grants no authority
while offline; its elapsed time does not become the new acquisition deadline.
After fresh radio authorization, the owner requests resume for that same epoch.
The IP task clears old address and lease publication, resets acquisition timing,
and sends the existing stack's NetworkDown event. The stack creates its new
transaction and socket. Clients remain closed until a fresh valid ACK publishes
address authority for the new association. A new address is permitted at this
explicit boundary because old sockets were disposed before admission. Terminal
address failures are never cleared; an expiry already in progress may
conservatively retain the operation.

The existing disconnect worker requires both confirmation and indication. Only
ordinary DOWN permits the next connect request. After fresh association and
packet authorization, the owner publishes the new generation and restarts the
same IP interface. Socket submission reopens only after a new address
publication. Old handles remain invalid because their actual disposal preceded
the request and the handle counter never resets. Any deadline, protocol or
platform failure retains the complete graph.

The native board owner calls `board_radio_reconnect` with an absolute deadline
at a drained transport boundary, before polling can start another SDIO operation.
A busy attachment refuses the request without changing the session. Its existing
`board_radio_poll` advances an accepted operation using the admitted WPA2
profile and reports PENDING until the address is usable. Socket clients continue
to use the existing copied request/reply APIs. No USB command is added.

Physical reconnect, RF behavior and entropy quality remain unqualified.

## Clean link loss is recovered without disposing sockets

When the network adapter reports a clean loss (see the
[network adapter](radio-network.md#security-authority-belongs-to-one-association)),
the session stays in its network stage and `a733_radio_session_poll` returns
`A733_SESSION_LOST`. The owner then calls `a733_radio_session_recover` with the
same target and credentials that reconnect admits, or OPEN authority, and an
absolute deadline inside the runtime lease. Admission requires the link to be
down rather than unresolved, an idle Wi-Fi worker and data worker, and a
supplicant that is authorized or in a group-key handshake.

Recovery reuses the reconnect phases with three differences:

- Security retirement skips controlled-port closure and the pairwise-key
  delete, then deletes the remaining group keys with KEY_DEL. The pinned
  [disconnect handler](radio-association.md#disconnection-events-preserve-ordering-and-ownership)
  invalidates the firmware station. The pinned host path never closes the port
  afterwards, and the reference driver refuses the pairwise delete for an
  invalid station, so no command for that station is sent. The pinned
  supplicant's disconnect handling deletes the group keys, which the driver
  sends as KEY_DEL. The local pairwise record is wiped and the port is recorded
  closed.
- No disconnect command is sent, because the link is already down.
- A refused or failed reassociation that leaves the link down is retried until
  the deadline. Each attempt starts a new association generation. Rearm therefore
  accepts any association later than the retired one, not only the next one.

Only that reassociation step is retried. A second loss after reassociation
succeeds is terminal. During the WPA2 4-way handshake it leaves the network
adapter without authority to resume. After the adapter resumes, it leaves the
address unassigned. In both cases recovery cannot finish, and when the recovery
deadline passes the session faults and retains the complete graph.

During recovery the session keeps servicing socket requests, so their timeouts
and CLOSE still complete. After the address returns, the same IP interface
resumes and socket submission reopens. Firmware key and port state after a
disconnection indication has not been observed on hardware. Loss with
outstanding work or unresolved roaming still retains the complete graph.

[checks/radio-recovery-loss.c](../checks/radio-recovery-loss.c), built and run
with `checks/radio-reconnect.py --fixture loss`, executes the actual session,
network adapter, socket service and IP task on the modeled MMIO of the reconnect
composition. An idle fixed-address OPEN association with one open UDP socket
receives a source-layout disconnection indication. The session reports LOST
without fault, the network is offline, and submissions are refused while the
socket survives. Recovery refuses a foreign BSSID and missing OPEN authority. Its
first reassociation fails in the indication; the retry succeeds at association
generation three, and the address and a new TCP exchange follow. A separate
part of the same image retires a WPA2 security pair after loss: no port command,
the pairwise key wiped locally, one group-key KEY_DEL with its hardware index,
and rearm accepted for a later generation. Key state there is a modeled input,
and the WPA2 handshake after recovery is not executed.

The focused AArch64 composition in [checks/radio-reconnect.c](../checks/radio-reconnect.c)
passed once with actual FreeRTOS tasks, providers, session workers, disconnect and
connect parsing, socket disposal, an IP-task FIFO receipt and a new TCP exchange.
It refused expired admission, unread replies and live sockets, then rejected the
disposed old handle after reconnect. Separate cases in that image refused rearm
with a live key, fault or attached worker; they exercised fresh RSN creation and
controlled-port retirement without resetting the data sequence. Their initial
retired pair is modeled. That OPEN composition links the board owner wrapper
without executing it.

A separate WPA2 composition in
[checks/radio-reconnect-wpa.c](../checks/radio-reconnect-wpa.c) executed the actual
`board_radio_reconnect` wrapper and session polling. Preserved terminal RAM shows
confirmed old port closure, two key deletions, detach, disconnect, reassociation,
fresh RSN rearm, M1–M4 processing against cached peer vectors, two new keys and
port authorization, generation-two address restoration and a new TCP exchange.
The client suspended only after its successful receive and CLOSE assertions.
The borrowed generation still equals the retained platform clock token.
Board startup and `board_radio_poll` were not executed by this fixture. Firmware
upload, initial association and the old authorized security state are explicit
modeled checkpoints; register, peer firmware, entropy and RF effects remain modeled.

The WPA2 fixture reached its loop assertion because its readiness publication
overwrote the client's completion value. It emitted no PASS. The operation's
completion was instead verified from the saved terminal RAM, exact executable
and client suspension; the corrected reporting source has not been executed.
Earlier vector-format setup and request-after-poll refusals remain preserved.
Four distinct linked images cover the OPEN result and these WPA2 attempts; the
101 production objects were reused unchanged for each WPA2 image. Frozen inputs,
images, attempt history and terminal classification are under
`~/.cache/sunstoneos/evidence/radio-reconnect-20261020/`.

The later [DHCP extension](radio-network.md#address-acquisition-and-dns-ownership)
passed its two changed IP-task boundary cases without repeating radio or WPA2
reconnection. A separate native `sunstone-a733-board` build covers the actual
board caller, session and changed network structure layout. That image was not
executed. These results and exact compiled closures are retained under
`~/.cache/sunstoneos/evidence/radio-dhcp-reconnect-20261020/`.

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

After open-port authorization and before network start, the fixture delivers
one broadcast ARP frame. The session must discard it, and network start must
then succeed with the transport ready. The session code without this discard
retains the session in the same case. Both runs are preserved under
`~/.cache/sunstoneos/evidence/audit-radio-20261020/`, in `checks-final/session/`
and `checks-negative4/session/`.

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
