# NPU clock and reset tables

The A733 NPU's local clock and reset tables and the apply-with-readback helper
are statics of the [startup sequencer](../drivers/npu/a733_npu_startup.c),
beside its [PLL table](npu-pll.md). The
[platform module](../drivers/npu/a733_npu_platform.c) holds the fabric-record
validation, which the sequencer and the session validators share; it has no
state machine, takes no lock and makes no resource request. The
[header](../include/a733_npu_platform.h) also defines the result codes and the
register callbacks of the sequencer.

Status: host-modeled and checked in the linked images under Unicorn. Physical
startup, inference and recovery are unqualified. The initial operating point
is 492 MHz at 800 mV, as listed in the pinned board kernel. That profile is a
functional bring-up choice and not a performance target.

## Resources and their owners

The pinned kernel and the A733 User Manual define these resources:

| Resource | Control | Owner and startup behaviour |
| --- | --- | --- |
| NPU module | CCU `0x02002000 + 0xb00`; divider bits 0–4, selector bits 24–26, gate bit 31 | Sequencer: requires the gate closed; the open table selects PLL_NPU at divider 1 and opens the gate |
| NPU bus and resets | CCU `+0xb04`; bus gate bit 0, core reset bit 16, AXI reset bit 17, AHB reset bit 18 | Sequencer: requires gated and reset-asserted; releases AHB, AXI and core, then enables the bus |
| NPU SRAM reset | CCU `+0xb04` bit 19 | Requires released; never written |
| PLL_NPU | CCU `+0x2a0`, gate control `+0x1920`, gate status `+0x19a0` | Sequencer: requires disabled and enables it by method 2, or adopts it as an earlier start left it ([npu-pll.md](npu-pll.md)) |
| Shared SRAM routing | SYSCTRL `0x03000008` bit 1 | Sequencer: requires set (routed to the CPU), clears it while the NPU resets are asserted and sets it again at local stop ([routing design](designs/npu-sram-route.md)) |
| NPU clock-management mode | CCU `+0x1b1c` bit 0 | Requires disabled |
| PCK clock | R_CCU `0x07010000 + 0x1ac` bit 0 | The image's GPU/PCK manager; the sequencer holds a stable lease |
| NPU power domain | PCK `0x07060000 + 0x4000` | [Power-domain owner](power-domains.md): the sequencer requests ON through its row and reads policy and status |
| Supply | AXP8191 DCDC2 | Power-domain owner verifies 800 mV; the sequencer holds a shared rail token and reads the authority word |
| Interrupt | SPI 65, GIC interrupt 97, level high | The image's interrupt binding |

The module selector supports PLL_NPU, PERI0 outputs at 800, 600 and 480 MHz,
VE0, VE1 and DE. The open table selects PLL_NPU only.

PLL_NPU also appears in the source selectors for MBUS (`+0x588`, selection 5),
video encoder (`+0xa80`, selection 6), video decoder (`+0xa88`, selection 6)
and DRAM0 (`+0xc00`, selection 4). The sequencer's register pass refuses an
active consumer, but software cannot exclude every writer of these words. The
session record's clock evidence must bind that exclusion. An idle NPU cannot
establish memory or interconnect quiescence.

## Fabric record and clock scopes

`a733_npu_platform_scopes` validates the fabric record: the exact AHB, NSI
and MBUS selector, divisor and enable words and their integer rates. Each
selector must name the 24 MHz system clock or a PERI0 output; the rate must
equal the parent rate divided by the divisor. The function returns the shared
clock scopes the NPU needs: fabric, reference and oscillator, plus PERI0 when
a fabric clock selects it. It refuses DDR, DE or NPU fabric selections and
leaves the output unchanged on refusal.

## Open and stop tables

A table entry names an address, a field mask, the field's value, and the
read-only status bits that are written as zero and ignored on readback.
The sequencer's apply helper reads each word, skips a field that already
holds its value, writes the new word and reads it back; consecutive entries on
one word reuse the value last read. It stops at the first entry that fails.

The open stage runs after domain 4 is ON and PLL_NPU has locked:

1. PLL_NPU output gate (`+0x2a0` bit 27), with LOCK as status;
2. the shared SRAM routed to the NPU: SYSCTRL `0x03000008` bit 1 cleared and
   read back, while the resets are still asserted;
