# TYPEC24 fixed USB host power

The FUSB302 and SY6280 on the Orange Pi Zero 3W control attachment and VBUS
for the exposed TYPEC24 connector. The native provider selects a fixed USB
source/data-host role at default current. It supplies a fresh attachment token
to the USB2 host controller. Physical operation remains unqualified.

The implementation uses the shared byte I2C engine on S_TWI1, the native pin
manager, common clock and rail leases, and read-only PMIC snapshots. It performs
actual controller and GPIO operations. It does not supply successful electrical
observations through application callbacks.

## Board and source constraints

The board schematic revision 1.2, sheet 11, connects FUSB302MPX at seven-bit
address `0x22` to S_TWI1 on PL12/PL13, function 3. PL9 observes its active-low,
open-drain interrupt. PL8 enables the SY6280 switch with a high output. The
switch connects VCC5V to VBUS_TYPEC; its enable has a 100 kΩ pulldown.
TYPEC16 is a separate power-input/USB0 connector.

The SY6280 data sheet revision 1.0E distinguishes two parts. SY6280 discharges
its output when disabled; SY6280A does not. Both block reverse current while
disabled. The 4.7 kΩ current-setting resistor gives a nominal limit near 1.45 A,
which does not establish the board's available power budget. The admitted image
must identify the populated part, qualify discharge and reverse blocking, and
establish a 4.75–5.50 V source capable of at least 500 mA.

