# Local Bluetooth service discovery for the Audio Gateway

The native SDP server publishes the configured Hands-Free Audio Gateway (AG)
service, and optionally an Advanced Audio Distribution Profile (A2DP) sink, to
an established Bluetooth peer. SDP is the Service Discovery Protocol;
it describes services and their connection parameters. The server handles Service
Search, Service Attribute, and combined Service Search Attribute requests over
actual L2CAP PSM 1 channels. This document assumes familiarity with the
[native L2CAP worker](bluetooth-l2cap.md) and Bluetooth link ownership.

The implementation is [bt_sdp_server.c](../drivers/bluetooth/bt_sdp_server.c),
with its interface in [bt_sdp_server.h](../include/bt_sdp_server.h).
It supplies service discovery only. An advertisement does not establish pairing,
profile compliance, hands-free control, or a voice connection.

## Records describe an explicitly admitted profile

The database contains the SDP server record at handle zero, one immutable AG
record at a caller-selected handle of at least `0x10000`, and an optional A2DP
sink record at a second, distinct handle. The server record
contains its service class and SDP version 1.0. The AG record contains:

- Service classes Hands-Free Audio Gateway (`0x111f`) and Generic Audio (`0x1203`).
- L2CAP and RFCOMM protocol descriptors, including the selected server channel.
- Public Browse Group membership and Hands-Free profile UUID `0x111e`.
- The caller-approved HFP version, Network value and supported-feature bitmap.

The current record encoder supports HFP 1.9 syntax. The caller must establish
that the actual AG service implements the advertised profile and owns the
RFCOMM channel. The version is an explicit configuration input; the implementation
does not supply a default compliance claim.

Network indicates incoming-call rejection capability and equals BRSF feature
bit 5. SDP feature bits 0–4 equal the corresponding BRSF bits, and SDP bit 5
(wide band speech) equals BRSF bit 9 (codec negotiation). Admission rejects
higher SDP feature bits and BRSF bits beyond 9. Super-wide speech and enhanced
voice-recognition advertisements remain unsupported. The
[AG engine](bluetooth-hfp.md) states which BRSF value each of its modes uses;
with call control, Network must be 1.

The sink record exists only when the caller sets `sink_handle`. It contains:

- Service class Audio Sink (`0x110b`).
- An L2CAP descriptor with PSM `0x0019` and an AVDTP descriptor with version 1.3.
- Public Browse Group membership and the A2DP profile UUID `0x110d`, version 1.3.
- SupportedFeatures with one or more of headphone (1), speaker (2), recorder (4)
  and amplifier (8).

The caller must run an [AVDTP sink](bluetooth-a2dp.md) that accepts PSM
`0x0019` on every link the policy admits. The record makes no claim about
audio rendering.

## Admission reserves only incoming SDP channel generations

Initialize the zero-filled server before L2CAP. Its policy callback can be chained
beneath the RFCOMM registry's PSM callback. Because that registry shares a callback
context with service authorization, an application can use an aggregate context
and a small routing wrapper. No RFCOMM implementation change is required.

Other PSMs and outbound SDP delegate unchanged to the actual policy. Incoming
PSM 1 requires exact policy success, a connected ACL generation without a flush,
free channel capacity, and no existing server reservation for that link. Admission
reserves L2CAP's next channel generation. The worker processes only that generation,
link and PSM; it never adopts a channel by matching a reusable wire CID alone.

One serialized task owns the server and lower workers. Each server step advances
the actual UART, H4, HCI, ACL and L2CAP workers, maintains bounded session state,
and performs at most one new request or response action. Other L2CAP channels and
unrelated HCI records remain owned by their callers. All objects, callbacks,
contexts and lower buffers must stay alive while any ownership is retained.

## Requests and continuations have fixed bounds

Request headers and values use big-endian SDP encoding. Search patterns accept
up to twelve UUIDs in their 16-bit, 32-bit or Bluetooth-base 128-bit forms.
Every UUID must occur in a matching record. Attribute selectors accept up to
64 ordered, nonoverlapping IDs or ranges. Responses contain only selected
attributes, in ascending attribute-ID order.