3. AHB reset released (`+0xb04` bit 18);
4. AXI reset released (bit 17);
5. core reset released (bit 16);
6. module selector PLL_NPU and divider 1 (`+0xb00` bits 24–26 and 0–4);
7. bus gate (`+0xb04` bit 0);
8. module gate (`+0xb00` bit 31).

Steps 3 to 8 are the local open table, in the order of `npu_clk_init` in the
pinned VIPLite `vip_drv_device_platform.c` (lines 265–359). Domain 4 is ON
before step 1, as the board's vendor kernel powers it before its probe.

The stop table follows `npu_clk_uninit` in the pinned VIPLite platform:
module gate off, bus gate off, then the AHB, AXI and core resets asserted.
The sequencer then routes the shared SRAM back to the CPU. PLL_NPU, the SRAM
reset and domain 4 stay as they are.

## Native register adapter

The [native adapter](../drivers/npu/a733_npu_platform_native.c) is the only
register path of the sequencer. It reads the reference and PERI0 words, the
fabric words, the PLL_NPU consumers, the NPU module and bus words, the
PLL_NPU words, the clock-management modes, domain-4 policy and status
(`PCK+0x0`, `PCK+0x8`) and the shared SRAM routing word. It writes five
words, each under field rules:

- `+0xb00`: only the selector, divider and gate; selector PLL_NPU; divider 1,
  2 or 4; the gate may close without other changes;
- `+0xb04`: only the bus gate and the AHB, AXI and core resets;
- `+0x1920`: only the software enable and No-Auto bits, which never clear;
- `+0x2a0`: never LOCK, never disables the PLL or its LDO, changes factors
  only to `0x00305100` with the output closed, and opens the output only with
  the PLL, LDO, lock enable and LOCK set;
- SYSCTRL `0x03000008`: only bit 1, and only while `+0xb04` holds the AHB,
  AXI and core resets asserted and the bus gate closed, and the module gate
  at `+0xb00` is closed.

Every PCK600 write is refused. The caller holds the register lock for every
write. Device mappings and EL1 access to the CCU are prerequisites of the
image. A physical bus stall can prevent a callback from returning; only an
independent watchdog covers that failure.

## Shared SRAM

The manual maps shared SRAM at CPU addresses `0x00074000`–`0x000f3fff` and
requires the NPU power domain to stay on while that SRAM is in use. Bit 1 of
SYSCTRL `0x03000008` routes it to the CPU (set, as the boot ROM leaves it) or
to the NPU (clear). With the CPU routing the board's NPU produced wrong
outputs; with the NPU routing they matched the reference
([routing design](designs/npu-sram-route.md#problem-and-reference)). Startup
evidence must therefore exclude inherited CPU and DMA users of this memory,
and later allocations must keep the domain on for their lifetime.

## Command admission stays separate

The startup path loads no device firmware. It sets registers, and the
controller then runs its native bootstrap commands. READY does not admit a
command, broaden model admission, or prove device execution, correct
inference, bus drain or safe shutdown. The compiled convolution keeps its
[fixture-specific preparation limits](nbg-conv-loader.md).

## Sources and checks

The source repository is
[the board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f),
pinned to `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The operating point comes
from
[`sun60iw2p1-cpu-vf.dtsi`](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/arch/arm64/boot/dts/allwinner/sun60iw2p1-cpu-vf.dtsi),
SHA-256 `d5535630db3d0acdeace832d1dfad684fab9ab0c31c482b493e36a7f3387e5ad`,
which assigns 800 mV to 492 MHz for every listed NPU speed bin. The primary
reference is *A733 User Manual V1.00*, sections 2, 4.1.6.6, 4.1.6.56,
4.1.6.120, 4.1.6.249, 4.1.6.255, 4.1.6.264, 4.1.6.271 and 5.3. Its PDF SHA-256
is `cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`; the
shared source lock names it `a733-user-manual-1.00`.
`checks/npu-platform-reference.py` extracts the clock and reset fields,
domain timings, operating-point values and startup order from the cached
pinned files.

The tables and the adapter run in `checks/npu-startup-host.c` and
`checks/npu-startup.c` through the sequencer, and in the linked images through
`checks/npu-application-clock-reference.py` and `checks/npu-stop-native.py`
([npu-startup.md](npu-startup.md#checks)).
