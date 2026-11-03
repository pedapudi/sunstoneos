# DSI PHY preparation with an inherited PLL

SunstoneOS implements bounded analog and low-power host preparation for either
A733 DSI controller. It requires an already enabled display PLL whose factors,
gates and reference clock match an explicit plan. AArch64
compilation passes; no host check remains. Physical signaling remains unqualified.

The [PHY API](../include/a733_dsi_phy.h) prepares the registers required by the
[command transport](dsi.md). Cold PLL activation, retuning, shared platform
ownership binding, scanout and panel-specific setup remain required work.
The preparer does not write PLL, CCU gate or reset registers.

## Exact clock plans exclude rounding and truncated dividers

The A733 manual describes a stable display-PLL oscillator range of
1.26–2.52 GHz, an actual operating frequency limit of 2.4 GHz, and output
limits of 1 GHz for HS and 1.2 GHz for LS. HS supplies the DSI serial clock;
LS supplies the timing-controller function clock. These clock limits do not
establish qualified panel lane rates.

The planner follows the published Linux clock function with a stable 24 MHz
reference. It chooses the smallest HS divider that reaches the oscillator's
minimum frequency. It requires an integral reference multiplier and an exact
LS divider. The LS factors follow the source's preference for a factor of
three, then two, then one.

The planner rejects divider-field overflow, rounded rates, oscillator rates
above 2.4 GHz and output rates above the manual's limits. For example, HS
480 MHz and LS 120 MHz produce a 1.44 GHz oscillator, multiplier 60, HS
divider 3, and LS dividers 3 and 4. It refuses the source's 48 MHz HS request
because the selected clock algorithm would overflow its four-bit divider.

The runtime compares the inherited PLL against the resulting factor image.
It requires enabled PLL/LDO/voltage control, both output gates, lock detection,
and disabled fractional/spread-spectrum modulation. It samples lock three
consecutive times at intervals of at least five microseconds, then waits
another twenty microseconds. It repeats clock checks and lock filtering before
reporting completion. Observed lock and factor readback do not measure output
frequency or analog signal quality.

The published Linux and U-Boot A733 paths enable the PLL before changing its
factors. The available manual gives clock specifications but no DSI PLL
cold-start register sequence. An arbitrary disabled initial image therefore
has no established safe activation sequence. The runtime rejects that state
before writing analog or host registers. Cold PLL initialization remains a
separate implementation requirement.

## One authority covers the host, PHY and shared reset

The privileged admission callback must authenticate an existing combined lease
for the host, PHY, PLL, VO power, rails, reference clock and pins. It must also
exclude timing-controller, video, LVDS, interrupt and remote-firmware writers.
A nonzero token alone does not establish ownership. The production callback
and common registry binding remain unimplemented.

The preparer verifies the controller's 150 MHz clock selection, divider and
gate, plus the shared bus gate/reset-release bits. The callback must establish
that the parent clock actually supplies 150 MHz and that the PLL reference
supplies 24 MHz. The current clock/pin managers are unchanged.

Host and PHY module enables must initially be clear. Analog output enables,
LVDS/combo enable bits, transmit-force controls, debug overrides and interrupt
enables must also be inactive. Every baseline check completes before the first
write. Reset is shared between host and PHY; this component never uses reset
to obtain an acceptable baseline.

The source's analog sequence configures LPX/HS timing counts, termination
calibration, current bias, analog regulators and transmitter settings. It
enables the combo supply and MIPI path, waits at least one microsecond, then
enables clock termination, dividers and the PHY module. The subsequent source
configuration sets timing words and the requested one-to-four-lane mask.
Consecutive fields in the same word are combined into masked updates while
preserving the source's ordering between words.

Host preparation installs the LP11 and low-power data-transmit instructions,
their loop settings, command ECC/CRC enables and the source's conditional
branch outside the LP command path. It completes a barrier before enabling
the host. High-speed/video instructions and pixel registers remain outside
this component's implementation. Programmed HS timing defaults do not qualify
high-speed transmission.

Each poll performs at most one admission, MMIO or barrier operation, with
monotonic deadline checks before and after callbacks. Masked updates preserve
unrelated bits and verify the resulting word. Waits yield to the caller.
An uncertain write, late operation, changed register or clock failure retains
the combined lease and partial state. Terminal polls perform no I/O.
No rollback, reset, resource release or automatic retry occurs.

`READY` reports completed register preparation. The pure
`a733_dsi_phy_ready()` predicate lets the command authority provider check the
same controller, lane count and lease. That provider must still authenticate
the live platform reservation and conditions required by the command API.
The predicate alone grants no hardware authority.

## The board's DSI1 route has separate display limits

The manual lists four data lanes, bidirectional LP communication on data lane
zero, and 5.6 Gbit/s aggregate PHY throughput. It lists single-link operation
without display-stream compression up to 1920 × 1200 at 60 frames per second.
Only DSI0 supports display-stream compression; the board's LCD connector uses
DSI1. A panel selection must respect that route and receive physical validation.

Dual-link, compressed, high-speed video, ultra-low-power transitions and panel
reads remain required supported modes where the physical route permits them.
Panel power, reset, initialization commands, timing, backlight and touch remain
separate configuration and driver responsibilities.

## No host check covers preparation or the composed command

No host check covers the following, for both controller addresses and all lane
counts. The driver does not prove analog behavior, PLL frequency, panel
reception or physical register access.

- Four distinct clock plans compared with the unchanged published clock
  functions.
- The complete PHY register image compared with the unchanged analog and
  configuration functions, and every affected LP host field compared with
  unchanged host preparation.
- The analog delay, repeated lock sampling, restart after an unlocked sample,
  unrelated-bit preservation and copied configuration.
- Failed or late operations at authority, initial reads, lock polling, partial
  analog activation, host enable and the final barrier.
- Unsupported cold PLL state, factor or modulation mismatch, active consumers,
  failed readback, missing lock, clock regression and quiet retained failures.
- Rejection of inherited manual transmitter, analog test and host-control bits
  before any write.
- A DCS packet submitted to the command controller after preparation.

The freestanding AArch64 object contains 3,088 bytes of code/read-only data.
Its largest direct stack frame is 64 bytes; nested and callback frames are
additional. It requires `memcpy` and `memset`. No physical board access or
performance measurement has run.

The PHY/clock references use Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The independent bootloader inspection
uses U-Boot revision `b791be842935b27268ae3d00e943a9075495f30a`.
The A733 User Manual V1.00, sections 15.5.1–15.5.5, supplies capability and clock
limits. Its PDF SHA-256 is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.

The recorded results of the removed host checks are in [the evidence record](evidence/dsi-phy.jsonl).
