# USB video host class

The CPU runs the USB Video Class (UVC) host protocol on FreeRTOS. The class
supports MJPEG and YUYV frame descriptors, version-dependent probe controls,
and USB 2 isochronous or bulk streaming endpoint selection. It is implemented
and host-tested; physical camera operation remains unqualified.

The owner task parses descriptors with `sunstone_uvc_descriptors`. It selects a
mode and drives `sunstone_uvc_negotiate_begin` and
`sunstone_uvc_negotiate_complete` through asynchronous transport controls.
Each positive result supplies the next setup packet, data buffer and length.
Negotiation sends SET_CUR PROBE, GET_CUR PROBE, SET_CUR COMMIT and SET_INTERFACE.
The probe sizes are 26 bytes before UVC 1.1, 34 bytes for UVC 1.1 and 48 bytes
for UVC 1.5. The accepted format, frame and interval must match the selected
mode. Isochronous selection chooses the smallest alternate setting that holds
the device's negotiated maximum payload.

After initialization, the owner calls `sunstone_uvc_expect_mode` with the
negotiated mode. YUYV publication requires exactly width times height times two
bytes; a shorter frame is discarded.

Each completed UVC payload enters `sunstone_uvc_payload` with a transport lease.
A frame contains references to packet data; the class does not copy image bytes.
The transport must keep those bytes stable until its release callback runs.
Applications acquire complete frames and release an immutable generation and
sequence ticket through `sunstone_uvc_release_ticket`. Disconnect drops queued
frames and preserves acquired frame leases until their explicit release.
Consumers must release acquired frames before reinitializing the class object.
All calls execute on the USB owner task.

Frame identifier changes complete a nonempty frame without a preceding error.
End-of-frame also marks complete frames. Payload errors, malformed headers, lost packets and overflow discard
frames until the next frame identifier. If a failed transfer has no trustworthy
header, the class also discards the first subsequent observed frame before
resynchronizing. Three frame slots and 1024 packet
references per frame bound memory use. A frame exceeding either the negotiated
byte count or reference count is discarded. Bulk input must preserve UVC payload
boundaries: a short USB packet or negotiated maximum payload terminates a payload.
The transport adapter supplies start and end markers through the fragment entry
point.

The optional [hardware JPEG consumer](usb-jpeg.md) gathers MJPEG payloads once
into the existing decoder's contiguous input. Raw frame delivery retains the
original USB buffers. The native camera owner can select MJPEG explicitly
before starting; the JPEG demo uses that selection and refuses YUYV fallback.
The default camera demo prefers VGA YUYV when available.

The host test checks probe sizes, bandwidth selection, actual retained image
bytes, detach ownership, overflow, malformed payloads and queue backpressure.
It includes synthetic payload fault cases and descriptor facts from a public C270
report. The report identifies UVC 1.0, 38 YUYV/MJPEG modes and 11 isochronous
alternate settings. The largest setting carries 3060 bytes per service interval.
The fixture preserves every reported video mode and endpoint alternate setting;
it omits audio, extension controls and unrelated descriptors.

The control replay uses nine real GET_CUR probe responses from the Logitech
MJPEG-switch capture attached to Wireshark issue 8414. It verifies the accepted
frame size and 3060-byte alternate-setting choice for the captured responses.
The attachment does not identify itself here as a C270 capture. It contains no
completed isochronous video payloads, so actual captured image assembly remains
unverified. The image byte and failure tests use synthetic payloads.

Raw reports and captures remain outside source control. The source record
`usb-uvc-sources.json` pins their URLs, revisions, hashes and license boundaries.
`checks/usb-uvc-c270-extract.py` and `checks/usb-uvc-capture-extract.py` reproduce
the numerical fixtures from those pinned files. Pass the descriptor source's
SHA-256 as the descriptor extractor's second argument. Format the resulting
headers with clang-format before comparing them with the tracked fixtures.

The bulk fragment entry point retains each transfer fragment separately. It
parses the UVC header at payload start and applies EOF only at payload end.
The transport identifies payload boundaries from short packets or the negotiated
maximum payload length, exposed as `sunstone_uvc_negotiation.payload`. A
replenished transport pool must preserve every packet referenced by an acquired
frame. Physical operation still requires a supervised camera run.

## Source

The protocol adaptation follows Linux v6.12
`drivers/media/usb/uvc/uvc_video.c`, especially streaming control sizes, probe and
commit, payload header flags and alternate-setting capacity. The source is GPL
2.0 only and remains source-available. Its Linux USB, V4L2, allocation and URB
interfaces cannot execute on FreeRTOS; this class supplies owner-task callbacks
and retained packet views for those interfaces. The adaptation preserves the
source's copyright notice. Linux supplies the development reference; the
adapted class runs on the CPU under FreeRTOS.

The downloaded source has SHA-256
`d5c1f227b0423736a4acfeb9da8ff57ff5f44c9fd8df33af56280cb76a20b2a9`.
Its URL is
<https://raw.githubusercontent.com/torvalds/linux/v6.12/drivers/media/usb/uvc/uvc_video.c>.
The raw source remains outside the tracked tree.
