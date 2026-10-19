# Shared runtime PMIC observations

The runtime PMIC provider owns the always-on I2C controller and PL0/PL1 once
for the platform lifetime. Clients submit bounded rail snapshots to its
designated task and retrieve copied results. Independent review, focused host
and source checks, and native compilation passed. A returning physical test
also completes provider initialization, one DCDC1 snapshot, request reap,
client-lease release and idle-state checks through the production native adapter.
DCDC1 is enabled with selector `0x17`, decoded as a programmed 3.3 V. Inherited
pin and I2C settings match after the transaction; no regulator data is written.
Evidence is `pmic-provider-probe-20261018/physical/runtime/session.json`.
This result covers one task and one rail under the returning monitor. Other
rails, concurrent clients and accelerator-application startup remain unqualified.
Supervised execution still requires reviewed entry and recovery conditions.

The implementation consists of [the provider API](../include/a733_pmic.h),
[transaction state machine](../drivers/power/a733_pmic.c) and
[native adapter](../drivers/power/a733_pmic_native.c). It uses the actual
shared clock registry, pin manager, rail ledger and platform I2C mutex.
There is no callback that substitutes a rail-read result or bus readiness.

## Controller ownership precedes transactions

The provider reserves exclusive S_TWI0 ownership and stable APBS0, APBS1,
reference and oscillator ownership. APBS0 clocks the always-on GPIO block that
contains PL0/PL1; APBS1 clocks the I2C controller. Before GPIO or I2C access,
the provider reads APBS0 at `0x0701000c` and accepts source0/DCXO or
source4/SYS_CLK24M with any encoded divider from 1 through 32. It accepts APBS1
source0 or source4 with divisor field zero. When either bus uses source4, a read
of PLL_REF at `0x02002000` must show bits31:27 set, N between 80 and 116,
and `DCXO_Hz × N = 24,000,000 × M`. The inherited oscillator must be enabled
and stable. Source0 uses its decoded 19.2, 24 or 26 MHz rate; source4 uses the
normalized 24 MHz reference. The enabled bus must be idle with both lines
released. Exact integer cross-products check the inherited TWI denominator
against 10–400 kHz using the APBS1 parent, independently of the APBS0 divider.
The first baseline records both buses' selector/divisor bits and the TWI divisor.
When either bus uses source0, it also records the decoded DCXO rate. Later
checks refuse changes to those values, including a supported source change.
These are programmed clock
settings; electrical timing is not measured.

Before the pin manager changes muxes, the provider checks the legacy engine,
enhanced driver, DMA enables, FIFOs, autonomous PMU devices and interrupt
state. GIC interrupt 235 must be masked, inactive and not pending. It then
releases the I2C mutex, acquires PL0/PL1 through the actual pin manager, and
rechecks the bus. Register and I2C mutexes are never nested. Pin configuration
is supplied as exact inherited mux, pull and drive fields; only mux 2 is
admitted. The pin manager briefly switches through disabled mux even when
the requested configuration matches the inherited configuration.

Privileged boot admission must exclude legacy software, secure/remote firmware,
DMA and interrupt handlers for the full provider lifetime. It must establish
Device mappings, pin voltage, pullups and independent recovery. Register
readbacks and numeric handoff identities do not establish these facts.
There is no cold bus reset, clock retuning, recovery pulse or teardown API.

## Snapshot ownership

The finite queue contains eight copied requests. Each request has an exact
provider identity, owner and monotonically allocated request ID. Its rail
ledger ticket stays at a fixed address and prevents the parent rail lease
from being released. Only an authenticated shared lease for the selected
rail permits submission. The provider revalidates the ticket before access
and publication.

One snapshot reads AXP8191 address `0x36` in this order:

| Byte | Register | Meaning |
| --- | --- | --- |
| 1 | `0xff` | Normal register page must be selected |
| 2 | `0x0e` | Chip identity must be `0x03` |
| 3 | `0x0f` | Supported revision must be `0x00` |
| 4 | Rail-specific enable register | Raw enable byte |
| 5 | Rail-specific selector register | Raw voltage selector |
| 6 | `0xff` | Page must still be normal |

The provider holds the common I2C mutex across all six byte reads. Each read
uses START, write address, register index, repeated START, read address, final
NACK and STOP. Sending the register index does not write PMIC register data.
The worker advances one bounded state per poll; it does not wait in a hardware
polling loop. A caller deadline bounds the complete snapshot, including mutex
acquisition and result publication.

A successful result contains raw bytes, enable state and a decoded programmed
voltage where supported. Reaping the result ends its rail borrow. The copied
snapshot remains meaningful only while the same parent rail lease remains
live. It proves neither physical wiring nor measured voltage or settling.
Radio must select its supply mapping from separately admitted board topology.

