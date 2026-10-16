# SD and eMMC use shared platform ownership

The [storage platform binding](../drivers/storage/a733_mmc_platform.c) connects
[read-only card operations](mmc.md) to the shared clock registry, banked pin
manager, rail ledger and asynchronous PMIC provider. The
[native adapter](../drivers/storage/a733_mmc_platform_native.c) uses the designated
FreeRTOS task and common register mutex for every controller, clock, GPIO-voltage
and interrupt-register access.

The binding implements a cold startup and a permanent read-only session. Physical
storage operation remains unqualified. Independent watchdog recovery, observed
board wiring, cold card state and eMMC population remain admission prerequisites.
Neither software ownership nor a successful register read establishes these facts.

## One-bit reads preserve media contents

The selected controller uses a 24 MHz system reference divided by thirty. The
existing MMC core divides the resulting 800 kHz module clock by two, producing a
400 kHz card clock. Identification and reads retain one-bit SDR signaling and
programmed I/O. The public API provides startup polling, card information,
bounded block reads and result collection. It exposes no raw controller pointer,
command escape, write, erase, partition switch or recovery operation.

| Selected device | Controller | Clock/reset addresses | Interrupt ID | Active signals |
| --- | --- | --- | --- | --- |
| SD | SDC0, `0x04020000` | `0x02002d00` / `0x02002d0c` | 193 | PF1 command, PF2 clock, PF3 data 0; mux 2 |
| eMMC | SDC2, `0x04022000` | `0x02002d20` / `0x02002d2c` | 195 | PC5 clock, PC6 command, PC10 data 0; mux 2 |

SD reserves PF0–6. The unused data pins remain inputs with pull-ups, and PF6
provides active-low card detection. Removal refuses further controller access
and retains the session. Detection is sampled before each bounded operation;
it cannot guarantee that a card remains inserted between samples.

eMMC reserves PC0, PC1, PC5, PC6, PC8–11 and PC13–16, covering its complete
physical route. Unused signals remain inputs. PC1 reset remains an input and
must read high under independently established external pull authority. The
binding never drives or pulses card reset. A failed command does not establish
that eMMC is absent or authorize a retry through another controller.

## Shared ownership precedes hardware access

The binding acquires an exclusive SDC0 or SDC2 clock token. It also reserves the
shared fabric and system-reference ancestors. When an inherited AHB, NSI or MBUS
profile selects a supported peripheral-PLL output, it reserves and checks that
parent. The binding never changes a shared parent or storage-fabric gate.
SDC1 retains its separate radio ownership.

The shared fabric reservation covers storage AHB/MBUS gates and the MSI-LITE1
interconnect. Startup requires their inherited profile to be enabled and stable.
The platform checks oscillator/reference configuration, selected fabric rates,
parent branch gates and status under the common mutex. Tokens exclude cooperating
writers; authenticated boot admission must also exclude remote and legacy writers.

Both slots adopt the board's inherited supply arrangement:

| Shared rail | Required observation | Purpose |
| --- | --- | --- |
| DCDC1 | Enabled, programmed to 3.3 V | Upstream source of the switched card supply |
| DC1SW2 | Enabled, programmed to 3.3 V | Board card/eMMC supply and shared 3.3 V distribution |
| CLDO5 | Enabled, programmed to 1.8 V | Board PC and PF voltage domains |

One actual rail token reserves all three outputs. The rail ledger also reserves
DCDC1's voltage aliases. The existing PMIC worker reads each output asynchronously;
the storage consumer never owns the PMIC bus. It authenticates the provider
lifetime and checks chip identity, page, enable and decoded voltage before
accepting each snapshot. The provider is [read-only](pmic-runtime.md).

This conservative profile requires all three outputs even for the initial
one-bit SD route. PMIC snapshots prove programmed values, not wiring, measured
voltage or settling. Those remain authenticated physical prerequisites.
No regulator output is changed.

GPIO voltage checks are also read-only. SD requires PF's manual 3.3 V mode,
3.3 V status and the 3.3 V voltage-selector bit. eMMC requires PC's manual 1.8 V
mode and its distinct 1.8 V status encoding. Every writer that could change the
bank voltage must remain excluded for the session's lifetime.

## Startup refuses inherited activity and faults

Admission requires a cold native card and a locally gated controller held in
reset. An active or failed inherited controller must not be presented as cold.
The selected interrupt must already be disabled, inactive and without a pending
request. Startup preserves unexplained pending state and refuses it.

