# Native Wi-Fi packet transport

The packet worker sends bounded Ethernet frames through the AIC8800D80 SDIO runtime and converts a limited receive format back to Ethernet. It is implemented and checked in a host controller model. Physical radio operation remains unqualified.

Authentication traffic uses EAPOL, the Ethernet protocol that carries 802.1X authentication and WPA key exchanges. A WPA2 association remains `SECURITY_PENDING`; this worker permits EAPOL but cannot authorize ordinary traffic for that association. A supplicant, key installation, encrypted receive handling, and an IP stack remain required for secure networking.

## Ownership and admission

The privileged owner attaches a freshly initialized `aic8800_data` object with `aic8800_transport_start_data`. The worker shares the existing transport, card, controller, clock, and runtime lease. It retains one transmit packet and one decoded receive frame. No allocation or interrupt callback occurs in the worker.

`aic8800_transport_send_data` copies an Ethernet frame of 14–1514 bytes. The source address must match the initialized interface. EAPOL packets must contain a complete four-byte EAPOL header and its declared body. Their destination must be the associated access point or the standard PAE group address. PAE identifies the port access entity used for 802.1X authentication.

Ordinary Ethernet traffic requires an open association and a separate nonzero caller authorization record passed to `aic8800_transport_authorize_open_data`. The record identifies the caller's admission; it does not establish permission by itself. There is no API to open the WPA2 controlled port, which is the authorization boundary between authentication traffic and ordinary network traffic.

Every admitted association receives a monotonically increasing generation. Open-port authority applies to one generation. Disconnect, unresolved roaming, or another generation revokes it. A generation cannot wrap during the Wi-Fi object's lifetime. An active transmit operation faults if its association disappears or changes; its copied packet and completion evidence remain owned.

The owner consumes transmit results with `aic8800_transport_take_data_result`. It borrows a decoded receive view through `aic8800_transport_peek_data` and acknowledges ownership through the generic transport consume operation. The view contains the association generation and is valid until consume. Asynchronous consumers must copy the frame and its generation before consuming it. A view preserved after failure provides evidence, without granting current transmission authority.

Public wrappers reject inputs and outputs that alias the transport, card, controller, attached workers, or borrowed firmware blobs. Direct worker entry points require the same ownership discipline and are reserved for the serialized transport implementation. Objects cannot be reinitialized or released after uncertain failure without external quiescence and disposal.

## Transmit framing and completion

The pinned full-MAC interface supplies a 28-byte transmit descriptor. The native worker serializes fields explicitly, using little-endian scalar values and Ethernet byte order for the EtherType. It copies the Ethernet source and destination into the descriptor and places the Ethernet payload after it. Best-effort traffic uses access category 1 and traffic identifier 0 for a QoS peer, or `0xff` for a peer without QoS.

The D80 packet header contains the descriptor-plus-payload length, type `0x01`, and CRC-8 polynomial `0x07` over its first three bytes. The worker aligns the record to four bytes, adds the source's zero terminator when needed, and zero-pads the transfer to 512-byte blocks. The maximum frame occupies 1536 bus bytes. The transport's CMD53 transfers this packet directly from the worker's request buffer. The worker does not change that buffer until the transport reports the transfer, so no second copy into the transport buffer is needed. After an uncertain transfer, the controller keeps borrowing the buffer, and the worker keeps it unchanged in its fault state.

The source reserves two firmware buffers for other work. A data transfer therefore waits for at least three reported buffers before sending its single packet. Credit polling and every controller request remain bounded by the existing deadlines. Binding and Wi-Fi management requests run before queued data. An outstanding Wi-Fi management confirmation drains before another transmit operation begins.

Ordinary traffic uses host identifier zero. The reference driver requests no firmware confirmation for these packets. The native result reports completion of the SDIO transfer only; acknowledgement and delivery remain unknown.

EAPOL requests a firmware confirmation through host-identifier bit 31 and a 31-bit sequence number. Sequence numbers are never reused during the worker's lifetime. The source confirmation carries a status word and a used index. Dispatch ignores transport-type bit 7, as the published receive path does. The reference indexes its descriptor ring modulo 256, which discards bit 31. Native correlation compares all lower 31 bits and reports the complete received index, allowing firmware to retain or remove the flag. Physical confirmation of the returned index width remains required; an incompatible index is retained as an unrelated record and cannot complete a different sequence.

