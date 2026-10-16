# Header pin ownership has host evidence; physical configuration is unqualified

The native [pin manager](../include/a733_pins.h) provides exclusive leases for
PB4–PB8, including the external I2S0 pins. It validates requested functions,
checks inherited register fields, and preserves neighboring pins during
configuration and restoration. The [AArch64 adapter](../include/a733_pins_native.h)
limits access to the required PIO registers. Both components compile for the
target; no physical pin access has run.

GPIO input, selected alternate functions, and disabled I/O are implemented.
GPIO output, external interrupts, other pin banks, power transitions, and the
production FreeRTOS service remain separate work. The manager does not configure
alternate peripherals or establish that their signals work electrically.

## PB4–PB8 have explicit function and electrical constraints

The [board wiring record](board-wiring.md) maps PB4, PB5, PB6, PB7, and PB8 to
header pins 7, 12, 35, 40, and 38 respectively. Function 3 selects their I2S0
signals: MCLK, BCLK, LRCK, DOUT0, and DIN0. External receive uses PB5, PB6, PB8,
and optionally PB4. PB7 remains available to a separate owner when capture does
not use it.

| Pin | Admitted alternate function codes | Other admitted modes |
| --- | --- | --- |
| PB4 | 2–8 | 0 input; 15 disabled |
| PB5 | 3–8 | 0 input; 15 disabled |
| PB6 | 2–7 | 0 input; 15 disabled |
| PB7 | 2–8 | 0 input; 15 disabled |
| PB8 | 2–8 | 0 input; 15 disabled |

The pinned pin table defines each code's meaning. The same code can identify
different peripherals on different pins. Claims therefore require a per-pin
configuration, with no inference from an I2S or GPIO client name. Function 1
is GPIO output, and function 14 is external interrupt; this component refuses
both. An inherited configuration outside the admitted set also prevents a claim.

Pull codes are 0 for disabled, 1 for pull-up, and 2 for pull-down. Code 3 is
refused. Drive codes 0–3 follow the reference setter's admitted values;
codes 4–15 are refused despite the four-bit register field. The reference labels
those drive values 10, 20, 30, and 40 mA. Those labels are source semantics and
do not establish actual current, waveform quality, or suitability for a device.

Bank voltage remains unchanged. Platform integration must establish compatible
voltage, external pull arrangements, drive settings, and peripheral quiescence
before acquiring pins. There is no implicit microphone electrical profile.
The manager does not access bank-voltage, clock, or PMIC registers.

## Leases require an explicit platform handoff

Initialization receives an authorized PB mask from the platform and performs no
MMIO. That authorization means the platform has handed those pins to this
manager and excluded other owners. Register values alone cannot prove that a
pin is unused. The caller supplies its owner identity and expected inherited
mux, pull, and drive values for every requested pin.

All users share one manager instance and one scheduling-aware register lock.
Every other writer of these PIO words must participate in that lock, including
writers of adjacent pins. The native adapter forwards lock acquisition to the
platform with an absolute monotonic microsecond deadline. Failed acquisition
returns `BUSY` without register accesses. A lock callback must honor its deadline;
the manager cannot bound an uncooperative callback.

Acquire validates the complete request and checks existing leases before any
register write. Under the shared lock, it verifies all expected fields and
refuses requested pins with their interrupt-enable bits set. An expected-state
mismatch returns `CONFLICT` without changing registers or assigning a token.
Invalid codes, duplicate pins, unauthorized pins, and unsafe output aliases are
also refused before mutation.

A successful claim creates a token containing an owner identity, pin mask, and
monotonically increasing generation. Generation exhaustion prevents new claims.
The manager requires the complete token for release, which rejects stale,
modified, or partial tokens. Tokens identify leases; privileged service code
must still authenticate callers and protect manager memory.

## Configuration and restoration preserve other owners

The A733 PIO block starts at `0x02000000`. The pinned source assigns the first
bank offset `0x80` and bank stride `0x80`, placing bank B at offset `0x100`.
Silicon profiles 4 and 10 use identical fields for this component.

| PIO offset | Access | Purpose |
| --- | --- | --- |
| `0x100`, `0x104` | Read/write | Four-bit mux fields; eight pins per word |
| `0x120`, `0x124` | Read/write | Four-bit drive fields; eight pins per word |
| `0x130` | Read/write | Two-bit pull fields; sixteen pins per word |
| `0x110` | Read only | GPIO input levels |
| `0x150` | Read only | Bank B interrupt enable; claim and ownership checks |

Acquire switches only the requested pins to disabled I/O, applies drive and pull
fields, then selects their requested functions. Each update reads the current
word, checks the owned fields, preserves all other bits, writes the changed
word, and verifies the entire readback. A group of pins sharing a word uses one
combined mask. The register lock covers the complete sequence across words.

Release requires an explicit `users_quiescent` assertion after the owning
peripheral has stopped. It verifies the current owned fields, switches those
pins to disabled I/O, restores their original drive and pull fields, and restores
the original muxes. It clears the token only after every readback succeeds.
Restoration uses current neighboring fields, preserving changes made by other
leases since acquisition. This sequence does not establish glitch-free switching
or physical output isolation; both require hardware qualification.