After checking supplies and inherited clocks, the binding masks the selected
interrupt and waits for the distributor write to complete. It acquires the
complete pin route, observes card presence or eMMC reset, selects the module
clock and enables the local bus. It holds reset for at least ten microseconds
before release. The existing MMC core then performs its bounded cold setup and
card identification.

An actual seven- or twelve-pin ownership token alone cannot establish that remote
firmware has stopped routing the same peripheral elsewhere. Authenticated
admission excludes every alternate route and unselected pad writer before
startup. For eMMC this includes SDC3, NAND and SPIF activity. Later writers must
honor the same controller and pin ownership.

Each bounded card step revalidates the actual clock, pin and rail tokens, PMIC
provider health, card presence, GPIO voltage and clock/reset/interrupt state.
Pin-manager failure is checked under its common register mutex. All controller
callbacks execute while that mutex remains held, without recursively calling
pin-manager operations.

## Completion releases only the read buffer

Read admission validates the full range and buffer length before hardware
observations. Buffers must be disjoint from the complete platform, shared
managers, PMIC provider and native adapter storage. The unchanged MMC core owns
the buffer through command completion and its final card-status check.

A completed buffer returns to its caller only through `take` with a fresh
deadline and successful admission. Every fallible observation precedes the
final metadata operation that releases the buffer. The session retains its
controller, pin and rail ownership after successful reads.

Idle polling returns cached ready or complete status. It does not report current
hardware health. A new read, card-information request or result collection needs
a fresh deadline and new admission checks.

Failures retain all acquired tokens and any borrowed buffer. An uncertain write
also retains the common mutex when held. Terminal polling performs only pending
ownership-retention bookkeeping; it issues no further hardware accesses. External
recovery owns retained sessions. There is no close or implicit rollback path.

## Sources, integration and remaining work

The source references are the A733 user manual revision 1.00, the Orange Pi
Zero 3W revision 1.2 schematic and the pinned board kernel at
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. The board DTS defines PF6 detection
and SD supply identities. The SoC DTS, pin tables and clock driver define routes,
interrupts and clock fields. Manual sections 11.3 and 18.14 define controller and
GPIO-voltage behavior. The pinned bootloader PMIC table defines output decoding;
[shared PMIC evidence](pmic-runtime.md) covers the provider.

A native consumer links the two new platform sources, `sunxi_mmc.c`, the common
clock registry, pin manager and pin tables, rail ledger, PMIC provider, GPU rail
bridge, shared platform locks and FreeRTOS runtime. No standalone build target
is needed until an application adopts this binding.

The focused [composition check](../checks/a733-mmc-platform.py) uses the actual
resource providers and MMC core with modeled MMIO, time and cards. It reuses the
existing card model without replaying its protocol or capacity suites. Its
source snapshots include compiler-derived dependencies before execution. Native
compilation covers the AArch64 SRE configuration; it does not execute native MMIO
or establish physical qualification.

The focused batch passed both slot compositions, source-identity checks,
sanitisers and both native object compilations. It covered a read and buffer
release on each route, invalid-range refusal, controller ownership conflict,
SD removal, low eMMC reset, failed pin-manager health, wrong supply/voltage
profiles and uncertain local-clock writes. Reset timing and the absence of
fallible observations after buffer release were asserted directly.
A separate narrow check covers rejection of storage that overlaps the PMIC
adapter's private backing. The admission path delegates to the actual PMIC
provider's storage guard; the successful card workloads remain reusable.

Evidence is preserved under
`~/.cache/sunstoneos/evidence/mmc-platform-20261016/batch1/`, including exact
inputs, the extracted reusable card model, compiler commands, executable and
native objects. The private-storage correction and changed native core are
recorded in the adjacent `private-storage/` evidence directory. The unchanged
protocol/capacity evidence remains in
[the card-driver record](mmc.md).

SDC3 remains required. It occupies `0x04023000`, uses interrupt 196 and clock/reset
words at `0x02002d30` and `0x02002d3c`. It shares the eMMC pins through mux 3.
The manual supplies a separate register description, and the kernel selects its
v5p6x controller path. The current core explicitly binds eMMC to SDC2. SDC3 needs
controller-compatibility work and mutually exclusive route admission before use.

The existing core's version admission remains unchanged and requires observed
version 5.3 or later, below 6.0. A device-tree compatibility string does not prove
that the physical version register meets this condition. A mismatch remains a
retained refusal requiring investigation.

SD v1, wider buses, faster clocks, voltage switching, tuning, DMA, interrupts,
hotplug, recovery, storage writes, alternate eMMC partitions, command queuing and
power management remain required storage work. UFS remains separate. Physical
read qualification must preserve the inserted card and wait for independent
watchdog recovery. Performance work follows functional hardware coverage.
