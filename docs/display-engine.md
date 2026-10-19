# A733 display engine: one immutable RGB framebuffer

The native display-engine driver programs one full-frame, linear XRGB8888
surface for LCD1 and DSI1. It is implemented and checked against published
register-programming code. Physical scanout is unqualified. Platform resource
binding, a selected panel, and board execution remain prerequisites.

The implementation is in `drivers/display/a733_display_engine.c`, with its
interface in `include/a733_display_engine.h`. It allocates no memory, starts no
task, and reads no framebuffer bytes through the CPU. A privileged FreeRTOS
owner supplies bounded MMIO, time, barrier, and admission callbacks.

## The pixel path uses UI channel 3 and display output 1

The source identifies A733 as display-engine generation 0x352. Its channel
configuration 0 assigns UI channel 3 alone to display output 1. The native
profile follows that configuration:

`mapped framebuffer → UCH3 layer0 → scaler/CSC bypass → blender1 pipe0 → LCD1 → DSI1`

All offsets below are relative to display-engine base `0x05000000`.

| Component | Offset and configuration |
| --- | --- |
| UCH3 | Channel base `0x220000`, physical UI channel ID 3, source channel ID 6 |
| Overlay | `0x221000`, layer0 enabled; layers1–3 disabled |
| Channel color conversion | `0x220800`, enable bit0 must be clear |
| VSU8 scaler | `0x224000`, enable bit0 must be clear |
| Channel-to-output route | `0x802c[15:12]=1` |
| Output1 port0 route | `0x8034[3:0]=11`, the UI-channel encoding `8+3` |
| Blender1 | `0x2a1000`, pipe0 uses port0, opaque source-over composition |
| Output1-to-TCON route | `0x8010[7:4]=1`, selecting LCD1 |
| Output1 dimensions | `0x8148`, width-minus-one and height-minus-one |
| Output1 control | `0x8140`, one pixel per cycle, progressive, interrupts disabled |
| Output1 line buffer | `0x8054=0x1000` |

The Linux source partitions the shared line buffer as `0x2000` for output0 and
`0x1000` for output1. This implementation uses that partition. An older U-Boot
implementation assigns `0x3000` to a sole active output; that policy is not used.
The native driver excludes both display outputs while it owns the DE.

UCH3 has no AFBD or TFBD compressed-buffer decoder in the pinned source tables.
Its scaler and color conversion remain bypassed. The driver also requires the
output1 gamma matrix, gamma/test-color control, color-temperature correction,
demo/skin controls, dithering, smart backlight processing, and CRC controls to
be disabled. It verifies their control registers before each action. The source
lists no output1 debanding block. Cold/reset provenance must establish that
other channels, writeback, offline composition, and queued register updates
cannot run; inactive output bits alone do not establish that condition.

## Address admission preserves the framebuffer lifetime

XRGB8888 means little-endian bytes B, G, R, X. The overlay uses format encoding
4, global alpha 255, and no crop, scaling, premultiplication, interlace, or
downsampling. Each row starts at `device_address + row * pitch`; pitch is a
32-bit byte count. The top address consists of a 32-bit low word and an 8-bit
high field. Bottom-field addresses remain zero.

The planner requires four-byte address and pitch alignment, pitch at least
`4 * width`, and a nonzero surface owner. It checks both the complete padded
image, `pitch * height`, and the final pixel byte address,
`device_address + (height - 1) * pitch + 4 * width - 1`. The supplied mapping
must contain the padded image and fit entirely below the 40-bit address limit.
All multiplication and range checks use 64-bit arithmetic.

These calculations bound the programmed rows. They do not establish hardware
burst overfetch behavior. The memory authority must authenticate sufficient
mapped padding for actual fetch bursts, address translation, IOMMU policy, and
cache visibility. The device address can be an IOVA under that policy; it is
never inferred from a CPU pointer. The source address fields do not establish
that every 40-bit address is reachable on this board.

