# I2S0 clock and pin binding

The native I2S0 binding prepares clocks, reset and header pins for a future
external microphone. It implements bounded acquisition and release, with
retained ownership after partial failure. Host checks cover resource conflicts,
register ordering and deadline handling. Physical clock timing, reset behavior,
DMA capture and microphone audio remain unqualified.

The binding accepts 16 kHz normal I2S with two 32-bit wire slots, a 24.576 MHz
module clock and optional 12.288 MHz MCLK. The [capture core](i2s.md) supports
16-bit or 32-bit containers and mono or stereo capture within that wire format.
Sensor alignment, sign, channel selection and startup requirements still need
a microphone specification. The board has no built-in microphone.

## Shared audio clocks have platform lifetime

The parent object owns a stable 24 MHz reference and AUDIO1 configuration.
The local binding owns I2S0 and its selected header pins. Closing a capture
releases local resources while preserving the parent object's clock leases.
There is no parent shutdown, retune or forced-release API. Parent storage and
its admitted tokens must remain valid for the platform lifetime.

The platform supplies an authenticated common lease service through
`a733_audio_operations`. This service is a required integration dependency;
the component does not implement a production FreeRTOS clock service.
A register mutex protects read-modify-write operations, but does not establish
clock ownership. Every clock writer, including GPU users of PLL_REF, must obey
the same lease service.

| Lease scope | Required authorization |
| --- | --- |
| Reference stability | Keep the oscillator selection and PLL_REF configuration stable, including all consumers sharing that reference |
| AUDIO1 stability | Keep the AUDIO1 multiplier, DIV5, modulation, output/enable gates and auto-gating stable; coordinate DIV2 users of the shared multiplier |
| AUDIO1 modification | Exclusively own AUDIO1, both outputs, and module/reset/gate writers for I2S0, I2S1, I2S2, I2S2 ASRC, I2S3, I2S4, OWA transmit and DMIC |
| Local I2S0 | Exclusively own its module clock, bus gate, reset, controller and FIFO DMA request route |

The `cold` initialization flag selects a preparation policy. It grants no
resource authority. Cold preparation waits for the reference and exclusive
AUDIO1 modification leases before inspecting or changing the PLL. It refuses
an enabled PLL or any enabled module gate among the listed consumers.
An existing active PLL can be adopted only when its profile already matches;
adoption acquires the AUDIO1 stability lease and performs no PLL writes.

After cold preparation, the common lease service atomically publishes the
established profile and converts exclusive AUDIO1 modification ownership into
shared stability ownership. It must publish before admitting another consumer.
A late or uncertain conversion retains both old and replacement token records.
A stable token protects the shared multiplier even when another consumer uses
DIV2. The binding preserves DIV2's divider field.

## The clock chain produces 16 kHz without inherited rate assumptions

The source-defined CCU base is `0x02002000`. The binding reads the oscillator
selection at `0x07090160`: selection codes 0 and 3 represent the 24 MHz source.
It requires enabled, locked PLL_REF with equal multiplier and divider values,
so the reference remains 24 MHz. It never programs that shared reference.

| Stage | Source-defined fields | Required value and resulting frequency |
| --- | --- | --- |
| AUDIO1 | CCU offset `0x0280`, multiplier bits 8–15, encoded as N − 1 | N = 128; 24 MHz × 128 = 3.072 GHz |
| AUDIO1 DIV5 | Same word, bits 16–18, divisor encoded minus one | Divide by 5; 614.4 MHz |
| I2S0 module | Offset `0x1200`, parent bits 24–26 and divisor bits 0–4 | Parent 2 selects DIV5; divide by 25; 24.576 MHz |
| I2S0 bit clock | Capture core divider code 8 | Divide by 24; 1.024 MHz |
| I2S0 frame clock | Two 32-bit slots | Divide bit clock by 64; 16 kHz |
| Optional MCLK | Capture core divider code 2 | Divide module clock by 2; 12.288 MHz |

The multiplier is within the source range 75–129 and the PLL range
1.8–3.096 GHz. The binding requires modulation disabled in the AUDIO1
control and pattern words. Admission conservatively checks both AUDIO1 outputs,
although I2S0 selects DIV5. At CCU offset `0x191c`, bits 1 and 2 must be set
to select No-Auto mode, and software-enable bits 17 and 18 must also be set.
Bits 17 and 18 at offset `0x199c` report the effective gate state. An inherited
active PLL requires both status bits set before adoption. These checks preserve
all unrelated fields and never write either gate register. The selected DIV5
path remains protected by its stability lease; admission of both outputs does
not reserve DIV2 for the I2S0 lifetime.

