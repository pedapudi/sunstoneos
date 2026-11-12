# A retained first-stage request selects FEL before card loading

**Status: implemented and host-tested.** No card is written.
The RAM proof requires a dedicated queue profile; the current daemon does not
admit it. Physical cold-card recovery remains unqualified.

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
DRAM initialization or storage access. A clear request continues at boot0's
original reset stub, `0x47b00`. The matching request requires translation and
data caches off and the known clear ROM redirect words. It clears the request
with barriers and readback after disarming a recognized watchdog and checking
mode zero. Cold ROM enables the instruction cache; the requested path disables
and invalidates it before the final branch-predictor cleanup and ARM ROM entry.
A disarm refusal keeps the request pending and parks. An unconfirmed clear
after disarm enters FEL without a success state. The gate changes no voltage,
clock or secondary power policy.

The captured ROM's entry at `0x20` bypasses BOOT sampling but still checks the
second software redirect at `0x070501d4`. All redirect words therefore remain
zero. The ROM consumes no RTC request itself; the gate makes the selection.
The existing CPU-local transition supplies AArch32 entry from an AArch64
caller: CPU0 RVBAR, DSB, `RMR_EL3 = 2`, ISB. The AArch32 gate then branches
to `0x20` itself.

## The RAM proof executes the actual gate

The queue profile admits one immutable ELF containing an AArch64 launcher,
the exact position-independent AArch32 gate and a fresh nonce request. It
checks ELF permissions and fixed reservations, the gate hash and its two
mutable observation words. It grants no storage access.

After arming WDT0 in mode `0x41`, the runner sets and reads back the RTC request
before launch. The launcher checks EL3, cache-off entry, the nonce, watchdog,
request and acknowledged OFF secondaries with clear start requests. It changes
no secondary policy. It publishes its report, enables the gate's RAM-only
observation word and resets CPU0 into the gate at `0x41001000`.

Only the matching gate path disarms WDT0, clears the request, records completion
and branches to ROM. In observation mode it also records `RETURN_ROM` with mode
zero; the production gate preserves the preceding RTC stop/failure record.
The runner requires a distinct usable FEL instance, no retained fault, matching
nonce/report, consumed gate state, unchanged gate code, request zero and mode
zero. A watchdog reset or any mismatch fails the proof and ends board access.
There is no generic probe return epilogue after the gate.

After primary EL3/core/cache admission, a launcher refusal records stage
`0x0c01`: values 2, 3, 4, 5 and 7 identify request framing, watchdog state,
the retained request, redirects and observation publication. Values `0x100+c`,
`0x200+c`, `0x300+c` and `0x400+c` identify core `c` status, start request,
policy and policy acknowledgment. Gate refusal values are `0x80+r`, where
`r=0,1,2,3,6` identifies a missing request, local entry, redirects, watchdog
contract or disarm readback. Those gate crumbs exist only in observation mode.
The ordinary gate preserves earlier fault and stop records.

The session budget is 60 seconds, with `finish_by` 55 seconds and a 3-second
native progress window. The worked allocation is preparation 25 seconds,
upload 5 seconds, request arming 0.25 seconds, native execution 0.05 seconds,
ROM enumeration 10 seconds and capture 2 seconds: 42.30 seconds total.
That leaves 12.70 seconds before `finish_by` and 17.70 seconds in the session.
Arming plus native execution consumes 0.30 seconds of the 4-second watchdog
interval; native progress consumes at most 0.05 seconds of its 3-second window.
These are test budgets, not measured cold-boot latency.

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
The gate occupies 424 bytes; the packaged first stage is 246,272 bytes
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
