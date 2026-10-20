# TYPEC24 host power and DisplayPort negotiation

The FUSB302 and SY6280 on the Orange Pi Zero 3W control attachment and VBUS
for the exposed TYPEC24 connector. The native provider selects a fixed USB
source/data-host role at default current. It supplies a fresh attachment token
to the USB2 host controller. An optional USB Power Delivery (PD) profile negotiates
a fixed 5 V contract and four-lane DisplayPort alternate mode. Both profiles
remain physically unqualified.

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
probed. The native adapter requires APBS1 source 0, raw DCXO, with divider 1.
Manual section 4.2.5.3 and the pinned CCU driver identify this source separately
from source 4, the normalized 24 MHz reference. Under its OSC/APBS1 leases,
the provider verifies XO_CTRL0 enabled/stable bits and decodes the raw
24, 19.2 or 26 MHz rate with `a733_clock_dcxo_hz`. It derives the I2C divider
from that rate and does not retune the shared parent. It observes both local gate/reset bits
clear, releases reset, enables the gate, and programs a source-derived divider
at or below 100 kHz. Legacy, autonomous driver, DMA, FIFO and IRQ conditions
must be inactive before transactions begin.

## Admission and ownership

Construct one `a733_typec_native` before scheduling. Its address, configuration,
provider tasks and shared managers remain private and immutable. The two tasks
must be distinct: one runs `a733_typec_poll`, and the other runs
`a733_typec_native_power_poll`. The PMIC provider uses its own existing task.
All three use the same monotonic counter and shared lock services.

The native path acquires exclusive S_TWI1 ownership and its dedicated controller
mutex, separately from the PMIC's S_TWI0 mutex. Both controllers use the common
register mutex for shared clock/reset words. It holds stable APBS1/REF/OSC,
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

Both Type-C tasks target one service interval per millisecond. The provider
revokes admission when entry gaps exceed 2 ms, allowing headroom around a 1 ms
RTOS tick. This scheduling guard does not extend the 5 ms I2C deadline or 10 ms
attachment and power-health bounds. An active CC or PD transaction requires
continued polling with task yields; a one-tick sleep is permitted only between
transactions. Physical scheduler latency remains unqualified. The GPIO task observes
PL9 and checks deadlines renewed only by real provider progress and attachment
observations. Host activity never extends them. Detach, expiry or failure closes
host admission permanently. The GPIO task lowers PL8 independently of the I2C
task and continues even when that task retains the S_TWI1 mutex.

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

## Bounded PD and DisplayPort profile

Set `electrical.displayport=1` before native construction to enable the PD2
source/data-host profile. Its qualified source range is 4.75–5.25 V, with at least
500 mA available. It advertises one fixed 5 V, 500 mA power object and accepts
only a matching current request. The existing electrical qualification supplies
the power budget; PD messages cannot increase it. The image must exclude USB3
and hold COMB0 ownership before negotiation can configure the partner for DP.

The existing Type-C task services the real FUSB302 through the already-owned
S_TWI1 engine. Each FIFO transfer is one bounded I2C transaction, with a
repeated START for reads, ACK for intermediate bytes, a final NACK, and completed
STOP before publication. The maximum copied transfer is 40 bytes. The original
single-byte API remains available. FUSB302 hardware sends GoodCRC acknowledgments;
software verifies received CRC32, SOP framing, message identifiers, roles and
protocol fields before advancing negotiation.

After accepting the fixed power contract, the provider sends Discover Identity,
Discover SVIDs, Discover Modes, Enter Mode, DP Status and DP Configure. SVID is
the protocol's alternate-mode identifier; the DP value is `0xff01`. The provider
selects a supported four-lane pin assignment C or E, accounts for partner plug
versus receptacle capability fields, and requires the Configure acknowledgment.
Both CC orientations select their corresponding FUSB302 transmit pin.

Each message wait is bounded by 30 ms, including receive processing and duplicate
traffic. Negotiation cannot outlive the original `a733_typec_begin` deadline or
one second after PD service starts. There is one negotiation attempt. Unsupported
requests, negative responses, malformed packets, bus failure or expiry permanently
revoke Type-C admission and enter the existing retained shutdown path. Hardware
packet retries are limited to the FUSB302's three retries; software does not
restart the session. The independent GPIO task remains responsible for VBUS.