The A733 manual defines these fields in sections 4.1.6.254 and 4.1.6.263.
Section 4.1.4.6 specifies software enable, No-Auto selection, then effective
status observation. The source names ending in `-auto` do not describe the
low bits' active-high No-Auto polarity; the ordinary source gate helper sets
these bits when enabled. The pinned manual is
`~/.cache/sunstoneos/sources/camera-research/A733_User_Manual_V1.00.pdf`, SHA-256
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.

Cold preparation programs the multiplier and DIV5 while the PLL is disabled,
then enables the LDO, PLL and lock detection in source order. Polls require
three consecutive lock observations at least three microseconds apart and a
further 20-microsecond settling interval. A final lock observation precedes
output enable. Effective gate status is checked only after output enable.
Cold preparation polls pending gate propagation under the same deadline and
retains the exclusive change lease until both outputs report enabled.
Control fields are rechecked during those polls. No stable lease is published
before effective status succeeds. Every poll is bounded; expiry retains the leases and state.
These delays follow the source helper and require physical qualification.

## Local setup preserves other register fields and pins

The caller supplies the expected inherited mux, pull and drive settings for
PB5/BCLK, PB6/LRCK and PB8/DIN0. Optional MCLK adds PB4. All selected pins use
function 3. The [shared pin manager](pins.md) validates those settings and
refuses conflicts, including other alternate functions on the same pins.
PB7/DOUT0 is not selected. Bank voltage remains unchanged; pull and drive codes
require an electrical choice appropriate to the microphone and wiring.

The binding requires I2S0's module gate and bus gate disabled, with reset
asserted at CCU offset `0x120c`, bit 16 clear. After pin acquisition, it waits
at least ten microseconds with reset still asserted. It then sets the module
parent/divider, deasserts reset, enables the bus gate and enables the module.
Register updates preserve fields outside the owned masks under the common
mutex. Lease callbacks and pin-manager operations run outside that mutex.

The capture core claims an already authorized DMA0 channel after local setup.
The existing native DMA controller and memory adapter remain separate
prerequisites. A production FreeRTOS DMA service is still pending.
The binding does not configure global DMA clocks or memory mappings.

Close starts only after the capture core has disabled receive DMA requests,
stopped reception, proved DMA drain and disabled the I2S controller. The binding
then disables the module clock, asserts reset, disables the bus clock and
restores the original local clock fields. It restores the leased pin fields
before releasing local clock/controller ownership. Parent leases survive.

## Deadlines and partial failure retain ownership

Resource callbacks return `READY`, `BUSY`, `PENDING` or `RETAINED`. Initial
`BUSY` has no admitted effects. `PENDING` records ownership in `OPENING` or
`CLOSING`, and later polls continue the same transaction. A busy callback during
an admitted transaction preserves every earlier token. The core does not touch
I2S or claim DMA before acquisition reaches `READY`.

Resource deadlines are absolute, nonwrapping monotonic microseconds. They are
separate from the DMA controller's tick-based capture timeout. The core checks
time after final callbacks and each configuration access. The binding checks
time around MMIO, lock acquisition and external callbacks. Clock observations
returned to the core also enforce and advance the parent's monotonic lower
bound, including the final acquisition observation. No common mutex is
held while waiting for PLL lock or reset settling.

An uncertain register access, regressing clock or expired admitted transaction
enters retained recovery. That object performs no further hardware access,
rollback or release. Pin tokens and all prior clock tokens remain recorded.
If a callback completed release just before a late return, the saved identity
is conservative evidence; it does not prove that the common service still
holds the lease. Recovery must reconcile that identity before any reuse.
An unfinished open has no cancellation API and can reach retained recovery
at its deadline. It cannot silently discard partially acquired resources.

The native adapter permits only the required clock and I2S registers. Reference,
consumer, modulation, gate-control and gate-status registers are read-only. It checks that
clock writes preserve unowned fields and uses AArch64 device barriers.
Stable Device mappings and watchdog protection are prerequisites. A synchronous
MMIO access can still stall the bus; software deadlines alone do not establish
recovery from that failure.

## Focused checks establish host behavior

