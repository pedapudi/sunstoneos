# Recorded recognition with concurrent USB storage

This fixed application runs the existing recorded Mimus recognizer at EL0 while
the board's privileged USB2 owner reads sector zero from one USB mass-storage
device. It uses the supervised board image, workload zero and the existing PMIC,
clock, pin, rail, Type-C and USB providers. Storage operations are read-only.
Both microSD cards remain outside this application.

The native application and host capture are implemented and tested offline.
Physical concurrent operation, scheduling and recovery remain unqualified.
No new hardware launch is admitted while the boards are unavailable.

The recognizer, models, punctuation and formatter are the existing Mimus
implementation. `apps/mimus/source.json` pins its source. The two exact prepared
models use the sizes and hashes in `apps/bluetooth/recognition-models.json`.
`inputs.json` pins a recorded 16 kHz mono, 16-bit WAV from the accepted physical
Mimus parity run. Model bodies and recognition requirements are unchanged.
The fixed WAV contains 57,440 samples.

| Allocation | Address or linked extent |
| --- | --- |
| Board executable, ordinary stacks, transcript and scratch | Image RAM below `0x41c00000` |
| Newlib heap | Linked allocation of 4 MiB below `0x41c00000` |
| Board cached device pool | Linked allocation of 2 MiB below `0x41c00000` |
| Board uncached USB DMA pool | `0x41c00000..0x41e00000` |
| Board session page | `0x41f00000` |
| Results, monitor stacks and translation tables | `0x42000000..0x42200000` |
| Base model | `0x44000000`, 7,501,968 bytes |
| Overlay model | `0x44800000`, 6,560,592 bytes |
| Recorded WAV | `0x45000000`, 114,924 bytes |
| Existing recognizer arena | `0x46000000..0x47000000` |

The existing EL3 identity hook publishes only the three fixed input extents
through the board's coherence helper. Cached startup clears BSS before this
hook. The application carries no mutable identity state across that boundary.
The recognizer verifies SHA-256 of all three inputs after startup, before
publishing recognition-active state. Publication and input authentication serve
separate purposes. Inputs remain reserved throughout the retained session.

The generated storage adapter delays its first read until recognition is active.
It records overlap when the actual storage driver's checked result copy completes
while recognition is still active. Combined success requires this overlap,
successful recognition and a completed copied sector. A slow device that finishes
after recognition does not establish the combined workload.

Only the USB2 task accesses mutable controller state. It publishes a bounded
atomic liveness observation for the EL0 recognizer and an immutable attachment
identity. The existing recognizer yield checkpoint closes admission and parks
on expired or failed observations. USB0 uses fresh concurrent Type-C metadata,
the pinned attachment identity and acquired storage result state before result
publication and throughout transmission. Publication contention preserves an
unexpired observation. All buffers remain allocated permanently. USB owners keep
polling after completion, and the board's independent power tasks continue after
failure. Recovery requires the existing supervised whole-board power removal.

USB0 accepts a fixed 16-byte little-endian request: operation byte, zero byte,
16-bit count, 32-bit offset and eight zero bytes. Operation zero queries status
with zero count and offset. Operation one copies up to 1,024 transcript bytes.
Operation two copies the 512-byte sector with zero offset. Requests may arrive
in fragments. Responses have a 64-byte header followed by the requested copy.
The header contains eight 32-bit words: magic `0x424d494d`, version one, state
(pending zero, success one, failure two), copied length, full transcript length,
requested offset, storage state and operation. The epoch and generation follow
as two 64-bit words; the final 16 bytes are zero.

`host.py` freezes and rechecks exact input bytes, assembles fragmented USB replies
and collects transcript plus sector followed by a final live status query. Its
transport adapter accepts an already claimed device and an identity-check
callback from the coordinator. It repeats only completed pending responses within
an absolute deadline and query budget. Transport failures propagate immediately.
Bytes already queued to USB cannot be recalled; partial captures and a refused
final status do not establish success.

`prepare.py` wraps the existing board launcher preparation. It requires fresh,
image-bound DDR evidence for the complete model slots, WAV reservation and arena,
in addition to ordinary board admission. The generated bundle keeps the board
bundle intact and adds the exact input uploads. It performs no device access.
Historical evidence cannot supply fresh admission for this image.

Enable `SUNSTONE_MIMUS_BOARD_APPLICATION` with `SUNSTONE_BOARD_SUPERVISED`, the
existing pinned board dependencies, SMP newlib and Mimus source. Build target
`sunstone-a733-mimus-board`. The private CMake composition reuses the board's
source and flags, applies exact-match client and USB0 adapters, and links the
existing constructor/heap pattern. It excludes the standalone Mimus entry,
USB and recovery owners.

`check-native.py` executes linked application, storage-copy and serialization
instructions. It models scheduling, enumeration, controller health, read-command
bus completion and USB FIFO calls. It does not execute boot or identity hooks.
`check-recognition.cc` runs the new task's input authentication and unchanged
recorded recognizer on the host; scheduling and overlap are modeled there.
`check-host.py` covers fragmented capture, pending replies, transport refusal and
fixed input preservation. These checks establish software behavior, not physical
USB transfers, accelerator execution or native scheduling. No performance claim
follows from their timing.
