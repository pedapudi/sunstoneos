# Native IPv4 service over the radio transport

The network service runs FreeRTOS+TCP with the native AIC8800 Ethernet
transport. It implements fixed IPv4 or DHCPv4 addressing, DNS client lookups, ARP,
ICMP replies, UDP, outgoing TCP connections, and one bounded IPv4 TCP listener. Applications submit copied
requests through `sunstone_net.h`; the privileged service owns socket records
and radio polling. This document assumes familiarity with FreeRTOS tasks and IP sockets.

The implementation passed native AArch64 compilation and the composed network checks. The composed model uses
actual FreeRTOS tasks, FreeRTOS+TCP, socket allocation, the Ethernet adapter,
and the SDIO controller model. Physical radio networking remains unqualified.

## Source and configuration

The external stack is FreeRTOS+TCP 4.4.2 at revision
`b86ad086e9870099bb0ac18a75dcb7f6dd508cc7`, under the MIT license.
The archive SHA-256 is
`60a2eea0de133948bb9a12f09abcba8e47800c3bbf61fe42fc7db6af453f142a`.
The source comes from the [FreeRTOS repository](https://github.com/FreeRTOS/FreeRTOS-Plus-TCP/tree/b86ad086e9870099bb0ac18a75dcb7f6dd508cc7).
The source authenticator compares the complete source directory, license, and
release history with that archive before building.

The private configuration enables software IP, UDP, and TCP checksums.
It sets a 1500-byte MTU, a 1460-byte TCP MSS, and four MSS units for each
TCP send and receive stream. IPv6 and service discovery remain required follow-on work.
The application API supports UDP open, TCP open, connect, send, receive, close,
address status, DNS resolution, listen, accept, and cancellation.

The optional `cmake/radio-network.cmake` builds `radio-network-model` with
`SUNSTONE_TCP_DIR`, `SUNSTONE_RSN_DIR`, and `SUNSTONE_NETWORK_C_HEADERS`.
Private configuration headers precede the general kernel headers for this
target. The external TCP and RSN sources remain unmodified and outside Git.
A production image must link the same adapter, allocator, stack sources,
configuration, and socket disposal hook with its qualified platform binding.

## Ownership and memory remain bounded

The service has four registered client tasks and four sockets. Each client
may have one outstanding request or unread reply. Requests and replies copy
at most 1472 payload bytes; applications receive opaque handles rather than
stack pointers. Task identity restricts access to each client's sockets.
Public request and result spans must exclude the service, radio objects, and
borrowed immutable firmware blobs; admission checks those ranges.

The image reserves a 256 KiB heap for the pinned FreeRTOS `heap_4` allocator.
The heap supplies socket objects, event groups, TCP streams, stack tasks, and
stack queues. Definite allocation refusal leaves the allocator usable.
The IP task and its interface remain allocated for the image lifetime.
The service does not reset or replace a live stack instance.

The Ethernet pool contains 24 fixed 1536-byte slots. The adapter copies each
outgoing frame into an eight-entry queue before releasing the stack's frame
when requested. A full queue refuses the frame and releases that descriptor.
The worker owns one additional current frame until its SDIO result is known.
Ordinary transmission completion proves bus transfer; it does not prove an
access point acknowledgement or remote application receipt.

Incoming validated Ethernet frames are copied into the fixed pool. Successful
IP-event admission transfers descriptor ownership to the IP task. Pool
exhaustion or a full IP queue drops the copied frame and releases any acquired
descriptor. The adapter consumes the original radio record after this decision.
The [receive dispatcher](radio-dispatch.md) owns the transport receive path.
It routes command confirmations and security packets to their existing workers,
copies scan and station events into a bounded queue, and retires complete
unsupported records. Ambiguous framing or malformed active confirmations retain
ownership and stop reception. The network service exposes copied event reads
and diagnostic snapshots without allowing a second raw-record consumer.

## Deadlines preserve socket and packet ownership

Each request carries a finite timeout in FreeRTOS ticks. The deadline begins
when the service copies the request, including queue time. Stack send and
receive waits are zero. The worker checks the deadline before advancing an
operation and before reporting successful completion. A late result retains
known byte counts, received payload, and the socket handle in its reply.
A positive send result means that the stack accepted those bytes. On an ARP
cache miss, the pinned UDP stack consumes the datagram to generate an ARP
request. Applications needing reliable delivery must use an acknowledged
protocol or retry according to their protocol.

Public FreeRTOS bind and close functions contain unbounded internal waits.
The adapter instead submits their source-defined IP events with zero queue
wait. Bind completion requires the actual socket event group and bound-list
membership. Close completion requires the stack's socket-free hook after
buffer and event-group disposal. The hook retires the allocation address
before returning it to the heap, so address reuse cannot complete another
socket's close.

A timed-out open, connect, or close retains its socket record. Explicit close
can finish disposal, including after a previous close has completed in the
IP task. A timed-out receive does not consume a future frame. A late receive
that already removed bytes reports those bytes in the timeout reply.
Waiting for a reply does not cancel the underlying request.

Packets admitted to SDIO remain owned through an uncertain transfer or fault.
A queued frame that has never been admitted may be dropped when its separate
queue timeout expires. Terminal faults stop further radio polling and retain
all unresolved storage for external quiescence and recovery.

## Security authority belongs to one association

Activation requires a completed authorized association, idle data transmission,
a ready card and controller, and no outstanding SDIO or security transfer.
The bootstrap task transfers exclusive transport ownership to the worker.
Configuration, entropy contexts, and the radio object graph remain pinned.
Held receive records transfer with that ownership.

The adapter checks the association generation before admitting queued frames.
Generation change, link loss, or unresolved roaming revokes the interface and
all application handles. EAPOL remains owned by the supplicant and cannot enter
the ordinary Ethernet output queue.

A group-key renewal closes the controlled port for a few firmware actions. The
supplicant leaves its authorized state, while the association generation, link
and address lease stay unchanged. The network then reports not ready, which
pauses ordinary data admission and every pending socket request. Pending
LISTEN, ACCEPT and RESOLVE requests wait through this pause in the same way as
other operations. They end with `SUNSTONE_NET_ADDRESS_LOST` only after address
revocation or lease expiry, and with `SUNSTONE_NET_LINK_LOST` after a generation
change, link loss or network fault. Each request's own timeout still applies.

Production configuration requires cryptographic random-number and TCP sequence
providers plus an admission record identifying their qualification. A
random-number provider returns 0 for a temporary shortage or lock contention.
The FreeRTOS+TCP random-number hook then fails only that call. In the pinned
stack, with IPv6 and link-local fallback disabled, the effect depends on the
call site:

- A DHCP discover retransmission is skipped. The retransmission period still
  doubles, so a later check tries again until the period passes the discover
  limit and DHCP gives up, as it would without an offer.
- A DHCP request keeps the previous transaction identifier and is still sent.
- At DHCP start, no DHCP socket or timer is created, so address acquisition
  ends when the adapter's address timeout reports failure.
- A DNS lookup registers no callback and sends no query, so the resolve request
  ends at its own timeout.
- Automatic local port selection fails, so that bind or connect returns
  `-pdFREERTOS_ERRNO_EADDRNOTAVAIL`.

Each outcome is bounded. Any other random-number failure, and every TCP
sequence failure, closes output synchronously. The TCP sequence hook has no
error return; its placeholder result cannot authorize an outgoing SYN after
that closure.
The composition's deterministic providers are test inputs only.

The [retained session](radio-session.md#explicit-healthy-reconnect-preserves-the-existing-service)
supports explicit healthy reconnect with fixed addressing or DHCP after all socket
receipts and an IP-task offline fence. It keeps this stack instance and disposes
old handles before changing association generation. Automatic recovery after
unsolicited loss, stack retirement and full radio power modes
remain unimplemented.

## Composition and external proof limits

The fixture begins with a reconstructed authorized WPA2 association. It uses
real radio receive validation and packet-number checks, but its firmware peer
supplies decrypted CCMP records. It does not execute physical firmware,
authenticate an access point, establish real entropy, or perform CCMP decryption.
The existing RSN handshake evidence remains separate.

The synthetic peer validates outgoing IPv4 and transport checksums, answers ARP,
echoes UDP, and completes a TCP handshake and data exchange. The model also
exercises allocation refusal, socket limits, finite receive waits, IP-queue
saturation, retained close ownership, and association-generation revocation.
A separate short invocation checks that a temporary random-number refusal
leaves the network open and that a sequence failure blocks SYN output.
The `rekey` invocation withdraws supplicant authorization, as a group-key
renewal does, while an ACCEPT is pending and again while a RESOLVE is pending.
Both requests remain pending during the pause. After authorization returns, the
ACCEPT completes and the accepted socket exchanges data; the unanswered RESOLVE
ends at its own timeout. With the earlier address check, the ACCEPT ended
during the pause. Both runs are preserved under
`~/.cache/sunstoneos/evidence/audit-radio-20261020/`, in `checks-final/net-rekey/`
and `checks-negative/net-rekey/`.
Another delays a positive stack send result beyond its request deadline and
checks that the timeout reply preserves accepted bytes and the socket handle.
The lifecycle and entropy results reuse their original executable; the delayed
send check adds only a model link wrapper around the actual stack send call.
A final isolated admission check rejects result buffers overlapping retained
firmware blobs. It reuses the earlier protocol and lifecycle evidence.

Run `checks/radio-network.py --help` for source, compiler, emulator, and evidence
arguments. Evidence includes command output, the executable digest, and source
digests. The emulator has no host network interface. No physical queue request
is appropriate until SDIO clock, pin, power, recovery, firmware, RF profile,
entropy, and authorized network credentials have qualified platform bindings.

## Address acquisition and DNS ownership

A zero `network.dhcp` field preserves fixed IPv4. Fixed profiles may supply a
DNS server in `network.dns`. DHCP profiles require a finite `address_timeout`
and `lease_cap`, both in FreeRTOS ticks and no greater than half the tick range.
The stack's DHCP engine sends discovery and request messages and renews its
leases. The adapter never treats fallback defaults as a DHCP success.

Address authority is separate from association authority. The initial DHCP
acknowledgement must include a nonzero lease time. Each acknowledged lease
expires at the earlier of the server's original lease and the admitted cap.
The adapter uses the server value before the stack applies its minimum lease
or default lease substitutions. It clears the retained lease option after
the stack copies the renewal timer and before the initial request, so each
accepted acknowledgement must carry a fresh lease option. An unstable address
publication denies admission without permanently revoking an otherwise valid
lease. An acknowledgement after the lease deadline cannot restore authority.
A changed address also revokes the address lifetime; existing
sockets remain retained and require explicit close. Address loss returns
`SUNSTONE_NET_ADDRESS_LOST`, while association or provider failure returns
`SUNSTONE_NET_LINK_LOST`. DHCP control packets may pass during acquisition;
ordinary queued packets cannot enter SDIO without current address authority.

An explicitly admitted healthy reconnect retires the old DHCP lifetime in the
same IP task. Its processed offline epoch follows actual down processing, DHCP
socket closure and timer stop. The adapter parks the DHCP state machine before
acknowledging that epoch. After new radio authorization, an epoch-bound resume
request lets the IP task clear old address, DNS, gateway and lease publication
and start a fresh acquisition interval. It uses the existing NetworkDown/reset
path; it does not create DHCP sockets from the radio task. Only the new ACK
reopens client admission. Offline suppresses lease publication, and no failed
address or provider lifetime is reset.

This follows pinned FreeRTOS+TCP
[`prvProcessNetworkDownEvent`](https://github.com/FreeRTOS/FreeRTOS-Plus-TCP/blob/b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source/FreeRTOS_IP_Utils.c#L819-L955)
and [`vDHCPStop` and initialization](https://github.com/FreeRTOS/FreeRTOS-Plus-TCP/blob/b86ad086e9870099bb0ac18a75dcb7f6dd508cc7/source/FreeRTOS_DHCP.c#L319-L325).
The DHCP initialization at lines 959–975 chooses a new transaction ID and clears
the old offered address and server. A queued event in the parked state only
disables its timer (lines 798–801). The existing transaction-ID checks protect
against an old ACK; the DHCP wire format carries no native association epoch.

The focused [DHCP reconnect check](../checks/radio-dhcp-reconnect.c) passed two
cases on one AArch64 image with actual FreeRTOS IP-task and owned socket-service
execution. The healthy case checks the processed offline epoch, closed DHCP
socket and stopped timer, an event processed while parked, old lease expiry
during offline, a fresh transaction that rejects an old ACK, a changed address,
and a new UDP exchange followed by CLOSE. The acquisition-timeout case restores
the same pre-offline checkpoint and suppresses only the modeled server replies;
clients stay closed and failed address authority cannot be restored. Initial
DORA is setup for the healthy case and is not repeated by the timeout case.

Firmware and both authorized association generations are modeled checkpoints;
this check does not repeat WPA2 handshakes or execute board startup, the radio
session loop, or `board_radio_poll`. The previously completed reconnect evidence
covers those separate radio/security boundaries. Wire responses, SDIO hardware
and entropy remain modeled; physical DHCP reconnection remains unqualified.
Exact image, source/tool closure, setup stops and checkpoint-bound results are in
`~/.cache/sunstoneos/evidence/radio-dhcp-reconnect-20261020/`.

`SUNSTONE_NET_ADDRESS` waits for address authority and copies sixteen bytes:
IPv4 address, netmask, gateway, and DNS server, each in network byte order.
`SUNSTONE_NET_RESOLVE` accepts a copied ASCII hostname in `payload`, with its
length in `bytes` and no terminating NUL. The length must be between one and
253 bytes. The successful reply copies the IPv4 address into `address`.
A missing DNS server refuses resolution. DNS cache, multicast discovery and
IPv6 resolution are disabled in this profile.

The existing IP-task event hook owns DNS starts, cancellation, parser callbacks
and timeout callbacks. Only the pinned DNS source's bind and close call sites
are redirected to the stack's actual IP-task socket internals. The external
source remains unmodified. Lookup send and receive waits are zero; neither the
radio owner nor the IP task enters the public socket bind or close wait.
The temporary DNS socket closes synchronously, allowing the pinned stack's
asynchronous response parser to receive replies without a lingering socket
intercepting them.

Each client has one retained DNS slot. Completion is published only after the
IP task finishes the previous callback dispatch. `sunstone_net_cancel` admits
only the current client's outstanding ticket. Timeout, cancellation and address
or provider loss leave DNS storage busy until the IP task acknowledges actual
callback cancellation. A client may receive its terminal reply before that
acknowledgement; another DNS request then returns busy. No stale callback may
become a later request's result. Neither a software deadline nor cancellation
establishes physical transport drain or permits reuse of an unresolved SDIO
transfer.

The focused checker, `checks/radio-network-dhcp-dns.py`, builds one AArch64
composition and runs DHCP acquisition, renewal and expiry; acquisition failure;
fixed IPv4 DNS and cancellation; and renewal without a fresh lease option.
These four cases pass with actual stack parsing and callbacks over the SDIO
model. The fixture requires an actual ARP cache entry before DNS and an
IP-task cancellation acknowledgement before reusing a DNS slot. Standard
string routines required by enabled DNS also pass focused semantics checks.
Compiler evidence covers all 92 linked objects and 255 source dependencies.
The preserved batch is under
`~/.cache/sunstoneos/evidence/radio-dhcp-dns-20261017/batch3/`.

Earlier setup and fixture failures remain preserved beside the final image.
One earlier DHCP composition missed a 500-tick DNS deadline after ARP became
ready; the final composition passes, but does not establish timing robustness.
These checks do not qualify physical RF, network credentials, entropy or radio
operation. Run the focused checker's `--help` for pinned-source and tool inputs,
including TinyUSB required by the root build configuration.

## One IPv4 listener retains pending and accepted children

LISTEN and ACCEPT append operations 9 and 10 without changing copied request or
reply layouts. LISTEN takes a bound TCP handle and fixed backlog one. Its address,
port and payload fields must be zero. Only one listener may exist in the service.
ACCEPT takes that listener handle with the same zero fields. One successful
accept returns a distinct opaque child handle and copied peer IPv4/port. The
same registered task owns both handles. A second successful accept is refused.

Before LISTEN, the owner reserves a second entry in the four-socket ledger.
OPEN skips that entry even when its socket pointer is empty. The reservation
covers the stack's pending child before ACCEPT transfers it to the application.
It remains reserved after accepted-child disposal until the listener's own
disposal. The stack can admit another pending child after the first child closes.
A ledger reservation alone does not limit stack allocations: the pinned IPv4
listen handler checks `usChildCount >= usBacklog` before allocating, and copies
the child into the parent count in the sole IP task. Backlog one supplies the
actual child bound. Socket reuse is disabled.

LISTEN and ACCEPT run through a per-client handoff to the existing IP-task hook.
The pinned public accept function queues an accept event with `portMAX_DELAY`
after taking a child, even when its receive wait is zero. The event sender forces
that wait to zero in IP-task context. The accept event remains best effort when
the queue is full; the API ignores enqueue failure. This service accepts one
child and does not depend on that event to admit another connection.

Timeout, cancellation and link/address loss wait for the IP-task handoff fence.
They cannot discard a running accept. Before publishing completion, the IP task
registers the accepted allocation with the existing socket-free hook. The owner
then publishes the copied peer tuple and fresh handle. A late accepted child
remains independently closeable in its TIMEOUT, CANCELED or address-loss reply.
No reply exposes the stack pointer.

ACCEPT completion precedes a later listener CLOSE request on the IP queue.
Listener close recursively disposes unaccepted children; its free-hook receipt
therefore fences their disposal. Accepted children have left that automatic
close set and require their own CLOSE. CLOSE remains available after address
loss. Deadlines and software cancellation never establish disposal by themselves.

These contracts follow the pinned FreeRTOS+TCP source at revision
`b86ad086e9870099bb0ac18a75dcb7f6dd508cc7`: `FreeRTOS_Sockets.c`, lines 3888–4079
and 4724–4780, implements accept/listen; lines 2280–2328 handles child disposal.
`FreeRTOS_TCP_State_Handling_IPv4.c`, lines 125–160, checks backlog before child
allocation. `FreeRTOS_TCP_State_Handling.c`, lines 1000–1040, records child
ownership. `FreeRTOS_IP.c`, lines 1474–1481, bounds IP-task event submission.

### The generated IPv4 return path checksums the outgoing extent

The pinned stack computes an IPv4 TCP reply checksum before updating its network
descriptor length. A minimal SYN occupies 54 Ethernet bytes; its option-bearing
SYN-ACK occupies 66. The original checksum call supplies the shorter incoming
length and returns an invalid-length result, leaving the reply checksum invalid.
The generated-source adaptation in
`patches/freertos-tcp-ipv4-return-checksum.patch` supplies the outgoing IP length
plus the Ethernet header to that call. Both native board and network-model builds
apply the patch to a separate generated file. The authenticated external source
and its MIT notices remain preserved.

This configuration uses fixed 1,536-byte buffers from
`uxNetworkInterfaceAllocateRAMToBuffers` and disables zero-copy TX. The expanded
reply fits the actual allocation after buffer padding. The adaptation does not
admit variable buffers or a different zero-copy configuration. An independent
peer checksum and a complete real-stack handshake exercise the changed path.

### Bounded listener evidence

The selected native board image links the actual service, IP stack, radio owner
and providers. Seven AArch64 composition cases execute real FreeRTOS tasks,
FreeRTOS+TCP and service ownership with modeled RF/security checkpoints and SDIO.
They cover the copied exchange and backlog refusal, a full accept-event queue,
cancellation and deadline fences, late and address-lost accepted children, and
listener disposal with an unaccepted child. Initial burst input may be dropped
by modeled RX; an additional SYN with the accepted child live exercises the
actual backlog refusal branch. The model does not substitute accept or the
listen handler's decisions.

Four sanitizer cases execute the actual application/profile code with modeled
socket replies. Five host cases consume the actual compiled USB result producer
and check success, failure, pending output and profile opt-in binding. Those
fixtures do not execute the full native image. Earlier outgoing-client and
radio/entropy evidence remains in its original frozen scope.

Inputs, exact linked objects, results and source-review notes are preserved under
`~/.cache/sunstoneos/evidence/net-inbound-20261020/`. Physical radio networking,
entropy quality, RF operation and platform recovery remain unqualified. Socket
disposal and copied result delivery do not establish TCP or RF wire drain.
