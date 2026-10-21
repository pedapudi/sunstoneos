# Native radio receive ownership and record dispatch

The native receive dispatcher routes AIC8800 runtime records without allowing
firmware prints, unsupported frames, or unclaimed messages to block later
network traffic. It runs under the privileged worker that owns the transport.
It uses the existing Wi-Fi, security, data, and RSN implementations for protocol
completion. This document assumes familiarity with the radio transport and
FreeRTOS task ownership.

Native AArch64 compilation and focused emulator composition pass. Physical
radio operation, firmware behavior, entropy, and recovery remain unqualified.
The dispatcher does not authorize transmission, install keys, complete an
unsupported command, or establish an association.

## One worker owns receive advancement

`aic8800_dispatch_init` claims a runtime transport through a retained owner
identity and storage extent. Admission requires no pending controller request
and a ready host. The object remains pinned through failure; no detach or
reinitialization operation exists. Existing direct poll and consume calls stop
advancing the transport after that claim.

The dispatcher uses owner-checked transport operations to poll, inspect the
current record, consume a published record, and retire an unclaimed record.
Retirement checks the owner, current pointer, aligned outer extent, and remaining
aggregate size. It observes the shared clock before and after advancing the
cursor. A stale pointer, different owner, or guessed extent cannot retire bytes.

The network adapter always creates a real dispatcher or adopts an explicitly
configured dispatcher already owned by the same transport. The same worker
continues to own command admission and transmit results. The socket service
adds `SUNSTONE_NET_EVENT`, which returns one copied event under the existing
client identity and deadline rules. Event retrieval remains available after
network revocation so a client can receive a retained disconnect indication.

The transport's finite receive lease remains in force. Qualified platform code
must renew that lease at an admitted idle boundary while preserving awake
ownership. Dispatch does not infer continued power, reset, or radio authority.

## Routing preserves existing protocol completion

The published SDIO receiver distinguishes data using type bit `0x10` and ignores
bit 7 when selecting control subtypes. Data records contain a 60-byte hardware
header. Control records contain a four-byte outer header followed by the declared
payload. Both record forms advance with the source-defined four-byte alignment.

The existing runtime parser handles binding first, then Wi-Fi management,
security commands, data confirmations and Ethernet validation, and RSN EAPOL.
The dispatcher preserves that order and each consumer's confirmation predicates.
A command confirmation belongs to its currently waiting request; its message ID
alone cannot complete a future request.

| Record | Owning behavior |
| --- | --- |
| Current command confirmation | Existing worker validates and completes the actual operation |
| EAPOL accepted by the supplicant | Actual RSN receive path owns processing and its bounded deferred queue |
| Validated ordinary Ethernet | Network adapter copies into its fixed IP pool before owned consumption |
| Scan or station event | Dispatcher copies validated metadata and payload into its event FIFO |
| EAPOL without an accepting supplicant | Dispatcher retains a copied EAPOL event; it grants no security authority |
| Firmware print, unsupported data, unclaimed control | Dispatcher retains a bounded diagnostic copy and retires the record |

The event queue holds four records in receive order. Each event has a monotonically
increasing sequence and the association generation observed during copying.
Scan frames and station indications retain their complete validated payloads.
Pointer members in copied metadata are null; payload bytes belong to the event.

A full event queue keeps the next event in the transport and applies backpressure.
Deadlines still advance. If the operation expires, its raw bytes and resource
ownership remain retained. Event retrieval can release queued copies after fault;
a validated terminal event is copied when space becomes available, without
consuming the failed raw record. No event can reopen a revoked network generation.

## Unclaimed records cannot complete an operation

The dispatcher distinguishes an unclaimed message from an ambiguous response.
A complete command header supplies the message ID and declared parameter length.
The dispatcher compares that identity with active binding, Wi-Fi, security,
and asynchronous station or scan ownership before deciding whether retirement
is permitted.

Valid foreign-VIF and duplicate scan or station indications follow the existing
Wi-Fi consumer's ownership rules. A duplicate association indication cannot
create another association. A repeated scan-complete indication cannot complete
the still-pending scan request confirmation. Unsupported roaming remains
unresolved and terminal when it cannot produce a supported event.

