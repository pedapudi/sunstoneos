# Native G2D fill and copy

The native FreeRTOS application implements one retained ARGB8888 fill or
source-copy operation per boot. This document assumes familiarity with DMA,
IOMMU translation and the supervised A733 launch contract. Software
implementation does not establish physical pixel correctness or safe resource
reclamation. Board testing is blocked while physical recovery is unavailable.

The caller supplies width, height, byte stride, operation, destination rectangle,
source origin and fill color in the sealed 904-byte session. Width is at most
1280, height at most 720 and stride at most 5120 bytes. The stride must cover
all pixels and be divisible by four. ARGB8888 bytes are B, G, R, A in memory.
Fill requires a zero source origin; copy requires a zero fill color. Bounds
checks use subtraction before computing offsets. Scaling, blending, rotation,
color conversion and additional formats remain required hardware modes outside
this initial application.

The caller uploads stride times height bytes, including row padding. A SHA256
digest authenticates the complete source even for fill. The destination starts
at zero, including padding. G2D writes only the selected destination rectangle.
A CPU copy never produces the operation's output pixels.

## Resources remain owned through every outcome

The existing board composition owns PMIC and USB services. G2D borrows the
shared PCK provider and an enabled DCDC2 rail at 800 mV. It reserves the common
fabric, oscillator, PLL reference and PERI0 parent, plus exclusive DE_SYS and
IOMMU ownership. The global IOMMU exclusion also reserves the other native
media master clock scopes against concurrent consumers.

The supported module clock selects the PERI0 400 MHz output and divides by four
for 100 MHz. Native code validates the normalized 24 MHz PLL reference, parent
integer ratio, output gates, immutable fabric selectors, domain1 status and
DE_SYS bus gates. It deasserts the DE_SYS and G2D resets, configures the disabled
module and enables its bus and module gates under the shared register mutex.
An uncertain register write retains that mutex. The sealed initial evidence
must establish independent drain and exclusion before these accesses.

Both IOMMU banks require global initial exclusion of all ten real masters.
Only master10 translates and only its micro fault registers are accessed.
Skipped masters remain excluded for the complete session. The initial proof
includes resolved inherited mappings, completed transactions, stable clocks,
exclusive RAM and retention until confirmed whole-board power removal.
Disabled registers or empty fault status do not establish this proof.

| Allocation | Size and alignment | Device address and permission |
| --- | --- | --- |
| IOMMU table arena | 32768 bytes, aligned to 16384 | Physical table base; never a payload mapping |
| Source | Page-rounded stride times height | `0x10000000`, read only |
| Destination | Page-rounded stride times height | `0x10400000`, read/write |
| RCQ commands | 4096 bytes | `0x10800000`, read only |
| Uploaded source and immutable output | Two private page-rounded write-back allocations | CPU only |

Each uncached source, destination and command allocation has an unmapped
4096-byte guard before and after its device-visible extent. Bytes after the
logical source and destination lengths retain a sentinel. Every uncached extent must have identical CPU and physical
addresses before pointer-derived mappings are accepted. The table arena has
its own complete allocation. The extended memory provider supplies every
allocation; physical addresses come from its actual leases. Table mappings
cover at most three distinct 4 MiB windows and never map the guards.

## Completion precedes the immutable copy

The register-command queue (RCQ) contains eight headers. It configures video
layer0, disables unused UI layers and scaling, selects blender COPY and writes
the bounded destination rectangle. A zero-dirty final header preserves the
hardware's even-header requirement. GPL source provenance and preserved
notices are recorded in [the source manifest](../apps/g2d/sources.json).

Submission clears stale task and configuration status, verifies their absence,
records the frame counter and enables one RCQ task. Success requires a fresh
task-end transition after submission. The frame counter remains diagnostic;
software does not infer undocumented increment semantics. A non-destructive IOMMU fault
scan follows completion and each bounded output-copy chunk. Guard checks and
fresh completion observations surround every chunk. The final copied digest
and source digest are checked before the terminal success publication.

USB exports the immutable private copy. Its 192-byte header binds the full
sealed session digest, source digest, output digest, dimensions, operation,
completion status, master10 fault-scan mask and guard result. Query is byte0.
ACK is byte1 followed by the generation as LE64 and the output SHA256 digest.
The response joins header and payload into one packet stream. The transmit
callback refills the FIFO before TinyUSB considers an intermediate zero-length
packet. A response whose length is divisible by the negotiated endpoint packet
size ends with one zero-length packet. The host consumes that packet before ACK.
ACK is accepted only after actual completion of the entire output transfer,
including its terminal zero-length packet when required.
A final query returns acknowledged status with the same proof and no payload.
ACK, timeout and task completion release no memory, mappings, clocks or power.
The shared session deadline bounds upload, setup, submission, copying and USB.

Physical qualification must compare fill and copy output, including arbitrary
source alpha, nonzero origins and untouched padding, with the CPU reference.
It must also establish actual MMIO and DMA
reachability, parent stability and retained failure handling. INTID83 is
identified by the pinned device tree; this first path polls its enabled RCQ
status while retaining interrupt routing exclusion. Interrupt delivery remains
unqualified.
