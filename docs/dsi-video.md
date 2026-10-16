# A733 DSI video host preparation

The native DSI video component prepares controller timing, pixel packets and
high-speed instructions. It starts the local instruction engine, checks its
status on request, and requests a bounded stop. It has passed host source
comparisons and an AArch64 compile. Display scanout and electrical signaling
remain unqualified.

The interface is in `include/a733_dsi_video.h`; implementation is in
`drivers/display/a733_dsi_video.c`. This document assumes familiarity with
memory-mapped peripheral drivers. DSI carries pixels from a timing controller
(TCON) over high-speed serial lanes. The display engine (DE) supplies the pixel
stream that TCON routes to DSI. Preparing the DSI host alone does not create that
stream.

## Admitted modes use four lanes and continuous clock

Both A733 DSI controllers accept this component's bounded configuration. The
board LCD connector uses DSI1. The component admits a single link on virtual
channel zero with four data lanes and continuous high-speed clock. Pixel formats
are RGB888 and loosely packed RGB666, each occupying 24 bits per wire pixel.
Video modes are burst and nonburst with synchronization pulses.

The pure planner takes active width and height, horizontal and vertical sync
widths, back and front porches, pixel rate, and both display PLL output rates.
All horizontal values are pixels; vertical values are lines; rates are hertz.
It requires nonzero dimensions, at most 1920 by 1200 active pixels, and totals
that fit the controller fields. Horizontal sync requires at least four pixels,
back porch at least five, and front porch at least twenty. Vertical porches and
sync widths must be nonzero. These are implementation admission limits; they do
not qualify a panel or a refresh rate.

For nominal pixel rate `P`, the admitted clock relationships are:

| Quantity | Nonburst | Burst |
| --- | --- | --- |
| Display PLL low-speed output, `LS` | `P` | `10 * P / 9` |
| Display PLL high-speed output, `HS` | `6 * LS` | `6 * LS` |
| Per-lane byte clock | `HS / 8` | `HS / 8` |
| Controller module clock | 150 MHz | 150 MHz |

The ratio six follows from 24 wire bits divided across four data lanes. The
planner requires exact integer rates and an exact plan from the inherited-PLL
component described in [DSI PHY preparation](dsi-phy.md). It refuses rounding and
field truncation. It does not program or retune any clock.

The burst ratio follows the A733 Linux encoder's transport-clock adjustment.
The host burst line counter independently follows the source expression
`floor(floor(htotal * 24 / 32) * 10 / 9)`. Its integer truncation is retained.
A production TCON binding must resolve its output clock against these rates and
the nominal mode; the planner does not establish measured frame cadence.

Other lane counts, RGB565, packed RGB666, noncontinuous clock, nonburst sync
events, other virtual channels, alternate end-of-transmission packet settings, dual
link and compression remain full-support
requirements. The published nonburst HFP expression assumes three bytes per
pixel, and its request threshold assumes four lanes. Its packed RGB666 clock
divider truncates `18 / lanes`. Those expressions do not establish correct
support for the excluded profiles. A733 specifies display stream compression
only on DSI0; the board's DSI1 route has no DSC support.

## Packet counts retain the source's wire overhead

Each long packet has a four-byte header, payload, and two-byte CRC. The planner
emits the source's pixel data type, payload byte count, six header ECC parity
bits, and CRC initialization. Reserved ECC bits are zero. The hardware produces
pixel payload CRC from the incoming stream. Blanking payloads consist of zero
bytes; the planner computes their CRC using polynomial `0x8408`, initial value
`0xffff`, over the admitted payload count.

For nonburst video, let `HSA`, `HBP`, `HFP` and `W` denote horizontal sync width,
back porch, front porch and active width in pixels. Let `HT` be their sum.
The source's blanking payload counts are:

| Packet | Payload bytes |
| --- | --- |
| Horizontal sync | `3 * HSA - 10` |
| Horizontal back porch | `3 * HBP - 10` |
| Horizontal front porch | `3 * HFP - 12` |
| Horizontal blank line | `3 * (HT - HSA) - 10` |
| Vertical blank | 4 |
| Active pixels | `3 * W` |

The subtractions account for the source's packet overhead in each interval.
The request threshold is `floor((HFP - 20) * 24 / 32)` and must fit ten bits.
A nonburst delay instruction uses loop count 49.

Burst mode suppresses the horizontal sync packets. Horizontal front-porch and
vertical-blank payload counts become zero. The back-porch payload starts at
`3 * HBP - 14`; the horizontal-blank payload includes that payload, its six-byte
packet overhead and the active pixels. The source adds one to four padding bytes
to the back-porch and horizontal-blank counts. It adds four bytes when the sum is
already aligned. The native planner retains this behavior.

Burst request edges, synchronization point 40, and trigger delay follow
`dsi_basic_cfg()` without its unused intermediate delay calculation. The planner
checks that the request edges fit the line and that the horizontal packet bytes
fit its four-lane byte budget. Burst delay loop count is zero. The burst vertical
sync-end packet uses the source's horizontal-sync-start data type.

## Preparation and engine operations retain combined ownership

