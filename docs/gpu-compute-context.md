# Offline GPU compute contexts

The compute-context initializer is **implemented and host-tested**. It constructs
the firmware record that connects one compute queue to its register state,
client command buffer and GPU virtual address space. Complete output bytes match
the pinned driver initializers. Publication, firmware attachment, executable
context-switch programs and physical compute execution remain outstanding.

The [interface](../include/pvr_compute_context.h) accepts an A733 configuration
and five caller-owned spans. The [implementation](../drivers/gpu/pvr_compute_context.c)
writes only the compute context and its firmware register-state record. It
performs no allocation, callbacks, device access, queue submission or publication.

## Validate the complete offline bundle before writing

Each span supplies a CPU pointer, exact byte length and firmware address:

| Object | Bytes | Initializer action |
| --- | ---: | --- |
| Compute context | 232 | Zero and fill the common fields and seven switch-register values |
| Firmware register state | 4 | Zero the context-state flags |
| Client command control | 32 | Reference its address; preserve its bytes |
| Client command ring | Power of two, 256 through 32,768 | Reference its address; preserve its bytes |
| Firmware VM record | 32 | Validate offline contents and reference its address |

The context contains a 152-byte common record, 56 bytes of static switch
registers and a 24-byte zero tail. Common fields include the queue addresses,
compute data-master number four, priority, context/owner identifiers and a
30,000 ms firmware hardware-recovery limit. That source constant is separate
from the OS operation deadlines used during publication and submission.

The accepted hardware identity is 36.56.104.183. Priority values zero, one and
two represent low, normal and high. Privileged admission owns priority
permission and unique context identities; zero is a valid source identifier.
No alternate graphics or transfer context is constructed.

Firmware addresses must fit the 16 MiB MIPS heap beginning at `0xc0000000`.
They require eight-byte alignment, except the four-byte register state requires
four-byte alignment. The initializer rejects overlap among every known CPU
span or firmware span, and between CPU spans and the input records. CPU byte
storage may be unaligned; typed input records require ordinary C alignment.

The VM record must contain a nonzero, 4 KiB aligned catalogue physical address
below 2^36, an unallocated catalogue-register marker, and otherwise zero bytes.
It excludes physical pages `0x1fc00000` through `0x1fc03fff`, matching native
workload-memory admission.
The native [GPU VM builder](gpu-vm.md) produces that initial record. This check
establishes its offline structure; admission must still own and publish its
page tables and validate every workload mapping.

All checks precede writes. Refusal preserves the context, register state,
borrowed buffers and input records. Successful construction writes 236 bytes
and examines a fixed set of five descriptors. Callers must keep those inputs
stable and the output storage private throughout the call.

## Context-switch programs remain a trusted producer requirement

The static register values follow the published order: context PDS0/PDS1,
terminate PDS/PDS1, resume PDS0, context PDS0 B and resume PDS0 B. PDS programs
perform the device-side context store, termination and resume operations.
The initializer receives their prepared register images, just as the published
kernel receives a static context stream from userspace.

These images are not direct GPU addresses. Their encoded program/data references
and feature-specific register semantics require a trusted producer. The
published stream decoder copies these seven values without establishing program
validity or resource ownership. This initializer preserves that separation.

The four-byte firmware register-state object is only a flags record. It does
not replace the GPU buffers and programs needed to save and restore compute
execution. Generating those programs, sizing their data and state storage,
validating register fields for the A733, and admitting every referenced GPU
address remain required work. Zero-filled placeholder programs are not supplied.

## Publication and attachment require separate ownership protocols

Before construction, the platform must reserve all backing, enforce reserved
firmware heap/configuration ranges, establish CPU/physical/firmware mappings,
and exclude conflicting physical aliases. The known logical ranges checked
here cannot establish those physical ownership properties.

Format the referenced client control record through the existing
[client command transport](gpu-cccb.md), using the compute-context address as
its firmware context. Keep the context unattached until its control, state,
VM tables and PDS resources are prepared and published under their respective
cache-ownership rules. This component supplies no attachment operation.

Retain all referenced backing through job completion and firmware context
cleanup. A KICK return acknowledges notification acceptance; it does not prove
GPU completion or permit buffer reuse. Never rebuild a device-visible context
or overwrite register state that firmware owns.

## Focused evidence compares complete source bytes

The [host check](../checks/pvr-compute-context.py) verifies cached source hashes
before extracting the unchanged queue initializer, register-state initializer,
compute-context initializer and static stream decoding functions. The fixture
compares every context/state byte for all three priorities, including zero and
maximum context identifiers. Source ABI structures verify sizes and offsets.

One sanitizer fixture connects the real native VM builder, compute initializer,
kernel queue formatting and client control formatting. It verifies the resulting
references and initial control cursors without attaching a context or sending
a notification. Forty-two refusal cases cover profile, priority, size,
alignment, range, overlap and VM-record errors; all output guards remain intact.

The first sanitizer/reference run passed. A separate focused refusal check
passed after aligning the root-address exclusion with native memory admission;
the full byte/reference and integration evidence remains reusable. Clang 21 also compiled the component
for freestanding AArch64 with strict alignment. Existing VM, client-transport,
GPU service, emulator and physical checks were not repeated.

Evidence is under `~/.cache/sunstoneos/evidence/gpu-compute-context-20261016/`.
`results.json` records source hashes, reference identity and the host result;
`target.json` records the target command and object identity. The later root
refusal result and final AArch64 object identity are under
`~/.cache/sunstoneos/evidence/gpu-compute-context-root-20261016/`.
The references are
[pvr_queue.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_queue.c),
[pvr_context.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/gpu/imagination/pvr_context.c),
and their pinned stream/ABI dependencies. No physical request or performance
measurement was made.