The surface and its padding become immutable before admission. The owner must
retain the allocation, mapping, cache state, and lease through every driver
state, including errors and local disable. The component has no release or
replacement API. A separate authority must prove that transactions have drained
before reclaiming memory or clocks.

## Startup programs an inactive producer before LCD1 starts

Timing admission calls the existing [LCD1 planner](tcon-lcd.md), which composes
the [DSI video planner](dsi-video.md). The profile therefore inherits its
four-lane, continuous-clock, progressive timing limits and exact clock
relationships. The driver neither measures nor changes clocks. The production
owner must bind the same timing and actual clock evidence to DE, LCD1, and DSI1.

The source driver normally uses a register command queue, or RCQ, to transfer
register images from memory. It also provides `FORCE_ATOMIC_FLUSH`, which copies
those images directly through AHB. The native component uses that direct path
only while the producer and LCD1 are inactive. It requires no RCQ allocation
and submits no register queue request.

`PREPARE` authenticates ownership and reads the initial controls. It writes the
published overlay and blender register blocks, reads each write back, installs
the shared routes, and programs output size and line-buffer depth. It sets
the source-defined MBUS urgency depth to 2048 and clears output1 RCQ header
address/length. Shared writes preserve unrelated fields. Conflicting TCON
routes cause refusal instead of changing another output's route.

The overlay and blender layer bits are set during preparation, while output1
global enable remains clear. `READY` therefore describes an inactive configured
producer. `START` reauthenticates the memory and display resources, enables
output1 automatic clock control, and then enables the DE producer. Its `ARMED`
result allows the separately authenticated LCD1 start. It does not establish
that the panel has received pixels.

Before these actions, the platform owner must establish stable power, clocks,
released resets, and recovery. The driver verifies internal DE reset/clock bits
`0x10010` at offsets `0x8000`/`0x8004`, and MBUS bits `0x11` at `0x8008`.
It performs no reset, CCU, pin, power-domain, or IOMMU writes. Ownership excludes
all other writers to the complete shared words and all autonomous RCQ users.

## Shutdown preserves clocks and memory until independent drain evidence

`OBSERVE` checks ownership and controls, then records the raw output1 status
word at `0x8144`. It does not acknowledge write-one-to-clear flags. The source
defines frame-end, RCQ-finish, and RCQ-accept flags, but its DE error flag mask
is zero. This component makes no comprehensive error-detection claim from that
status word. Platform IOMMU faults and LCD/DSI underflow remain separate checks.

The display coordinator first stops the DSI instruction engine while its pixel
feed remains available. It can then request DE `STOP`, which clears output1
global enable and verifies the write. Automatic clock control, all resource
leases, the programmed framebuffer, and its mapping remain held. The terminal
state is `DISABLED_RETAINED`.

Neither a cleared enable bit nor frame-end proves that the DE has stopped
accessing memory. The memory/platform authority must establish transaction
drain independently before the [LCD1 stop contract](tcon-lcd.md) can be met.
The source inspected here does not supply a sufficient drain proof. That
platform completion mechanism remains required work.

Each poll performs at most one admission, MMIO, or barrier callback, with a
monotonic absolute deadline checked before and after it. An uncertain write,
failed readback, callback failure, clock regression, deadline, or reentry ends
in `FAULT_RETAINED`. There is no automatic rollback, reset, retry, or release.
Terminal states perform no callbacks.

## Evidence covers source agreement and retained failure states

The primary register reference is the published Linux display driver at
[revision 2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/drm/sunxi_device/hardware/lowlevel_de).
Relevant files are `sunxi_de.c`, `de_top.c`, and the overlay, blender, scaler,
CSC, gamma, dither, smart-backlight, and CRC modules and platform tables.
The A733 User Manual V1.00, section 14.3, describes DE capabilities but does
not provide its register map. Source hashes are embedded in
`checks/a733-display-engine.py`; source-lock proposals accompany the evidence.

