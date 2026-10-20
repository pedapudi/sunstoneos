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
integer ratio, output gates, immutable fabric selectors and DE_SYS bus gates.
Before local clock or IOMMU access, a fixed power provider accepts domain1 only
when its command and status agree at OFF0 or ON8. An inherited ON state requires
no domain write. For OFF, it writes the five published DE_SYS delay values and
requests ON8 while preserving unrelated command bits. Status must reach ON8
within 10 ms and the original session deadline. Neither bound is extended by
polling or lock contention. The sequence follows the pinned Allwinner PCK600
driver; it does not reset the PCK controller or request poweroff.

The provider authenticates the actual G2D task, retained clock/PCK tokens and
readonly DCDC2 lease before taking the common register mutex. The complete
delay/request transaction holds that mutex; status polls release it after clean
observations. Failure while held retains the mutex, and every failure retains
all provider leases. READY is required before local G2D or IOMMU access. Domain
ON readback establishes no inherited traffic drain or electrical qualification.
The sealed initial authority must exclude and drain every DE_SYS consumer,
including DE, deinterlace, G2D and EINK, throughout the retained session.

After the provider reaches READY, native code deasserts the DE_SYS and G2D
resets, configures the disabled
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

The image with DE_SYS initialization has SHA256
`819165376f652c8353dc53aebee9d9c842d44957ea806c2e9bb0cbaac428e0c5`.
Evidence under `g2d-power-20261020/` records five focused cases on that image.
The linked task passes coherent OFF-to-ON initialization followed by RCQ source
copy, and adopts coherent ON without domain writes. Initial incoherence, status
timeout and uncertainty after the first delay write produce retained terminal
failures without submitting G2D work. The timeout keeps its original request
bound, and clean pending polls release the shared register mutex.

These cases use fresh function-call boundaries at EL1t. PMIC read-service
results, task identity, counter, Device translations, mutexes and peripheral
effects are modeled. They establish neither actual scheduling nor SMP behavior.
Every stopped case preserves CPU context, RAM, MMIO and model state. Root's
static comparison confirms unchanged boot object sections and symbolic
relocations; accepted startup checks retain their original scope.

Broader unchanged evidence is preserved under `g2d-native-20261020/` for image
SHA256 `4e043dad7e691ad50c4164dd9085ae118a0e0911d7fd8aea93e16610e7966328`.
Its frozen closure contains 169 linked objects and 448 compiled dependencies.
Focused source review covers RCQ layout, DMA ownership, clock fields, retained
IOMMU admission and TinyUSB packet completion.

The actual linked allocator, pool getters, upload constructor, G2D constructor
and IOMMU validator pass a model-only 64 by 64 source-copy fixture. The check
confirms exact source bytes, a zero destination, three bounded permission spans
and intact guards. Separate cases refuse differing CPU/physical addresses and
corrupted guards while retaining allocations. Prepared-pool authority and time
are modeled; no peripheral MMIO is mapped. Host execution also checks actual
RCQ fields, fragmented upload authentication and master10-only IOMMU programming
and fault scans. These checks establish software behavior under their stated
models. Physical G2D, USB and interrupt operation remain unqualified.

Twelve saved task cases execute that image's linked AArch64 task, clock and rail
registries, GPU resource manager, native clock accesses, IOMMU and copied-output
logic. They cover fresh completion, stale status refusal, global and master10
faults, configuration-only timeout, failure after copying, unchanged terminal
polling and two 32768-byte copy chunks. Separate fill and copy cases interpret
the actual emitted RCQ and mixer words with an ARGB8888 hardware-effect model,
then compare copied output with the sealed caller reference. Every trace excludes
other masters' micro registers. PMIC service results, scheduler mutexes,
translation observations and hardware effects remain explicit model boundaries.

The transport check reuses the terminal copied checkpoint. Actual configured
TinyUSB vendor, FIFO and endpoint code passes full-speed and high-speed streams
and an exact high-speed packet-length case with modeled DCD completions.
The raw constructor table, task checkpoints and case records are preserved
beside the frozen image. These models do not establish physical pixels,
interrupt delivery, USB operation or recovery.