A malformed inner message may retire only when its complete outer boundary and
sufficient identity fields prove that no active operation owns it. For example,
a complete header with an unknown message ID and an oversized parameter length
can retire without affecting a pending scan confirmation. A short header with
no trustworthy identity cannot make that determination and remains terminal.

Malformed active confirmations, truncated outer records, and ambiguous identities
retain the transport fault and unresolved input. The dispatcher never searches
for a guessed next header. Per-record validation does not assert that later
records in the aggregate are valid before their own processing.

The diagnostic queue retains copies of the first four unclaimed or unsupported
records. A copy holds at most the first 4,096 bytes of its record and marks a
longer record as truncated; the record itself still retires. Later diagnostic records retire with a
saturating overflow count so diagnostic backpressure cannot stop valid traffic.
This policy applies only after active protocol and event ownership are excluded.
Unsupported records do not count as delivered Ethernet frames or successful
operations. Diagnostics remain private because raw radio payloads may contain
sensitive information; the implementation does not print them.

## Focused composition exercises the changed paths

The native model uses actual FreeRTOS tasks, FreeRTOS+TCP, the network adapter,
the dispatcher, radio workers, and the SDIO/controller register model.
The network case allocates and configures an actual BSD RSN context. Its
completed association, key installation, controlled-port state, and decrypted
CCMP records are explicit reconstructed inputs. The model does not replay the
previous handshake workload or claim physical firmware execution.

The focused checks pass these behaviors:

- UDP exchange and actual RSN EAPOL receipt continue across firmware prints,
  unknown malformed messages, stale binding confirmations, and diagnostic overflow.
- A real source-layout disconnect becomes a copied service event and revokes
  network readiness.
- Foreign scan results, foreign or duplicate scan completion, and stale station
  indications retire before the actual pending scan confirmation completes.
- Five scan results retain ordering across the four-entry event queue; finite
  backpressure expiry preserves the fifth raw result and all copied events.
- Different owners, stale pointers, incorrect extents, and legacy advancing APIs
  cannot consume an adopted transport.
- Malformed active confirmations and incomplete command identities retain a
  terminal fault; subsequent dispatcher polls perform no backend access.

The traffic result uses its preserved executable. The scan fixture's request-ID
assertion was corrected to the published `SCANU_START_REQ` value before the
remaining cases passed; runtime code did not change between those executions.
Earlier firmware, controller, TCP lifecycle, and RSN handshake suites were reused.

Evidence lives under `~/.cache/sunstoneos/evidence/radio-dispatch-20261016/`.
`first-batch.json` records the passing traffic case and initial fixture failure.
`event-batch.json` records the corrected event and fault cases. Both executed
images and their source snapshots are preserved by digest.

The private `cmake/radio-dispatch.cmake` must follow `radio-network.cmake`.
It unconditionally adds the real dispatcher source to the native network target
and provides `radio-dispatch-model`. Its copied target properties include the
upstream RSN sources' required per-file compile options. The runner accepts
explicit source, compiler, emulator, and evidence paths; its emulator has no
host network interface. The current central configure also requires the pinned
TinyUSB source, although the dispatcher executable does not link USB code.

## Published protocol references

All protocol references use linux-orangepi revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
The native dispatcher implements the byte protocol and copies no reference code.
The referenced source remains outside Git with its existing notices.

| Reference | SHA-256 | Relevant behavior |
| --- | --- | --- |
| [SDIO receive](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/aicwf_txrxif.c) | `2c22a400aa25e38fa908d0f43313081811aa8b988641464555ed1643585e8ce4` | Record extents, subtype dispatch, and record advancement |
| [Command manager](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/rwnx_cmds.c) | `41e06e13c9ad59c244710c89abd9fd53d665739744f73323e8ed9297c11bc71a` | Waiting-command confirmation correlation |
| [Message handlers](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv/rwnx_msg_rx.c) | `9eed44d33b2f6241171e47648ba67e5d193d0e54558fa9f878e8821afe8f9f2b` | Asynchronous handlers and firmware-print separation |

Wider frame formats, additional management events, reconnection, network servers,
DHCP/DNS/IPv6, and power-mode transitions remain required work. Physical execution
requires native radio resources, qualified entropy and firmware, RF admission,
authorized network configuration, and independent recovery. No board operation
or performance measurement forms part of this evidence.