The platform fixture exercises cold startup, exact-profile adoption, all listed
consumer conflicts, shared parent retention after local close, partial pin
writes, busy locks, lock timeout, clock regression, uncertain module writes and late callbacks.
A late pin-release observation also retains the saved identity. A focused acquisition case makes the final core clock sample regress below
the parent's latest observation while remaining above the core's earlier
sample. It requires retained recovery before any DMA or I2S access. The shared
mutex model rejects nested acquisition during pin or lease callbacks.
ASan and UBSan reported no errors.

A source comparison decodes the fixture's observed clock words using field
positions and divisor conventions from the pinned CCU source. It confirms the
3.072 GHz, 614.4 MHz and 24.576 MHz chain and compares PLL enable transitions
with the source helper. The affected I2S/DMA fixture additionally checks pending
open/close, retained acquisition and late callbacks. Existing format and pin
encoding comparisons remain valid because those calculations are unchanged.

AArch64 freestanding compilation covers the capture core, platform binding
and native adapter. Native MMIO has not executed on the board. Raw evidence is
stored under `~/.cache/sunstoneos/evidence/i2s-platform-20261016/`.

The focused `--audio-gates` fixture checks inherited adoption, missing No-Auto
or software-enable bits, disabled effective gates, and delayed cold gate
propagation. Status-read failures, late status observations, lost control bits
and propagation timeout retain the parent leases and prevent publication.
The fixture requires every cold status read to follow PLL output enable and
refuses gate-register writes. This change reuses the earlier local I2S, pin,
DMA and clock-rate evidence; it does not repeat those lifecycles.
Its results and affected native objects are recorded under
`~/.cache/sunstoneos/evidence/audio-clock-admission-20261016/`.

The checks can be reproduced from the repository root with an external build
directory. Select the case affected by the change. After compilation, use
`/tmp/i2s-platform-check --audio-gates` for gate admission alone. The default
execution below covers the broader lifecycle; a clean integration does not
require either execution to repeat.

```sh
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -Iinclude checks/i2s-platform.c drivers/audio/a733_i2s_platform.c \
  drivers/audio/a733_i2s.c drivers/pinctrl/a733_pins.c \
  drivers/pinctrl/a733_pin_tables.c \
  drivers/dma/a733_dma.c drivers/dma/a733_dma_controller.c \
  -o /tmp/i2s-platform-check
/tmp/i2s-platform-check > /tmp/i2s-platform.log
python3 checks/i2s-platform-reference.py \
  ~/.cache/sunstoneos/sources/i2s-platform /tmp/i2s-platform.log
```

## Pinned sources define the initial profile

Sources come from Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The cache manifest records complete
URLs, SHA-256 hashes and copied-file provenance. Relevant files retain their
original license notices in the external cache. The component uses their
register facts; Linux is not a runtime dependency.

| Source | SHA-256 |
| --- | --- |
| [ccu_nm.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/clk/sunxi-ng/ccu_nm.c) | `105b20f6d9826c34b27ef5aa9bf200ecad1e268308cc65bd52e06e254376631f` |
| [ccu_gate.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/clk/sunxi-ng/ccu_gate.c) | `3d8c88d8b00d83b64819a88e6cacaa80eef941dd4d1c164d00238ba29054cd51` |
| [ccu_common.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/clk/sunxi-ng/ccu_common.c) | `bbeebb34c55060ea74b973713e35557927a0fa06b3d2597312620bf46ca21185` |
| [ccu_reset.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/clk/sunxi-ng/ccu_reset.c) | `d74d49ad45c05efcb834fd803fe4a134b2ad80b1e801b4d8cbda05dda61a337f` |
| [ccu-sun60iw2.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/clk/sunxi-ng/ccu-sun60iw2.c) | `1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77` |
| [ccu_mult.h](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/clk/sunxi-ng/ccu_mult.h) | `1ecf95b5e4d35807b4b8fa5dca3b454af5b0924991fcee276e730b8e528d7c79` |
| [ccu_div.h](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/clk/sunxi-ng/ccu_div.h) | `a379fcb476803bc3fda7143f79d18cfcecb4320bc9f04facbea2a5b0d7c1d17e` |
| [snd_sun60iw2_i2s.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/sound/platform/platforms/snd_sun60iw2_i2s.c) | `0dc8d0c0912fced3dac450ffd98f5694bf0146e7608c922e6a562e6e7ed795d5` |