Malformed requests with a complete header receive the defined error response.
A header too short to establish its transaction identity faults that server
session and retains the raw request. Unknown record handles, invalid syntax,
inconsistent PDU sizes and invalid continuations have distinct error codes.
A failed session does not discard records belonging to another session.

Attribute replies obey the negotiated remote MTU and the requested maximum byte
count. Encoded attribute data may split across any byte boundary. Sequence lengths
refer to the complete result. The three fixed records fit within 256 assembled bytes;
no heap or dynamically registered record exists.

A continuation contains an opaque query identity and byte offset. Its twelve
bytes expose no pointers or secrets. The server retains the original request
parameters and compares every byte, excluding transaction ID and continuation.
A continuation is valid only for that query on the admitted channel generation,
at the expected offset, before the original query deadline. Each fresh query
receives a new identity. Identity exhaustion refuses further queries.

The server retains one pending response per session and waits for controller
retirement before accepting its next request. Up to four sessions share the
existing finite L2CAP pools. Further input remains under lower-layer backpressure;
a remote peer cannot force unbounded allocations.

## Deadlines preserve lower ownership

The caller supplies response and complete-query intervals in microseconds.
Each request has a bounded response interval, including controller retirement.
Continuations share the original query bound. Accepted lower transmissions use
the existing independent ERTX transport interval so partial UART framing can
finish after a caller deadline. Timeout never grants permission to reuse an
unresolved buffer or channel.

Close has a separate absolute deadline. It stops new responses and waits for
accepted response transmission to retire before issuing L2CAP disconnect.
Only confirmed closure and retired lower leases permit channel release. Owned
receive records can then be discarded explicitly. An uncertain lower fault keeps
all unresolved resources pinned for external recovery.

## Validation uses the actual lower software stack

The focused fixture drives incoming PSM 1 admission through the real RFCOMM
registry and SDP callback, then sends requests through the actual UART/H4/HCI/ACL/
L2CAP workers. Register values, peer packets and controller completions are model
inputs. The model supplies an explicit policy for its synthetic peer; it does
not create or qualify a pairing implementation.

The composed check passes with address and undefined-behavior sanitizers. It
compares the records with independent encoded bytes, including the sink record
and its absence when unconfigured, and exercises UUID forms,
attribute selection, MTU segmentation, malformed requests, continuation replay,
query expiry and channel replacement. Controller-credit backpressure retains
pending work; close waits for actual credit retirement. A short request remains
retained until confirmed channel closure. The new server also compiles for
freestanding AArch64.

Executed inputs, source references, model binary and native object are retained
under `~/.cache/sunstoneos/evidence/bluetooth-sdp-server-20261016/model2/`.
Prior SDP client, RFCOMM, HCI and controller workloads are reused; no physical
radio operation or performance measurement is included.

The primary references are
[Bluetooth Core 5.4, Volume 3, Part B](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-54/out/en/host/service-discovery-protocol--sdp--specification.html),
sections 3–5, and
[Hands-Free Profile 1.9](https://www.bluetooth.org/DocMan/handlers/DownloadDoc.ashx?doc_id=574215),
section 6.3 and Tables 6.5–6.6. The latter PDF has SHA-256
`56f5fcce59cc4342c38d13ed44cc87b28ce73d1f2214dc909d121c378575fa41`.
The fixture extracts wire constants from the already pinned
[BlueZ SDP header](https://github.com/bluez/bluez/blob/0845b8f6ef2ac004b1c953cf4fe4ca3458cd8e36/lib/sdp.h),
retaining its license notice outside the repository. Runtime code is original;
no BlueZ implementation is linked.

Incoming ACL establishment, actual security policy, AG control, voice transport,
audio codecs, resource bindings and physical qualification remain separate
requirements for headset microphone capture.