A zero status retains the outstanding operation, matching the reference callback's refusal to free it. A nonzero matching status completes ownership and records its raw value. The result exposes `tx_done`, retry required, software retry required, and acknowledged as separate bits. Completion alone does not imply acknowledgement. The worker performs no automatic retry. Unknown, stale, and unsupported confirmations remain in the raw receive queue.

## Receive formats and backpressure

The published receive format places an 802.11 frame after 60 bytes of hardware metadata and padding. The descriptor length bounds the frame. The worker accepts station frames arriving from the distribution system (`FromDS`) with the current interface and access-point identity. A unicast destination must match the local MAC address; multicast destinations are permitted.

Supported records must request upload and carry no reported decryption or receive-error status. Like the source receive path, it uses the upload flag without requiring the hardware descriptor's success and completion bits. This conversion rule does not establish physical reception success. It accepts ordinary data and QoS data with a two-byte QoS header, without host reordering or aggregate MSDUs. An MSDU is the payload delivered by the 802.11 MAC. Protected, fragmented, four-address, management, monitor, reordered, and aggregated records remain opaque.

The conversion requires the RFC 1042 LLC/SNAP prefix and a bounded EtherType payload. LLC/SNAP identifies the protocol carried inside an 802.11 data frame. The Ethernet destination comes from address 1, the source from address 3, and the transmitting access point from address 2. Short headers, invalid body lengths, foreign addresses, and unsupported encapsulations remain raw records. They never produce a decoded Ethernet view.

EAPOL receive remains available during authentication. Ordinary receive requires the current open-port authorization. Both converted and opaque records retain the transport's existing backpressure: the next record cannot overwrite them until consume. A fault retains the current record. In particular, an unsolicited disconnect during an outstanding EAPOL operation retains both the disconnect's raw bytes and its parsed station event.

## Reference identity and validation

The primary source is the [published board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/net/wireless/aic8800/aic8800_fdrv), revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Downloads and compiled reference helpers remain outside Git. The exact file hashes are pinned in `checks/aic8800-data.py`.

| Source | Protocol evidence |
| --- | --- |
| `ipc_shared.h` | Transmit descriptor fields and layout |
| `rwnx_tx.c`, `lmac_mac.h` | Ethernet payload extraction, station identity, access category, EAPOL confirmation selection |
| `aicwf_sdio.c` | D80 header CRC, alignment, terminator, block padding, and credit reserve |
| `sdio_host.c`, `hal_desc.h` | Confirmation index ownership and status bits |
| `aicwf_txrxif.c` | Data-record length plus 60-byte receive header |
| `rwnx_rx.h`, `rwnx_rx.c` | Receive metadata, 802.11 address selection, QoS header and LLC removal |
| `Makefile` | Published SDIO, full-MAC, and receive-reordering configuration |

The focused check compiles the source descriptor declarations outside Git and produces independent transmit and receive packets. The native host model compares transmitted bytes with those packets and exercises the real runtime, transport, card, and controller APIs. Initialization and association are explicit model checkpoints; the check does not simulate firmware execution or a real network.

The sanitized model passed packet comparison, the reserved credit threshold, EAPOL status correlation, open-port admission, receive conversion and backpressure, unsupported receive retention, generation revocation, partial writes, timeout, and late bus completion. Separate affected-case checks passed retention of an unsolicited disconnect during uncertain EAPOL transmission and dispatch of a confirmation with transport-type bit 7 set. Existing initialization, binding, firmware upload, and association evidence is reused.

Evidence is stored under `~/.cache/sunstoneos/evidence/radio-data-20261016/`. `model/results.json` contains the source comparison and composed model result. `retained-disconnect/results.json` and `high-bit-cfm/results.json` contain the affected-case results. `aarch64/final-results.json` records freestanding AArch64 compilation of the changed worker, transport, and Wi-Fi sources, with commands and object hashes. `final-inputs.json` maps the committed inputs to these results.

```sh
python3 checks/aic8800-data.py \
  --reference-dir "$RADIO_REFERENCE_DIR" \
  --evidence "$RADIO_DATA_EVIDENCE_DIR"
```

`RADIO_REFERENCE_DIR` must contain the pinned source files. `RADIO_DATA_EVIDENCE_DIR` must be outside the repository. No board-test executable is provided: board clock, pin, power, recovery, and radio lifecycle bindings remain prerequisites. Integration must link `drivers/net/aic8800_data.c` wherever the transport is linked. Full encrypted and reordered receive handling, supplicant and key operations, network-stack integration, and hardware qualification remain required.
