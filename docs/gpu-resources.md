# GPU resources require a verified inherited clock and rail configuration

The GPU resource manager reserves shared clocks and power resources, verifies
the inherited configuration, and enables supported bus gates. Its native adapter
uses register reads and the existing AXP8191 I²C reader. Host and AArch64 emulator
checks pass. Physical execution and integration with the GPU service remain
outstanding.

The supported baseline has an enabled, stable oscillator and a locked integer
PLL_REF output of exactly 24 MHz. The peripheral PLL chain must produce the
400 MHz peripheral clock, and DCDC4 must already be enabled at 800 mV.
Cold PLL programming, regulator writes, voltage changes, clock modulation, and
other operating points are unimplemented. An unsupported baseline causes a
retained failure. The manager does not report a requested rate or voltage as
ready without examining its configuration registers.

The implementation is [a733_gpu_resources.c](../drivers/gpu/a733_gpu_resources.c).
Its [public header](../include/a733_gpu_resources.h) defines leases, diagnostics,
and the platform callback contract. The
[native adapter](../drivers/gpu/a733_gpu_resources_native.c) supplies physical
register and inherited-bus I²C operations. Its
[lock configuration](../include/a733_gpu_resources_native.h) requires platform
locks shared with every register and PMIC bus user.

## Leases arbitrate shared settings and GPU ownership

One manager owns the following resource inventory:

| Resource | Stable lease guarantee |
| --- | --- |
| Oscillator | Enabled, stable detected oscillator protected against writers; no raw-rate promise |
| GPU parent clock | The reference PLL, peripheral PLL, output gates, and divisor retain the verified 400 MHz configuration |
| DCDC4 | The enabled 800 mV setting remains fixed |
| Power-controller clock | The shared PCK controller remains clocked |
| GPU AHB gate | The enabled GPU control-bus gate remains fixed |
| GPU memory-bus gate | The enabled GPU memory-bus gate remains fixed |
| GPU local resources | One owner controls GPU clock, reset, and top-level power policy |

Trusted privileged clients use fixed registration slots and monotonically
increasing lease tokens. Stable leases share matching resources. Parent-clock
and bus-clock requests also reserve the oscillator. GPU-local ownership requires
all resources and excludes a second GPU owner.

A change guard excludes stable leases for the resources it covers. It reserves
permission to perform a separately implemented transition; it does not program
hardware. Callers must include every resource their transition changes. An
uncertain external transition must end its guard as failed, retaining ownership.
GPU-local writes are permitted to the exclusive GPU owner while its stable
lease remains held.

An atomic admission lock serializes registry changes and resource callbacks.
A competing call receives `BUSY` without I/O or output changes. Callers can
queue that request until the active operation finishes. Shared-register updates
also use the platform register lock, so another device's fields survive a
concurrent update to the same register.

Admission publishes the token before hardware access. Failed acquisition or
release retains the admitted lease and disables subsequent manager I/O. Other
leases remain recorded. The manager cannot discard that state or resume after
a fault without independent platform recovery. This contract covers cooperative
privileged clients; all hardware writers must participate in its ownership and
lock rules.

## Acquisition verifies settings before enabling gates

The native adapter reads oscillator status at `0x07090160` and requires enable
and stability bits. OSC-only ownership does not declare a raw frequency. Concrete
PCK clients validate their own device clocks; a PCK gate has no rate calculation
here. The parent-clock check then reads the reference PLL, peripheral PLL, and peripheral modulation controls
at `0x02002000`, `0x020020a0`, `0x020020a8`, and `0x020020ac`.

