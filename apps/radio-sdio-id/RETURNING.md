# Reusable radio discovery requires fitted wiring and a disposal proof

The existing [identity application](README.md) already connects the native
single-core returning framework, shared providers, SDC1 card enumeration and
AIC8800 transport. It returns through the framework while retaining the radio
resources after successful identity work. That result requires power removal
before another attempt. A second application would duplicate the constructor
without resolving reuse.

This design is for native radio maintainers. Reusable discovery remains blocked;
no image or physical execution is admitted by this document.

## Enumerate the card before starting a module producer

Extend the existing application with an explicit discovery mode. Stop at the
platform's `A733_RADIO_READY`, before borrowing the card or constructing the
AIC8800 transport. Preserve the existing one-bit 400 kHz enumeration sequence:
CMD5 operating-condition negotiation, CMD3 relative-address assignment, CMD7
selection, and CMD52 reads of the common control registers, function information
and card information tuples. Report the operating-condition response, relative
address, register revisions, capabilities and each function's manufacturer/device
identity. These are SDIO identities; they do not establish silicon subrevision.

Discovery must issue no CMD53 transfer, firmware load or bootstrap register
write. The existing transport identity operation writes `0x40500058 = 0x40`;
its completion cannot substitute for passive card discovery or producer drain.
No access-point credentials or RF operation are needed.

Reuse the existing absolute deadline of 1.5 seconds after monitor entry and its
two-second monitor backstop. Preserve the card parser's bounds of 4,096 bytes
and 128 tuples per function, with at most eight common/function records.
Each byte requires a CMD52 transaction, so the existing one-millisecond yields
cannot guarantee complete discovery of a maximally sized valid card description
within that deadline. A timeout must retain ownership. Startup and discovery
latency have no physical measurement.

## Successful discovery must complete disposal before ROM return

A success-only disposal path needs an explicit source comparison and focused
checks before implementation. Closing application admission and retaining a
buffer are insufficient. The path must establish all of these conditions:

- No card borrower, transport, firmware or Bluetooth producer was started.
  The last command response has been consumed and no command remains pending.
- Command START is clear, the card is not busy, and controller error status is
  clear. DMA was disabled throughout the admitted controller lifetime.
- The module's selected state and inherited reset/oscillator condition permit
  the proposed card deselection and pin restoration. Controller idleness alone
  does not establish that module condition.
- The source-defined card-clock stop completes before local clock/reset and
  pin restoration. Restore only the owned SDC1 masks and admitted initial pin
  state, then release provider ownership and use the existing ROM return.

`sunxi_sdio_stop` changes software state only. It neither stops the card clock
nor drains the controller. The current card and radio platform APIs
provide no disposal operation. Their retained state must not be cleared to make
another generation appear fresh. Any uncertain observation or failed disposal
keeps the complete lifetime retained under its existing recovery requirement.

## Published sources leave fitted-board admission unresolved

The inspected official build is pinned at
`a95129bf30ea11886cc35f08ebb3e69695a26656`. Its Zero 3W configuration selects
`sun60i-a733-orangepi-zero3w.dtb`, `aic8800_fdrv` and `aic8800_btlpm`.
The selected A733 kernel configuration enables the AIC SDIO interface and
`CONFIG_AW_RFKILL`. The pinned kernel reference is
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.

| Source fact | Consequence for admission |
| --- | --- |
| Schematic V1.2 sheet 6 supplies Wi-Fi VBAT from DCDC1 at 3.3 V and global I/O from DCDC8 at 1.8 V; sheets 9 and 14 connect PG, PM and module I/O to that supply | The selected board DTS instead names BLDO5 and CLDO1 for PG/PM. A fitted-board routing record must identify the actual supplies. |
| Sheet 14 draws PM2 through zero-ohm R61 to module `BT_RST_N` and zero-ohm R1130 to `LPO` | The drawing does not establish fitted resistor population or the electrical role of PM2. A DTS active-low reset declaration cannot resolve that ambiguity. |
| The selected SDC1 node and its base node contain no `mmc-pwrseq` property | Startup comes through the rfkill/BSP helpers; a generic MMC power-sequence binding supplies no missing module reset/clock contract here. |
| The Wi-Fi helper waits 10 ms before driving PM1 high; the AIC BSP waits another 50 ms before rescan | These source intervals support the current startup sequence. They do not prove inherited PM2 suitability or successful enumeration. |
| The Bluetooth helper initializes its active-low PM2 reset asserted; its power-on path drives PM2 high after enabling supplies and waiting 10 ms | These Linux operations do not establish a ROM/DDR launch's inherited output state or a safe fitted-module clock/reset transition. |
| BSP power-off calls the Wi-Fi helper, waits 100 ms and rescans; the helper calls regulator disable for its named supplies | Actual rail shutdown also depends on regulator constraints and other consumers. This is not a proven disposal sequence for this diagnostic. |

