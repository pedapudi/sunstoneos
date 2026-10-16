# Camera, display and IOMMU clock preparation

The native media clock controller prepares selected A733 camera, display and
IOMMU clocks under retained resource ownership. It implements clock and reset
control. Physical power, pin and recovery qualification remain outstanding.

The first profile requires inherited stable parents and powered domains. It
prepares CSI and ISP at 324 MHz, the display engine at 600 MHz and the DSI1 host
at 150 MHz. Camera MCLK1 and MCLK2 can be configured for 24 MHz; their output
gates remain disabled until sensor power and pin sequencing authorize them.

## Authority precedes register access

`a733_media_clock_init` binds the common clock registry and an existing stable
OSC/PCK lease from the GPU resource manager. The controller acquires its actual
registry token before calling the external handoff authenticator. Registry and
PCK token checks run before each active poll that can access registers.

The authenticator must bind the owner, handoff, exact tokens, Device mappings,
selected-domain quiescence, supply operating points and independent recovery.
It must exclude every overlapping writer, including remote firmware. A callback
that returns success without those facts is not a production implementation.
Native register observations separately establish the required clock state.

The common registry reserves these scopes:

| Scope | Ownership |
| --- | --- |
| PLL_VIDEO0 and PLL_DE | Stable parent factors and outputs, including REF and OSC ancestors |
| VI | All CSI, ISP, common analog PHY, camera master clocks, reset and power-domain writers |
| DE_SYS | Both display-engine outputs, DMA, internal shared memory and routing, reset and power-domain writers |
| VO0 | All output-0 shared TOP words, LCD, DSI, LVDS, reset and power-domain writers |
| IOMMU_SYS | Both IOMMU bank clocks, resets and mapping controls |
| PLL_VE0, PLL_VE1 and VE_DEC | Reference-parent and exclusive decoder authority for the separate codec controller |
| DISPLL1 | Local display PLL1, with direct OSC ancestry |

VO0 ownership depends on DISPLL1 stability. This controller reserves VO0 and
DISPLL1 jointly and exclusively, so the display coordinator can delegate cold
PLL preparation while keeping the same immutable token. A separate PLL writer
conflicts with any VO0 consumer. General concurrent consumer operation requires
an atomic publication operation that is not implemented here.

Every profile also holds shared FABRIC and DCDC2 authority. Fabric ownership
covers stable selectors and global gates; it does not establish parent rates.
The controller selects the necessary PLL scopes from the admitted fabric words.
The existing PCK lease retains its clock provider separately.

IOMMU mode additionally reserves VI, DE_SYS and VE_DEC against known native
masters. It still requires authenticated exclusion and drain of all ten real
masters, including consumers without registry scopes. Both banks must be
disabled, and inherited table ownership must be resolved before preparation.
VI, DE, video-decoder, video-encoder and USB2 power domains must remain accessible
through the final IOMMU fault scan. A clock token alone cannot supply these facts.

## Native observations and controlled writes

The adapter reads the oscillator selection, reference and selected PLL factors,
modulation controls, output controls and effective gate status. Each selected
output requires its No-Auto bit, software enable and effective enable set.
The manual defines No-Auto as one. A cleared low control bit permits automatic
gating and is refused by this profile.

The controller checks admitted AHB, NSI and MBUS selectors and rates, required
global gates, the PCK gate and both policy/status values of each required domain.
It accepts only the implemented OSC/PERI0 fabric-parent choices. Native reads
must observe the expected values even when the authenticator accepts admission.

All selected module gates must initially be disabled. Already deasserted reset
bits can be adopted under authenticated quiescence. Preparation deasserts
resets, configures disabled modules, enables bus gates and then module gates.
Each read-modify-write and its readback execute under the common register mutex
in one poll. Unrelated fields are preserved from the immediately preceding read.

| Group | CCU offsets and owned fields |
| --- | --- |
| Camera | VI reset `0x1884[16]`; CSI bus/reset `0x1844[0,16]`; CSI `0x1840` mux 5; ISP `0x1860` mux 4; module dividers and gates; MBUS leaf `0x05e4[8:9]` |
| Sensor clocks | MCLK1/2 `0x1804/0x1808`, mux and both dividers zero, gate disabled |
| Display engine | DE_SYS reset `0x0a74[16]`; DE bus/reset `0x0a04[0,16]`; DE `0x0a00` mux 0, divider one, gate enabled |
| DSI1 output path | VO0 reset `0x16e4[16]`; DPSS_TOP0 `0x16c4[0,16]`; LCD1 bus/reset `0x150c[0,16]`; DSI1 bus/reset `0x158c[0,16]`; DSI1 module `0x1588`, mux 2, divider one |
| IOMMU | Both bus/reset words `0x058c/0x05b4[0:2,16]` |