`a733_typec_dp_snapshot` publishes a copied token after Configure succeeds.
The token binds the permanent provider, owner, attachment generation, CC polarity,
pin assignment, HPD level and HPD event sequence. HPD means Hot Plug Detect;
its state comes from DP Status and Attention messages because the A733 HPD pad
is unconnected. The attachment generation stays fixed. The event sequence increases on a published-mode HPD level change or an HPD
interrupt, including level changes without an interrupt. This detects a low/high
cycle between consumer validations.

Validation requires a current CC/VBUS observation younger than 10 ms. A token's
immutable acquisition timestamp may be older while the provider keeps publishing
fresh observations of the same mode. The timestamp does not represent another
HPD transition.
Consumers validate before AUX, training, enable and continued display progress.
A concurrent HPD change invalidates the old DP token without revoking CC power.
Identity, freshness or underlying provider loss closes admission permanently.
The DP consumer owns the separate exclusive COMB0 lease and retains display
resources when its token becomes invalid.

The limited profile excludes power/data-role swaps, VCONN, electronically marked
cables, higher-power contracts and automatic renegotiation. A missing DP SVID in
the bounded discovery response is a retained refusal. DP controller/PHY setup,
AUX, link training and visible output belong to the display consumer.

The [source manifest](typec-pd-sources.json) pins the board Linux FUSB302, TCPM,
PD definitions and DisplayPort references. The adapted code runs on the native
FreeRTOS CPU; Linux supplies no runtime service. The combined adaptation uses
[GPL version 2](typec-pd-GPL-2.0.txt).

## Evidence and remaining integration

The original fixed-host focused check passed with the actual native Type-C, TWI, PMIC, pin, rail,
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

The PD software evidence is under
`~/.cache/sunstoneos/evidence/typec-pd-20261020/`. The real CC monitor, PD service,
TWI state machine and copied-token APIs execute against modeled hardware.
Both orientations complete a contract and DP configuration, selecting C and E.
Checks cover HPD interrupts, old-token refusal, over-budget requests, negative
VDM responses, CRC corruption, wrong SVIDs, late responses, duplicate traffic
and the original negotiation deadline. Electrical admission and task authority
are modeled; these results do not qualify a cable, power source or display.

The native composition additionally executes the actual pin, PMIC, clock, rail,
TWI and lower MMIO adapter paths with PD disabled. It verifies the byte API and
a three-byte repeated-START read, intermediate ACKs, final NACK and completed
STOP. The affected 26 MHz configuration verifies a source-derived 100 kHz I2C
rate. Frozen inputs, artifacts and checkpoints preserve the exact executable
for each case. The original independent GPIO/I2C-failure evidence remains
applicable to the retained shutdown mechanism; a physical PD fault is untested.

The composed DisplayPort executable also completes one PD-enabled native path
with CC1 selected, pin C configured and HPD high. The actual PMIC, display,
Type-C and power task bodies execute from the admitted constructor checkpoint;
the real token validator accepts the negotiated publication. At the first DP
supply request, the trace records the PMIC holding controller mutex 1 while
Type-C holds controller mutex 2 and advances an S_TWI1 transaction. The DP
controller has not been prepared at this checkpoint. Raw DCXO is 26 MHz and the
native TWI divider is 97, giving 100 kHz.

This composition evidence is frozen under
`~/.cache/sunstoneos/evidence/displayport-native-20261020/typec-negotiated-ae83/`.
`negotiated-manifest.json` binds the exact executable, admitted session, runtime
contract, constructor, task contexts, hardware state and check sources. The
selected executable SHA-256 is
`ae83b12f6e69b820f3bd8888335bd41314366c694e9197554f766fbd4c406b17`.
The scheduler model preserves task priorities and the actual delay and SVC 105
yield boundaries. It is serialized and cooperative; it does not qualify SMP
execution, tick preemption, priority inheritance or physical scheduling latency.
The 200 MHz DP APB value is a qualified model input, not a measurement. Earlier
stopped fixtures remain separate: they do not count as native driver failures
or as completed negotiation. The later DisplayPort consumer continues from this
checkpoint rather than fabricating a token or substituting a validator.

Full hardware support still requires dual-role/device operation, role swaps,
VCONN and applicable cable/accessory support, further PD profiles, and physical
SuperSpeed and DisplayPort qualification.
