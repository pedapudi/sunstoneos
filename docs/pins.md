# Shared pin ownership and GPIO for banks B–M

The [pin manager](../include/a733_pins.h) implements exclusive pin leases,
source-defined mux selection, GPIO input and output, and external interrupt
control. The [native adapter](../include/a733_pins_native.h) performs AArch64
MMIO with barriers through a restricted register allowlist. Physical pin
operation, interrupt delivery and electrical behavior remain unqualified.

One manager and one scheduling-aware register mutex cover both the main PIO
controller and the always-on controller. Every writer of their shared words
must participate in that authority. The manager configures pins; peripheral
controllers still need their own clock, reset, DMA and controller ownership.

## The bank-qualified API shares ownership with the PB API

`a733_pins_init_banks` accepts an explicit platform handoff of authorized pins.
It performs no hardware access. Bank indices follow letters: B is 1 and M is
12. The platform must establish the pin handoff, compatible voltage, stable
Device mappings, clocks, security attribution and independent watchdog recovery.
Register values alone do not establish that a pin is unused.

The source tables describe 201 pins: B0–10, C0–16, D0–23, E0–15, F0–6,
G0–14, H0–19, I0–16, J0–27, K0–25, L0–13 and M0–5. Each request supplies
expected and desired mux, pull and drive codes. Pull codes are 0 for disabled,
1 for pull-up and 2 for pull-down. Drive codes are 0–3; the four-bit field does
not admit values 4–15. The reference labels these codes 10–40 mA, which does
not establish measured current or suitability for an attached device.

A bank-qualified lease contains an owner identity, generation and complete
per-bank pin masks. Claims accept at most 32 pins. The manager checks every
requested field and interrupt-enable bit before reserving the complete lease.
Duplicate pins, unauthorized pins, stale tokens, partial tokens and conflicting
owners are refused. Tokens identify leases; privileged service code must
separately authenticate callers and protect manager memory.

The existing PB4–PB8 signatures retain their input, alternate and disabled
modes. They share the expanded manager's pin state, lock and generation sequence.
Their token format cannot release a bank-qualified lease. The manager layout
has changed: every embedding consumer must be rebuilt, and builds must link
`drivers/pinctrl/a733_pin_tables.c` with the manager. This is source compatibility,
not binary compatibility.

## Board routes and UART controllers have explicit exclusions

The board wiring and pinned tables establish these routes:

| Function | Pins and mux codes |
| --- | --- |
| Bluetooth UART1 | PG6–PG9, mux 2 |
| Header UART1 | PE12/PE13 TX/RX; PE10/PE11 RTS/CTS, mux 8 |
| Header UART2 | PB0/PB1, mux 2 |
| Header UART6 and SPI3 | PE0/PE1 use UART6 mux 8 or SPI3 mux 5 |
| Camera 1 | PK0–PK9 mux 2; PE5 MCLK mux 2; PE6 GPIO control; PE7/PE8 TWI11 mux 8 |
| Camera 2 | PK10–PK19 mux 2; PE9 MCLK mux 2; PE10 GPIO control; PE14/PE15 TWI9 mux 8 |
| DSI1 | PD10–PD19, mux 4 |
| LCD control | PJ22 enable, PJ23 reset, PJ27 TE input |
| Touch control | PK22 reset, PK23 interrupt; PK24/PK25 TWI12 mux 5 |
| I2S0 | PB4–PB8, mux 3 |
| Always-on header GPIO | PL2/PL3; PL4/PL5 also provide S_TWI2 mux 3 |
| Radio control inputs | PM0 WLAN wake; PM4 Bluetooth wake |

A lease reserves UART controllers used by either its inherited or requested
configuration. Another lease cannot use a different pin route for that same
controller. This prevents simultaneous Bluetooth and header UART1 claims.
Each stable configuration also accepts only one documented route set for a
controller. Route sets follow signal assignments, not adjacent pin numbers:
PF2/PF4 form a UART0 pair, while PM2/PM3 and PM4/PM5 are separate S_UART1 pairs.
Restoration retains the inherited route reservation until release finishes.
Physical pin overlap also excludes UART6 and SPI3 on PE0/PE1.

