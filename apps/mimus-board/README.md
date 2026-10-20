# Recorded recognition with keyboard control and concurrent USB storage

This fixed application runs the existing recorded Mimus recognizer at EL0 while
the board's privileged USB2 owner reads sector zero from one USB mass-storage
device. It uses the supervised board image and the existing PMIC,
clock, pin, rail, Type-C and USB providers. Storage operations are read-only.
Both microSD cards remain outside this application.

Workload zero starts after storage attachment. Workload four accepts one boot
keyboard and one high-speed storage device behind the existing self-powered
high-speed hub. Press and release a key, then press unmodified Enter. After
input authentication, the observed release followed by Enter starts the same
recorded recognition. A key press before the
neutral report does not start it. Keyboard recognition repeats within the retained session after explicit result
acknowledgment. Workload zero remains one-shot. The recording supplies speech while no microphone is attached.

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

Only the USB2 task accesses mutable controller state. In keyboard mode it also
owns every TinyUSB HID call. It requires mounted subclass-one keyboard metadata
and the actual TinyUSB BOOT protocol mode. The pinned stack attempts
SET_PROTOCOL during enumeration but tolerates a stall; a stalled request leaves
REPORT mode and is refused. Raw eight-byte reports alone do not establish boot
semantics.

The owner copies completed reports into a single 32-byte CPU mailbox. The EL0
recognition task consumes those bytes through release/acquire publication; it
never calls HID, MMIO or providers. Reports bind the attachment generation,
address, speed, interface, endpoint and consecutive sequence. Reserved-byte,
rollover, duplicate-key, short-report or identity errors refuse the session.
A neutral report followed immediately by Enter publishes immutable evidence
with the two sequences and exact input bytes. Other valid keys require another
neutral report. The producer holds the mailbox until consumption and stops
requesting input after acceptance. It resumes after the previous recognition result
and its acknowledgment have completed USB delivery. Each rearm clears the neutral
condition while preserving attachment identity and consecutive report sequencing.
A held Enter cannot trigger the next recognition. No keyboard events enter the host desktop.

The USB2 task publishes a bounded atomic liveness observation for the EL0 recognizer and an immutable attachment
identity. The existing recognizer yield checkpoint closes admission and parks
on expired or failed observations. USB0 uses fresh concurrent Type-C metadata,
the pinned attachment identity and acquired storage result state before result
publication and throughout transmission. Publication contention preserves an
unexpired observation. All buffers remain allocated permanently. USB owners keep
polling after completion, and the board's independent power tasks continue after
failure. Recovery requires the existing supervised whole-board power removal.

USB0 accepts a fixed 16-byte little-endian request: operation byte, zero byte,
16-bit count, 32-bit offset and a 64-bit recognition generation. Operation zero
queries status with zero count, offset and request generation. Operation one
copies up to 1,024 transcript bytes. Operation two copies the 512-byte sector
with zero offset. Keyboard workload four also accepts operation three to copy
128 trigger-evidence bytes, and operation four to acknowledge the result with
zero count and offset. Nonzero keyboard operations require the exact current
recognition generation. Workload zero retains zero in the final request word
and its original operations zero through two.

Responses have a 64-byte header followed by the requested copy. The first eight
32-bit words contain magic `0x424d494d`, version, state (pending zero, success
one, refusal two), copied length, full transcript length, requested offset,
storage state and operation. Session epoch and attachment generation occupy
bytes 32 through 47. Workload zero retains version one and sixteen reserved zero
bytes. Keyboard version three places its 64-bit recognition generation at byte
48, the 32-bit generation limit `4294967295` at byte 56 and zero at byte 60.

One result generation owns its transcript, copied sector and trigger evidence
until the USB0 owner observes full transmit completion of all three exports
and the matching ACK response.
Transcript delivery must cover a contiguous prefix ending at its full length;
retransmission is permitted. A stale ACK returns refusal without rearming. An ACK for a ready result before
full export also returns refusal; an ACK during recognition remains pending. A valid ACK takes effect only when its entire response finishes
transmission. That callback synchronously closes successful result admission
before publishing the acknowledgment to the recognition task. Status during
this transition is pending, including a response finishing after rearm.

