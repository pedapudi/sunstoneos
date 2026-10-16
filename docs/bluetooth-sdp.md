# Native Bluetooth SDP service discovery

The Service Discovery Protocol (SDP) client retrieves advertised service records
from an established Bluetooth peer. It opens a dedicated PSM 1 channel through
the [native L2CAP worker](bluetooth-l2cap.md), negotiates that channel, and sends
ServiceSearchAttribute requests. The caller selects a 16-bit service UUID; the
client requests all attributes from each matching record.

The implementation is [bt_sdp.c](../drivers/bluetooth/bt_sdp.c), with its public
interface in [bt_sdp.h](../include/bt_sdp.h). It is a discovery component for the
headset stack. Advertised services do not establish peer authentication, an
RFCOMM connection, hands-free control or audio transport.

## One task owns the query and its lower transport

`bt_sdp_begin` binds a zero-initialized client to an active L2CAP worker and an
established ACL link generation. The L2CAP admission callback must authorize
PSM 1 on that link's actual security state. The client and every lower worker
and H4 receive buffer must occupy disjoint storage.

The same task then calls `bt_sdp_step`, which advances the actual UART, H4, HCI,
ACL and L2CAP workers. Each call processes at most one SDP response. Packets and
results on other L2CAP channels remain owned by their callers; unrelated HCI
records remain available through the lower interfaces. Those owners must share
the task's scheduling and retain their own backing.

A query owns one opaque L2CAP channel generation. Wire CIDs and HCI handles
continue to follow the lower layers' identity and quarantine rules. The client
never obtains controller credits from an SDP response. It releases its copied
L2CAP transmit record only after the lower worker reports a terminal result.

## Continuations preserve the request and its transaction identity

SDP fields use big-endian encoding. Every request has a distinct transaction ID,
starting at 1 for the dedicated channel. Only one request is outstanding. The
client accepts a response only on its owned channel, with the matching ID, after
the complete request has entered UART.

The response's first ACL record must have been acquired after that transmission.
This is an acquisition ordering check. It does not supply physical arrival
timestamps or a wire generation field. A response for an earlier transaction
cannot append bytes or complete the current query.

A partial response supplies an opaque continuation value of at most 16 bytes.
The client copies it into the next request, which preserves the service and
attribute selection and uses a new transaction ID. Repeated continuation bytes
are permitted because their format belongs to the server. The segment limit
bounds a server that continues indefinitely.

Response fragments may split data-element headers or values at any byte boundary.
The client first assembles attribute bytes, then validates the complete result.
It advertises a per-response attribute limit that reserves room for the SDP
header, byte count and largest continuation field within its actual receive MTU.
The full SDP PDU length, byte count and continuation length must agree.

## Parsed records retain complete attribute values

The parser checks data-element types, size encodings, nested lengths and fixed
traversal bounds. Record lists must contain ascending, unique attribute IDs.
Every returned record must include its required handle and service-class list;
record handles must also be unique within the response.

`bt_sdp_record` returns a view only when the complete query has validated.
Each record identifies its advertised handle, attribute range and whether its
service-class list contains the requested UUID. UUID comparison accepts 16-bit,
32-bit and equivalent Bluetooth-base 128-bit representations. A matching search
UUID can occur elsewhere in a record, so the service-class match is explicit.

`bt_sdp_attribute` returns the retained value's offset and length within the
client's `bytes` array. These include its data-element header. Unknown attributes
keep their original encoded values, including binary and text bytes. The client
does not interpret text as a terminated string.

Protocol descriptors support a single protocol stack or alternatives. A stack
beginning with L2CAP followed by RFCOMM exposes the advertised RFCOMM channel
byte and optional L2CAP PSM. Zero PSM means the descriptor omitted that field.
These are advertisements for a future RFCOMM owner to validate and admit.
Profile-specific required attributes, version compatibility, security and server
channel validity still belong to that owner and the hands-free profile layer.

