# I2S0 finite capture is implemented; physical audio is unqualified

The native [capture component](../include/a733_i2s.h) configures the external
I2S0 receive path and transfers a finite sequence into RAM through the
[serialized DMA controller](dma.md). It supports normal I2S with two wire slots,
CPU-generated clocks, mono slot selection, and stereo capture. It has focused
host and pinned-source evidence. No microphone is attached, and no I2S register
access or audio capture has been attempted on the board.

Platform clock, reset, pin, and FIFO ownership callbacks remain to be connected.
The component calls the native DMA controller directly; the production FreeRTOS
DMA service remains separate work. Continuous capture, playback, external clock
input, wider TDM frames, Bluetooth PCM, timestamps, and power transitions remain
implementation requirements. A finite transfer does not establish continuous
microphone support or replace qualification of the remaining audio hardware.

## The source defines the external route and its conflicts

The [board wiring record](board-wiring.md) identifies schematic sheets 14 and 18.
The pinned device tree assigns I2S0 registers at `0x02532000`, a `0xa0`-byte
window, and DMA0 request 3 for receive and transmit. The RX FIFO is at offset
`0x10`. The pin controller assigns function 3 to each selected pin below.
These are source-defined routes; electrical operation remains unqualified.

| Capture signal | Header pin | SoC pin | Ownership |
| --- | --- | --- | --- |
| Bit clock, BCLK | 12 | PB5 | Required output |
| Frame clock, LRCK | 35 | PB6 | Required output |
| Data input, DIN0 | 38 | PB8 | Required input |
| Master clock, MCLK | 7 | PB4 | Optional output; omitted when `mclk_hz` is zero |

PB7 on header pin 40 provides DOUT0 at function 3 and DIN1 at function 4.
This capture component selects neither function on PB7. The platform must lease
PB5, PB6, and PB8 exclusively, and PB4 only when MCLK is requested. The pin lease
must reject alternate functions already assigned to GPIO, SPI2, PWM, LCD,
trace, TWI1, clock output, or OWA on those pins. PB4 also has an HDMI alternate
function. The precise alternatives differ by pin in the pinned pin controller.
The platform must establish suitable voltage, drive, and pull settings from the
selected microphone and board electrical requirements.

Bluetooth uses the separately wired I2S1 controller at `0x02533000`, DMA request
4, and module PCM signals. That route still needs its own protocol, ownership,
and headset qualification. Configuring I2S0 does not configure Bluetooth audio.

## Formats separate wire timing from stored sample meaning

`a733_i2s_plan` checks a requested format without I/O and leaves its output
unchanged on refusal. It accepts 16-bit or 32-bit containers, 16-bit or 32-bit
wire slots, and one or two captured channels. A container must fit its wire
slot. Mono selects wire slot 0 or 1; stereo captures slot 0 followed by slot 1.
The wire always has two slots, even for mono. Normal polarity, MSB-first order,
and the I2S one-bit data delay are fixed by this initial implementation.

The module clock must be a known, stable platform lease. The bit clock is
`rate_hz × 2 × slot_bits`. Both bit-clock and optional MCLK divisions must be
exact integer divisions supported by the controller. Supported divisors are
1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 176, and 192. The accepted
sample-rate range of 8–192 kHz is a software policy; it is not a measured limit.

For example, a leased 24.576 MHz module clock produces 16 kHz audio with two
32-bit slots at a 1.024 MHz bit clock, using divisor 24. Optional 12.288 MHz
MCLK uses divisor 2. The core does not select a PLL or change a shared clock.

| Capture container | FIFO and DMA configuration | Stored result |
| --- | --- | --- |
| 16 bits | Sample-resolution code 3, RXOM 1, two-byte accesses | Raw 16-bit word, little-endian memory |
| 32 bits | Sample-resolution code 7, RXOM 1, four-byte accesses | Raw 32-bit word, little-endian memory |

The component performs no arithmetic conversion or implicit sign extension.
A signed microphone sample uses the sensor's two's-complement convention.
For a microphone with 24 valid bits in a 32-bit slot, request the 32-bit
container and retain all bits. This is distinct from packed three-byte audio
and from a sign-extended, right-aligned 24-bit value in a 32-bit container.

