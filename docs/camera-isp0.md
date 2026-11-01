# Recorded Bayer processing through ISP0

The native application processes a recorded 640×480 BGGR12 frame through the
A733 TOP DDR reader, ISP600, VIPP0 and camera DMA0. A host can request repeated
captures of the same immutable input. Each capture produces a private NV16 copy
that remains immutable until USB finishes delivery and accepts its acknowledgment.
No sensor is required. Physical output and numerical parity remain unqualified.

## The fixed profile follows the selected vendor source

The driver follows OrangePi Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` with `ISP_600` and
`CONFIG_ARCH_SUN60IW2`. The [initialization sources](../drivers/camera/isp0-sources.json)
and [replay sources](../drivers/camera/isp0-replay-sources.json) record URLs,
hashes, licenses and execution roles. Linux supplies development references;
FreeRTOS runs the native driver. This profile requires no opaque runtime tuning
blob. Existing adapted code retains its license notices.

TOP mode 12 supplies Bayer samples in little-endian sixteen-bit containers.
Each sample must be at most 4095. The input has a 1280-byte stride and 614400
payload bytes, followed by 64 mapped zero prefetch bytes outside its digest.
ISP0 uses the complete vendor default coefficient image with optional modules
and 3A disabled. VIPP0 uses unity geometry. Output contains a 307200-byte Y plane
followed by 307200 bytes of interleaved U and V, with a 640-byte stride.

The shared provider retains VI/IOMMU clocks and verifies DCDC2 at 800 mV.
The effective Zero 3W DTS overrides the generic SoC rates with 324 MHz for both
CSI and ISP. Sensor MCLK and sensor programming remain disabled. The separate
[sensor camera route](camera-route.md) uses ISP1 pass-through; live sensor
routing into ISP0 remains unfinished.

## Capture completion permits retained reuse

TOP/CSI traffic uses IOMMU master 1; ISP parameters use master 2. Both banks
share the guarded camera directory. CPU and physical addresses must match for
this fixed mapping. The uncached pool holds device backing; a separate cached
pool holds upload storage and the immutable output copy.

| Extent | Device address | Mapped bytes | Access |
| --- | --- | ---: | --- |
| Bayer input and prefetch | `0x100000` | 618496 | Read |
| ISP load | `0x200000` | 32768 | Read |
| ISP save | `0x210000` | 28672 | Read/write |
| ISP save-load | `0x220000` | 16384 | Read/write |
| VIPP load | `0x230000` | 4096 | Read |
| Y output | `0x300000` | 307200 | Read/write |
| UV output | `0x380000` | 307200 | Read/write |

Each capture requires fresh ISP SAVE/LOAD, VIPP load and DMA capture-done flags,
TOP START self-clear and a zero cycle count. IOMMU scans cover both masters
before and after copying. Guard scans bracket copying; completion and DMA
status checks also surround each bounded copy chunk.

After consumer acknowledgment, the owner scans for faults and rechecks retained
completion. It clears observed ISP/VIPP flags and verifies zero, reloads the
same parameter address and READY fields, then calls the existing retained DMA
rearm operation. TOP accepts its idle enabled state and starts one further frame.
Input, parameters, mappings, allocations and provider leases remain retained.
Neither successful replay nor capture-done establishes global bus drain.

## Interrupts wake the owner within the original deadline

The IRQ build routes DMA0 interrupt 149, VIPP0 interrupt 153 and ISP0 channel-0
interrupt 161 to CPU0 at priority `0xd8`. Handlers mask their GIC source and notify
the owner while preserving raw device status. ISP enables SAVE bit 2 and error
interrupts; LOAD bit 3 stays disabled because its earlier event must not mask
the later SAVE wake. Completion still requires both SAVE and LOAD.

Arming retains its source index and phase through GIC write propagation and
active-interrupt deferrals. It masks once, waits for RWP and inactive state,
clears only the owned pending bit, programs source enables and rearms the GIC.
A deferred poll never repeats an earlier source's writes. The task blocks on
notifications only while waiting for DMA or the final producer fence. The
original 20-second session deadline bounds every capture and wait.

## Sequence-bound acknowledgments retire the copied consumer

The first capture preserves the version-1 response and legacy final ACK.
Command 2 consists of one command byte, little-endian generation and capture
sequence as two 64-bit words, then the copied digest. It acknowledges the
current frame and requests the next capture. Command 3 uses the same body and
acknowledges the final replay frame without requesting another.

Subsequent responses use version 2. The 32-bit field at byte 148 contains the
capture sequence for copied results, failures and receipts, and zero for pending results.
The remaining geometry fields keep their sealed values. Sequence overflow,
stale acknowledgment, legacy ACK after replay and replay after final ACK fail.
A query while the producer owns the copy returns only a pending header.

The USB serializer transfers ownership only after its final data or zero-length
packet callback. Atomic publication transfers the copy from producer to USB,
and the accepted acknowledgment transfers it back. Every replay keeps the
same immutable recorded input. Uploading a different input sequence requires
separate admission and is not implemented by this protocol.

The host capture API defaults to one frame. Its optional `frames` argument
accepts one through sixteen captures under the same absolute deadline. It
saves each complete frame before ACK and records partial outputs if a later
capture fails. The final receipt binds the last sequence, digest and proofs.

## Independent CPU output is available for comparison

[The CPU reference checker](../checks/isp0-pixel-reference.py) uses the identified
host FFmpeg build for Bayer interpolation, RGB24 conversion and BT.601 output.
It interleaves planar output into NV16 and records input, output, executable and
shared-library hashes. Its [source record](../drivers/camera/isp0-reference-sources.json)
identifies the host-only dependency and available source.

The caller declares full or limited range and a maximum per-component error
before capture. The default error budget is zero.
The checker reports errors separately for Y, U and V. The available vendor source
does not define ISP600 interpolation, rounding or mandatory color conversion.
A reference file therefore supplies an independent comparison, without proving
that its algorithm is equivalent to this ISP profile.

## Host evidence leaves physical qualification open

Evidence is under `~/.cache/sunstoneos/build/camera-isp-1031/`; its README records
commands, hashes and frozen image locations. ASan/UBSan checks cover retained
rearm and deferred IRQ arming. The complete parameter image matches the pinned
vendor setters. Linked-image checks cover repeated full/high-speed delivery,
sequence refusals, late faults, staggered IRQ completion, timeout and a second
capture after USB acknowledgment. QEMU runs the rearm check in a FreeRTOS task.
CPU reference checks cover neutral endpoints, Bayer phase and mismatch reporting.

Register effects, pixels, USB and IRQ entry are modeled in the linked checks.
They do not establish physical completion, electrical USB operation or numerical
parity. Independent review approved each code commit. The frozen retained ISP
ELF fails the new probe runner's watchdog admission and is host-only.

A board run needs a source-backed finite stop and retirement condition covering
TOP reads, ISP/VIPP parameters, output writes and IOMMU page walks, followed by
integration with the shared probe contract. No board run is queued. Physical
IRQ delivery, pixels and repeated operation remain unqualified; OV5640/CSI
qualification also needs the absent camera module.
