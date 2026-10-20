# Normalized clock admission in TWI, USB2, radio and CE

The generic TWI11 adapter, USB2 host platform, radio platform and crypto engine
validate a 24 MHz **reference output**, rather than requiring a 24 MHz raw board
oscillator. On the 26 MHz board, PLL_REF can supply that output with N=96 and
M=104. The providers now use the existing `a733_clock_ref24_valid()` decoder;
they do not retune the oscillator, PLL_REF or PERI0.

The previous guards required DCXO selector 0 or 3 and equal reference multiplier
and divider. That rejected a correctly normalized reference on the 26 MHz board.
The replacement requires the actual oscillator's enabled/stable bits, enabled
and locked PLL_REF, supported N range, and exact integer equation
`DCXO_Hz × N = 24,000,000 × M`. Rate arithmetic downstream of PLL_REF remains
based on 24 MHz. Shared clock leases, register locks, pin/rail authority, expiry
and retained-failure behavior remain required.

## Sources and selected parents

The A733 User Manual V1.00 is pinned in `sources.lock.json` at revision
`d0dc4d615be00b76e3a0818d0818eff8f69ffa15`, PDF SHA-256
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
The cached text has SHA-256
`219d3c21e5377d18a096765f429555d0693f7fa9b2ae2eb5bb3f225d8b6a09c2`.
Section 9.1.7.16 defines DCXO detection and enable/stability bits; sections 4.1.4.1,
4.1.6.1 and table 4-3 describe the normalized reference and its factors. The pinned
Linux `ccu-sun60iw2.c` at revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` has SHA-256
`1d32bb8555f8ff205d35210f638f9d42cc28aa9295c46963aa1c810d47b06d77`.
Its `pll_ref_clk` factors and fixed `pll_peri0_clk` parent agree with this path.

| Provider | Selected path and retained arithmetic | Authority |
| --- | --- | --- |
| `drivers/i2c/a733_twi_native.c` | Fixed TWI11 uses APB1 source 0 SYS_CLK24M or source 3 PERI0_600M, divided by its admitted APB1 divider. Existing standard-mode TWI timing uses the admitted APB1 rate. | Manual §§4.1.6.68, 4.1.6.158; Linux APB1 parent table |
| `drivers/usb/a733_usb2_platform.c` | USB2 reference and MF select source 0 SYS_CLK24M; suspend selects source 1 SYS_CLK24M. The enabled pipe path uses PERI0_480M divided by four. Existing DWC clock constants therefore remain based on 24 MHz. | Manual §§4.1.6.199–201, 4.1.6.204; Linux `usb_ref_parents`, `usb2_suspend_parents`, `usb2_mf_parents`, `usb2_u2_pipe_parents` |
| `drivers/net/a733_radio_platform.c` | Fabric sources use SYS_CLK24M or admitted PERI0 branches. SMHC1 explicitly selects source 0 SYS_CLK24M with M=30/N=1, retaining its 800 kHz input. The module's independent RF clock is not reconfigured. | Manual §4.1.6.130 and §11.3.3; Linux `smhc_parents` |
| `drivers/crypto/a733_ce.c` | CE selects PERI0_400M. PERI0 retains its fixed PLL_REF parent and existing divider/gate checks. TRNG separately retains its RC16M lease and enable-bit requirement; `ce_clk2m_rc` is RC16M/8, not raw DCXO/8. | Manual §§4.1.6.6, 4.1.6.117, 9.1.7.16 and 19.1.3; Linux `ce_parents` |

TWI11 is not the always-on S_TWI1 controller used by Type-C. APBS1 source 0 is
raw DCXO; source 4 is normalized PLL_REF. This correction changes neither the
S_TWI1 adapter nor its 26 MHz timing. It also does not widen USB selectors to
accept raw DCXO with unchanged 24 MHz constants.

## Focused software evidence

`checks/provider-clock-reference.py` compiles the actual changed clock-check
function bodies against the actual public structures with address and
undefined-behavior sanitizers. TWI uses the complete native prepare function;
USB2, radio and CE use their parent-admission functions. The check accepts valid
24, 26 and 19.2 MHz normalization, rejects invalid factors and lost enable/lock
state, covers selected TWI/radio PERI0 paths, and preserves CE's separate
TRNG RC16M requirement. Register reads and existing lock, pin, time and ownership
dependencies are modeled. This is not whole-provider or native-startup execution.

Evidence is under `provider-clock-reference-20261020`. Fixture setup repairs
and previously completed cases are retained; only affected or unexecuted cases
resume. Astra's focused source review found no remaining parent/rate mismatch.
No application image was rebuilt or old frozen evidence relabeled. The source
checks establish configured-rate admission, not measured board frequencies,
electrical readiness, radio operation, USB transfers, camera operation or CE
execution. Existing component and physical-admission requirements still apply;
see [camera platform](camera-platform.md), [USB2 host](usb2-host.md),
[radio platform](radio-platform.md) and [crypto engine](crypto-engine.md).

## I2S, PWM and HDMI follow-up

The I2S AUDIO1 reference and PWM profile guards use the same decoder. AUDIO1
has a fixed PLL_REF parent in the pinned Linux `pll_audio1_clk` definition;
manual §4.1.6.51 and table 4-3 define the existing integer plan. Its 24 MHz
reference arithmetic is unchanged. No audio sample format, divider, DMA or
buffer-lifetime behavior changes.

PWM0 channel 5 continues to select SYS_CLK24M. Its APB0 profile accepts SYS24M
or PERI0_600M; the finite-pulse and fan timing still use their existing 24 MHz
basis. Manual §§18.7.3 and 18.7.5.1 distinguish these clocks from raw DCXO.
The optional LED's APBS0 route needs a separate check: manual §4.2.5.2 and
Linux `r_apbs_parents` identify source 0 as raw DCXO and source 4 as normalized
PLL_REF. Source 0 therefore retains the raw-24 MHz restriction required by the
existing LED plan. On a 26 MHz board, that LED configuration is refused; source
4 can use the existing normalized 24 MHz plan. This patch does not silently
reinterpret a caller's fixed 24 MHz APBS0 rate as 26 MHz.

The HDMI native restriction is deliberately unchanged. Its `CCU_PREPARE` stage
selects HDMI_SFR source 1 at CCU offset 0x1690, which manual §4.1.6.232 and Linux
`hdmi_sfr_parents` identify as raw DCXO. The native binding reserves OSC and
FABRIC, without a REF lease. The S_PPU register at 0x070101ac is only a gate
(manual §4.2.5.17); it does not establish a 24 MHz source. Replacing the baseline
restriction with normalized-reference validation would leave that raw-SFR
consumer inconsistent. Supporting 26 MHz requires a separate complete audit of
HDMI's selected SFR source, parent leases and actual consumer behavior, with
appropriate affected native evidence.

The added cases are selected without replaying the earlier four providers:

```sh
python checks/provider-clock-reference.py --providers i2s pwm --output evidence
```

Both pass with address and undefined-behavior sanitizers against the actual
`reference()` and `profile()` function bodies and public structures. The cases
cover valid oscillator encodings, malformed reference/enable state, PWM PERI0
rate mismatch, LED source-0 refusal at 26/19.2 MHz, source-4 acceptance and APBS0
readback mismatch. Astra's delta source review found no remaining mismatch.
Evidence is under `audio-pwm-hdmi-clock-20261020`. Earlier provider evidence
remains under its original frozen inputs. No HDMI, whole-image, startup or
physical checks were executed for this follow-up.