The [FUSB302 data sheet](https://www.onsemi.com/download/data-sheet/pdf/fusb302-d.pdf),
revision 2, defines MEASURE register `0x04` with VBUS selection in bit 6 and the
six-bit threshold code in bits 5:0. The pinned Linux header shifts those fields
incorrectly. This implementation follows the primary data sheet. Its code-zero
VBUS threshold is nominally 420 mV; the electrical table gives no minimum or
maximum accuracy. Admission therefore requires a qualified upper threshold no
greater than the USB Type-C vSafe0V limit of 800 mV. VBUSOK alone cannot establish
that limit or an accurately regulated 5 V output.

The same data sheet defines source polling, CC pull-ups, comparator selection,
latched comparator-change status, VBUS status, and interrupt masks. The code uses
the published Linux driver's default-current CC thresholds: code 4 for Ra and
code 38 for the Rd/open boundary, with a 100 µs settling interval after a
measurement change. CC means configuration channel; Rd identifies a sink
termination, and Ra identifies a cable termination. The first profile accepts
one Rd and an open other CC pin. It excludes cables requiring VCONN cable power
and audio/debug accessories.

The A733 manual section 18.11.7 gives S_TWI1 the TWI0 register map at
`0x07084000`. Its R_CCU word `0x0701019c` uses gate bit 1 and reset bit 17.
S_TWI0's additional PMU registers are absent from the S_TWI1 map and are never
probed. The native adapter requires APBS1 to select the 24 MHz crystal oscillator
(DCXO24M) with divider 1;
it does not retune that shared parent. It observes both local gate/reset bits
clear, releases reset, enables the gate, and programs a source-derived divider
at or below 100 kHz. Legacy, autonomous driver, DMA, FIFO and IRQ conditions
must be inactive before transactions begin.

## Admission and ownership

Construct one `a733_typec_native` before scheduling. Its address, configuration,
provider tasks and shared managers remain private and immutable. The two tasks
must be distinct: one runs `a733_typec_poll`, and the other runs
`a733_typec_native_power_poll`. The PMIC provider uses its own existing task.
All three use the same monotonic counter and shared lock services.

The native path acquires exclusive S_TWI1 ownership, stable APBS1/REF/OSC,
DCDC1/DC1SW2 rail leases, and PL8/9/12/13 pin leases. The rail ledger also reserves
the aliases of DCDC1's voltage word. Actual PMIC reads must report both supplies
enabled at 3.3 V. Their requests and rail borrows stay held. Pin acquisition
requires an authenticated bank-L output-latch handoff and initially input or
disabled functions. Mapping checks precede native S_TWI1 access.

The privileged image must establish electrical qualification, Device mappings,
EL1 security access, stable pin-bank power, remote-controller exclusion and
independent recovery from a stalled interconnect. Nonzero qualification and
recovery identities refer to those image-owned facts; they do not discover or
prove them. No runnable image may invent these identities to enable the port.

## Attachment and independent shutdown

The provider first confirms the GPIO disable output, verifies the qualified FUSB302 device
identity and initializes source-only polling with VCONN and PD transmission off.
It classifies both CC pins, then requires 120 ms of a stable Rd comparator result.
During debounce it also reads the latched comparator-change interrupt, so a
change between samples restarts attachment. It rechecks the other CC pin and
measures low VBUS before requesting the GPIO task to enable the switch.

VBUS must rise within 275 ms. The provider then enables CC/VBUS and fault
interrupts and periodically reads the actual CC/VBUS and fault status. A published
token contains the provider address, owner, attachment generation, polarity and
observation time. The timestamp is conservatively anchored to the STATUS0
request, before any receive or STOP delay. Tokens expire after 10 ms. A short
atomic publication lock and permanent revocation flag protect concurrent host
snapshot/validation calls; the host never reads mutable service fields.

Both Type-C tasks must run at least once per millisecond. The GPIO task observes
PL9 and checks deadlines renewed only by real provider progress and attachment
observations. Host activity never extends them. Detach, expiry or failure closes
host admission permanently. The GPIO task lowers PL8 independently of the I2C
task and continues even when that task retains the common I2C mutex.

Ordinary metadata or pin-lock contention before effects leaves shutdown pending
for at most 5 ms. A return of `-1` revokes host admission but does not terminate
that pending power task. Actual authority loss, uncertain pin writes, clock
failure or an exhausted shutdown deadline requires independent system recovery;
software then cannot claim that VBUS is off. An already-observed low GPIO output
can acknowledge another shutdown request without another hardware write.

After successful GPIO shutdown, the I2C task attempts a bounded low-VBUS
observation if its bus remains usable. A retained bus prevents that observation,
so discharge remains uncertain even if the GPIO task confirmed PL8 low. Every
clock, rail, pin, PMIC request and bus reservation remains held. There is no
resource release, automatic reattachment or reset retry in this profile.

## Evidence and remaining integration

The focused check passed with the actual native Type-C, TWI, PMIC, pin, rail,
clock, counter, translation and lock implementations. FreeRTOS task/queue
behavior, MMIO responses and electrical qualification were modeled. Both cable
orientations completed attachment and detach. Saved execution checkpoints also
covered pre-pin authority loss, external VBUS refusal, retained I2C failure,
stale observation, invalid tokens and brief registry contention. Delayed STOP
completion preserved the observation's request-start timestamp.

The compiled native adapter passed both single-core and SMP configuration
checks. The executed trace preserved S_TWI0's shared clock/reset bits, selected
100 kHz I2C, held VCONN off, selected source/data-host role, and waited more than
120 ms before enabling power. The evidence is under
`~/.cache/sunstoneos/evidence/typec-native-20261017/`.

The initial execution reached its instruction cap during normal interrupt
configuration. It resumed from preserved CPU, memory and modeled-hardware state
on the same executable. Startup and successful cases were not replayed. The
canonical runner uses the established bounded allowance; its trace assertions
were checked against the preserved execution without another emulation run.

The check requires the pinned local toolchain, FreeRTOS sources, Python Unicorn
and pyelftools dependencies. It refuses to overwrite an evidence directory:

```sh
~/.cache/sunstoneos/venv/bin/python checks/a733-typec-native.py \
  --output /path/to/new-evidence-directory
```

The USB2 host application must pin the first token generation, snapshot and
validate before protocol progress, and retain its DMA objects on provider loss.
A host fault must not suspend either Type-C task. The usable image also needs
a qualified power source, exact switch population and threshold bounds, an
attached USB device, and independent recovery.

Full hardware support still requires dual-role/device operation, USB PD,
role swaps, VCONN and applicable cable/accessory support, SuperSpeed and
DisplayPort alternate modes. Those requirements remain separate from this
initial fixed USB2 host path.