The check requires the source-defined enable, supply, lock, and output flags.
It uses the shared [detected-reference decoder](clock-leases.md#detected-oscillator-and-normalized-reference)
to require exact REF24 from the detected 19.2, 24 or 26 MHz input. It verifies
multiplier limits and the peripheral PLL's divide ratio. It derives
400 MHz from the peripheral multiplier, output divisor, and fixed divide-by-three
stage. Peripheral PLL input-divider bit 1 must be zero, selecting division by
one. Enabled modulation is refused. These checks establish programmed settings;
they do not measure oscillator or output frequency.

The PERI0 2X, 400M-all and 400M branches must each remain enabled without
automatic gating. CCU `+0x1908` requires bits 11, 2 and 1 set for No-Auto,
and bits 27, 18 and 17 set for software enable. The same upper bits must be
set in effective status at `+0x1988`. Other branches do not affect this
400 MHz profile. These registers are read-only. A733 manual sections
4.1.6.249 and 4.1.6.258 define the polarity; vendor clock names ending in
`-auto` use ordinary gate helpers and do not imply that zero disables auto-gating.

The rail snapshot holds the common PMIC/I²C lock while reading address `0x36`:

| Register | Required interpretation |
| --- | --- |
| `0xff`, before and after the snapshot | Normal register page selected |
| `0x0e` | AXP8191 identity `0x03` |
| `0x0f` | Supported revision `0x00` |
| `0x10` | DCDC4 enable bit 3 set |
| `0x15` | Seven-bit selector 30, which programs 800 mV |

A failed identity or page check stops the native snapshot before reading rail
configuration. The adapter performs no PMIC data write. The inherited I²C driver
must already have valid clocks, pins, and idle bus state. Its existing bounded
transfer and STOP handling remain in use. The outer deadline is checked between
transfers; one in-progress transfer can consume its own 10 ms transfer deadline
and separate 10 ms STOP deadline before reporting failure.

GPU-local acquisition verifies asserted external reset and disabled local clocks
before enabling any bus gate. PCK gate enablement uses `0x070101ac[0]`. GPU AHB
and memory-bus gates use `0x020025c0[7]` and `0x020025e0[16]`. Only a GPU-local
owner may enable those GPU gates. Shared requests can adopt them when they are
already enabled.

The reference clock driver ORs `0x010000ff` and `0x41055800` into the AHB and
memory-bus registers. Those values overlap other critical gate bits. This
implementation requires every affected non-GPU bit already set before enabling
the GPU gate. The native update verifies that condition again while holding the
register lock. An unsupported baseline is refused without enabling other blocks.

Release preserves all shared gates, oscillator and PLL settings, and the
always-on DCDC4 rail. Enabled gates remain available for the manager's OS
lifetime. Releasing GPU-local ownership additionally verifies asserted reset,
disabled local clocks, and top-level power-domain status zero. It establishes
no guarantee about outstanding memory transactions and authorizes no buffer
reuse.

## Native integration requires shared scheduling locks

Initialize the native adapter at EL1 with scheduling-aware register and I²C
locks, then initialize one resource manager from its operations. Lock acquisition
must honor absolute microsecond deadlines and avoid spinning behind a preempted
owner. The adapter requires `CNTFRQ_EL0` to declare 24 MHz and derives time from
the architectural counter. Peripheral addresses must have valid Device mappings.

A PMIC transfer failure retains the I²C lock, bus diagnostics, and the resource
lease. The adapter performs no later transfer or close operation. After a successful
snapshot, close must restore the owned CONTROL fields, clear START/STOP, and
leave idle status with released bus lines. The adapter reads those settings
before releasing the I²C lock. Failed restoration also retains ownership.
Recovery must
reconcile both the bus and resource manager. A register-lock refusal leaves the
register untouched while the manager retains its admitted lease.

Connect successful all-resource admission to the
[GPU power controller](gpu-power.md). Its selected rate and rail setting come
from the returned resource record. Admission contention should be queued before
power sequencing; the power controller's Boolean callback cannot express
`BUSY`. Shared settings must stay reserved through firmware shutdown and the
controller's release callback. The [native platform binding](gpu-platform.md)
now implements pending admission/release and shared FreeRTOS locks. Its callbacks
fit the asynchronous service interface; concrete board composition and physical
qualification remain required.

## Focused checks cover ownership and native binding

The changed behavior is resource ownership, inherited-state verification, gate
enablement, and native I²C binding. The relevant risks are releasing resources
after uncertain I/O, admitting conflicting clients, changing unrelated clocks,
and activating a bus for an inherited live GPU.

The [host check](../checks/a733-gpu-resources.c), compiled with AddressSanitizer
and UndefinedBehaviorSanitizer, passes shared leases, change guards, stale-token
refusal, partial acquisition, concurrent admission, and 86 retained faults.
Concurrent callers cannot enter hardware callbacks during another admission.
A live GPU is rejected before gate writes. Shared GPU-gate requests cannot
activate a disabled gate without exclusive GPU ownership.

The [reference comparison](../checks/a733-gpu-resources-reference.py) compiles
original clock-gate, PLL-rate, and PMIC voltage-decoding functions. Their outputs
match the native gate values, 400 MHz parent rate, and 800 mV DCDC4 interpretation.
Source assertions verify field positions, multiplier/divisor offsets, oscillator
selection, and modulation controls.

The A733 User Manual V1.00, section 4.1.6.6, additionally defines the peripheral
PLL input divider omitted from the kernel clock profile. The focused
`--pll-input-only` host case admits the unchanged divide-by-one configuration
and refuses divide-by-two before writes, retaining ownership. It passes with
sanitizers; the changed runtime compiles for AArch64. Evidence is in
`~/.cache/sunstoneos/evidence/peri0-input-divider-20261016/`.
Earlier gate, lifetime and native-adapter checks remain reusable.

The focused `--parent-gates-only` case checks each required No-Auto, software
enable and effective-status bit, failed or late reads, unrelated branches, and
retained ownership without writes. Evidence is in
`~/.cache/sunstoneos/evidence/peri0-gates-20261016/`. The focused host check passes with address
and undefined-behavior sanitizers; both affected native units compile for AArch64. The native harness
fixture now supplies enabled inherited PERI0 gates. Its earlier I²C, lock and
resource lifecycle results remain applicable; those paths were not repeated.

The [AArch64 harness](../checks/a733-gpu-resources-native.c) and
[emulator runner](../checks/a733-gpu-resources-emulate.py) execute the native
adapter's register operations and I²C binding. MMIO registers, counter time,
locks, and the I²C transport are modeled. The harness verifies the six-register
snapshot, retained failures at each transport read, wrong-page refusal, and
register-lock refusal, and ignored or incomplete I²C restoration. It executes no
physical I²C controller or GPU firmware.

Evidence is `~/.cache/sunstoneos/evidence/gpu-resources-20261016/`, including
commands, input hashes, host and emulator results, original reference outputs,
and AArch64 objects. The manager uses 4,308 bytes of object text; the native
adapter uses 1,626 bytes. Each has a maximum individual static frame of 96 bytes.
These counts describe the compiled build.

Reference sources are the [pinned board kernel](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f)
clock definitions and helpers, plus the [pinned bootloader](https://github.com/orangepi-xunlong/u-boot-orangepi/tree/b791be842935b27268ae3d00e943a9075495f30a)
AXP8191 register map, voltage functions, and I²C control definitions. Original notices remain with cached
sources and compiled reference extracts. The source identity manifests record
individual hashes and download paths.

No board executable or physical queue submission exists for this resource
backend. The first board trial requires qualified independent watchdog recovery,
shared lock integration, an inherited I²C bus, and a stopped GPU. It should
capture the clock and rail baseline, acquire all GPU resources once, perform the
bounded power/identity/shutdown trial, and release the lease. An unsupported
baseline should stop that trial and supply evidence for cold initialization.

The RAIL bridge used by the shared rail ledger only reserves and authenticates
metadata. Its acquire/status/end functions do not call the clock validator,
read MMIO, or establish a voltage. Updating parent admission therefore does not
change the SD rail path. Clock admission samples settings at acquisition under
held ownership; it does not provide continuous frequency monitoring.

The focused detected-reference checks are preserved under
`~/.cache/sunstoneos/evidence/accelerator-clock-reference-20261018/`.
Actual provider code accepts exact 26 MHz N96/M104 and 19.2 MHz N100/M80
profiles and retains wrong ratios, lost reference lock, and disabled or unstable
DCXO before writes. GPU/NPU PERI0 cases preserve reserved bit24; the NPU PLL
case also retains final control corruption without opening the output.
These modeled-register checks do not establish physical operation. No native
image was rebuilt; accelerator launch still requires a final image built from
the accepted startup and providers, plus actual resource and recovery admission.