## Query expiry does not release transport ownership

The caller supplies one absolute query deadline, including channel setup and all
continuations. Expiry produces `BT_SDP_EXPIRED`, prevents result publication and
stops new requests. Remote ErrorResponse codes are retained separately from local
storage limits and malformed responses. No automatic SDP retransmission occurs.

A copied transmit packet has an independent ERTX-duration lower transport bound.
The worker may finish transmitting an already accepted packet after query expiry
so that it can preserve UART framing and retire controller ownership. It cannot
publish a late response or start another continuation after that expiry. The
per-call worker deadline bounds lower progress and is separate from the query
status; callers must continue scheduling required ownership retirement.

`bt_sdp_close` cancels a pending query and requests channel teardown under a
separate close deadline. It permits discarding this client's retained receive
records, including a malformed response held for diagnosis. Closure reaches
`BT_SDP_CLOSED` only after confirmed L2CAP closure and all associated backing has
retired. A timed-out opening can remain quarantined until confirmed ACL closure
or externally proven recovery. Close expiry or a lower fault retains ownership.

Validated result views remain available through confirmed closure. Only
`bt_sdp_release` ends their lifetime and allows the client storage to be reused.
Caller timeout, a successful SDP response, and controller buffer retirement are
distinct events; none alone permits reuse of the complete client.

## Fixed bounds make unsupported sizes explicit

| Resource | Bound |
| --- | --- |
| Query selection | One 16-bit service UUID, all attributes |
| Assembled attribute bytes | 4,096 |
| Response segments | 16 |
| Service records | Eight |
| Attributes across all records | 64 |
| Data elements traversed | 512 |
| Parser traversal stack | Eight levels, including the outer buffer |
| Advertised RFCOMM alternatives | Four per record |
| Continuation value | Protocol maximum of 16 bytes |

These are implementation storage limits. They do not change the protocol's legal
record sizes or establish support for every service representation. Exceeding a
bound reports `BT_SDP_LIMIT` with retained diagnostic and transport ownership.
Separate ServiceSearch and ServiceAttribute operations, arbitrary 128-bit query
selection and a local SDP server remain unimplemented.

The lower firmware, resource, UART, scheduling and recovery prerequisites remain
required. No board discovery is qualified. Pairing, key storage, RFCOMM, the
hands-free audio-gateway role and SCO/eSCO audio remain required headset work.

## Sources and focused validation

The primary protocol reference is
[Bluetooth Core 5.4, Volume 3, Part B](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/service-discovery-protocol--sdp--specification.html).
Section 3 defines data elements. Sections 4.1–4.4 define byte order, transaction
matching, continuation and errors. Section 4.7 defines the combined query, and
section 5.1 defines universal service attributes and protocol descriptors.

The independent wire reference is
[BlueZ revision `0845b8f6ef2ac004b1c953cf4fe4ca3458cd8e36`](https://github.com/bluez/bluez/tree/0845b8f6ef2ac004b1c953cf4fe4ca3458cd8e36).
The external fixture extracts constants and the packed SDP header from
`lib/sdp.h`, preserving its original license and copyright notices. Runtime code
is original; no BlueZ implementation is linked into SunstoneOS.

The [composed fixture](../checks/bt-sdp.c) exercises actual UART, H4, HCI, ACL and
L2CAP workers. It compares emitted request bytes and covers continuation across
split data elements, alternative protocol stacks, retained results during
controller ownership, confirmed close, late replies, malformed data and limits.
The composed check passed with address and undefined-behavior sanitizers, and
the new SDP object compiled for AArch64. The [runner](../checks/bt-sdp.py)
archived executed inputs and artifacts under
`~/.cache/sunstoneos/evidence/bluetooth-sdp-20261016/model2/`.
The lower runtime and earlier test suites were reused. These results establish
software behavior without physical qualification or performance measurements.
