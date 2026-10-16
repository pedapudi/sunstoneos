# Native SPI master transfers

The [controller](../drivers/spi/a733_spi.c) implements single-lane, eight-bit
master transfers on SPI0–4 and S_SPI. It supports clock modes 0–3, either bit
order, either chip-select polarity, full duplex, writes, reads and command-response
transactions. Host checks and native compilation pass. Physical SPI operation
remains unqualified.

## Controller resources and board routes

| Controller | Register base | GIC interrupt ID | Module clock / bus-reset offset |
| --- | --- | --- | --- |
| SPI0 | `0x02540000` | 56 | Main CCU `0x0f00` / `0x0f04` |
| SPI1 | `0x02541000` | 57 | Main CCU `0x0f08` / `0x0f0c` |
| SPI2 | `0x02542000` | 58 | Main CCU `0x0f10` / `0x0f14` |
| SPI3 | `0x02543000` | 59 | Main CCU `0x0f20` / `0x0f24` |
| SPI4 | `0x02544000` | 60 | Main CCU `0x0f28` / `0x0f2c` |
| S_SPI | `0x07092000` | 242 | R_CCU `0x0150` / `0x015c` |

Main CCU is at `0x02001000`; R_CCU is at `0x07010000`. Every listed bus word
has gate bit 0 and reset-deassertion bit 16. The module word contains gate bit
31, source bits 24–26, and linear divider fields at bits 0–4 and 8–12.

The board exposes SPI3 on PE0–4, function 5. PE0 is CS0, PE1 is clock, PE2 is
MOSI, PE3 is MISO and PE4 is CS1. Acquire the data pins and selected CS only.
UART6 shares these pins; PE4 also supports LEDC. The board describes SPI0 on
PC12/2/4/3 for clock/MOSI/MISO/CS0. Its storage and boot ownership must be
resolved before any transaction. Other controller routes require board-specific
pin and electrical admission. A controller number does not establish an exposed
or available bus.

## Clock and ownership admission

The [clock planner](../include/a733_spi.h) selects the fastest realizable rate
below a supplied maximum. It uses a stable 24 MHz source and two divisors in
1–32. SPI0–4 select SYS_CLK24M; S_SPI selects DCXO. The exact wire rate is
`24000000 / divisor`; `actual_hz` reports its integer floor.

Accepted maxima range from 24194 Hz to 24 MHz. The lowest selected rate is
approximately 24193.548 Hz. A maximum of 24000 Hz is refused because the next
available divisor would produce 23437.5 Hz, below the documented operating range.
Old normal sampling is used throughout this range.

The [native binding](../include/a733_spi_native.h) requires an authenticated,
privileged platform reservation. It compares every requested format and clock
field with that reservation, then reads the actual clock, reset and GIC state.
The interrupt must be masked and inactive. The reservation must also cover the
parent source, selected pins, rail, recovery and exclusion of remote users.
A production reservation provider remains to be implemented.

The caller programs the clock under the common register mutex before beginning.
The driver soft-resets only an exclusively owned, disabled, idle controller.
It changes no shared clock, pin, power or reset register. Native register accesses
require Device-nGnRnE mappings. Independent watchdog recovery is required because
a software deadline cannot interrupt a stalled bus access. Initial pin handoff
must establish the peripheral's inactive CS level before controller ownership.

## Transfer and buffer ownership

A transaction borrows disjoint TX and RX buffers until explicit completion
reaping. Command-response transfers hold CS across both parts. Software controls
CS and observes minimum setup and hold times. Each poll moves at most 64 received
bytes and 64 transmitted bytes; smaller budgets are accepted. FIFO starvation
can stretch the clock.

The receive count includes both FIFO and internal buffer contents. The manual
permits 192 bytes; Linux's 128-byte receive size controls its software thresholds
and does not bound the observed hardware count. Completion requires the transfer
interrupt, XCH clear, all requested bytes accounted for, empty FIFO/internal
buffers and no error. TX FIFO acceptance alone does not establish completion or
peripheral acknowledgement.

Timeout, access uncertainty, ownership loss and hardware errors retain the
controller and borrowed buffers in a terminal fault state. Received bytes remain
invalid after failure. The driver does not automatically retry, abort, reset or
release resources. A successful close disables the idle controller; the platform
still owns interrupt, pin and clock release.

## Sources and focused evidence

Register semantics follow A733 manual sections 18.8 and 18.9. Controller versions,
CCU fields, interrupts and board routes follow the pinned
[board Linux source](https://github.com/orangepi-xunlong/linux-orangepi/tree/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f).
The relevant controller is `bsp/drivers/spi-ng/spi-sunxi.c` and its header.
The [upstream controller](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/70dd4ee72eb2d5793c62e71c14e3aba78e5a4e56/drivers/spi/spi-sun6i.c)
uses an older internal divider at offset `0x24`; that register is absent from
the A733 byte-transfer path.

Evidence is stored under `~/.cache/sunstoneos/evidence/spi-20261016/`:

- `batch1` preserves the passing source comparison and sanitized host model.
  Cases cover wire data, repeated transfers, CS timing, full receive capacity,
  combined completion/error status, uncertain accesses and retained ownership.
- `native-correction` preserves passing native authorization, CCU/GIC admission
  and access-allowlist checks, plus both AArch64 objects. The initial native
  harness had an unsigned-loop overflow; only the corrected check and previously
  unrun compilation were executed afterward.

The native C check uses anonymous host mappings and replaces four `dsb sy`
statements with compiler memory barriers. Its archive records both source
identities. It establishes C admission behavior, without executing physical
MMIO or validating Arm barrier effects. The original sources compile for AArch64.

## Remaining required support

Native pin/clock/rail acquisition and recoverable shutdown precede physical
transactions. Qualification needs a connected peer, all supported wire formats,
error recovery, shared-pin conflicts and concurrent workloads. Boot storage must
remain protected.

DMA, interrupt-driven scheduling, calibrated high rates, slave operation,
dual/quad transfers, three-wire and bit-aligned transfers, SPI1 DBI operation,
power transitions and applicable flash protocols remain required. The separate
SPIF controller is outside this driver's register aperture and remains in scope
for its own implementation. No benchmark has run.