GPIO input reads require a valid lease and a requested subset configured as
inputs. The manager verifies their fields and interrupt state before reading
the data register. Failure leaves the output argument unchanged. It never writes
the data register or its SET/CLR aliases, because output-latch ownership and
output transition behavior remain outside this component.

## Uncertain changes retain their tokens

Acquire reserves all requested pins and publishes their token before any possible
configuration write. A write failure, readback mismatch, or deadline exhaustion
after a write returns `RECOVERY` with that token retained. No compensating writes
or automatic rollback follow. A timeout before any write can return `BUSY` and
remove a provisional reservation without changing registers.

A failed access or unexpected owned-field change also places the manager in a
failed state. It retains every existing lease and refuses further configuration,
release, and input-register access. `a733_pins_held` can still inspect ownership
under the shared lock without MMIO. Independent reset and platform reconciliation
are required; there is no timeout-based or forced release operation.

A failed acquire can report `RECOVERY` with an empty token if the failure occurred
before reservation. The manager remains failed because the register access was
uncertain. A populated token always remains authoritative, regardless of how
many writes completed. The caller must retain it and distinguish failure from
ordinary contention.

The current [I2S capture API](i2s.md) has a boolean platform acquire callback whose
failure contract requires no effects. That contract cannot represent a retained
partial pin acquisition. Integration must add a result that carries retained
ownership, preserve the pin token across the I2S failure, and prevent retry or
cleanup from releasing uncertain resources. Returning false and discarding the
token would violate this manager's ownership contract.

Calls check deadlines before and after bounded register operations. A late final
write or verification read enters `RECOVERY` and retains the token, including
when a release has already restored the hardware fields. A late GPIO observation
leaves its output argument unchanged. Regressing clock readings are refused;
observations under the shared lock preserve a monotonic lower bound across calls.

Synchronous MMIO can stall or fault before C code regains control. The native
adapter's return value cannot recover such a condition, so independent
hardware-watchdog protection remains mandatory.

## Focused evidence covers ownership and source compatibility

The host fixture exercises the actual manager with a shared mutex and register
model. It checks simultaneous conflicting claims, disjoint leases in shared
words, restoration after a neighboring lease changes fields, stale tokens,
GPIO input, lock contention, deadlines, active IRQ refusal, invalid encodings,
and retained acquire/release failures. Each modeled write asserts that unrelated
bits remain unchanged. Address and undefined-behavior sanitizers passed.

The reference check compiles unmodified register-address helpers and hardware
profiles from the pinned source. It compares every exposed pin's mux, drive,
pull, input-data, and interrupt-enable addresses for both silicon profiles.
It also derives mux availability from the real-silicon pin table and checks
admission against the reference pull and drive limits. The source comparison
passed. Both native files compiled for freestanding AArch64 without warnings.
A focused follow-up covers deadline expiry during the final acquire/release
write or readback, late GPIO input, and a regressing clock after mutation.
The follow-up evidence is `~/.cache/sunstoneos/evidence/pins-deadline-20261016/`.
Only those cases ran for the deadline change; the source-encoding comparison
and earlier ownership checks retain their original evidence.

Raw evidence and input identities are in
`~/.cache/sunstoneos/evidence/pins-20261016/`. Reproduce only affected checks when
their relevant inputs change:

```sh
cc -D_XOPEN_SOURCE=700 -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -pthread -Iinclude \
  checks/a733-pins.c drivers/pinctrl/a733_pins.c -o /tmp/a733-pins-check
/tmp/a733-pins-check
python3 checks/a733-pins-reference.py \
  --source-dir "$HOME/.cache/sunstoneos/sources/pins" \
  --soc-source "$HOME/.cache/sunstoneos/sources/board-kernel/pinctrl-sun60iw2.c" \
  --build-dir "$HOME/.cache/sunstoneos/evidence/pins-reference"
```

The next integration step is a shared production lock and platform authorization
record, followed by an I2S acquisition interface that retains partial failures.
Clock/reset ownership and physical electrical qualification remain separate
requirements. No physical test proposal or performance measurement was added.

## Pinned sources define the implemented fields

All sources below use board-kernel revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`. Downloaded sources remain outside Git;
temporary reference compilations preserve their original license notices.

| Source | SHA-256 |
| --- | --- |
| [Common pin controller](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/pinctrl/pinctrl-sunxi.c) | `3bc32a1fd6be151be68979a7c44fe400adb657f21cb66f4e043d31aea4273406` |
| [Register helpers](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/pinctrl/pinctrl-sunxi.h) | `cd345d552621857017e5a34c6c182c3fcc630d2394174c99edd9f2c135e9cab8` |
| [A733 pin table](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/pinctrl/pinctrl-sun60iw2.c) | `ab05ddccecd152edf3fafd238ade22299aebe3448646f02f0551edef0c6e1ec7` |

The pinned `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` supplies the PIO base
and window. The common driver also contains an optional named-resource I2S pad
routing hook. The A733 device tree supplies no such named resource, and the
Orange Pi Zero 3W board override adds none. This adapter therefore contains no
pad-routing register write; actual pad routing still needs physical qualification.
