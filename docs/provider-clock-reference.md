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
