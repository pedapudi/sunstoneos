# Power consumers share exact rail reservations

The rail ledger provides native metadata ownership for the 39 outputs in the
AXP8191 source table. It performs no I2C transaction, voltage change, clock
operation or electrical qualification. A usable lease still requires verified
board wiring, voltage, enable state, settling time and exclusion of other
firmware. The PMIC provider separately owns S_TWI0 and its pins.

The public API is [a733_rail_leases.h](../include/a733_rail_leases.h). Private
storage is initialized once before concurrent access and remains at its original
address for platform lifetime. The ledger uses a nonblocking atomic lock,
128 reservation slots and 64 snapshot tickets. Capacity exhaustion refuses
admission without reclaiming another owner's resources.

## Output ownership includes shared voltage selectors

The pinned U-Boot revision `b791be842935b27268ae3d00e943a9075495f30a`,
`drivers/sunxi_power/pmu_axp8191.c`, defines nine DCDC, six ALDO, five BLDO,
five CLDO, six DLDO, six ELDO and two DC1SW outputs. Their enum values follow
that table. A rail mask uses `A733_RAIL_BIT(enum_value)`.

DC1SW1 and DC1SW2 use DCDC1's voltage selector. Reserving any of these three
outputs therefore reserves all three in the same mode. Exclusive ownership
wins if shared and exclusive requests expand to the same alias group. This
conservative rule also excludes concurrent switch-enable changes. Other output
enable bits share PMIC bytes; the provider must serialize read-modify-write
operations and preserve every unowned bit.

DCDC2 retains its existing common clock-registry authority. The ledger obtains
its own shared or exclusive `A733_POWER_DCDC2` guard. DCDC4 retains its existing
GPU resource-manager authority. The ledger obtains a metadata-only RAIL guard
in that same manager. Existing stable GPU leases conflict with a new exclusive
DCDC4 request, and new stable guards exclude existing GPU change requests.

GPU metadata guards use separate slots 16–31. Existing fixed GPU client IDs
remain 0–15, and conflict detection examines both sets. At most 16 DCDC4
bridge guards can coexist; additional requests remain pending within their
admission deadlines. Guard admission performs
no GPU readiness or PMIC readback. Ordinary GPU release APIs cannot discard a
bridge guard. The GPU manager's private layout changes, so all consumers must
be compiled against the same header.

These guards establish ownership against participating software. They do not
make legacy direct I2C readers compatible with a new provider that exclusively
owns the controller. Provider integration must replace or serialize those
readers through the actual bus owner before concurrent operation.

## Admission publishes authority only after both guards exist

`a733_rail_acquire` reserves a slot and returns `PENDING` with its exact token.
The token records a fresh generation, trusted owner and expanded masks.
`a733_rail_poll` obtains required DCDC2 and DCDC4 guards and checks their actual
identities. It returns `READY` only after the final deadline check. Pending
reservations exclude conflicting ledger requests but grant no device access.

A conflict in either legacy manager leaves admission pending. Partial admission
keeps its first guard. Expiry or uncertainty after reservation retains the
reservation and all remaining guards. A terminal ledger fault prevents new
admission, release and device use; it does not reclaim ownership.

The timer uses the same provider and context as the common registry. It reports
monotonic microseconds. Metadata lock ordering is ledger, then common registry
or GPU manager. No callback may reenter the ledger. These calls never acquire
the register mutex, PMIC mutex or scheduler lock.

Only trusted privileged code handles these records. Exact authentication,
address checks and a sealed binding detect stale tokens, copied ledgers and
accidental rebinding. They do not provide a hostile-task memory sandbox.

## A snapshot ticket prevents release during an I2C request

The provider calls `a733_rail_borrow` with an active exact token and the selected
rail mask before publishing a request. A successful borrow writes a ticket in
private provider storage. That storage remains at the same address through
completion. A copied ticket cannot authenticate or end the borrow.

The provider validates the ticket before hardware access and before publishing
copied results. It calls `a733_rail_end_borrow` after certain completion or
cancellation before access. Duplicate completion and stale generations refuse.
The parent cannot release while a ticket exists. An uncertain active request
keeps the ticket and the provider's own hardware ownership until independent
recovery. Borrow, status and completion operations make no timer or hardware
callback and return `BUSY` rather than waiting for a metadata lock.

## Cleanup records each irreversible release

Release requires independently established consumer quiescence or cancellation
before device access. It enters `CLOSING`, where no further device operation or
snapshot may start. Repeated release calls resume metadata cleanup only.

The common DCDC2 guard is released first. Its successful removal is recorded
before attempting the GPU DCDC4 release. Each guard release has a deadline
admission check. A later failure retains only guards still held; the record
never claims ownership of an already released guard. No fallible callback
follows the final release and slot clear. A successfully released token is
stale and cannot authenticate a new generation.

## Radio and PMIC use independent local clock scopes

Common scope bits 53, 54 and 55 are SDC1, RADIO_MODULE and S_TWI0. All are
exclusive. S_TWI0 adds stable APBS1, REF and OSC ancestors. Its scope includes
the legacy and autonomous PMU/DRV engines, DMA routes, controller, IRQ and
local gate/reset. Idle legacy state alone cannot establish autonomous-engine
exclusion. PL0/PL1 and remote-firmware handoff remain separate prerequisites.

SDC1 owns its dedicated host and local clock/reset. RADIO_MODULE owns the
shared Wi-Fi/Bluetooth reset, firmware and power-sequencing lifetime. It has
no clock parent and grants no rail or pin authority. Separate SDIO and UART
consumers borrow that one module owner rather than acquire competing owners.

FABRIC includes the shared STORE_AHB and SD-monitor fields at CCU+0x5c0
bits24/29, STORE_MBUS at +0x5e0 bit30, and MSI-LITE1 at +0x59c gate0 and
reset16/17. A stable FABRIC lease permits observation. Changing these fields
requires exclusion of every storage consumer and actual selected-parent
ownership. SDC1 local authority cannot reset an active microSD/eMMC path.

The source contracts are the pinned board kernel's `ccu-sun60iw2.c`,
`ccu-sun60iw2-r.c` and `sun60iw2p1.dtsi`, plus manual sections 4.2.5.3 and
4.2.5.16 for the S_TWI source and gate/reset words. Software ownership alone
does not qualify the live bus, source rate, remote handoff or board supply.

## Focused ownership checks passed

The composed host check passed with AddressSanitizer and UndefinedBehaviorSanitizer.
It uses the actual common registry and GPU manager for cross-authority conflicts,
fixed-client coexistence, alias exclusions, borrowed snapshot lifetime, partial
cleanup, caller mutation and retained faults. The source comparison matches all
39 output identifiers and the shared DCDC1 selector. All 16 affected native
translation units compile. These checks establish metadata behavior only;
physical PMIC, rail and peripheral qualification remains pending.

Run this batch with `checks/rail-leases.py`, supplying a fresh evidence directory,
the generated FreeRTOS SMP port directory and the pinned PMIC source file. The
runner archives source identities and the executable before its one host
execution, then records the native dependency closure. Prior clock, GPU, NPU and
service execution suites are reused because their runtime behavior is unchanged.