If the selected microphone specifies MSB-aligned 24-bit data, valid sample bits
occupy bits 31–8; bits 7–0 follow that sensor's padding contract. The application
must discard those padding bits and sign-extend bit 23 of the remaining value.
For an unsigned raw word `u`, a portable signed conversion is
`(int32_t)((u >> 8) ^ 0x800000u) - 0x800000`.
That interpretation requires confirmation from the sensor data sheet and a
physical capture. No selected microphone currently establishes its alignment,
padding, channel strap, startup time, or valid sample rate.

## Capture ownership extends through verified DMA drain

The caller supplies an initialized DMA0 controller and an explicitly authorized
channel. All I2S calls and calls on that DMA controller must share serialization.
The component claims the chosen channel through the existing controller API;
it does not alter global DMA routing or initialize DMA clocks. Its platform
callback must also lease the I2S0 FIFO request route against other clients.

Platform acquisition can return ready, busy, pending or retained recovery.
Pending acquisition preserves partial ownership in `OPENING`; only ready
acquisition presents a fresh reset controller. Retained recovery preserves
all tokens and prohibits further hardware access. Open and close take absolute
monotonic microsecond deadlines, separate from DMA capture ticks. The
[I2S0 platform binding](i2s-platform.md) implements pin ownership and a bounded
16 kHz audio-clock profile with explicit shared PLL leases. The source clock controller identifies module register offset
`0x1200`, its divider at bits 0–4, mux at bits 24–26, and gate at bit 31.
Bus gating and reset use offset `0x120c`, bits 0 and 16 respectively. Parent
choices include the audio PLL outputs and the 200 MHz peripheral PLL output.
These facts guide the platform callback; the capture core writes none of them.

Open verifies that global receive/transmit operation and interrupt requests are
inactive before programming the receive format. It preserves the reset FIFO
threshold while selecting RXOM 1. Start enables the module, requests an RX FIFO
flush, and clears the receive counter. Bounded polls wait for flush completion.
The flush-bit clear observation is a qualification requirement; the source
reference writes the flush bit but does not establish its observed timing.

Before receive starts, the DMA controller validates descriptor storage, RAM
mappings, FIFO access, cache ownership, and overlap constraints. The capture
component requires every segment to read the fixed I2S0 RX FIFO through request
3 with an eight-access burst. Destination access width and burst must match;
segment lengths must contain whole captured frames. The controller arms DMA
before I2S enables reception and RX DMA requests.

Each open permits one finite capture. The caller retains immutable transfer
metadata and exclusively leased allocations until `a733_i2s_owns` is false.
The DMA controller's ownership predicate alone is insufficient while I2S still
holds a buffer. The component allocates no memory and creates no task.

Completion, cancellation, FIFO errors, and deadlines use this stop order:

1. Disable RX DMA requests and verify the register value.
2. Disable reception and verify the register value, retaining module clocking.
3. Stop or finish the DMA chain through the existing controller. Poll until it
   proves the channel disabled and outstanding transactions drained.
4. Disable the I2S module and verify the register value, then release buffer
   ownership. Close releases the DMA channel and finally the platform lease.

Active polling reads the channel's pending-event register without acknowledging
it. This allows I2S to stop producing requests before the DMA controller's poll
can stop a channel or return buffers. Half and package events do not terminate
the chain. Queue-end, fault, or deadline observations begin the bounded stop.

Preparation and capture share a deadline in the DMA controller's monotonic tick
units. DMA drain uses the controller's separate stop deadline. Each call performs
a bounded amount of work; callers schedule subsequent polls. Timeouts report a
failure and initiate shutdown; elapsed time never proves that buffers are safe.

An uncertain register access, failed readback, changed DMA routing, or unproven
DMA drain enters `RECOVERY`. The component retains outstanding buffer and
resource ownership and provides no forced release or automatic retry. Recovery
requires independent hardware reset and platform reconciliation. A platform
release failure also retains its lease. Synchronous MMIO itself can stall,
so independent hardware-watchdog protection remains mandatory.

