# DSI low-power command transport

SunstoneOS implements a bounded low-power transmit path for the A733 DSI
controllers. It constructs generic and Display Command Set (DCS) write packets,
loads command SRAM, starts the instruction engine, and observes local completion.
Host checks and AArch64 compilation pass. Physical signaling and panel operation
remain unqualified. No panel has been selected.

The [controller API](../include/a733_dsi.h) requires a prepared host and physical
layer (PHY). Its platform authority callback must authenticate their combined
lifetime ownership. The production authority provider, PHY setup, display
scanout and panel configuration remain unimplemented.

## Register ownership follows the shared host and PHY reset

The pinned A733 device tree and display driver identify these resources:

| Resource | DSI0 | DSI1 |
| --- | --- | --- |
| Host base | `0x05506000` | `0x05508000` |
| Display combo PHY base | `0x05507000` | `0x05509000` |
| GIC interrupt ID | 91, from SPI 59 | 92, from SPI 60 |
| CCU module clock offset | `0x1580` | `0x1588` |
| Shared host/PHY gate and reset offset | `0x1584` | `0x158c` |
| Four-lane pin group | PD0–PD9 | PD10–PD19 |
| Pixel timing controller route | TCON0 | TCON0 or TCON1 |

The CCU base is `0x02002000`. Each shared gate/reset word uses bit 0 for the
bus gate and bit 16 for reset release. The host and PHY must retain the same
authority over this word; stopping one consumer cannot reset the other.
Each module clock has a five-bit divider, a source selector at bits 24–26,
and a gate at bit 31. Sources are the 24 MHz system clock and 200/150 MHz
outputs from the peripheral PLL. The device tree selects the 150 MHz output.

The [hardware inventory](hardware.md) records four DSI1 lanes on the board's LCD
connector. The published board configuration also selects DSI1 and contains a
disabled panel example. Its reset, power, command sequence and timing values
are not an adopted SunstoneOS panel configuration.

The display engine at `0x05000000` supplies pixels to a timing controller
(TCON), which drives the chosen output. Both display-engine outputs can route
to TCON0 or TCON1. TCON0 is at `0x05501000`; TCON1 is at `0x05502000`.
Their top-level control block at `0x05500000` uses the VO power domain.
The display engine has a separate power domain, clocks, resets, memory traffic
path and IOMMU attachment. Command SRAM transfers do not establish any of these
pixel-fetch dependencies.

HDMI remains a separate required path: display engine → TCON3 at `0x05730000`
→ HDMI at `0x05520000`, under the VO1 power domain. TCON4 at `0x05731000`
routes to eDP. The source also exposes RGB, LVDS, dual DSI and display-stream
compression. Shared combo PHY and pin modes require explicit exclusion when
those paths are implemented.

## Packet bytes include the complete protocol overhead

Command SRAM holds 256 bytes at host offsets `0x300`–`0x3ff`. A short packet
occupies four bytes. A long packet contains a four-byte header, payload, and
two-byte CRC; the admitted payload limit is therefore 250 bytes.

| Packet | Data type | Admitted payload bytes |
| --- | --- | --- |
| Generic short write | `0x03`, `0x13`, `0x23` | 0, 1, 2 respectively |
| DCS short write | `0x05`, `0x15` | 1, 2 respectively, including command |
| Generic or DCS long write | `0x29`, `0x39` | 1–250 |

Virtual channels 0–3 are supported. The encoder supplies six header parity bits
and clears the two reserved ECC bits. Long-packet CRC uses the reflected
polynomial `0x8408`, initial value `0xffff`, and the payload length. Header and
CRC bytes are excluded from that length. Unused packet storage is zeroed,
including padding needed for the final 32-bit SRAM write.

Two defects in the published board wrapper are excluded from the native path.
Its long-write wrapper passes `payload_length + 1` to the CRC primitive, reading
past the declared payload. Its ECC primitive leaves the two reserved bits
uninitialized. The unchanged common Linux packet constructor and upstream
sun6i DSI implementation establish payload length and defined ECC bits.
The board primitive's six parity bits and its CRC function are also checked.

## A failed transfer retains ownership without resetting hardware

