# Bind the NPU register pass to the fabric after EL3 entry

Observed in startup ELF `89921f9c2958…`: the retained first failure at
`a733_npu_startup.c:337` is CCU `0x02002588`, value `0x81000000`.
That word enables MBUS, selects PERI0_600M and divides by one. The submitted
session instead names `0x84000000` and 400 MHz, captured before entry.
The separate retained reason 9 means the client lost liveness while waiting
for platform readiness; it is not the sequencer's numeric stage.

Observed: the EL3 monitor authenticates and seals the session, then calls
`a733_el3_mbus_restore()` before EL1. The restore changes the ROM's exact
400 MHz state to boot0's 600 MHz state. The sealed session is not rewritten.
Session preparation now applies the existing `tools/vendor-mbus.py` rule to
both the expected word and rate. Evidence retains the original capture and
labels the post-entry expectation separately. The native exact comparison
still refuses if normalization is skipped or the fabric changes.

The source comparison identifies no additional missing clock, reset or
firmware operation in this stage. Later register values absent from the
capture remain unknown. Host models qualify the implementation's guards,
not those physical values.

## The complete reference sequence and its native counterpart

Observed vendor kernel: Orange Pi revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, with standard CCU and PCK600
domains enabled. The NPU register pass itself performs reads only; setup
writes belong to the earlier entry/resource owners or later local opening.

| Operation | Vendor operation, order and readiness | SunstoneOS counterpart |
| --- | --- | --- |
| Shared fabric | NSI driver selects PERI0_600M, sets MBUS and NSI to 600 MHz, releases resets and enables gates. Selected mux/divider helpers OR update bit 27 into each RMW, with no explicit polling in those helpers. | EL3 restores only the ROM MBUS state using the six-step SRAM sequence below. Existing AHB/NSI configuration is retained and checked. |
| Supply | Board NPU supply is DCDC2. Probe enables the regulator; a voltage write requires the optional `npu-setvol` property. Bin 4 selects 852 MHz/800 mV. | PMIC provider verifies DCDC2 at 800 mV; no DCDC2 selector write. |
| Domain 4 | PCK initialization writes delay offsets `+0x170=0x1f1f1f`, `+0x174=0x1f1f`, `+0xc00=0x08080808`, `+0xc04=0x808`, `+0xc10=8`. ON writes policy nibble 8, DSB, then polls status nibble 8 within 10 ms. Existing ON is adopted. | Existing power owner implements these operations; PASS checks coherent OFF/ON before requesting ON. |
| Master gates | NPU clock init enables AHB master bit 6 at CCU `+0x5c0`, then MBUS master bit 18 at `+0x5e0`. | Shared owner enables its leased bits before domain access. Unowned fields are preserved. |
| PLL_NPU | Disabled PLL: output off, LDO on, PLL on, lock detection on, LOCK qualification, output on. Rate change: output off, factors, lock detection off/on, LOCK qualification, output on. | Existing bounded enable/retune/adoption under exclusive ownership. Factors can be prepared while the local module remains reset and gated. |
| LOCK | Three consecutive samples, 3 µs apart, at most 100 samples; 20 µs settle. | Existing `a733_pll_lock_wait` implements the same qualification. |
| Local opening | Probe releases core reset early. Clock init releases AHB, AXI and core resets; enables AHB master; sets PLL/module rates and parent; enables MBUS master, bus gate, then module gate. Device initialization waits 2 ms. | Domain ON and PLL LOCK precede SRAM routing under held resets, local reset release, module selection, bus/module gates and the existing 2 ms settle. |
| Firmware | A733 `check_smc_set_freq` selects a gear without an SMC. No applicable NPU/MBUS mailbox request occurs in the selected clock/reset/domain path. | No firmware request is missing. SCP's message-11 handshake belongs to suspend/resume. |

Source anchors: vendor `vip_drv_device_platform.c:226–358,655–790,1273–1387`;
`ccu_gate.c:157–202,224–240`; `ccu_nm.c:250–344`;
`ccu_common.c:47–66`; `ccu_div.h:142–162`;
`sunxi-nsi.c:1066–1125,1431–1465`;
`pck600_domains.c:104–154`. The cached sources and complete file links are
recorded in the build's `fabric-vendor-review/review.md`.

## The SRAM window already supplies every MBUS update handshake

Observed in `boot/a733/mbus-window.S`: all writes use DSB SY and the routine
uses registers only, executing from SRAM at `0x5f000`.

1. Clear MBUS gate bit 31, set update bit 27; poll update clear.
2. Clear MSI_LITE0 reset-release bit 17 at `0x02002594`; wait 10 µs.
3. Clear MBUS divider bits 4:0, set update; wait 10 µs; poll update clear.
4. Select MBUS source 1, set update; wait 10 µs; poll update clear.
5. Restore the original MSI_LITE0 word.
6. Set MBUS gate and update; poll update clear. Verify the final masked
   word is `0x81000000` and MSI_LITE0 is released.

Every poll has a 240,000-counter-tick bound, or 10 ms at 24 MHz. A timeout
restores the bridge and gate before returning its failed step. Clocked GPU
or NPU, busy DMA0 or a loaded CE task skips the window without writes.
Exact native admission still rejects an expected 600 MHz record if that
skip leaves the bus at 400 MHz.

Observed binary references: `physical-rom.bin`, SHA-256 prefix
`a8e31821e8a1611d`, routine `0x2084–0x215c`, performs this sequence with
source 4. `boot0_sdcard_orangepizero3w.fex`, SHA-256 prefix
`829ea8e3b65d71a0`, routine `0x57a4c`, selects source 1 at
`0x57b1e–0x57b28`. Its surrounding DRAM/NSI resets and PLL_DDR programming
belong to cold initialization and are not copied into live startup.
`scp.fex`, SHA-256 prefix `07e6b97628101963`, selects source 1 during DRAM resume
at file offsets `0x93fa–0x9408`; its later message-11 handshake is at
`0x6930–0x696e`. None establishes a missing ordinary NPU-start handshake.
Full hashes, filenames and disassembly references are in
`fabric-firmware-review/binary-provenance.json` and `source-provenance.json`
in the build directory.

## Every native admission sub-step has a retained marker

Observed implementation: each marker precedes the sub-step's reads. All
previous predicates, masks, ordering and failure records remain in force.

| Marker | Sub-step |
| --- | --- |
| `0x0630` | Domain-4 coherent policy/status and register ownership |
| `0x0631` | Oscillator and PLL_REF |
| `0x0632` | Shared SRAM still owned by CPU |
| `0x0633`, `0x0634`, `0x0635` | AHB, NSI, MBUS words and stable-update state |
| `0x0636` | PERI0 control, modulation and gate/status words |
| `0x0637` | Selected PERI0 output; values 0, 1, 2 identify fabric entries |
| `0x0638`, `0x0639`, `0x063a` | Video encoder, decoder and bus/reset guards |
| `0x063b`, `0x063c` | NPU module and bus/reset guards |
| `0x063d`, `0x063e` | DRAM clock and PHY reset guards |
| `0x063f` | Automatic clock-management modes |
| `0x0640`, `0x0641`, `0x0642` | PLL_NPU profile/bin, gates, tuning/pattern guards |
| `0x0643` | Register pass complete |

Later controller OPEN requires the completed pass and platform startup.
The client readiness marker may interleave with bootstrap markers. A clean
refusal can subsequently write retained reason 9; the separate first-failure
record keeps the original register, full value and source location.
