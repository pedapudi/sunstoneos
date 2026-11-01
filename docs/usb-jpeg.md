# USB camera JPEG decoding

The optional OS camera consumer uses the existing A733 JPEG decoder and its
native platform adapter. `apps/os/usb-jpeg-native.c` accepts a retained camera
frame from the privileged USB owner. It gathers scattered MJPEG payloads once,
directly into uncached decoder input. The encoded dimensions must match the
camera frame dimensions. The existing decoder parser, register programming,
IOMMU mappings and guarded planar output buffers perform the decode.

The OS must authenticate the JPEG session extent before calling the consumer.
Its evidence identifies initial isolation and transaction drain, guarded memory,
electrical operation and supervised whole-board power removal. Nonzero evidence
identifiers alone do not establish those facts. Admission supplies the live board
prefix, a distinct owner, the decoder power client and the vendor operating point:
624 MHz at the recorded 800 mV supply. The existing adapter reserves IRQ81 and
both decoder masters through that authority. Camera bytes have a runtime digest;
they do not require a preselected fixture digest.

The consumer accepts one frame per image lifetime. Completion copies active Y,
Cb and Cr rows into private cached output only after the existing guard, codec
status and IOMMU checks. COPIED authorizes that private output; it establishes no
DMA drain. Input, output mappings, power, clocks and allocator leases remain
reserved after success and failure until whole-board power removal. Failure never
retries setup or resets the decoder. The USB frame lease must remain immutable
through the synchronous input gather. Allocation must be serialized with other
users of the uncached pool; the pool has no internal lock.

The standalone JPEG image and the OS consumer share
`apps/jpeg/native-shared.h`. This preserves the existing source-backed platform
sequence and its bounded retained-copy verifier. The OS consumer supports the
same 512 KiB compressed input and padded luma capacity of 307,200 bytes.

Software validation builds the standalone JPEG target after extraction and
compiles the native camera adapter using that target's complete AArch64 flags.
The JPEG executor host model checks the real driver's register composition,
successful completion and retained timeout and fault states. These checks do not
establish physical camera decoding. Physical qualification requires an admitted
session with the evidence described above and an attached MJPEG camera.