CCU base is `0x02002000`. Reset-only words have no invented bus-gate bit.
Global AHB/MBUS gates, parent PLL controls and PCK policies remain read-only.
The DSI1 local PLL, PHY, shared TOP selection and panel sequence belong to the
display controller. Clock preparation does not establish pixel timing or panel
readiness. LCD1 and COMBOPHY CCU module gates remain disabled for that local-PLL
route.

## Faults retain ownership

Successful preparation retains the registry token, PCK lease and enabled clocks.
`PREPARED` records completed initialization; it is not a fresh readiness scan.
The coordinator must authenticate the same token before delegating local work.
No independently releasable child may invalidate the retained authority.

An uncertain register access, failed readback or deadline violation retains
ownership. A fault while the common register mutex is held retains that mutex
and disables later register access. If the registry lock is busy during fault
retention, subsequent polls retry only that metadata operation. Terminal polls
perform no callbacks or MMIO. The API provides no release or disposal operation.

Clock/reset acknowledgement is not proof that camera writes, display reads or
IOMMU page walks have drained. Independent transaction-drain and recovery
providers remain necessary before reclaiming mappings or shutting down domains.

## Electrical and integration prerequisites

The board schematic contains conflicting supply assignments. Its power-sequence
sheet 2 assigns camera PE/PK/MCSI to BLDO1 and display PD/PJ to BLDO2. Detailed
PMIC sheet 6 connects DCDC1 to camera/LCD supply nets and SWOUT2 to PE/PD.
Detailed pin sheet 9 connects PK/MCSI/PJ to the global 1.8 V net, tied to DCDC8
on sheet 6. Board device-tree supply declarations also differ from those nets.

These conflicts require board-revision and inherited-supply observations before
pin or rail changes. The controller does not choose a voltage. Existing pin
ownership covers PB4–PB8 only; camera PK/PE, display PD/PJ and their control pins
still need a concrete bank-aware provider. Camera sensor I2C buses also need a
native controller binding. The current PMIC bus reader is not that transport.

No inspected source requires a global shared-SRAM remap for this camera/display
profile. Display-engine line buffers are internal shared memory owned by DE_SYS.
Camera parameters, images and IOMMU tables require separate exclusive DRAM
ownership and cache/mapping admission.

## Source and validation record

The register and topology reference is `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c`
at Linux revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` in the board kernel
repository. Its PLL declarations establish REF ancestry for VIDEO0, DE and VE0/1.
The board device tree sets both camera rates to 324 MHz. Camera `vin.c` and the
DRM display clock functions deassert resets before enabling their clocks.

The A733 User Manual V1.00 defines gate polarity and status in sections
4.1.6.249–251, 4.1.6.256 and the corresponding status sections. The pinned PDF
SHA-256 is `cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
The source checker pins the extracted text and kernel files independently.

The focused model uses the real registry, PCK manager and media controller.
It exercises combined and individual profiles, parent conflicts, stale PCK
identity, live-module refusal, missing gates/domains, adjacent-field preservation,
callback reentry, uncertain writes, deadlines and deferred fault retention.
The source checker decodes the modeled clock trace using pinned parent/mux fields.
AArch64 compilation covers the changed registry, controller and native adapter.
Physical camera, display and IOMMU preparation remain unqualified.

The recorded focused batch passed the host model under AddressSanitizer and
UndefinedBehaviorSanitizer, the pinned-source comparison and AArch64 compilation.
Runtime and adapter objects contain 9,164 bytes of text/read-only data and no
static writable data. The largest direct stack frame is 288 bytes in initialization;
callback and nested-call stack requirements are separate.
Reusable raw evidence and exact input snapshots are under
`~/.cache/sunstoneos/evidence/media-clock-20261016/batch1/`.
The source checker uses the cached board kernel, camera source, power-domain
source and manual extraction; its paths and hashes are part of the check.
