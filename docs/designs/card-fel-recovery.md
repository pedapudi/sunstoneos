# A retained first-stage request selects FEL before card loading

**Status: request consumption and FEL re-entry qualified from SRAM.** The gate
has executing host checks, and no card is written. One physical run on port
3-3 executed the gate in SRAM through the ROM's FEL execute command. It
consumed the RTC request, recorded its final progress, left WDT0 re-armed and
reached a new FEL instance 0.537 seconds after the execute request. Card
loading, recovery after a watchdog reset through a card and cold boot remain
unqualified ([evidence](../evidence/card-fel-recovery.jsonl)).

## The first stage consumes the vendor request

RTC general-purpose register 2 is `0x07090108`. The vendor FEL request is
`0x5aa5a55a`. U-Boot revision `b791be842935b27268ae3d00e943a9075495f30a`
defines that value and index in `arch/arm/mach-sunxi/rtc.c`. The selected
sun60iw2 headers place the RTC at `0x07090000` and its general-purpose words
at offset `0x100`. `sunxi_board_run_fel()` sets the request with barriers and
readback before cleanup and reset.

The pinned boot0 SHA-256
`829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3`
tests the request in `0x4a990`, reached from main at `0x5e1ae`. Its matching
path clears only index 2 through `0x4aa08`, completes cache cleanup and branches
to ARM ROM `0x20` through `0x48ec6`. The helper's initial loop reads and prints
eight RTC words; it does not clear them.

The early SunstoneOS gate applies the same request before boot0's board setup,
DRAM initialization or storage access. The gate is stackless and
position-independent AArch32 code
([source](../../boot/a733/card-fel-gate.S)). A clear request continues at
boot0's original reset stub, `0x47b00`, and writes no RTC word. The matching
request requires translation and data caches off and the known clear ROM
redirect words. The gate then disables and invalidates the instruction cache,
checks a recognized watchdog state and applies its WDT0 policy (below) with
readback. It then clears the request with barriers and readback, invalidates
the branch predictor and branches to ARM ROM `0x20`. A policy readback refusal
keeps the request pending and parks with WDT0 still armed. An unconfirmed
clear enters FEL with a failure code. The gate changes no voltage, clock or
secondary power policy.

The WDT0 policy is the 32-bit word that immediately follows the gate code; the
code loads it PC-relative, so every profile runs identical code bytes. Zero
disarms WDT0: the card first stage appends zero, because FEL must not reset
under a watchdog. A nonzero value is a WDT0 mode that the gate writes with the
key and then restarts through the control register (A733 manual §8.3.6.4,
key `0xa57`). The FEL proof uses `0xb1`: enabled, 16 seconds, the largest
interval (§8.3.6.6). Before a nonzero policy, or with WDT0 already enabled,
the gate requires system-reset configuration and reset output.

On the requested path the gate records its progress in the RTC LAST word,
`0x07090104`, as stage `0x0c02` in the high half and a value in the low half
([codes](../../include/a733_card_fel_gate.h)). Each store is followed by a DSB.
The gate leaves the fault, ESR, ELR and FAR words unchanged.

| Value | Meaning |
| --- | --- |
| `1` | Request matched; gate entered |
| `2` | Watchdog mode, configuration and output checked |
| `3` | WDT0 policy applied and read back (mode zero for the card, `0xb1` for the proof) |
| `4` | Request cleared and read back as zero |
| `5` | Before the branch to ROM `0x20` |
| `0x81` | Refused: MMU or data cache enabled |
| `0x82` | Refused: a ROM redirect word is nonzero |
| `0x83` | Refused: unrecognized watchdog mode, configuration or output |
| `0x84` | Request readback nonzero after the clear; FEL entered anyway |
| `0x86` | Refused: WDT0 mode readback differs from the policy |

The captured ROM's entry at `0x20` bypasses BOOT sampling but still checks the
second software redirect at `0x070501d4`. All redirect words therefore remain
zero. The ROM consumes no RTC request itself; the gate makes the selection.

## A warm reset cannot enter the gate

The A733 manual places CPU_SUBSYS_CTRL at `0x08000000` (§5.1.5).
Its CPU0 registers have separate execution-state and reset-address roles:

| Register | Address | Documented role |
| --- | --- | --- |
| CLU0_CPU0_CTRL_REG | `0x08001000` | Bit 0 selects AA64NAA32 at processor reset (§5.1.6.6) |
| CLU0_RVBARADDR0_L | `0x08001004` | Low reset-address bits for AArch64 execution (§5.1.6.7) |
| CLU0_RVBARADDR0_H | `0x08001008` | High reset-address bits for AArch64 execution (§5.1.6.8) |

