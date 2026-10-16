# LCD1 supplies the DSI1 timing and pixel-request path

The native TCON LCD component configures LCD1 to feed DSI1 from a display-engine
pixel producer. It prepares timing and trigger registers, starts the local
controller, samples status, and disables the local controller after external
quiescence. Source comparisons, a host lifecycle model and AArch64 compilation
provide software evidence. Physical scanout remains unqualified.

The implementation is `drivers/display/a733_tcon_lcd.c`, with its interface in
`include/a733_tcon_lcd.h`. This document assumes familiarity with peripheral
register programming. TCON is the timing controller; DE is the display engine
that supplies its pixels. DSI serializes those pixels for a panel.

## The admitted route is LCD1 to DSI1

LCD1 registers occupy `0x05502000`; the shared video-output control block starts
at `0x05500000`. DSI1 connects to the board LCD connector. The A733 manual defines
DSI1 source-selection bit 4 at shared offset `0x04`: zero selects LCD1 and one
selects LCD0. The selector is not the numeric TCON index.

The component selects LCD1 and the display PLL clock for LCD1. The DSI1 PHY clock
source must already select the display PLL before PHY preparation, while that
PHY is inactive. The component checks this prerequisite and preserves that mux
bit. It never switches a live PHY clock or programs a PLL or CCU register.
The LCD1-to-DSI output gate at shared offset `0x20`, bit 17, stays closed during
preparation and opens only during authenticated start.

The shared source, clock-selection and output-gate words also affect other
outputs. Their complete words require exclusion of every competing writer
through the component's lifetime. Read/modify/write operations span polls;
locking only the changed bit is insufficient. Unrelated bits are preserved.

The pixel-source selector chooses DE. Programming this selector does not route
a particular DE output: the source's `tcon_de_attach()` returns without changing
hardware on this generation. A production DE binding must establish the actual
DE-to-LCD1 route, mapped buffers, visibility and ownership before start.

LCD0 routes, dual-link operation, RGB, LVDS, interlaced timing, CPU-panel command
mode, dither, color processing and other DSI profiles remain required work.
This component admits progressive, single-link LCD1 operation up to 1920×1080
with the four-lane RGB888 or loose-RGB666 modes described in
[DSI video preparation](dsi-video.md). These admission limits do not establish
a qualified resolution or refresh rate.

## Actual DCLK is explicit and uses only phase zero

The caller supplies both the DSI timing configuration and `actual_dclk_hz`.
The latter must equal the authenticated DSI1 display PLL low-speed output.
The internal divider is one, so LCD1's actual pixel clock, DCLK, equals that
PLL output. The existing DSI planner validates the exact PLL and lane-rate
relationships. No register comparison measures the actual clock frequency.

For nonburst video, DCLK equals the DSI nominal pixel rate. For burst video,
DCLK equals ten ninths of that nominal rate. The application must distinguish
these values when choosing timing and evaluating frame cadence. The native
planner computes its safe-period FIFO threshold from actual DCLK; the Linux
wrapper configures TCON before raising the burst transport clock. The unchanged
low-level comparison receives actual DCLK explicitly.

The published driver opens all three DCLK phases with enable value `0xf`.
The manual requires a divider of at least six when phases one or two are used.
At divider one, the native driver enables only phase zero with value `0x1`.
The selected CPU-trigger DSI path requires the ordinary pixel clock. Electrical
qualification must confirm this path and the actual source frequency.

## Timing arithmetic rejects field overflow and insufficient blanking

Horizontal timing values are pixels and vertical values are lines. Let `HT`
and `VT` denote their totals, and `W` and `H` denote the active dimensions.
The component writes the source's timing encodings:

| Register field | Encoded value |
| --- | --- |
| Active width and height | `W - 1`, `H - 1` |
| Horizontal total | `HT - 1` |
| Horizontal back porch | horizontal back porch + sync width − 1 |
| Vertical total | `2 * VT`, in half-line units |
| Vertical back porch | vertical back porch + sync width − 1 |
| Horizontal and vertical sync | respective sync width − 1 |
| Trigger block size and count | `W - 1`, `H - 1` |
| Trigger block space | `HT - W - 40` |
| Trigger start delay | `floor(3 * HT / 8) - 1` |
| Transmission start threshold | 10 pixels |
| Safe-period FIFO threshold | `floor(actual_dclk_hz * 15 / 1000000)` |

The source block-space expression reduces to `HT - W - 40` for the admitted
24-bit, four-lane profile with DSI divider six. The manual requires block space
greater than twenty pixel periods. The planner rejects blanking totals of sixty
pixels or fewer instead of allowing unsigned subtraction to wrap.

Vertical back porch must be at least four lines, avoiding the source's warning
path for smaller porches. Vertical front porch must be at least two lines.
Sync widths fit ten bits after subtraction, combined back porches fit twelve
bits, and `2 * VT` fits thirteen bits. The existing DSI limits and all TCON
field bounds apply before the output plan is changed.