## Enabled drivers consume the supply arrays

The selected configuration sets `CONFIG_AW_BSP=y`, which makes the AIC BSP
and Wi-Fi Makefiles define `CONFIG_PLATFORM_ALLWINNER`. The AIC top-level
Makefile includes the BSP for `CONFIG_AIC_WLAN_SUPPORT=y`; its SDIO selection
includes `aicsdio.c`. Wi-Fi module startup calls
`aicbsp_set_subsys(AIC_WIFI, AIC_PWR_ON)`, reaching the BSP power-on path.
Its references to the rfkill helpers are direct extern calls, not weak stubs.

`CONFIG_AW_RFKILL=y` links `sunxi-wlan.c`, `sunxi-bluetooth.c` and the rfkill
parent. Parent probe calls both initialization helpers. The Wi-Fi helper reads
`wlan_power` and `wlan_power_vol`; Bluetooth reads `bt_power` and
`bt_power_vol`. Each calls `regulator_get` with each supplied name and fails
initialization if lookup fails. Power-on requests the supplied voltage and
regulator enable. The exported Wi-Fi power call returns early if its helper
never initialized; successful driver binding cannot be assumed from configuration.

`CONFIG_REGULATOR=y`, `CONFIG_AW_MFD_AXP2101=y` and
`CONFIG_AW_REGULATOR_AXP2101=y` enable the real provider implementation.
The regulator driver selects its AXP8191 descriptors from the detected PMIC
variant and registers BLDO5 and CLDO1. The regulator core falls back to lookup
by regulator name, matching the DTS's `axp8191-bldo5` and `axp8191-cldo1` names.
These arrays therefore reach executable consumers when probe succeeds; they
are not unused annotations. This source trace does not establish which PMIC
variant was detected on either physical board.

The DTS marks CLDO1 `regulator-always-on`; BLDO5 lacks that constraint.
The regulator core disables hardware only for the last user when the
always-on constraint permits it. Calling regulator disable does not prove
CLDO1 shut down or establish the fitted routing of either output.

The MMC host separately requests optional `vmmc`, `vqmmc`, `vdmmc`,
`vdmmc33sw`, `vdmmc18sw`, `vqmmc33sw` and `vqmmc18sw` supplies.
The selected SDC1 nodes provide none of those properties. They do not consume
the rfkill supply arrays. The selected Bluetooth low-power driver controls
PM3/PM4 wake and UART RTS; it does not parse `bt_power` or establish PM2
reset/LPO authority. The additional consumer references, their exact pinned
bytes and license status are recorded in [sources.json](sources.json).

## Physical observations must resolve routing and powered state

Before another native radio attempt, obtain the fitted board revision and
module marking, the R61/R1130 population or an equivalent verified module
pinout, and the actual PG/PM/module-I/O supply connections. Bind the existing
read-only PMIC snapshots and authenticated pin handoff to that board's current
powered lifetime. Voltage readback alone cannot establish connectivity or the
PM2 net's function. Keep these observations separate from a later bounded
SDIO enumeration result.

The initial discovery clock remains 400 kHz, with PG0–PG5 on SDC1 function 2
and fixed 1.8 V I/O. Existing provider checks observe inherited supplies and
clock parents; no PMIC voltage or enable writes are permitted. Neither microSD
nor USB supply may be cycled or voltage-changed. SDC1 at `0x04021000` is the
radio controller; SDC0 is not part of this design.

A runnable implementation therefore needs fitted routing and resistor-population
evidence, the current lifetime's PM2/reset/oscillator authority, and a reviewed
success-only drain/restoration sequence for that module state. Source inspection
cannot manufacture those physical facts. The existing producer's refusal remains
correct while they are absent.

## Frozen source evidence supports the blocker

The source audit is preserved outside Git at
`~/.cache/sunstoneos/evidence/radio-returning-20261028/source-audit/`.
Its manifest verifies fourteen pinned source files against the existing source
records and preserves eight native input files. Eight additional consumer files
were compared byte-for-byte against their pinned upstream URLs. The manifest SHA-256 is
`99e3ddaa776c348835d23801cd9a868c5e2ec8fa09c8c8e4188a5fb57e5707d9`.
The original source URLs and revisions are recorded with each verified digest.
The existing [platform source record](../../docs/radio-platform.md) and
[radio source table](../../docs/radio.md#source-and-artifact-identities) retain
the source provenance and licensing context.

This audit changes documentation only. Existing native constructor and producer
checks remain applicable to their unchanged inputs. No new native build, board
operation, timing measurement or physical qualification is claimed.