The Cortex-A55 TRM §B2.93 defines `RVBAR_EL3` as read-only, derived from the
RVBARADDR inputs and describing AArch64 reset entry. The AArch32 reset vector
instead depends on `SCTLR.V` and VINITHI (§B1.81). The pinned vendor BL31, the
TF-A A733 port and SunstoneOS's `cpu_start.c` all select AArch64 before using
these registers. An AArch32 warm reset therefore cannot be directed to a gate
in DRAM, and the RAM proof uses no reset.

## The RAM proof executes the gate from SRAM

On a card, the ROM loads the first stage into SRAM and runs it in AArch32 with
the MMU off. The ROM's FEL execute command does the same for an uploaded image.
A measured SRAM probe executed through that command at `0x52000` ran in
AArch32 supervisor mode with SCTLR `0x00c51838`: MMU and data cache off,
instruction cache on ([SRAM probe result](../boot.md#sram-probe-result)).
The proof therefore uploads the gate to `0x52000` and executes it there.

The proof ELF ([sources](../../apps/card-fel-recovery/)) contains two read-only
loads. The first holds the exact gate code at `0x52000` followed by the policy
word `0xb1`. The second is a 96-byte profile at `0x52400`. The profile names
its version, the gate address and code length, the RTC stage `0x0c02`, the
final progress value 5, the WDT0 policy and the code's SHA-256. The runner's
admission ([tools/card_recovery.py](../../tools/card_recovery.py)) requires an
AArch32 executable entered at `0x52000`, loads only inside
`0x52000..0x52d80`, code matching the profile hash, and a policy word equal to
the profile's `0xb1`. The packaged card first stage contains the same code
followed by policy zero. The profile has no storage, DRAM, clock or rail
authority, and the runner initializes no DRAM.

The runner sequence is as follows:

1. Reset the board to a fresh FEL instance and upload both loads with the
   tool's `write` command.
2. Clear the crumb words and arm WDT0 in mode `0x41` (a four-second interval).
3. Write the session tag to `0x07090100` and the request to `0x07090108`, and
   read both back.
4. Execute `0x52000`, ending that transaction after at most 2 seconds.
5. Wait for a new FEL instance, read the request word, then read WDT0, disarm
   it and read the crumb words in one command.

The `write` command leaves the instruction cache enabled. The reviewed FEL
tool, built with the A733 `icache_fix` patch, runs a helper before every
register read or write command: it clears `SCTLR.I`, invalidates the
instruction cache and returns. The request-arming command in step 3 is the
last command before the execute command, so instruction fetch from the
uploaded gate starts with the cache off. The gate also clears `SCTLR.I` and
invalidates the instruction cache itself. The runner refuses any FEL binary
other than the reviewed one and runs a frozen copy.

The gate's branch to `0x20` re-enters the ROM from inside the execute command.
On the board the ROM sent that command's status before running the code: the
transaction completed normally in 0.013 seconds. If the status is lost to the
disconnect instead, the transaction fails or the runner ends it after
2 seconds. In the captured ROM,
the paths from `0x20` and from the reset vector at `0x44` meet at the same FEL
routine (`0x2514`, called with `r0 = 0x7f`). Both then run the same clock and
USB initialization and the FEL loop at `0x1061c`. The last run of the earlier
DRAM-based recovery image (queue entry `1794485680312692366-e77f3e34`)
measures that path: after an AArch32 warm reset with USB still configured, the
next command reached a new instance 1.79 seconds after the launch, and WDT0
still read `0x41`, so no watchdog reset had occurred. The runner therefore
accepts a normal completion and also treats a failed execute transaction, or
one it ends at 2 seconds, as the expected disconnect. It accepts that only for this execute command, after the
command has started, with the watchdog and request armed. It sends no further
request to the old instance and waits for a new one.

The proof policy keeps the board recoverable when the ROM does not
re-enumerate. WDT0 then expires 16 seconds after gate entry and resets the
board to FEL, the path every returning image takes. The RTC words survive that
reset. The runner tells the two outcomes apart by the WDT0 mode it reads first
and by the time from the execute request to the new instance:

| Outcome | WDT0 mode read first | Instance after execute | Runner result |
| --- | --- | --- | --- |
| ROM re-enumerated | `0xb1`, still counting | under 16 s | `returned`; WDT0 disarmed over FEL |
| No re-enumeration | `1`, the reset remnant | 16 s or more | `watchdog-reset`; proof failed |

Acceptance requires the first row together with the request zero, LAST
`0x0c020005`, the matching session tag, no fault word and WDT0 mode zero after
the runner's disarm. Any other receipt fails the proof; a normal return with an
invalid receipt quarantines the instance. The ROM's FEL entry clears SRAM
`0x44000..0xf4000`, so the runner reads no SRAM after the return.

The host checks execute the actual gate code at `0x52000` and at the card
location `0x83000` with both policies, the instruction cache on and off, and
the packaged card sector (`checks/card-fel-recovery.py`). They cover each
refusal, both readback failures, omitted stores, the cold path and the tool's
cache helper. `checks/board-runner.py` models the execute disconnect, the
bounded transaction, the ordering of uploads, arming and execution, both
outcomes in the table and each invalid receipt.

The session budget is 60 seconds, with `finish_by` 55 seconds and a 3-second
native progress window. The worst-case allocations below take their bases from
the run named above.

| Stage | Re-enumerated (s) | Not re-enumerated (s) | Basis |
| --- | --- | --- | --- |
| Preparation and reset to fresh FEL | 15 | 15 | Measured 6.0 |
| Upload | 1 | 1 | Measured 0.01 per load |
| WDT0 and request arming | 0.5 | 0.5 | Measured 0.05 |
| Gate execution | under 0.001 | under 0.001 | About 120 instructions |
| Execute transaction end | 2 | inside the WDT0 interval | Runner limit |
| WDT0 interval from gate entry | none | 16 | Policy `0xb1` |
| ROM to a usable new instance | 5 | 5 | Measured 1.79 after re-entry, 1.9 after a WDT0 reset |
| Capture and WDT0 disarm | 1 | 1 | Measured 0.03 |
| Total | 24.5 | 38.5 | |

The re-enumerated path leaves 30.5 seconds before `finish_by`. The other leaves
16.5 seconds before `finish_by` and 21.5 seconds in the session. In the
re-enumerated path the runner must disarm WDT0 before the 16-second interval
ends: execute end, enumeration and capture take at most 8 seconds, a margin of
8 seconds. The initial `0x41` interval covers arming and execution, about
0.05 seconds of 4 seconds. The gate records its last progress within
microseconds, well inside the 3-second window. These are test budgets, not
measured cold-boot latency.

## Integration after the proof

Autonomous card boot is permitted only after a confirmed cold power-on. Every
warm reset, including watchdog expiry, must enter FEL unless the OS explicitly
requests a card reboot. That request must authorize one boot and be consumed
before the OS starts. An absent FEL request does not authorize card boot.
Without an explicit reboot request, an unknown reset cause must select FEL,
so a hung OS cannot create a card-boot loop.

The reset-cause predicate remains unresolved. The pinned A733 manual's PRCM
register inventory (§4.2.4) and RTC register inventory (§9.1.6) identify no
retained power-on-versus-warm reset cause. RTC `0x07090160[13]` reports the
current reset level (§9.1.7.16); `0x070901f4[3]` enables the POR reset output
(§9.1.7.21). Neither records the preceding reset. WDT `0x02050004[0]` is an
interval-interrupt pending bit, and RTC `0x07090304` is ROM software progress;
[the reset audit](../watchdog-reset-evidence.md) explains those exclusions.

The pinned U-Boot's `arch/arm/mach-sunxi/rtc.c` implements software boot requests,
and `board_sun60iw2.c` issues the watchdog software-reset command without
reading a cause latch.
Its AXP8191 driver decodes PMIC register `0x50[2:0]` as supply-rise, button and
IRQ power sources. The inspected code does not establish that field's retention
or clearing semantics, or that it distinguishes an SoC warm reset from power-on.
The source identities are in [the recovery inventory](../vendor-firmware/data/card-fel-recovery.json).
Installation requires an A733-specific reset indication, its retention and
clearing rules, and an audit of ROM consumption before the gate reads it.
The current request-only RAM proof does not implement this cold-only policy.

The card runner must set the request before execution that could reset through
a bootable card. A card-aware OS must set it before an expected watchdog stop
and verify the write before parking. Its admission and renewal predicates must
name that recovery policy and the exact request value. Existing zero-request
contracts remain distinct. The RAM proof does not implement or qualify those
OS contract changes.

The early gate is packaged with the pinned boot0 by changing only its initial
branch, header length/checksum and appended gate sector. The normal path keeps
the vendor setup and loader. Its stock package location remains unchanged;
the FAT32 loader and below-sector-2048 installation are subsequent work.
The gate occupies 472 bytes of code and a 4-byte policy word; the packaged first stage is 246,272 bytes
(sectors 16..496). The current installer's 480-sector boot0 range ends at
495 and does not authorize that extra sector. No installation plan is emitted.

The captured ROM admits the new aligned length `0x3c200` under its
`0xa7800` limit. Its conditional checksum path does not establish universal
authentication of appended bytes; the host producer independently checks the
pinned original and the complete derived checksum. The source identities and
offsets are in [the recovery inventory](../vendor-firmware/data/card-fel-recovery.json).

RTC retention requires the manual's RTC supply condition, `VDD_RTC > 0.7 V`.
The RAM proof establishes request consumption and direct FEL entry. It does
not establish watchdog-reset-to-card execution, power-loss retention, recovery
from an inaccessible fabric, or autonomous cold boot. Each remains a separate
physical qualification requirement.