The recognition task then clears its old transcript, advances the generation
and rearms the keyboard gate. The USB2 owner starts a new sector-zero read only
while recognition of that generation is active. Its copied completion must
match the active generation before recognition returns. Earlier reads cannot
satisfy the next overlap requirement. Missing overlap, expired liveness or
uncertain device state retains failure. All allocations stay reserved. The
existing absolute session deadline bounds reuse; acknowledgment at the maximum
generation retains failure before identity wrap. No per-recognition allocation
accumulates: the unchanged recognizer constructs and destroys its temporary
objects and recreates its arena on every call.

`host.py` freezes and rechecks exact input bytes, assembles fragmented USB replies
and collects transcript plus sector followed by a final live status query. Its
transport adapter accepts an already claimed device and an identity-check
callback from the coordinator. It repeats only completed pending responses within
an absolute deadline and query budget. Transport failures propagate immediately.
Keyboard capture exports the complete result and sends its generation-matched
ACK. A caller passes `recognition_generation` to collect each subsequent result;
the default is one. It polls boundedly through the preceding generation’s pending
status while the task consumes the previous ACK. `capture_bundle()` selects the
frame version and required trigger read from
identity metadata returned by frozen-bundle validation. Keyboard capture requires
trigger generation equal to session generation, consecutive neutral/Enter
sequences, boot keyboard metadata and exact reserved bytes. The final live query
follows all three outputs.

The 128-byte trigger starts with four little-endian words: magic `0x4752544b`,
version one, extent 128 and record count two. Offset 16 holds attachment
generation. Bytes 24–31 hold address, speed, interface, keyboard protocol,
endpoint, subclass, boot mode and zero. The neutral and Enter records begin at
offsets 32 and 80; each holds a 64-bit sequence and eight report bytes followed
by 32 zero bytes. The existing storage result remains a copied sector.

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
`check-keyboard-host.py` covers the actual native trigger frame, strict host
refusals, production preparation and capture entry from a nondeployable frozen
bundle. Its platform/DDR/accessory claims and transport are explicitly modeled.
`check-host.py` covers fragmented capture, pending replies, transport refusal and
fixed input preservation. These checks establish software behavior, not physical
USB transfers, accelerator execution or native scheduling. No performance claim
follows from their timing.

The keyboard integration reuses model parity from
`~/.cache/sunstoneos/evidence/mimus-board-20261019/` and the USB dual-leaf path
from `usb-host-dual-leaf-20261019/`. Affected keyboard/native/host checks pass
under `mimus-keyboard-20261019/`. The six sanitizer cases include real CPU producer/consumer threads plus stale
sequence, generation, address, duplicate-key and modified-Enter refusals.
Instruction fixtures model HID and MSC bus
completion, acquired TinyUSB metadata, recognition state, scheduling and FIFOs.
They execute the linked protocol-mode getters, HID checked copy, mailbox and
keyboard gate, storage checked copy and fragmented USB0 serializer. They do not
execute boot or identity hooks. A supported keyboard, hub, storage device, fresh
electrical/DDR admission and available whole-board recovery remain physical
prerequisites. No attached accessory or physical concurrent operation is assumed.

The earlier one-shot keyboard image SHA-256 is
`bd2007542c2cd2ef3696f73ebdbc865bdc012454f8394564694efd11b76142d1`.
Separate private identity-hook cases admit workloads zero/four and refuse
one/two/six/eight; exception level, cache/session validation and publication are
modeled. Static linked review places the mailbox and evidence in ordinary BSS,
the 4 MiB heap at `0x413584c0..0x417584c0`, and the cached device pool at
`0x41759000..0x41959000`. Only the identity and build-ID objects changed after
the successful application instruction cases; their explicit object comparison
preserves those results. Shared startup, scheduler and physical DMA remain
outside these checks.

The repeated-generation change uses `check-repeated.cc` to run the actual task
lifecycle, authentication, mailbox and reset with a modeled recognizer and
scheduler. It covers two generations, held Enter, stale/duplicate ACK, immutable
results and missing fresh storage overlap. `check-native.py --keyboard` executes
the linked repeated MSC and USB delivery path, including a partial ACK response
and immediate status during rearm. Its recognition and scheduler boundaries
remain modeled. Existing model parity is reused; these checks do not establish
physical concurrency, arithmetic changes or timing.