The provider reads DCDC1/2/4/8/9, BLDO1/2/5, CLDO1/2/3/5, ELDO6 and DC1SW2.
DCDC1/DC1SW2 decoding covers valid selectors through 28; DCDC2/4 through 87;
BLDO1/2/5 and CLDO1/2/3/5 through 29. DCDC8 and DCDC9 decoding stop at
selector 101. The pinned bootloader getter
uses a power-of-two `ALIGN` macro with a 100 mV step at selector 102 and above;
this does not establish the higher-range voltage formula. Such selectors
retain their raw bytes with `decoded=0`. Required DCDC8 selector 100 is in
the source-defined linear region and represents 1.8 V.

DCDC9 reads enable register `0x11` bit0 and selector `0x1a` bits6:0.
Selectors 0–70 decode as 500 mV plus 10 mV per step. Selectors 71–101 decode
as 1,200 mV plus 20 mV per step above selector 70, reaching 1,820 mV.
The higher branch remains undecoded; the table's declared 3.4 V maximum does
not resolve the getter ambiguity. Selector 70 represents 1.2 V.

ELDO6 reads enable register `0x23` bit3 and selector `0x3f` bits5:0.
Selectors 0–40 cover 0.5–1.5 V in 25 mV steps; selector 12 represents 0.8 V.
Selectors 41–63 remain undecoded. Raw bytes and enable state remain available
independently of voltage decoding for both outputs.

The schematic's detailed sheet 6 connects DCDC9 to `ELDO-INPUT` and U1A's
ELDOIN pin P12. It connects ELDO6 to `VDD-CPUS` and `VDD-USB`, and SWOUT2 to
the USB analog supplies. Sheet 2 gives conflicting ELDO and analog-supply
labels. USB platform admission must resolve the actual board revision and
wiring separately. A PMIC snapshot cannot select or prove that topology.

BLDO2 reads enable register `0x20` bit7 and selector `0x2b` bits4:0.
Its source-defined range is 0.5–3.4 V in 100 mV steps; selector 13 represents
1.8 V. Selectors 30 and 31 retain their raw bytes with `decoded=0` and no
programmed voltage. Enable state is decoded independently. The corrected
[PCIe integration](pcie.md) reserves BLDO1 for the PD bank, following the detailed
power sheet. Physical admission must still establish the fitted-board route;
a BLDO2 snapshot cannot establish BLDO1 stability. COMB1 analog supplies remain
separately identified and admitted.

DC1SW2 reads enable register `0x11` bit4 and the shared DCDC1 selector at
`0x12` bits4:0. Its range is 1.0–3.8 V in 100 mV steps. CLDO5 reads enable
register `0x21` bit7 and selector `0x33` bits4:0, covering 0.5–3.4 V in
100 mV steps. Enable status is independent of selector validity. Out-of-range
selectors retain their raw bytes with `decoded=0` and no programmed voltage.
The rail ledger protects DC1SW2 through its DCDC1/DC1SW alias group.

## Failure retains ownership

A queued request that expires before bus admission has no bus effects and
can be reaped. An active timeout, unexpected status, failed STOP, identity
mismatch, clock change or failed authority check retains its request and
resources. No result is published from partial reads. Arbitration loss causes
no STOP, reset or retry. The uncertain bus remains reserved for independent
recovery.

The designated provider task retains an I2C mutex it still owns. A fault
detected by a client latches provider failure immediately; the provider task
must poll again to mark its mutex retained. Clients cannot unlock another
task's mutex. A late failure observed after a proven idle bus and mutex release
retains the request and resource leases, without claiming the released mutex.

## Consumer composition

Initialize the shared managers, platform locks, native adapter and provider
before admitting clients. The adapter receives the real provider task handle
and common physical-counter clock. `a733_pmic_identity()` copies an identity
for consumer initialization; its success does not mean the bus is ready.
The provider task polls startup, and consumers use `a733_pmic_live()` before
submitting snapshots. They retain their exact rail tokens, wait for copied
complete results, check identity/enable/voltage, and reap each request.
An accepted submission returns PENDING and a nonzero request ID. Before
startup is ready, PENDING leaves the request output unchanged. Consumers must
initialize unused request storage to zero and check both status and identity.
The live check authenticates provider and clock readiness; it does not acquire
the pin-manager lock or independently revalidate pins. Each transaction checks
the private pin lease before acquiring the I2C mutex and checks the actual
mux afterward. During transfer, the unreleased lease and privileged exclusion
contract protect that route.

The existing early reader has a single-owner boot contract. Existing GPU and
NPU paths that call it cannot independently coexist with this runtime owner.
Their required migration is a separate async snapshot admission step before
resource startup, using the same still-live rail lease. The GPU manager holds
its own metadata lock while invoking its synchronous rail callback. That
callback must not validate a DCDC4 ledger borrow, because validation would
reenter the GPU lock. A future bridge must consume a previously admitted
immutable snapshot or restructure that acquisition path.

## Sources and validation scope

