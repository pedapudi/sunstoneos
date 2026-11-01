# USB serial receive leases and bridge control

USB serial is implemented and host-tested for CDC ACM, CP210x, CH340/341,
FT232/2232 and PL2303 through the pinned TinyUSB CDC host class. Physical
serial transfers, sustained throughput, electrical flow control and reconnect
recovery remain unqualified.

TinyUSB owns descriptor matching, initialization and line coding. The
[CDC adaptation](../drivers/usb/host/serial-source.py) verifies the source hash
before generating its private build copy. It replaces the receive FIFO path
with controller-owned buffers and expands the control buffer to sixteen bytes
for the CP210x flow-control structure. The generated file retains the upstream
MIT notice. The [source record](usb-serial-sources.json) gives URLs, revisions,
hashes, licenses and execution locations.

## Receive ownership prevents reuse during application access

The USB task is the only caller of the [serial adapter](../include/sunstone_usb_serial.h).
The transport supplies four noncacheable 4096-byte DMA pages per endpoint.
The serial adapter submits one transfer at a time and retains completed pages
until their application views are released. It can submit another free page
while the application retains a previous page. No payload copy occurs between
controller completion and application access.

A full receive pool stops rearming the endpoint and increments backpressure.
USB bulk flow control then limits host reception. A bridge may still overflow
its UART FIFO if its peer ignores RTS/CTS or lacks that wiring. A host test
cannot establish lossless operation at one megabaud.

FTDI status occupies the first two bytes of every USB packet. The adapter
publishes one payload view per packet and skips status-only packets. It keeps
the original DMA layout intact. Each view has a distinct token, so releasing a
previous packet cannot release a subsequent packet in the same buffer.

Disconnect invalidates ready data and retains both outstanding DMA and held
application views. A proven controller drain calls
`os_usb_serial_native_quiesce(address)` before retiring DMA slots and control
state. A held application view continues to prevent reuse of its attachment
index until release. The transport must drain old completion events before
reusing that index. Timeouts alone do not establish quiescence.

The counters record raw received and released USB bytes, including FTDI status;
dropped counts unread ready bytes discarded on disconnect. Errors count failed
or oversized completions. Stale counts rejected transfer cookies. Backpressure
counts attempts to arm reception with every serial slot occupied.

## Line coding and flow control use bridge protocols

The native owner exposes asynchronous line configuration to the application
service. Bits, parity and stop fields use CDC line-coding values. Baud requests
of 230400, 460800 and 1000000 are checked against captured control requests
from the actual upstream driver functions. FTDI and CH34x divisors may round
the requested rate. PL2303 HX uses its divisor encoding for one megabaud.

The [flow-control extension](../drivers/usb/host/serial-control.inc) programs
FTDI channel-indexed RTS/CTS, CH34x register `0x2727`, CP210x GET/SET_FLOW and
PL2303 variant-specific register masks. CP210x preserves the device's threshold
words; PL2303 preserves bits outside the flow mask. Linux supplies the CP210x
and PL2303 protocol reference. WCH supplies the CH341 flow-register reference.
CDC ACM has no standard request for automatic RTS/CTS; the adapter rejects that
mode and supports ordinary DTR/RTS control.

## Echo preserves the receive buffer through transmit completion

The native application service can submit a retained receive view for transmit.
The transport validates that the outgoing range belongs to a completed,
retained page from the same device. The controller holds that page through the
OUT completion. An application release arriving earlier is deferred until
transmit completion. This serial echo path copies no payload bytes.

Other caller buffers use one bounded copy into an acquired noncacheable OUT
page. The caller still retains its buffer until the asynchronous write result.
The fallback is limited to 4096 bytes and refuses concurrent writes.

## Host checks exercise upstream code and ownership

Run `checks/usb-serial.py --output OUT` in an integrated checkout. During
independent development, `--services CHECKOUT --transport CHECKOUT` supplies
the sibling headers. The checks run with address and undefined-behavior
sanitizers and record their commands, results and input hashes outside source.

The protocol check opens descriptors derived from public operator recordings
of RPLIDAR CP2102, CH340 and FT232R hardware. It captures baud and flow request
bytes from the generated TinyUSB class, including FTDI channel selection,
CP210x thresholds and PL2303 masked updates. These recordings establish real
descriptor layouts; the control traffic is produced by the host fixture.

The lifetime check exercises multiple FTDI packets, duplicate releases, pool
backpressure, disconnect retention and stale completions after attachment
reuse. The native service check verifies pointer identity for borrowed echo,
deferred receive release, transport result conventions and quiescence before
reattachment. These checks do not model electrical signaling, UART timing or
physical DMA visibility.
