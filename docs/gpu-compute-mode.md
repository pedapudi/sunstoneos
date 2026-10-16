# Native core-count admission for A733 computation

The A733 GPU is documented as **BXM-4-64 MC1**, a single-core GPU. The native
backend now observes its core count and capabilities after power admission.
The service requires that observation before firmware startup and again before
COMPUTE. Native instruction-model and FreeRTOS composition checks pass; physical
observation and GPU execution remain unqualified.

The primary A733 User Manual V1.00 names MC1 in its overview and block diagram.
Its address table assigns GPU0 the range `0x01800000..0x0188ffff`.
The pinned manual has SHA-256
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
The source-lock entry `a733-user-manual-1.00` identifies the download and revision.
MC1 describes the GPU core count; it does not describe the board's eight CPU cores.

## Hardware observations bind to retained power ownership

The native backend retains its existing BVNC hardware-identity check.
BVNC identifies the GPU revision; A733 requires `36.56.104.183`.
The pinned board [core-discovery implementation](https://raw.githubusercontent.com/orangepi-xunlong/linux-orangepi/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/modules/gpu/img-bxm/linux/rogue_km/services/server/devices/rogue/rgxmulticore.c)
provides the next observations:

| Register offset | Access | Meaning |
| --- | --- | --- |
| `0xf308` | One 32-bit read | Physical GPU core count |
| `0xf300` | One 64-bit read | First core's capabilities and identifier |

A count of one permits the capability read. Compute capability bit 4 and primary
capability bit 3 must both be set. The capability identifier occupies bits 0–2.
Reserved capability bits are retained for diagnosis and ignored during admission,
as in the source driver. Zero or a count above 15 is malformed. A count from two
to fifteen reports `PVR_COMPUTE_MODE_MULTICORE_REQUIRED` and performs no capability
read or secondary-bank access.

Each power poll performs at most one identity, count, or capability transaction.
Every read follows power ON and retained resource admission. Clock checks before
and after the transaction cover the OPEN deadline. The CPU must run at EL1 with
MMU/cache enabled and pinned identity Device-nGnRnE mappings for all allowlisted
pages, including `0x0180f000`. Initialization verifies those mappings without
accessing the GPU. Native reads recheck the target mapping and use ordered MMIO.

The observation records identity, raw count/capabilities, power token, and service
epoch. Its admission callback checks the current backend lifecycle, platform ON
state, delegated resource ownership, token, epoch, and healthy service state.
The callback performs no additional GPU reads. Changed ownership, firmware reset,
failed power admission, or failed observation prevents its reuse. Failure retains
power and memory ownership for controlled recovery.

The compute configuration requires `compute_mode(context)`, returning exactly one
on success. Service initialization refuses a missing callback. The worker checks
it after enable and before firmware startup, then again before COMPUTE formats
its client ring. A failed or late callback faults the session and preserves its
backing. The native backend installs this callback automatically. A separate
platform implementation must enforce the same observation and ownership contract.

## MC1 uses the existing source-checked context and command bytes

Offline construction selects the single-core profile before powering hardware.
The post-power observation admits execution of that prepared profile. Context
programs retain their existing full GPU state addresses: the size field starts at
the state allocation, and register data begins 48 bytes later. No per-core address
offset is needed for MC1. The 512-byte resume allocation retains the architectural
maximum-core layout already emitted by the producer.

The fixed numerical job retains its source-checked single-core flag, zero extra
execute count, and existing compute control stream. Shader instructions, context
register images, buffers, and command bytes do not change. Completion still
requires the firmware timeline, numerical output, and guards; KICK acceptance is
only submission acknowledgement.

The architecture supports multicore variants. The pinned feature table gives a
register-bank stride of `1 << 19` bytes and a maximum of three slave cores.
These architectural limits do not prove those cores exist on A733 or authorize
additional MMIO mappings. Mesa advertises the architecture feature but hardcodes
one runtime core. Its shared-register USC program explicitly lacks per-core
address adjustment; its PDS setup and compute execute-count paths also contain
unfinished multicore handling.

An unexpected physical count above one therefore requires a new supported profile:
qualified register apertures and capabilities, per-core shared-state addresses,
source-supported store/load programs, and compute distribution/execute-count
semantics. The driver reports this requirement explicitly. MC1 admission does
not silently reduce support for hardware that a future observation establishes.

## Focused evidence and integration requirements

The native instruction-model check validates pinned source offsets and widths,
MC1 capabilities, malformed and multicore counts, token/epoch replacement,
Device-page refusal, and deadlines expiring during either new read. The FreeRTOS
check covers failed admission after power, renewed refusal before client-ring
writes, and successful fresh admission through submission. Existing program-byte,
context-byte, completion, reset, and power-controller evidence is reused.

Exact executed sources, compiler inputs, ELF files, source manifests, and results
are retained in `~/.cache/sunstoneos/evidence/gpu-compute-mode-20261016/`.
The native emulator runner is `checks/pvr-compute-mode.py`; its required source
cache is `~/.cache/sunstoneos/sources/gpu-mode/`. The service runner selects the
new behavior with `checks/pvr-compute-service.py --cases 4 5 6` and the existing
required build arguments. Mechanical updates keep the older backend fixtures
compatible with the two additional observations; their old suite was not repeated.

Link `drivers/gpu/pvr_compute_mode.c` wherever the native GPU backend is linked.
The appended service callback and backend observation storage require recompiling
callers against the current headers. The compute service remains optional.
Physical qualification still requires independent recovery, a complete reserved
memory/platform image, correct rail/clock ownership, and an independent proven
block-and-drain implementation. Power OFF does not establish memory isolation.
Only the board-test coordinator may perform that qualification.