The caller supplies ordered register access, a write-completing barrier,
monotonic microsecond time, and an authority callback. A nonzero lease identifier
alone establishes no authority. The provider must authenticate power, pins,
shared host/PHY reset and gate, the inherited PLL, exact clocks, and exclusive
controller ownership. Production binding to that provider remains required.

Preparation requires completed PHY preparation, an enabled host with idle
instruction engine, disabled host interrupts, and no pixel producer. Baseline
reads reject incompatible source selection, FIFO reset/gating, EOT setting,
manual start, ready threshold/selector overrides, endian mode, FIFO self-test,
conditional jump or reported instruction faults.
The component writes 42 masked host words with full readback. It prepares the
eight instructions used by the continuous-clock path; unused extended
instructions remain outside this component's path. A completing barrier
establishes `READY`.

Starting requires separate authority for an armed TCON/DE pixel producer and a
prepared panel. The provider must validate the route, timing, memory visibility
and pixel format, and exclude command-mode users and competing interrupt
handlers. The driver installs the source's instruction graph, verifies it,
executes the clear/set start sequence, and observes the start bit still set.
`ACTIVE` establishes that local engine observation; it does not prove panel
reception, pixel delivery or a displayed frame.

The continuous-clock instruction path is:

```text
LP11 -> HS clock -> data stop -> HS data -> delay -> data stop -> ...
                                  |
                          conditional stop
                                  v
                            HS clock exit -> END
```

An observation request checks that the engine is active and inspects the
reported transfer-fast and unknown-instruction flags. The driver preserves raw
status and retains ownership on either error. It does not acknowledge flags or
infer success from an unreliable FIFO-underflow debug bit. The application must
schedule observations; an `ACTIVE` object does no implicit background work.
Observation provides no frame-progress or panel-status measurement.

A stop request requires the producer to remain available until the local engine
stops. The driver enables the source's conditional data-stop-to-clock-exit
branch, verifies that write, and polls the start bit until clear. It then checks
instruction errors and completes a barrier before reporting `STOPPED`.
The published driver's fixed 30 ms delay is insufficient evidence of idle; the
native operation instead uses the caller's absolute deadline.

Each poll performs at most one register, barrier or admission operation, with
time checks before and after it. Callback failure, deadline expiry, time
regression, failed readback, authority loss or reentry leaves the object
`RETAINED`. All resources remain held because the hardware state may be
uncertain. No rollback, retry, reset, resource release or restart occurs.
States between requested operations perform no I/O. Independently qualified
watchdog recovery remains a prerequisite for physical execution.

## Source evidence and qualification boundaries

Register fields and sequences come from the pinned Orange Pi Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`:

- `bsp/drivers/drm/sunxi_device/hardware/lowlevel_lcd/dsi_v1.c` supplies
  `dsi_basic_cfg()`, `dsi_packet_cfg()`, `dsi_start()` and `dsi_close()`.
- The adjacent `dsi_v1_type.h` defines register widths and offsets.
- `bsp/drivers/drm/sunxi_drm_dsi.c` supplies the A733 clock relationships.

The A733 User Manual V1.00 supplies controller capabilities and the DSI0-only DSC
restriction. Its PDF SHA-256 is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
The existing display source pins are reused; this component introduces no
additional external source dependency.

The host check compares ten complete 1 KiB register images with unchanged source
functions, excluding only unused extended instructions. Cases cover both
controllers, admitted formats and video modes, plus minimum admitted porches.
All defined header ECC bits and blanking CRC values agree. The source's
uninitialized reserved ECC bits are excluded; native reserved bits must be zero.
Source start and stop writes also agree. A bounded instruction-graph check
establishes that the admitted conditional stop reaches `END` in the model.

The lifecycle check covers producer authorization, refusal before writes,
readback failure, uncertain writes, I/O errors, deadlines, reentry, error flags,
engine stop timeout and quiet retained states. AddressSanitizer and
UndefinedBehaviorSanitizer reported no failures. A focused admission check also refuses ready-threshold, ready-selector and FIFO
self-test overrides before writes. The existing command and PHY check corpora
were reused without rerunning them.

The initial source/model executable and inputs remain preserved. The final
admission change adds one baseline read and tightens accepted control bits;
packet arithmetic, writes and instruction paths are unchanged. The focused
admission result supplements the initial comparisons without repeating them.

Evidence is preserved under
`~/.cache/sunstoneos/evidence/dsi-video-20261016/`, including executed
source snapshots, unchanged-source reference, executable, logs and hashes.
The AArch64 object occupies 4244 bytes of text and read-only data. Compiler stack
reports show direct frames of 752 bytes for planning, 640 bytes for initialization
and 64 bytes for polling. Initialization calls planning; stack provisioning must
include their nested frames and callees. These are static build facts, not
performance measurements.

Physical qualification still requires cold PLL provisioning or an authenticated
inherited PLL, production combined resource ownership, TCON/DE routing and feed,
panel power/reset and initialization, and selected panel timing. Read responses,
BTA, panel acceptance, frame progress, signal timing and the excluded profiles
remain unresolved. The source's noncontinuous-clock stop graph does not establish
a finite exit using its published conditional branch, so that mode requires
separate resolution. HDMI uses a separate TCON/output route and remains separate
implementation work.
