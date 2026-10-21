# I2S0 receive and transmit streams are software-tested; physical audio is unqualified

I2S0 is the external digital-audio controller on the 40-pin header. Its native
support has three layers:

- The format planner in [a733_i2s.h](../include/a733_i2s.h) checks a requested
  format and computes the controller register values, clock divisors, pin set and
  DMA access width. It performs no I/O.
- The [stream driver](i2s-stream.md) programs I2S0 from that plan and moves
  audio through the native [DMA controller](dma.md). It supports finite receive,
  transmit and simultaneous receive/transmit periods, hardware-linked repeated
  playback, checked-copy repeated capture, and finite playback with an explicit
  final period. The [DMA service](dma-service.md) runs it in one serialized
  worker.
- The [platform binding](i2s-platform.md) leases the I2S0 clocks, reset and pins,
  and the [native application](i2s-application.md) connects the service to DMA0
  startup, linker-owned memory and USB observation.

Status: implemented and software-tested in host models and two-core emulator
compositions. No microphone or playback device is attached, and no I2S0 audio has
been captured or played on the board. The remaining requirements are:

- Repeated capture needs the device ordering contract described in
  [i2s-stream.md](i2s-stream.md) before physical admission.
- Period handoff timing, FIFO continuity, cache visibility and final serial-wire
  drain need physical qualification. Cancellation can truncate queued output.
- A microphone and a playback device must be selected, wired and qualified.
- External clock input, wider TDM frames, Bluetooth PCM, wire timestamps and
  power transitions are not implemented.

## The source defines the external route and its conflicts

The [board wiring record](board-wiring.md) identifies schematic sheets 14 and 18.
The pinned device tree assigns I2S0 registers at `0x02532000`, a `0xa0`-byte
window, and DMA0 request 3 for receive and transmit. The RX FIFO is at offset
`0x10`. The pin controller assigns function 3 to each selected pin below.
These are source-defined routes; electrical operation remains unqualified.

| Signal | Header pin | SoC pin | Ownership |
| --- | --- | --- | --- |
| Bit clock, BCLK | 12 | PB5 | Required output |
| Frame clock, LRCK | 35 | PB6 | Required output |
| Data input, DIN0 | 38 | PB8 | Required input for capture |
| Data output, DOUT0 | 40 | PB7 | Required output for playback |
| Master clock, MCLK | 7 | PB4 | Optional output; omitted when `mclk_hz` is zero |

PB7 on header pin 40 provides DOUT0 at function 3 and DIN1 at function 4.
Capture leaves PB7 unselected; playback selects DOUT0 on PB7 and leaves PB8
unselected, and simultaneous operation selects both. The platform must lease
PB5, PB6 and the selected data pins exclusively, and PB4 only when MCLK is
requested. The pin lease
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
wire slots, and one or two channels. A container must fit its wire slot.
Mono capture selects wire slot 0 or 1; mono playback uses slot 0. Stereo uses
slot 0 followed by slot 1.
The wire always has two slots, even for mono. Normal polarity, MSB-first order,
and the I2S one-bit data delay are fixed.

The module clock must be a known, stable platform lease. The bit clock is
`rate_hz × 2 × slot_bits`. Both bit-clock and optional MCLK divisions must be
exact integer divisions supported by the controller. Supported divisors are
1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 176, and 192. The accepted
sample-rate range of 8–192 kHz is a software policy; it is not a measured limit.

For example, a leased 24.576 MHz module clock produces 16 kHz audio with two
32-bit slots at a 1.024 MHz bit clock, using divisor 24. Optional 12.288 MHz
MCLK uses divisor 2. The planner does not select a PLL or change a shared clock.

| Sample container | FIFO and DMA configuration | Stored result |
| --- | --- | --- |
| 16 bits | Sample-resolution code 3, RXOM 1, TXIM 1, two-byte accesses | Raw 16-bit word, little-endian memory |
| 32 bits | Sample-resolution code 7, RXOM 1, TXIM 1, four-byte accesses | Raw 32-bit word, little-endian memory |

The driver performs no arithmetic conversion or implicit sign extension.
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

## Platform clock facts guide the binding

Platform acquisition can return ready, busy, pending or retained recovery.
Pending acquisition preserves partial ownership; only ready acquisition presents
a fresh reset controller. Retained recovery preserves all tokens and prohibits
further hardware access. Resource deadlines are absolute monotonic microseconds,
separate from DMA period ticks. The [I2S0 platform binding](i2s-platform.md)
implements pin ownership and a bounded 16 kHz audio-clock profile with explicit
shared PLL leases. The source clock controller identifies module register offset
`0x1200`, its divider at bits 0–4, mux at bits 24–26, and gate at bit 31.
Bus gating and reset use offset `0x120c`, bits 0 and 16 respectively. Parent
choices include the audio PLL outputs and the 200 MHz peripheral PLL output.
The stream driver writes none of these clock registers.

The register sequence, DMA ownership and stop order are specified in
[i2s-stream.md](i2s-stream.md). The stream driver disables its DMA request before
any DMA stop, keeps every allocation until the DMA controller proves drain, and
retains all resources after an uncertain register access or failed drain.

## Focused evidence

The host fixture `checks/i2s.c` links the actual stream driver, format planner
and DMA controller. It checks period completion with delayed drain, raw-word
preservation for 16-bit and 32-bit containers, pending, busy, retained and late
platform operations, invalid catalogues, a refused memory mapping, flush timeout,
cancellation, receive overrun, DMA faults, DMA deadline exhaustion, lost DMA
routing and retained uncertain stops. Its register model asserts that the DMA
request is off whenever a DMA channel starts or stops and whenever the CPU
acquires a captured period. Address and undefined-behavior sanitizers pass.

The reference check extracts unmodified functions from the pinned audio driver
and compares accepted and refused clock ratios, wire timing, I2S delay, channel
counts, and reversed RX mapping-register order. It passes for 135 supported
plans and 1,602 refused ratios. Sample-resolution and DMA-width choices also
have direct source evidence; physical FIFO packing remains unqualified.

Reproduce only affected checks when their relevant inputs change:

```sh
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iinclude checks/i2s.c drivers/audio/a733_i2s_stream.c drivers/audio/a733_i2s.c \
  drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c -o /tmp/i2s-check
/tmp/i2s-check
python3 checks/i2s-reference.py \
  --source-dir "$HOME/.cache/sunstoneos/sources/i2s" \
  --build-dir "$HOME/.cache/sunstoneos/evidence/i2s-reference"
```

The two-core service compositions and the native application evidence are
described in [i2s-stream.md](i2s-stream.md) and
[i2s-application.md](i2s-application.md).

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
