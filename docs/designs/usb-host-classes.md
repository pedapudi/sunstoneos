# USB host classes share one OS owner

Status: implemented and host-tested; qualified on the board only for the
source-disabled Type-C control cases in [the USB status](../status/usb.md).
Physical devices, DMA visibility, sustained traffic and recovery are open.

Applications use input events, camera frames and serial streams through the
application kit. The USB owner task performs enumeration, class requests and
transfer completion. Application tasks receive completed data and release it
explicitly; they never access controller registers.

## TYPEC24 carries the host connection

The pinned vendor board device tree selects the USB2 controller's default host
role, its independent USB2 PHY and COMB0 for SuperSpeed. The board schematic
connects these signals to TYPEC24, the Type-C/DisplayPort connector. The FUSB302
on S_TWI1 detects attachment. PL8 enables the SY6280 switch from the board's
5 V supply to connector VBUS. TYPEC16 supplies board power and carries USB0,
including FEL. USB1 data pads have no documented connector route.

The host classes use the Type-C, clock, rail and power-domain providers and
change no supply voltage or clock operating point. USB2 uses the vendor
reference clocks: 24 MHz reference, microframe and suspend clocks, and the
480 MHz PERI0 branch divided by four for replacement PIPE. These register
settings need physical verification; a device-tree selection does not
establish actual connector power or fitted-board wiring.

The source pins and routing evidence are in [USB host support](../usb2-host.md),
[Type-C power](../typec.md) and [cold USB preparation](../usb-platform.md).

## Class code uses the existing host stack

TinyUSB supplies host enumeration, hubs, HID and CDC serial control protocols.
The pinned CDC driver includes ACM, CP210x, CH34x, FTDI and PL2303 support.
Its FIFO receive path is adapted to expose completed DMA buffers and to remove
FTDI status headers from every USB packet. TinyUSB has no video host
class in the pinned source. UVC negotiation and payload handling therefore use
the USB Video Class layouts and the Linux UVC driver as references.

Storage and audio profiles keep their narrower descriptor admission. Images
that select the broader class service admit complete configuration descriptors,
class controls and endpoints together; a parser alone does not make a device
reachable through the native controller.

## DMA ownership lasts through application release

The native controller allocates buffers from its retained, non-cacheable DMA
reservation. A transfer submission owns its destination until completion.
Completed camera payloads and serial packets remain in that memory while an
application holds a lease. Camera frames may consist of multiple payload spans;
UVC headers and bridge status bytes are excluded by adjusting span boundaries.
Neither operation requires a payload copy.

An attachment generation and a release ticket identify each completed lease.
Disconnect retires unpublished data and input state. It preserves memory held
by an application until release. A completion, timeout or detach notification
alone cannot release memory that the controller may still write; slot and
endpoint teardown establish that condition independently.

## Budgets for the requested traffic

At 1 Mbaud with eight data bits, one start bit and one stop bit, a serial
stream carries 100,000 payload bytes per second. Four 4 KiB receive buffers
provide about 164 ms of storage before accounting for bridge packet headers.
Qualification measures maximum application hold time, queue occupancy and
device overrun status; passing baud-control tests cannot establish loss-free
physical streaming.

A 640 by 480 YUYV frame contains 614,400 bytes. At 30 frames per second it
requires 18.432 MB/s of payload. A high-speed endpoint with three 1,024-byte
transactions per microframe has a nominal service capacity of 24.576 MB/s.
Endpoint and memory admission cover the negotiated mode, including UVC
headers. The retained packet pool holds complete queued frames while new
transfers run; a larger span count alone supplies no memory.

Transfer completion wakes the owner through the existing USB2 CPU interrupt
route. The owner drains the event ring and rearms the source with the existing
empty-ring race check. Host checks exercise the interrupt-selected build. Physical qualification
measures sustained completion latency and data continuity with the actual hub,
cable and peripheral.

Eight queued isochronous transfers at one transfer per 125 microsecond
microframe require replenishment within 1 ms, including owner execution.
A longer queue increases that scheduling margin. Serial backpressure uses the
bridge's supported flow control or an application hold-time bound; finite
receive memory cannot preserve an indefinitely stalled UART source.

## Validation

Acceptance uses public device descriptors and recorded traffic for boot
keyboards, mice, a C270-class camera, a CP2102 lidar adapter, CH340 and FT232R.
Raw captures and downloaded sources remain in the cache. Component source
records identify download URLs, revisions, hashes, licenses and execution roles.
Tests cover malformed descriptors, packet boundaries, short transfers,
backpressure, detach, stale completion and explicit release.

The native class images link the transport, class and service branches with
manifest grants enabled. Each image record names its hash, Git revision,
accessory, connector, expected result and remaining launch prerequisites.
Missing electrical admission, peripherals or recovery authority blocks a board
run and is listed in the handoff; host checks do not supply that authority.

## Open work

The class images (input, serial, camera, camera JPEG) have not run with
physical devices; the list is in [the USB status](../status/usb.md#open-work).