The controller map and register fields come from A733 User Manual 1.00,
sections 4.2.5.3, 4.2.5.16 and 18.11.8–18.11.10. Board Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` supplies the R_CCU, RPIO and
S_TWI0 routing. Bootloader revision
`b791be842935b27268ae3d00e943a9075495f30a` supplies the AXP8191 table/getter
and I2C helper definitions. These are the sources already pinned for
[the early power probe](power.md).

[The focused model](../checks/a733-pmic.c) composes actual provider, rail,
clock, GPU-guard and pin-manager code. Its reusable register backend supports
radio composition through `PMIC_MODEL_NO_MAIN`.
[The reference check](../checks/a733-pmic-reference.py) executes the original
bootloader voltage getter for supported selectors and checks I2C helper order
and status constants. It does not execute original I2C MMIO instructions.
Successful early physical reads do not qualify this new concurrent provider.

The focused batch passed on 2026-10-16. AddressSanitizer and UndefinedBehaviorSanitizer
reported no errors in actual shared-manager composition. The cases cover all
six rail snapshots, borrow retention, two client owners, stale request refusal,
autonomous/DMA/IRQ exclusion, arbitration loss, queued and active expiry,
failed STOP, aliased outputs and replacement of a caller token during a callback.
The reference getter matched all 1,268 supported raw-selector cases. Both
provider translation units compiled for AArch64 against the real FreeRTOS
headers. Native task checks and actual MMIO effects remain unexecuted.

Inputs, source snapshots, commands, executables and results are retained in
`~/.cache/sunstoneos/evidence/pmic-runtime-20261016/initial/`.
The sanitizer executable has SHA-256
`e74651afe48d8c210f6ae9c5cfc5b54f873116fceff209154f9c3e964401fb45`.
The rail dependency is commit `4ae15a55b51b4b654a6a5f3de660a67b30da6bc3`.
Its earlier ledger tests remain applicable; this batch exercises the new
provider's composition with that unchanged dependency.

[The MMC output check](../checks/a733-pmic-mmc.py) separately validates DC1SW2
and CLDO5 through the actual provider. Both snapshots share one live
DCDC1/DC1SW2/CLDO5 lease. The check verifies the six-byte register sequence,
borrow lifetime, reserved selectors and enable status. Its source comparison
matches 472 valid raw-selector cases against the original getter. The focused
sanitizer batch and changed provider-core AArch64 compilation passed; existing
provider scenarios were not repeated. Evidence is retained in
`~/.cache/sunstoneos/evidence/pmic-mmc-20261016/initial/`.

The BLDO2 addition uses the same pinned bootloader profile and unchanged
snapshot state machine. Runtime validation is pending in the composed PCIe
platform check. Its assigned cases cover the 1.8 V snapshot, disabled output,
valid range endpoints and reserved selectors. No separate provider workload is
required. This addition does not qualify physical voltage or board wiring.

DCDC9 and ELDO6 runtime validation is pending in the composed USB platform
check. Its assigned cases cover both asynchronous snapshots, enabled and
disabled outputs, supported range boundaries, the DCDC9 step transition and
undecoded selectors. That batch also compiles the actual provider; no separate
provider workload is scheduled for these additions.


## Read-only adoption includes the inherited PLL reference route

The A733 User Manual V1.00, section 4.2.5.3, pages 219–220, identifies APBS1
selector4 as SYS_CLK24M and defines the divisor as M+1. The pinned kernel's
`ccu-sun60iw2-r.c` identifies that selector as `pll-ref` in `r_apbs_parents`.
The pinned boot0 sequence at `0x49314..0x49328` selects source4 at
`0x07010010` while preserving the divisor. Adopting only source0 would refuse
this inherited configuration even when its reference is already 24 MHz.

The existing APBS1/reference/oscillator lease covers the additional route.
The provider reads PLL_REF only for source4 and does not require an enabled
PLL for source0. The native allowlist permits reading `0x02002000`; it permits
no new write. Neither shared-clock registers nor the inherited TWI divisor
are changed. Invalid reference settings or a later route change retain the
provider's leases and any held I2C mutex before transaction writes proceed.

`checks/a733-pmic-parent.c` composes the actual PMIC provider with the existing
clock, pin, rail and register model. Its focused cases cover source4 snapshot
completion, source0 without PLL_REF, changed parent routes and invalid PLL
settings. These are software checks; they do not qualify physical clock rate,
voltage, bus timing or recovery. Existing transaction and rail suites are reused.

The focused parent-adoption batch passes with AddressSanitizer and
UndefinedBehaviorSanitizer. The native PMIC adapter compiles for AArch64 against
the actual eight-core FreeRTOS headers. Existing provider, rail and source
comparison cases were not replayed. Exact source dependencies, objects,
executables and logs are retained under
`~/.cache/sunstoneos/evidence/pmic-parent-20261017/batch1/`.

The APBS0 ownership and read-only validation change passes the new actual-provider
case under `pwm-service-20261018/prerequisite/`, with modeled register values.
Unsupported sources, changed source/divider and read uncertainty retain failure
before further GPIO or I2C access. No clock or regulator writes were added.
The provider now stores its admitted APBS0 profile internally, so new native
images must compile provider, callers and fixtures against the matching layout.
Earlier frozen images and their evidence remain valid only for their own inputs.
