# Retained HDMI framebuffer image

The optional HDMI framebuffer image connects a fixed 1280×720 RGB565 surface to
DE1, TV0 and the HDMI controller. The original hardware-colorbar image remains
the default. Enable `SUNSTONE_HDMI_FRAMEBUFFER` when configuring `apps/hdmi` to
build the framebuffer image. Its distinct build identity and session suffix
prevent a colorbar admission record from authorizing DMA.

This image is for a supervised RAM session. It requests output stop before the
existing twenty-second software deadline. The complete frame, translation table,
clock, power and shared-register ownership remain reserved until manual power
removal, including after a successful stop. Stopping TV0 or disabling DE does not
prove that all memory transactions have drained. A stalled CPU or interconnect
may prevent software from stopping output; this image does not assert watchdog
or independent recovery qualification.

## Native path and memory

The application uses the existing clock registry, rail ledger, PMIC provider,
PCK manager, media-clock controller and register mutex. HDMI first prepares its
actual PHY/controller and disabled TV0. The framebuffer source borrows those
live PCK and supply reservations, obtains whole-DE and complementary IOMMU
exclusion, and prepares DE domain 1 using the published power/reset sequence.
The source is `drivers/display/a733_display_framebuffer.c`, which the
DisplayPort image (TV1) and the GPU display image (TV0) also use. Each image
supplies only its session values and output callbacks: the designated task and
unchanged session, the live output clock, rail, PMIC and PCK leases, and the
output state that admits each DE action.

Domain preparation first checks that the low four bits of the DE_SYS PCK command
and status registers agree at OFF (0) or ON (8), then asserts the DE bus and module resets and waits 10 µs. The
shared fixed domain-ON provider (`a733_de_sys_power`, also used by G2D) adopts
an ON domain without writes. For an OFF domain it writes the pinned
`0x170/0x174/0xc00/0xc04/0xc10` delay row, requests ON and waits at most
10 ms for the status. Any other state, or an expired wait, retains everything.
It adopts a verified stable 600 MHz DE PLL and authenticated fabric settings.
It does not invent a cold PLL image. Both IOMMU bus-clock groups and the global DE AHB/MBUS gates must already
be enabled, consistent with the authenticated initial handoff.

Serialized boot initializes the pool and constructs one complete allocation before
scheduling starts. Startup admission is checked again before releasing tasks.
The allocation must lie inside the uncached pool; in this image it owns the
pool's complete interval `0x41c00000..0x41e00000`. Native initialization
verifies every CPU translation.
The first 32 KiB contains the IOMMU tables. The remaining bytes are mapped read
only to display master 8 at IOVA `0x10000000`. A 4 KiB guard precedes the pixels;
the immutable surface starts at IOVA `0x10001000`, physical `0x41c09000`, with a
2560-byte pitch. Its 1,843,200 pixel bytes fit with more than 200 KiB of mapped
tail padding. The complete parent stays owned on partial construction failure.
No CPU cache clean is needed for the verified uncached mapping; publication
still uses a completion barrier. No cached alias is admitted.

The generated picture has eight colored bars, a black border and diagonal
marks, distinguishing framebuffer output from the controller's hardware pattern.
Published driver logic programs UCH3 layer 0, opaque RGB565 format `0x0a`, DE1
pipe 0, scaler bypass and the existing line-buffer partition. The pinned device
tree assigns mux value **4** to the TV0 route; its node name `tcon3` is not the
mux value. TV0 selects the DE source. The existing controller CSC converts
full-range RGB to limited-range HDMI RGB.

Native TV0 enable requires the actual DE object to be ARMED, its live clock
and memory generations to match, and its installed read-only translation to
resolve the exact immutable surface. Initial IOMMU installation requires a
separate authenticated record establishing prior transaction drain, inherited
mapping resolution and exclusion of all ten masters. Disabled hardware bits
are consistency checks, not that proof. Only master 8's micro-register set is
accessed; other masters remain excluded and unavailable throughout the session.

Normal stop disables HDMI/TV0 first, finishes any outstanding diagnostic fault
scan, then disables DE while retaining clocks and memory. Failures retain all
partially acquired ownership. No flip, live replacement, unmap, reset recovery
or buffer retirement is implemented by this image.

## Packaging and qualification

`apps/hdmi/prepare.py` snapshots the ELF, session and supplied admission records,
checks the fixed load/reservation geometry and produces the existing supervised
launcher bundle. Framebuffer admission additionally names
`720p60-rgb565-retained-v1`, the exact framebuffer authority, and hashed `isolation`
and `memory` records. The packer does not establish the physical facts asserted
by those records. The launch coordinator must qualify the analog profile,
supplies, initial isolation, RAM ownership and supervisor before upload.

HDMI monitor and cable availability are unknown. Visible output, EDID acceptance,
color accuracy, memory reachability and initial isolation require physical
qualification. No MIPI camera or display is connected; neither accessory is
used here. Other HDMI modes, formats, audio, CEC, HDCP, live frame replacement
and the broader DSI/display requirements remain separate required work.

The focused check is `checks/hdmi-framebuffer.py`. It builds the affected native
image, reuses its compiled objects in one composition, and continues four faults
from saved CPU/memory checkpoints. The nominal run powers DE_SYS from OFF and
must write the delay row and ON request in order. A second complete run starts
from an inherited ON domain and must reach DE start without any PCK write. The
hook sites and HDMI native structure offsets come from the fixture ELF and its
debug information. Hardware and scheduling are modeled; the
prepared HDMI PHY/EDID checkpoint reuses prior component evidence. The new path
runs actual allocation, table construction, ownership managers, media clocks,
DE, TV0 and application start/stop code. Source expectations for the new RGB565
format and TV0 route come from the already pinned published driver and device
tree. This check does not replay the old HDMI or DSI test matrices and does not
establish physical output.

The native image build and the five selected composition outcomes passed. The
artifact is preserved in `~/.cache/sunstoneos/evidence/hdmi-framebuffer-20261017`;
its image SHA-256 is
`6856bb229972c7f05e425d957069735a3bea8f944e1ff394503936adb7f63317`.
The outcomes cover normal start/stop, immutable-authority mutation, registry
failure before TV0 enable, uncertain DE disable, and cancellation during a
pending DE start. Each failure keeps the full memory allocation reserved.

The evidence preserves a fixture qualifier compilation error and an overbroad
trace assertion separately from passing results. Only the unfinished fixture
work continued. A corrected cancellation assertion required one fixture-only
relink; all native runtime objects and the image were reused. One bounded
prerequisite reconstruction supplied durable checkpoints for the four remaining
faults. The successful nominal stop path was not repeated. The registry header's
VO1 ownership clarification changes comments only; its compiled C tokens match
the image's archived header.