The kernel table lists UART muxes on PI0–PI16 and PJ16–PJ21 without a matching
signal map in the cached manual's port tables. The manager refuses those UART
claims and restorations until their route sets are established. Their GPIO
modes and other admitted mux codes remain available. UART operation on any
route still requires the owning UART driver and physical qualification.

## Configuration preserves neighbors and primes output latches

The main PIO block starts at `0x02000000`. Its bank offset is
`0x80 + bank_index × 0x80`, placing B at `0x100`. The always-on block starts at
`0x07025000`; L and M have offsets `0x00` and `0x30`.

| Register group | Main bank-relative offset | Always-on bank-relative offset |
| --- | --- | --- |
| Mux, eight pins per word | `0x00` | `0x00` |
| DATA | `0x10`, read only in the adapter | `0x10`, shadow-controlled writes |
| SET / CLR aliases | `0x14` / `0x18` | Unavailable |
| Drive, eight pins per word | `0x20` | `0x14` |
| Pull, sixteen pins per word | `0x30` | `0x24` |
| IRQ configuration / enable / status / debounce | `0x40` / `0x50` / `0x54` / `0x58` | `0x200` / `0x210` / `0x214` / `0x218` from controller base, bank stride `0x20` |

Acquire disables the requested muxes, updates drive and pull, prepares interrupt
fields or output latches, then selects the final muxes. Configuration updates
preserve neighboring bits and verify the full written word. GPIO output primes
the latch before selecting output mode. Main PIO uses owned-bit SET/CLR writes;
the manual defines these aliases even though the pinned kernel descriptor does
not enable its optional atomic-data helper.

Always-on output requires a complete platform-authenticated latch record for
its bank. The record contains an identity, owner, known-pin mask and initial
value. The platform must establish these values and exclude other DATA writers
for the manager's lifetime. Numeric identities alone do not prove provenance.
The manager refuses output without this handoff. It never initializes the
shadow from input or alternate-function DATA reads, whose values do not identify
latent output latches.

Before an always-on DATA write, the manager compares all currently selected
GPIO outputs with its bank shadow. It changes only leased bits in that shadow
and writes the complete bank value. Missing provenance or an unexpected output
value prevents a write. A failed write retains the intended shadow and all
leases for platform recovery.

GPIO input reads return only a requested subset of leased inputs after checking
their mux, pull, drive and interrupt state. Output writes likewise require
leased GPIO outputs and verify their resulting DATA values. The manual defines
DATA reads in output mode as software latch readback; they do not measure the
voltage on the pad.

Release requires the owning peripherals and handlers to be quiescent. It
verifies the lease, disables its muxes, restores its original fields, primes any
original GPIO outputs with their saved values, and restores original muxes.
An originally non-output pin has no inferred latent-latch value to restore.
Its next output acquisition must prime the latch again. These sequences require
physical qualification for glitches, isolation and external device timing.

## Interrupt leases own lines and coordinate bank-wide debounce

Function 14 selects external interrupt input. New interrupt claims require
masked lines without pending status. Inherited function 14 is refused because
a raw mux description does not transfer a live interrupt configuration.
The handoff must grant the manager exclusive authority over the bank's interrupt
clients and common debounce register.

Trigger codes are 0 rising edge, 1 falling edge, 2 high level, 3 low level and
4 both edges. Debounce bit 0 selects RTC32K or SYS24M; bits 6:4 divide that clock
by a power of two. All interrupt leases in a bank must request the same debounce
value. The first lease saves the inherited value. The last release restores it,
after checking that no unmanaged interrupt is enabled.

Acquisition programs trigger and debounce fields while the lines remain masked.
The caller binds the GIC handler before invoking `a733_pins_irq_enable`.
Pending reads return only leased lines. Acknowledge writes only the requested
owned bits directly to the write-one-to-clear status register. It never uses a
read-modify-write there and never requires a zero readback: a new edge or
asserted level can immediately set the status again.