`COMPLETE` means the finite DMA chain completed and stopped safely. It does not
prove a microphone was present or that the captured values are valid audio.
`FAILED` and `CANCELED` allow reuse only after the same shutdown proof; their
buffers contain no accepted audio result. Initial RX error status must be clear.
The core observes overrun and underrun flags without assuming undocumented
status-clear semantics; each subsequent open requires platform reset again.

## Focused evidence and integration requirements

The combined host fixture links the actual I2S and DMA controller implementations.
It checks finite completion with delayed drain, raw-word preservation, lease
conflicts, invalid FIFO routing and mappings, flush timeout, cancellation,
receive errors, DMA faults, deadline exhaustion, and retained uncertain stops.
Its register model asserts that the producer stops before DMA disable or CPU
buffer return. Address and undefined-behavior sanitizers passed.

The reference check extracts unmodified functions from the pinned audio driver
and compares accepted and refused clock ratios, wire timing, I2S delay, channel
counts, and reversed RX mapping-register order. It passed for 135 supported
plans and 1,602 refused ratios. Sample-resolution and DMA-width choices also
have direct source evidence; physical FIFO packing remains unqualified.
The evidence directory is `~/.cache/sunstoneos/evidence/i2s-20261016/`.

Reproduce only affected checks when their relevant inputs change:

```sh
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iinclude checks/i2s.c drivers/audio/a733_i2s.c \
  drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c -o /tmp/i2s-check
/tmp/i2s-check
python3 checks/i2s-reference.py \
  --source-dir "$HOME/.cache/sunstoneos/sources/i2s" \
  --build-dir "$HOME/.cache/sunstoneos/evidence/i2s-reference"
```

The next implementation step is the platform ownership adapter and integration
with FreeRTOS scheduling and buffer allocation. Physical testing requires
qualified independent recovery, exclusive coordinator ownership, and an
identified external source or microphone. Clock, pin, FIFO, signed-sample,
DMA-coherence, and sustained-capture evidence remains absent. No physical test
proposal is executable yet, and no performance measurements are needed now.

## Pinned source provenance

The implementation uses register and routing facts from board-kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Downloaded source remains outside Git.
The reference checker verifies source hashes before extracting functions and
retains their original license notices in its temporary compilation unit.

| Source | Relevant facts | SHA-256 |
| --- | --- | --- |
| [Audio controller](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/sound/platform/snd_sunxi_i2s.c) | Generic 16-slot compatible profile; format, RX mapping, FIFO preparation, start/stop, burst 8 | `d3446ec1e11a3fd69d53161f5612a0155e43391ee14569f1fecf942d7daec3a3` |
| [Register definitions](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/sound/platform/snd_sunxi_i2s.h) | Offsets and field positions | `f5eeda49997321150059b1c87cb016c7e2f3ed2e68a9c0abe3d0f5349b9b2a02` |
| [A733 clock setup](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/sound/platform/platforms/snd_sun60iw2_i2s.c) | Shared parents, module clock and reset dependencies | `0dc8d0c0912fced3dac450ffd98f5694bf0146e7608c922e6a562e6e7ed795d5` |
| [PCM DMA configuration](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/sound/common/snd_sunxi_pcm.c) | Matching source/destination bus width | `ce06fa49f38ccba2185030754548a41420e1e82ddc36e9ff326fc694a5cd64d6` |
| [PCM DMA width helper](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/sound/core/pcm_dmaengine.c) | 16-bit containers use two bytes; 32-bit containers use four | `017b8aca7246ffdf786f9e20a38f3e9c2b62f1f6115cfc6e23d20052ba999620` |

Board routing, request IDs, alternate pin functions, and CCU offsets come from
`arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi`, `bsp/drivers/pinctrl/pinctrl-sun60iw2.c`,
and `bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c` at the same revision. The cached
source manifest records downloaded input identities. Schematic provenance is
maintained in the [board wiring record](board-wiring.md).
