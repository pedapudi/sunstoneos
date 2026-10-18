# Native fixed GPU render application

The native application composes real firmware, reserved GPU memory, the native
power backend, the privileged GPU service and USB observation. It submits one
64 × 64 RGBA8 clear per boot. Physical rendering, interrupt delivery, inherited
PLL/rail ownership, the larger RAM reservations and watchdog recovery remain
unqualified. The executable must not run on the board until those prerequisites
have physical evidence.

One native USB provider owns the shared locks, GPU resource manager, rail ledger
and PMIC provider. The GPU platform borrows DCDC4 through that same provider.
The application uses the existing large memory profile: cached backing occupies
`0x48000000..0x78000000`; non-cacheable backing occupies
`0x78000000..0x79000000`. The boot path verifies the actual translations and
publishes both native pools before construction. Neither numeric reservation
identities nor software admission prove physical RAM ownership.

The application consumes the existing immutable board session at `0x41f00000`
with radio work disabled. Its version-two boot contract uses the existing large
memory handoff and the GPU contract magic `0x4153373347504331`. The session and
contract must match the image, epoch, generation, lease and absolute deadlines.
The application contract appends the board session's 32-byte SHA-256 digest
immediately after the standard version-two fields. Its byte count includes that
extension. EL3 copies those bytes with the base contract before admission,
then copies the complete session into private storage and verifies its digest.
EL1 consumes that sealed copy; the
version-two reserved words retain the existing RAM ownership metadata. Firmware, compute and graphics
owners are fixed at 9, 10 and 11, excluded from the session's other owners.
The GPU resource-manager client is 7. USB2 and radio tasks are absent.

The EL3 boot path configures GPU interrupt 95 as Group1NS, level-high and routed
to the primary CPU at an API-safe priority. It leaves delivery disabled. The
service owns subsequent masking and rearming. Interrupt dispatch forwards only
that interrupt to the actual service and preserves the existing timer and SGI
handlers. Physical GIC routing remains unqualified.

The client waits for the real PMIC provider, opens the service, waits for firmware
readiness and submits the fixed render. The existing render service owns both
command rings, fragment/geometry timelines, cache invalidation and complete
pixel/guard validation. The application collects all 64 copied output slices
before publishing success. CPU code never fills device output with expected
pixels. A queued submission or a firmware acknowledgement cannot publish output.

All GPU memory, firmware objects, command rings and power leases remain held for
the boot, including successful rendering. There is no implemented physical GPU
transaction isolation provider. The native backend accepts its absence for this
fixed lifetime. A CLOSE fails at the first stop mutation, before firmware shutdown, reset,
power-off or reclaim. Missing isolation cannot report successful shutdown or reusable memory.
No retry, resource release or automatic restart follows uncertainty.

## Build and observe

Configure `apps/gpu-render` using the repository AArch64 toolchain and the pinned
FreeRTOS and TinyUSB sources. Supply `SUNSTONE_LLD` and `SUNSTONE_GPU_FIRMWARE`.
The firmware must have SHA-256
`1db1c399c17401d1f79d46c880db81c724d748c784d4639433b076aba2f9c0d2`.
Build target `sunstone-a733-gpu-render`. The image identity and linker map are in
the build directory's `gpu-application` subdirectory. Downloaded sources,
firmware and build artifacts belong outside Git.

`apps/gpu-render/observe.py` attaches only to an already-running image selected
by USB bus/address and epoch. It never launches, resets or configures a device.
It reads copied output chunks and verifies every RGBA byte before creating a
fresh 16 KiB output file. A finite observation timeout bounds the complete copy.
The command requires `--bus`, `--address`, `--epoch` and `--output`.

The USB request is one little-endian 32-bit chunk index, from zero through 63.
The response header contains magic, version, result, chunk, pixel count, width,
height, epoch and checked-pixel count. Success includes 64 copied pixels. Pending
responses contain no pixels. The native USB worker owns transport behavior. The image performs no Type-C
source activation; observation does not qualify electrical operation.

## Evidence limits

The existing [render evidence](gpu-render.md) covers real paired submission with
explicit modeled timelines and poisoned output. Every modeled case rejects the
poisoned output or retains a transport/deadline failure. It supplies no successful
rendering evidence. The application reuses those component results.

A source review accepted the executable composition and failure paths before
the focused batch. The native executable compiled and linked. Symbol inspection
confirmed the real GPU and USB providers, pinned firmware, render workload and
interrupt dispatch, together with the exact large pool extents. Storage workers,
USB2, radio and Type-C code are absent from the linked image.

The focused native instruction-model check passes refusal at the first stop
mutation when isolation is absent. It also passes revocation before a later
kick, power poll or stop mutation. Both cases preserve published memory and
power ownership without another device access. Observer header checks pass
valid-state decoding and refusal of mismatched identity, epoch and validated
extent. These results establish software composition and retention behavior.

Evidence is retained under
`~/.cache/sunstoneos/evidence/gpu-application-20261017/batch1/`. It includes the
final ELF/map, build logs, actual compiler dependency files, native executable
and logs, parser checks and source snapshots. The initial build lacked the
native power adapter's private header selection; the build resumed after that
composition correction. The symbol checker resumed with the existing Python
environment providing ELF support. Successful component cases were not replayed.
Physical watchdog work remains deferred. Physical isolation and resource disposal
remain required for shutdown/reuse. General draws, textures, formats, repeated
submission and compute qualification remain separate hardware requirements.
