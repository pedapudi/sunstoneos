# SD and eMMC use shared platform ownership

The [storage platform binding](../drivers/storage/a733_mmc_platform.c) connects
[read-only card operations](mmc.md) to the shared clock registry, banked pin
manager, rail ledger and asynchronous PMIC provider. The
[native adapter](../drivers/storage/a733_mmc_platform_native.c) uses the designated
FreeRTOS task and common register mutex for every controller, clock, GPIO-voltage
and interrupt-register access.

The binding requires a reset-held controller baseline and a permanent session.
Physical storage operation remains unqualified. Observed board wiring, reviewed
recovery and eMMC population remain admission prerequisites. The read-only
[SD service](storage-read-application.md) also permits one bounded CMD0 attempt
from an established native SD 3.3 V signaling mode without claiming cold card
history. Neither software ownership nor a register read establishes wiring or
signaling mode.

## Read-only profiles preserve media contents

The selected controller uses a 24 MHz system reference divided by thirty. The
existing MMC core divides the resulting 800 kHz module clock by two, producing a
400 kHz card clock. SD uses SDC0. eMMC defaults to SDC2; setting
`emmc_host` to `A733_MMC_EMMC_SDC3` selects SDC3. SD rejects that selector.
Profile zero retains one-bit SDR signaling and programmed I/O. SD profiles one
and two negotiate four-bit default-speed and high-speed operation; eMMC remains
on its existing profile. The public API provides startup polling, card information,
bounded block reads and result collection. It exposes no raw controller pointer,
command escape, write, erase, partition switch or recovery operation. Writes
exist only through a bound [storage update service](storage-update.md).

| Selected device | Controller | Clock/reset addresses | Interrupt ID | Active signals |
| --- | --- | --- | --- | --- |
| SD | SDC0, `0x04020000` | `0x02002d00` / `0x02002d0c` | 193 | PF1 command, PF2 clock, PF3 data 0; mux 2 |
| eMMC, default | SDC2, `0x04022000` | `0x02002d20` / `0x02002d2c` | 195 | PC5 clock, PC6 command, PC10 data 0; mux 3 |
| eMMC, explicit selection | SDC3, `0x04023000` | `0x02002d30` / `0x02002d3c` | 196 | Same PC signals; mux 4 |

SD reserves PF0–6. In profile zero, unused data pins remain inputs with pull-ups.
Wider profiles select mux two for PF0/D1, PF1/CMD, PF2/CLK, PF3/D0, PF4/D3 and
PF5/D2 before identification; the controller remains one-bit until ACMD6 completes.
PF6
provides active-low card detection. Removal refuses further controller access
and retains the session. Detection is sampled before each bounded operation;
it cannot guarantee that a card remains inserted between samples.

eMMC reserves PC0, PC1, PC5, PC6, PC8–11 and PC13–16, covering its complete
physical route. Unused signals remain inputs. PC1 reset remains an input and
must read high under independently established external pull authority. The
binding never drives or pulses card reset. A failed command does not establish
that eMMC is absent or authorize a retry through another controller.

## Shared ownership precedes hardware access

SD acquires an exclusive SDC0 clock token. Either eMMC selection acquires SDC2
and SDC3 together in one exclusive token before hardware access. Both controllers
share one physical card, so acquiring them independently could admit conflicting
card operations before pin ownership is established. The actual PC pin lease
also blocks independently owned SDC2 or SDC3 routes. Each session reserves the
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

GPIO voltage checks are also read-only. SD accepts PF's manual 3.3 V modes
0/2 or adaptive mode 1, with sensed 3.3 V status 2 and the 3.3 V selector bit.
The exact inherited mode is retained; a subsequent mode, status or selector
change causes retained refusal before further controller commands. The A733
manual sections 18.14.4.6 and 18.14.6.1–3 define adaptive mode as adjustment of
the internal withstand circuit to detected voltage, separately from PF supply
selection. No voltage setting is written. eMMC requires PC's manual 1.8 V
mode and its distinct 1.8 V status encoding. Every writer that could change the
bank voltage must remain excluded for the session's lifetime.

## Negotiated rates retain shared-parent ownership

Nonzero SD profiles add a shared PERI0 lease. The platform verifies the enabled,
locked PLL and actual 400 MHz branch gates even when every fabric consumer uses
the 24 MHz reference. It never enables or retunes that shared parent. Missing
or changed parent state retains the session before further card commands.

After the core proves completed card-output disable, a callback runs within
the existing designated owner and common register mutex. It checks live time,
shared-clock health, voltage, masked interrupt and idle command/data state.
It gates only SDC0's module, selects PERI0_400M and its linear divider, then
reenables the module with exact readback. The recorded module configuration
becomes the expected value for every later live check. A failed access, lost
clock authority or wrong readback retains all resources without rollback.

The native filter enables the new forms only after successful actual SD profile
admission. It admits ACMD51 argument zero, ACMD6 argument two, and the exact
read-direction CMD6 check/switch arguments for the required high-speed profile.
It excludes eMMC CMD6 and every data-write direction from this read-only path.
GPIO voltage modes, selected supplies, reset and shared fabric policy remain
subject to the existing retained lifetime.

## Startup refuses inherited activity and faults

Admission requires a locally gated controller held in reset. The general binding
retains its cold-card contract; the read-only SD service may instead authorize
its documented first CMD0 initialization. An active or failed inherited controller
must not be presented as reset-held. A failed native SD lifetime cannot use the
first-initialization policy.
The selected interrupt must already be disabled, inactive and without a pending
request. Startup preserves unexplained pending state and refuses it.