The trigger FIFO and direct pixel transfer path are prepared with trigger mode
disabled. Only writable control fields are emitted. The frame-start bit is
write-one-to-start; native read/modify/write operations never copy it back as
one. Read-only CPU access flags and the current-block counter are also excluded
from write values and readback equality.

## Start requires a DE producer and prepared DSI host

Initialization copies the configuration and pure register plan. Preparation
requires stable power, bus clocks, released CCU reset, resolved boot ownership,
qualified pins and independent watchdog recovery. A nonzero lease identifier
alone proves none of these conditions. The production resource provider remains
unimplemented.

Baseline reads require a disabled global/output controller, no enabled DCLK,
no active CPU transfer, and no competing interrupt or test mode. Color
processing, dither, dual-link synchronization, LVDS output and frame-sync
production must be inactive. A reported FIFO underflow prevents preparation.
The controller performs fourteen masked writes with readback and completes a
barrier before reporting `READY`.

The source configures several enables inside its timing routine. Native
preparation defers global enable, LCD output enable, trigger enable and the
shared output gate. `READY` therefore leaves the output inactive.

Start requires a separate authority check: DSI1 and its panel are prepared,
the DSI engine remains idle, and DE is armed with correctly routed, visible
pixels of the required format and dimensions. The native sequence enables the
global controller, shared output gate, phase-zero DCLK, trigger mode and LCD
output, with readback. It then checks status and completes a barrier.
`ACTIVE` reports local enable/readback completion. It does not prove a displayed
frame, panel acceptance or pixel delivery.

An explicit observation request checks the active control profile and samples
the current-line and status registers. It reports FIFO underflow and DSI-trigger
underflow as retained faults. Ordinary event flags remain untouched. The driver
never acknowledges interrupt flags; the manual defines them as write-zero-to-clear.
A line sample does not establish frame progression. The caller must schedule
observations because the object performs no implicit background polling.

## Shutdown requires DSI idle and a drained DE producer

DSI must stop first while the TCON feed remains available. After DSI idle is
established, the higher-level owner must quiesce DE and drain its transactions.
The stop admission callback authenticates both facts independently. This
component's register reads cannot establish DMA drain or return framebuffer
ownership to the CPU.

Stop clears LCD output enable and then global enable. The manual states that
disabling global control resets the local module to idle. Only after those
writes and readbacks does the component disable DCLK and the shared output gate.
This ordering avoids relying on LCD output disable alone, which the manual
samples at the first blank line. Final status reads and a barrier establish
`STOPPED`. The control profile remains held; no restart or resource release is
provided.

Each poll performs at most one admission, register or barrier operation, with
monotonic deadline checks before and after it. I/O failure, uncertain writes,
failed readback, expired or regressed time, lost authority and callback reentry
cause `RETAINED`. The driver performs no rollback, reset retry or later I/O from
a retained state. Buffers, clocks, pins and shared ownership remain held until
an independent owner resolves recovery.

## Preserved evidence separates source agreement from physical qualification

The source reference uses Orange Pi Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`:

- `bsp/drivers/drm/sunxi_device/hardware/lowlevel_lcd/tcon_lcd.c` and its
  adjacent headers define timing, trigger, enable and stop operations.
- `bsp/drivers/drm/sunxi_device/hardware/lowlevel_tcon/tcon_top.c` and
  `tcon_top_type.h` define shared source, clock and gate registers.
- The A733 User Manual V1.00, sections 15.1.9 and 15.1.10, defines selector
  polarity, phase/divider restrictions, trigger access types and idle reset.

The manual PDF SHA-256 is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
Source hashes and commands are recorded with the evidence.

Eight source profiles cover both admitted video modes and formats at ordinary
and constrained blanking timings. Each comparison checks the complete defined
LCD and shared-top register images after preparation, start and stop. The
comparison explicitly asserts the source's enable values before applying the
four deferred-enable differences and the phase-zero correction. It performs no
other normalization.

The source comparisons completed before a fault-model assertion exposed an
invalid simulated clock regression: its initial time was already zero.
The initial executable, inputs and failure record remain preserved. Correcting
the model's initial time allowed the fault cases to pass separately; the runtime
and completed source comparisons were unchanged. AddressSanitizer and
UndefinedBehaviorSanitizer reported no memory or arithmetic failures.

The fault model covers authorization, field refusal, shared-word preservation,
readback corruption, applied writes followed by failure, callback deadlines,
time regression, reentry, underflow and quiet retained states. It also verifies
that missing DSI idle or DE drain prevents shutdown writes. Existing DSI packet,
PHY and video check corpora were reused without repetition.

Evidence is under `~/.cache/sunstoneos/evidence/tcon-lcd-20261016/`.
The AArch64 object contains 3040 bytes of code and read-only data. Direct stack
frames are 672 bytes for planning, 320 bytes for initialization and 80 bytes
for polling. Planning calls the existing DSI planner, so stack provisioning
must include nested frames. These are build facts, not performance measurements.

Physical qualification still requires the combined resource provider, DE route
and buffer binding, panel power/reset and initialization, verified clocks and
signal timing, frame-progress evidence and recovery. Other display routes and
profiles remain separate completion requirements.