One privileged owner initializes the controller and submits one packet at a
time. Submission copies and encodes the payload before returning. Rejected
submissions leave driver storage unchanged and invoke no callbacks.
The caller supplies an absolute monotonic deadline in microseconds.

Before writing, the controller authenticates its combined host/PHY lease and
checks the enabled, idle host. It requires disabled interrupt delivery, the
source's LP11 and low-power data-transmit instructions, and their loop counts.
LP11 means the lanes' idle low-power state. Conditional branches must be
disabled or target the source's NOP-to-clock-exit branch outside this command
path. Clock, rail, pin, PHY and remote-firmware ownership remain obligations
of the privileged platform provider.

Each poll performs at most one admission, register or barrier operation with
time checks before and after it. The sequence loads packet words, writes and
verifies the byte count, and writes and verifies `LP11 → LPDT → END` at offset
`0x48`. It completes a write barrier, rechecks the saved control word, and
performs the source's start-bit clear/set sequence at offset `0x10`.
After another barrier it waits for the engine's start bit to clear.

`ENGINE_DONE` reports local instruction-engine completion. Panel reception,
command acceptance and resulting display state require separate evidence.
The source identifies transmit-status bits as unreliable; they are not used
as a substitute for panel responses. ACK packets, bus turnaround (BTA), reads,
video transmission and concurrent command/video operation remain outstanding.

Successful completion retains the lease. The owner explicitly reaps the result
before another submission. A failed or late callback, clock regression,
unexpected register value or reentrant operation enters `RETAINED`.
Failed writes may already have affected hardware. Faults retain packet storage
and ownership, and further polls perform no I/O. There is no reset, release,
rollback, cancellation or automatic retry API.

## PHY setup verifies inherited clocks and prepares command transmission

The [native PHY controller](dsi-phy.md) prepares analog controls and host
instructions under an admitted, already-running display PLL. It derives exact
high-speed and low-speed rates from the 24 MHz reference, verifies inherited
factors and repeated lock observations, then composes with command transmission.
It rejects inherited test controls that could force transmitter output.

The A733 manual specifies an operating oscillator range of 1.26–2.4 GHz,
a high-speed output limit of 1 GHz and a low-speed output limit of 1.2 GHz.
Those specified limits do not establish a qualified board or panel lane rate.
Source comparisons cover complete analog and host images; focused admission
checks and native compilation pass. Physical signaling remains unqualified.

Required work includes cold PLL setup, shared power/clock/reset admission,
video transmission, TCON routing/timing, display-engine buffer ownership and
scanout, interrupts, panel power/reset and commands, and backlight control.
Panel timings and power requirements remain separate configuration inputs.

## Validation compares complete packets and register images

The focused host check uses unchanged published functions for host preparation,
packet header construction, both ECC implementations, CRC and command loading.
It compares all 256 packet bytes and the complete 1 KiB host register image
for 84 cases covering both controller addresses, all lane counts and virtual
channels, supported short writes and long-packet alignment/capacity boundaries.
The reference models hardware clearing the start bit after transmission.

The same AddressSanitizer/UndefinedBehaviorSanitizer session checks failure,
late return and reentry at each of 28 operations in a representative long
transfer. It checks malformed profiles, failed readback, missing authority,
an engine that never completes, clock regression, copied inputs, atomic
refusal, retained leases and quiet terminal states. These are host models;
they do not prove PHY signaling, panel behavior or physical register access.

The freestanding AArch64 object contains 2,678 bytes of code/read-only data.
Its largest direct stack frame is 96 bytes; callback and nested-call frames
are additional. It requires the runtime's `memcpy` and `memset`.

Evidence is in `~/.cache/sunstoneos/evidence/dsi-command-20261016/`.
`results.json` records the host command, compiler, source hashes and artifact;
`target.json` records the native object and stack/undefined-symbol inspection.
The source snapshot, generated reference, executable and native object are
preserved for reuse. No physical board access or performance measurement ran.

Reproduce only when relevant inputs change:

```sh
python3 checks/a733-dsi-reference.py \
  --source-root ~/.cache/sunstoneos/sources/display \
  --evidence /path/to/new/evidence
```

Published reference files are pinned to Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The cache's
`display/manifest.json` records URLs and hashes for the device tree, clock,
host, PHY, TCON and packet references. The test verifies the hashes of its
six source dependencies before compiling extracted functions.