One ASan/UBSan source/model batch compared complete overlay, blender, and top
register images through preparation, start, and stop. Four profiles covered
ordinary and padded pitch, high address bits, the 40-bit boundary, and the
existing burst/loose-RGB666 timing profile. The comparison executes unchanged
source functions with reduced host type and MMIO adapters.

The source comparison exposes two intentional control differences: producer
and automatic-clock enable are deferred until `START`; automatic-clock control
remains enabled after `STOP` pending external drain. The source blender blocks
overlap at one word; the native AHB copy writes that identical word once.
No other register-image difference is accepted.

The same batch checked address/timing rejection, authority denial, active RCQ,
enabled bypass blocks, conflicting routes, uncertain writes, readback mismatch,
post-write deadlines, clock regression, reentry, and stop refusal. No prior
DSI or LCD test was rerun.

The freestanding AArch64 object is 3024 bytes, with direct stack frames of
336 bytes for planning, 592 for initialization, and 96 for polling. Planning
nests the LCD/DSI planners, so the FreeRTOS task budget must include their
frames and callback frames. These build measurements do not measure execution performance.

Exact inputs, executables, commands, native object, stack report, and results
are preserved under
`~/.cache/sunstoneos/evidence/display-engine-20261016/initial-snapshot/`.
No physical hardware was accessed.

## Complete display support requires further work

Required follow-ons include production resource and memory binding, physical
fetch/scanout qualification, independent DMA drain, live RCQ updates, buffer
replacement and vblank retirement, interrupt/error handling, and recovery.
Further profiles must cover the remaining formats, layers, channels, scalers,
color processing, compression, dual outputs, HDMI/eDP routes, writeback, and
other supported display modes. The immutable RGB plane is one implemented
component of full board support.

## DE352 replay of two immutable addresses

The optional replay operation uses a fixed bank containing two page-rounded
frames followed by two 4 KiB command pages. Both frames have the same format,
pitch and dimensions. Each command page contains an aligned pair of 16-byte RCQ
headers and a 32-byte-aligned source word. One header copies the selected frame's
low address into UCH3 layer0 at DE offset `0x221010`; the second is inactive.
The bank stays below a 4 GiB IOVA boundary, so the unchanged high address is zero.
No caller supplies an arbitrary register destination or command stream.

`a733_de_replay_build()` constructs these pages before publication;
`a733_de_replay_valid()` checks every command-page byte. The native binding
validates the canonical pages and the complete bank's immutable READ mapping.
`a733_display_native_replay()` passes through the existing source/token handoff
and display coordinator, preserving media-clock, PLL, PCK and framebuffer-bank
ownership. One presentation may be pending at a time.

The update follows the pinned DE352 source rather than its DE201 double-buffer
fallback. It clears stale finish/frame-end flags using the published W1C rule,
programs the RCQ header address and 32-byte length, and publishes those writes.
The busy test, RCQ control read and update write form one bounded transaction
under the shared register mutex and existing FreeRTOS critical section. A busy
sample from an earlier task poll cannot authorize submission. Idle returns
pending without writing. Fresh RCQ finish completes the register copy; a cleared
and subsequently observed DE frame-end flag records a later controller frame
boundary. Readback also checks the selected address.

These observations do not prove that old framebuffer reads or read-ahead have
drained. Neither frame nor command pages may be overwritten, unmapped or
released. The pinned Linux CRTC code uses DE busy and RCQ completion when
signaling page flips, but its vendor commit tail omits the vblank wait before
old-plane cleanup. That cleanup path does not supply an independent transaction
retirement proof. The manual's DE chapter provides no such mechanism.

Replay source authority remains Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`: `de_top.c`, `de_base.h`,
`sunxi_de.c`, `sunxi_drm_crtc.c`, `sunxi_drm_drv.c` and `drm_atomic_helper.c`,
with hashes in `sources.lock.json`. Failed admission, uncertain submission,
readback mismatch, stale completion or deadline retains the bank and resources.
Arbitrary new-frame streaming and safe old-buffer recycling remain required
work, independent of this two-slot replay operation.
