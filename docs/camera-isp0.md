# Recorded Bayer processing through ISP0

The native ISP0 application accepts one recorded 640×480 BGGR12 frame and
configures the A733 ISP600 processing path to produce one NV16 frame through
VIPP0 and DMA0. It supplies input from the TOP DDR pattern reader, so this path
does not require a sensor. Physical pixel output remains unqualified. The
existing [camera route](camera-route.md) uses ISP1 pass-through and has a separate
sensor-dependent qualification boundary.

## Source and fixed processing profile

The implementation follows Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` with the effective
`ARCH_SUN60IW2`/`ARCH_SUN60IW2P1` configuration. That configuration selects
`ISP_600` and `isp600_reg_cfg.o`. The pinned implementation, complete A733
parameter defaults, manual references, hashes and execution roles are recorded
in [the source manifest](../drivers/camera/isp0-sources.json). Adapted source
retains its original licensing notices. No opaque runtime firmware or tuning
binary is required for this fixed profile.

The TOP reader at `0x05800800` uses source mode 12, Bayer12 input delivered to
ISPFE. Each input sample occupies a little-endian sixteen-bit container; native
upload validates that its upper four bits are zero. A frame contains 614,400
bytes, with a 1280-byte stride. Another 64 zero bytes are mapped for the
source-backed prefetch requirement and excluded from the input digest.

ISP0 at `0x05900000` consumes the complete source-default parameter image. Its
mandatory Bayer conversion remains configured; optional processing and 3A
modules are disabled. The application writes the effective load RAM format and
independent save-load input format fields, rather than replaying the default
image's example AHB addresses into MMIO. Full load, save and save-load extents
remain mapped even with optional modules disabled. VIPP0 uses unity geometry.
Its parameter image, enable order and parameter-ready readback rule are those
of the [camera route](camera-route.md), defined once in
`include/a733_camera_common.h`. ISP0 still reads, writes and verifies each
register within one poll.
Output is contiguous NV16: a 307,200-byte Y plane followed by 307,200 bytes of
interleaved U and V, both with stride 640.

The shared media clock provider holds VI and IOMMU clocks, with 324 MHz CSI/ISP
module rates. The power path retains the DCDC2 lease and verifies its decoded
800 mV setting. Local COMBO0/parser0 clocks follow source power preparation;
sensor MCLK, the MIPI PHY stream and sensor programming remain disabled.

## Memory and completion ownership

TOP/CSI memory traffic uses IOMMU master 1; ISP parameter traffic independently
uses master 2. The native application authorizes only those two micro-register
sets while preserving initial global exclusion of all masters. It installs the
same camera directory in both IOMMU banks. Every DMA extent is retained through
success, fault, timeout and host acknowledgment.

| Extent | Device address | Mapped bytes | Access |
| --- | --- | ---: | --- |
| Recorded Bayer input and prefetch | `0x100000` | 618496 | Read |
| ISP load | `0x200000` | 32768 | Read |
| ISP save | `0x210000` | 28672 | Read/write |
| ISP save-load | `0x220000` | 16384 | Read/write |
| VIPP load | `0x230000` | 4096 | Read |
| Y output | `0x300000` | 307200 | Read/write |
| UV output | `0x380000` | 307200 | Read/write |

The ordinary 2 MiB uncached pool holds the directory and guarded DMA allocations.
Each mapped data extent has an unmapped page before and after it. The allocator
must return equal physical and CPU addresses for this fixed identity-mapped
profile. The separate 2 MiB cached pool holds upload storage and the private
output copy. Preparation validates actual allocator rows and all table entries;
a reservation alone does not prove this mapping.

Before submission, the driver clears and reads back ISP/VIPP status, then arms
the downstream DMA before starting one TOP input frame. Completion requires
fresh ISP parameter load/save bits, VIPP load completion, DMA capture completion,
and TOP START self-clear with its one-frame cycle field still zero. Two complete
IOMMU scans cover masters 1 and 2, including a scan after the copied output is
hashed. Completion and DMA status are checked around each bounded copy chunk.
Guards are scanned once before the first chunk and once after the last chunk
is copied and hashed. A changed guard byte stays changed, so the second scan
detects a change made at any point during the copy.
Neither status readback nor a deadline establishes transaction drain, so the
application never releases or reuses device backing.

The authenticated session has a single 20-second native lifetime. USB exports
only the immutable copied output. Its callback and acknowledgment gates retain
the full frame until terminal transfer completion; ACK binds generation and the
copied digest and returns a header-only receipt. See [preparation and capture](isp0-preparation.md)
for the exact producer, result fields and supervised host contract.

## Software evidence and remaining physical work

The frozen native ELF is
`32eb5b933c9e6146618e16724a59405e86ed79ccae92888b989ed7ae1f82e2c1`, with build ID
`6fbc3d6611fc41390c50badcdf7b4027cc4269724487316cf4450700644fe26a`.
Evidence is retained below
`~/.cache/sunstoneos/evidence/isp0-native-20261020/`.
The complete parameter image was compared against the effective pinned Linux
setters under ASan/UBSan. Selected media/PMIC and cold USB platform profiles were
checked separately. Actual ELF instructions cover allocation, upload, shared
resource ledgers, ISP/DMA programming, IOMMU fault scans and copied output.
Explicit modeled effects supply device status and synthetic output bytes; they
do not establish ISP numerical correctness or physical completion.

The nominal allocation/copy evidence uses an EL1h bounded call fixture with
`CurrentEL=4`. Corrected downstream checks enter fresh functions in the linked
application's EL1t mode with an explicit SP_EL0. These fixtures do not resume
abandoned task stack frames or validate scheduler execution. Classified failure
cases retain terminal CPU/RAM checkpoints and require their intended injection
plus the matching native failure state. Earlier negative results with aliased
model state are excluded explicitly in the evidence scope record.

Root-owned checks cover seven inherited interrupt guards and three SRAM launcher
cases on the same image. Host checks cover complete table admission, malformed
input/geometry/proof refusals, durable capture, final ACK receipt and the actual
supervisor readiness boundary. Shared provider initialization and USB electrical
operation remain outside the native instruction fixture where callbacks are
modeled.

Physical qualification needs reviewed cold USB and power ownership, VI/ISP/TOP
exclusion, both IOMMU masters' retained recovery, and a known recorded Bayer
input. The first bounded board run must compare captured output against an
independent ISP reference or an explicitly reviewed expected image, then retain
all resources until external recovery. Sensor capture, wider formats, scaling,
3A tuning, interrupt completion and performance work remain separate work.