Release masks and acknowledges only its own lines, preserves other leases, and
restores its original trigger fields. The shared register mutex makes these
operations task-context operations; they are not a blocking interrupt-handler
API. A FreeRTOS service must serialize requests and arrange interrupt handling
without taking this scheduling-aware mutex from an ISR. GIC SPI mappings from
the source device tree are B–K: 69, 71, 73, 75, 77, 79, 81, 83, 85, 87;
L/M: 198/200. GIC interrupt IDs add 32. Physical delivery remains unqualified.

## Uncertain operations retain ownership

Reservation precedes any possible write. A failed access, unexpected owned
field, failed readback or deadline expiry after a write puts the manager in
`RECOVERY`. It retains all leases and refuses further MMIO. Ownership queries
remain available under the lock. There is no automatic rollback, forced release
or timeout-based token expiry.

A deadline reached before mutation may return `BUSY` and remove a provisional
reservation. A late input or pending-status observation leaves its output
argument unchanged. Calls use absolute monotonic microsecond deadlines;
regressing clock values are refused. Lock and MMIO callbacks must obey their
contracts. Synchronous MMIO can stall before C code regains control, so independent
hardware-watchdog recovery remains a prerequisite.

The [I2S platform binding](i2s-platform.md) retains partial pin acquisitions
through its lifecycle. Its existing workload evidence remains applicable to
that unchanged lifecycle. The shared pin state and PB entry points receive
focused compatibility checks when the manager changes.

## Focused evidence and remaining integration

The focused batch compares all mux admissions and register fields with the
pinned main and always-on sources. Its register model exercises Bluetooth/header
UART exclusion, camera groups, mixed PB ownership, GPIO input and output,
always-on latch provenance, interrupt W1C ownership, shared debounce, reentry
and retained failures. It includes the existing PB ownership and deadline
fixtures because their shared manager representation changed. It does not
repeat I2S, DMA, board workloads or performance measurements.

Run the affected batch into a new external evidence directory:

```sh
python3 checks/pins-expanded.py \
  --evidence "$HOME/.cache/sunstoneos/evidence/pins-expanded-20261016/checked"
```

The batch passed once with address and undefined-behavior sanitizers. The
manager, pin table and native adapter also compiled for freestanding AArch64
without warnings. Evidence is preserved in
`~/.cache/sunstoneos/evidence/pins-expanded-20261016/checked/`, including exact
inputs, commands, executables and object hashes. No physical accesses ran.

Production integration still needs authenticated platform
handoffs, the shared FreeRTOS service, GIC binding, unresolved UART signal maps,
independent recovery, and physical qualification. Pin configuration changes no
rail, PMIC, clock or security register. Conflicting voltage assignments in the
board records must be resolved before electrical use.

## Pinned sources

The kernel sources use revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`.
Downloaded sources and documents remain outside Git.

| Source | SHA-256 |
| --- | --- |
| [Common pin controller](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/pinctrl/pinctrl-sunxi.c) | `3bc32a1fd6be151be68979a7c44fe400adb657f21cb66f4e043d31aea4273406` |
| [Register helpers](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/pinctrl/pinctrl-sunxi.h) | `cd345d552621857017e5a34c6c182c3fcc630d2394174c99edd9f2c135e9cab8` |
| [Main pin table](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/pinctrl/pinctrl-sun60iw2.c) | `ab05ddccecd152edf3fafd238ade22299aebe3448646f02f0551edef0c6e1ec7` |
| [Always-on pin table](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/pinctrl/pinctrl-sun60iw2-r.c) | `431fac7b767f5be39015cdf67fdd3dec230cbc1ebeb2fa0c0de08ca0f6e223a1` |
| Source device tree `sun60iw2p1.dtsi` | `1c7313f655bdc4d018201664adcb1fa1d100280388b8975e193ebbfc6ad4ddfa` |
| A733 User Manual V1.00, §§18.14.3 and 18.14.6–7 | `cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d` |
| Orange Pi Zero 3W schematic v1.2 | `4e0e92afbb1cca97a9c8593b8f2679178d9738f242ea431ed3650e893fe28ed0` |

Manual tables 18-29 through 18-39 define the admitted UART route groups.
Main DATA, SET and CLR behavior comes from §§18.14.6.6–8; always-on DATA behavior
comes from §18.14.7.3. The source hardware profiles 4 and 10 agree on main
configuration fields; profile 1 defines the always-on layout.
