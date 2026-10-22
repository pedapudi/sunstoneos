# The rail ledger is the only authority for PMIC outputs

The rail ledger records which software owner holds each AXP8191 output that
the images use, and in which mode. The PMIC provider writes an output only
under an exclusive ledger token, and it writes only the CPU rails DCDC3 and
DCDC5, so no other output changes after boot. The ledger performs no I2C
transaction, voltage change, clock operation or electrical qualification. A
usable lease still requires verified board wiring, voltage, enable state,
settling time and exclusion of other firmware. The
[PMIC provider](pmic-runtime.md) separately owns S_TWI0 and its pins.

The public API is [a733_rail_leases.h](../include/a733_rail_leases.h). Private
storage is initialized once before concurrent access and stays at its address
for the platform lifetime. Status: implemented and host-modeled; no physical
rail behavior is qualified by it.

## Fifteen outputs are accepted

The ledger accepts the fifteen outputs that production code acquires: DCDC1,
DCDC2, DCDC3, DCDC4, DCDC5, DCDC8, DCDC9, BLDO1, BLDO2, BLDO5, CLDO1, CLDO2,
CLDO3, CLDO5 and ELDO6. Every other output in the pinned U-Boot table
(`drivers/sunxi_power/pmu_axp8191.c` at revision
`b791be842935b27268ae3d00e943a9075495f30a`) is refused with `INVALID`. The rail
enum keeps all 39 table entries in table order, and a rail mask uses
`A733_RAIL_BIT(enum_value)`.

DC1SW1 and DC1SW2 use DCDC1's voltage selector. They are accepted as aliases:
reserving any of the three reserves all three in the same mode, and exclusive
wins when shared and exclusive requests name the same group. Other outputs'
enable bits share PMIC bytes; the provider serializes read-modify-write
operations and preserves every unowned bit.

## Exclusive excludes every other holder

A shared holder excludes exclusive holders of its outputs, and an exclusive
holder excludes every other holder. The ledger keeps the union of exclusive
masks and a count of shared holders per output, so admission compares two masks
instead of scanning holders. An exclusive token grants a selector write only for
DCDC3 and DCDC5; on any other output it only excludes other holders.

The ledger takes no lease in the clock registry or the GPU resource manager.
The registry's `A733_POWER_DCDC2` scope and the GPU manager's DCDC4 guard
remain for their existing callers but grant no rail authority: nothing writes
DCDC2 or DCDC4.

## A token names its slot

The ledger has 32 holder slots and 10 borrow slots. The images create 31
holders; the borrows cover the provider's eight request slots, the camera's
lifetime borrow and one spare. A token identifier holds its slot index in its
low five bits and a never-reused generation above them, so status, poll,
release and borrow compare one slot. A full table returns `BUSY` and leaves the
output unchanged.

`a733_rail_acquire` admits the request and returns `PENDING` with the token;
`a733_rail_poll` returns `READY` on its first call without bus access. The
pair keeps the existing poll call sites working. Changing acquire to return
`READY` and removing the poll calls belongs to the consumers' packages.

## Borrows and domain rows pin a token

A borrow pins its parent token for one PMIC request. The ticket stays at its
original address until `a733_rail_end_borrow`, and a copy cannot authenticate or
end it. The [power-domain owner](power.md) pins a token the same way while a
domain row is counted (`a733_rail_pin` and `a733_rail_unpin`). Release returns
`BUSY` while any pin remains, so a rail cannot be released under a pending
request or a powered domain. Borrow, status and pin operations make no timer or
hardware callback.

## Faults latch in the power-authority state word

The ledger embeds the power-authority state word, one 64-byte line described in
[power.md](power.md). A ledger fault (a clock that runs backwards, a replaced
time source, a pinned parent that has disappeared, or `a733_rail_retain`) sets
`A733_POWER_LEDGER_FAULT`. All later status, borrow, release and acquire calls
then return `RETAINED`, and every reservation and borrow stays held.
Initialization also binds the clock registry to this word, so a registry fault
sets `A733_POWER_CLOCK_FAULT` there. One registry reports to one ledger.

The ledger lock is a nonblocking atomic flag; a busy ledger returns `BUSY`.
The only callback is the time source, which acquire and release call once.
Only trusted privileged code handles these records. Exact authentication and
address checks detect stale tokens and copied ledgers; they do not provide a
hostile-task memory sandbox.

## CPU rails keep a boot-baseline floor

For DCDC3 and DCDC5, `a733_rail_floor` records the first selector voltage that
the exclusive holder reads as the rail's boot baseline and returns it on every
later call. The [CPU clock driver](cpu-clock.md) never sets a target below it.
The floor stays in the ledger after the driver releases its token.

## Measured cost

`checks/rail-leases-cost.py` compiles the ledger, registry and GPU manager with
the production flags (`-march=armv8-a -mstrict-align -O2`, tuned for the
Cortex-A55) and runs each operation once under Unicorn. It counts the
instructions outside the measuring wrappers and the 64-byte lines of the ledger,
registry and GPU manager that each operation touches. The configurations hold
42 clock and 16 rail leases, then 126 clock leases and a full rail table.

| Operation | Previous ledger, 16 / full | Current ledger, 16 / full |
| --- | --- | --- |
| Rail status, one-rail token | 222 / 888 instructions; 34 / 256 lines | 37 / 37 instructions; 3 / 3 lines |
| Clock token status | 235 / 592 instructions | 45 / 45 instructions |
| Acquire, poll and release | 1,643 / 3,565 instructions | 335 / 335 instructions |
| Borrow | 823 / 1,531 instructions | 153 / 153 instructions |
| End borrow | 271 / 923 instructions | 86 / 86 instructions |
| Authority check per PMIC transaction | 498 / 1,507 instructions | 6 / 6 instructions, 1 line |
| Ledger storage | 20,096 bytes | 1,600 bytes |

The previous ledger, at merge base `d40ca4b72b42348880d89f1ebc4eaec61704b8d0`,
had 128 slots, so its full configuration holds 127 rail leases; the current
ledger holds 31. Equal costs in the two configurations show
that no operation scans holders. With `--limits` the check enforces the budgets
of `docs/designs/power-authority.md`.

## Checks

- `checks/rail-leases.c`, built with AddressSanitizer and
  UndefinedBehaviorSanitizer, covers acquire, poll and release in production
  order for all fifteen outputs with no registry or GPU lease and no bus
  access; refusal of every other output; exclusion; aliases; a full table;
  never-reused identifiers; borrow capacity; pins; and fault retention,
  including a registry fault reported in the state word.
- `checks/rail-leases.py` runs that model and compiles every affected native
  unit with its recorded flags, archiving inputs and the executable.
- `checks/rail-leases-cost.py` measures the costs above.

These checks establish metadata behavior only; physical PMIC, rail and
peripheral qualification remains pending.