After checking supplies and inherited clocks, the binding masks the selected
interrupt and waits for the distributor write to complete. It acquires the
complete pin route, observes card presence or eMMC reset, selects the module
clock and enables the local bus. It holds reset for at least ten microseconds
before release. The existing MMC core then performs its bounded cold setup and
card identification.

For SD, the fixed inherited ROM pattern `PF_CFG0[27:0]=0x0f3f3f33`
may also be handed off under explicit debug-client exclusion. It selects JTAG
on PF0/1/3/5 and disables PF2/4/6. The [read-only service](storage-read-application.md)
binds that authority to its first CMD0 policy. Generic pin acquisition already
performs full-field preflight, all-pin reservation, mux disable, pull/drive setup
and target readback. The controller baseline and IRQ requirements remain in
force; arbitrary alternate-function takeover remains unsupported.

An actual seven- or twelve-pin ownership token alone cannot establish that remote
firmware has stopped routing the same peripheral elsewhere. Authenticated
admission excludes every alternate route and unselected pad writer before
startup. For eMMC this includes the unselected eMMC controller, NAND and SPIF
activity. Later writers must honor the same controller and pin ownership.

Each platform poll revalidates the actual clock, pin and rail tokens, PMIC
provider health, card presence, GPIO voltage and clock/reset/interrupt state.
While a read or write transfer advances without a timed wait, that poll then
runs up to eight MMC core steps. Each core step still checks the owner task, the
shared-clock failure flag, the update service and the deadline. A pending
millisecond retry, such as a programming-status wait, ends the poll. Identification
runs one core step per poll. Pin-manager failure is checked under its common
register mutex. All controller
callbacks execute while that mutex remains held, without recursively calling
pin-manager operations. The native adapter records a successful register-mutex
entry and clears that record on leave or retain. Each access checks the record,
the owner task, the selected aperture and the retained-lock bit, so register
accesses need no kernel critical section to query the mutex holder. FIFO bursts
of at most 128 words are admitted once and then use plain device loads or
stores between two barriers. A write burst additionally requires the bound
update service to own the active sector and the burst to fit its remaining
bytes. SDC3 additionally requires its normal-DMA request
bits at offset `0x7c` to remain disabled. This is a read-only admission check;
an enabled request causes retained refusal. The binding never reads or writes
the unselected controller's registers.

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

A native consumer links the two platform sources, `sunxi_mmc.c`, the common
clock registry, pin manager and pin tables, rail ledger, PMIC provider, GPU rail
bridge, shared platform locks and FreeRTOS runtime. No standalone build target
is needed until an application adopts this binding.

The focused [selected-route check](../checks/a733-mmc-sdc3.py) uses the actual
resource providers and MMC core with modeled MMIO, time and cards. It compares
the named PC5/PC6/PC10 functions against both the real-silicon pinctrl table and
the manual's Port C table. It then checks the selected aperture, clock/reset
addresses, interrupt mask, actual mux readback and a read through each eMMC
selection. Ownership cases cover the joint controller token and a competing
real PC pin lease. Version and SDC3 DMA-request mismatches retain ownership.

The selected-route composition passed under AddressSanitizer and
UndefinedBehaviorSanitizer. It completed one read through each eMMC selection,
verified controller and pin conflicts, and refused unsupported observed versions
and enabled SDC3 DMA requests. The focused clock check passed joint ownership,
independent SDC1 ownership and unchanged token-layout checks. All three affected
native source files compiled for AArch64 SRE.

Evidence is preserved under
`~/.cache/sunstoneos/evidence/mmc-sdc3-20261016/`: `batch1/` records the named
source checks and compiler-derived dependencies; `continued/` records the
successful execution, corrected fixture snapshot and native objects. The
recorded input hashes identify which evidence remains reusable.

The compatibility names `v4p6x` and `v5p6x` select two initialization functions
in the pinned kernel's
[bsp/drivers/mmc/sunxi-mmc-v4p5x.c](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/mmc/sunxi-mmc-v4p5x.c).
Both use the same legacy clock and programmed-I/O register path. The SDC3 path
uses a smaller DMA descriptor chunk limit and lacks the other path's raw tuning
callback. These differences remain relevant to future DMA and tuning work.
Manual section 11.3.9 defines SDC3 separately, including its normal-DMA control.

The existing native initializer retains its default selection. Applications
using SDC3 call `a733_mmc_platform_native_init_selected` and supply the same
`emmc_host` value as the platform configuration. Native admission checks both
card kind and controller selection. Rebuild embedding consumers after the
platform configuration and adapter layout change. The common clock-token ABI
and all previously assigned clock-scope values remain unchanged.

The original composition evidence is preserved under
`~/.cache/sunstoneos/evidence/mmc-platform-20261016/`. Its eMMC fixture selected
mux 2, which is NAND rather than SDC2. That result establishes modeled controller
and ownership behavior only; it does not establish a correct eMMC pin route.
Its source-hash checks also did not compare named pin functions. The corrected
route requires the named-function and register-readback evidence above. The
original SD and unchanged [protocol/capacity evidence](mmc.md) retain their
stated scope. The separate PMIC private-storage refusal remains valid.

Native compilation covers the AArch64 SRE configuration. It does not execute
native MMIO, prove physical pin routing or qualify storage operation.

The existing core's version admission remains unchanged and requires observed
version 5.3 or later, below 6.0. A device-tree compatibility string does not prove
that the physical version register meets this condition. A mismatch remains a
retained refusal requiring investigation.

SD v1, voltage switching, UHS/tuning, DMA, interrupts,
hotplug, recovery, physically qualified storage writes, alternate eMMC partitions, command queuing and
power management remain required storage work. UFS remains separate. Physical
read qualification must preserve the inserted card and use its reviewed recovery
plan; the supervised read-only SD lifetime retains resources until complete
board/card power removal. Performance work follows functional hardware coverage.
