# A733 UART controller

The native UART controller implements bounded configuration, FIFO receive and
transmit, automatic RTS/CTS flow control, line-error reporting and interrupt
service. Host checks and an AArch64 compile pass. Physical serial operation,
Bluetooth HCI transport and platform resource binding remain unqualified on hardware.
The [platform binding](uart-platform.md) implements real clock/pin ownership,
local gate/reset control and a native adapter for polling or deferred IRQ service.
This document is for driver and platform developers.

## Controller profiles follow the manual

The public API is [a733_uart.h](../include/a733_uart.h); the implementation is
[a733_uart.c](../drivers/uart/a733_uart.c). Port numbers select these profiles:

| Ports | Physical base | RX and TX FIFO capacity, each | Automatic RTS/CTS | Working clock |
| --- | --- | --- | --- | --- |
| UART0, port 0 | `0x02500000` | 64 bytes | Unsupported | APB_UART |
| UART1–6, ports 1–6 | `0x02501000` through `0x02506000`, stride `0x1000` | 256 bytes | Supported | APB_UART |
| S_UART0–1, ports 7–8 | `0x07080000`, `0x07081000` | 64 bytes | Unsupported | APBS1 |

The format planner accepts five through eight data bits and no, odd or even
parity. It accepts one stop bit, 1.5 stop bits with five data bits, or two stop
bits with six through eight data bits. Baud generation uses the supplied clock
divided by sixteen and an integer divisor from 1 through 65,535.
The caller specifies the permitted relative error in parts per million, up to
30,000. An unsupported format leaves the output plan untouched.
At 24 MHz, 1,500,000 baud uses divisor one with no rounding error.

A733 manual V1.00, section 18.10, supplies the controller capabilities,
register map and programming sequence. Section 4.1 identifies APB_UART and
the separate gate/reset registers for UART0–6. The published
[controller source][core] and [register definitions][header] corroborate
32-bit register access, divisor aliases, forced configuration, FIFO status
and interrupt identification. The implementation preserves their license notices.

## Configuration and shutdown advance without polling loops

`begin` requires a fresh local reset and a quiescent peer. It checks line-control
reset state and rejects active receive/transmit hardware. It disables local
interrupts, enables the FIFO and halts transmission before selecting divisor
registers through the line-control DLAB bit. DLAB changes the meaning of offsets
zero and four from data/interrupt registers to divisor registers.

The driver writes the divisor with forced configuration enabled, then triggers
CHANGE_UPDATE. Each `poll` reads that update once. After hardware clears it,
the driver restores normal data-register access, resets the initially empty
FIFOs, configures flow control and verifies line/modem controls.
It then enables receive, line-error and, when applicable, modem-status interrupts.
The forced-configuration selection remains enabled as in the published driver.

`exchange` services FIFO status directly. `interrupt` handles one interrupt
cause: received data, receive timeout, line status, transmitter empty or modem
status. Each call moves at most its specified budget of receive bytes and the
same budget of transmit bytes; the maximum budget is 256.
`tx_irq` arms THRE without transmitting, so a native application can start TX
from an actual interrupt notification. The caller owns pending buffers and
advances them by the returned counts.
The controller stores no buffer pointers and has no internal transmit queue.

Transmit counts indicate bytes accepted into the FIFO within the deadline.
They do not establish transmission on the wire. The driver arms the transmitter
interrupt while the supplied span remains and disables it when that span is
consumed. A failed or late access may already have transferred its final byte;
the fault prevents automatic retransmission of uncertain data.

Receive processing captures line status before reading each byte. Returned
bytes retain parity, framing, break and overrun flags; the operation and controller
also retain aggregate flags. Reading line status and modem status clears hardware
indications, so every reader must share the same exclusive ownership.
An overrun means incoming data was lost. The driver cannot reconstruct it.
A full caller buffer leaves unread data in the hardware FIFO. The caller must
arrange flow control or accept overflow. With no receive slot, line status
remains unread and its interrupt can remain asserted. The caller must supply
capacity or mask and defer service, then re-enable the interrupt when space exists.

Before `drain`, the caller stops submissions, quiesces the peer and masks and
synchronizes the GIC interrupt. Each poll requires an empty transmit FIFO,
an empty transmit shift register, an empty receive FIFO and idle controller
status. Unread receive data remains intact and prevents completion.
DRAINED permits external resource teardown; FAULT retains every resource lease.
Neither state restores an inherited console configuration.

## Platform ownership must precede register access

The controller takes ordered 32-bit register callbacks and a monotonic
microsecond clock. Each access checks the absolute deadline before and after
the callback. Configuration and drain keep their original deadline across polls.
Clock reversal, expired deadlines, unexpected busy interrupts and uncertain
accesses cause a sticky fault. Further operations perform no register accesses.
A deadline cannot interrupt a stalled bus transaction; independent hardware
watchdog recovery remains mandatory before physical use.

The platform must provide the following prerequisites:

- Exclusive ownership of the selected controller, its pins and required I/O
  rails, including exclusion of console, firmware, other CPU and DMA users.
- A stable, source-supported clock rate matching `clock_hz`, a fresh local
  reset and a quiescent peer. Preserve every other user's shared clock lease.
- Accessible Device mappings for the selected register range, ordered callbacks
  and sufficient privilege for those accesses. The controller does not alter
  translation tables; the platform binding supplies a native memory-mapped adapter.
- Serialized task and interrupt access. An interrupt handler must not wait on a
  task-held blocking mutex. GIC configuration, acknowledgement, masking and
  synchronization remain external.
- Retained ownership after uncertain failure. An external recovery procedure
  must resolve hardware state before resetting or reusing the object.

UART0–6 gates use CCU offsets `0x0e00` through `0x0e18`, stride four, with gate
bit zero and reset bit sixteen. Their shared APB_UART clock uses offset `0x0538`.
The driver does not write those shared resources. The SoC device tree specifies
GIC interrupt IDs 34–40 for UART0–6 and 233–234 for S_UART0–1; its SPI numbers
exclude the first 32 interrupt IDs.

## Bluetooth and header routes share controllers

[Radio wiring](radio.md) assigns UART1 TX/RX to PG6/PG7 and RTS/CTS to PG8/PG9.
The module firmware defaults to 1,500,000 baud with automatic flow control.
The [native Bluetooth application](bluetooth-native.md) now integrates SDIO
firmware bootstrap, reset/wake signaling, this UART binding, H4 packet framing,
HCI commands, pairing and headset audio. Its software evidence is separate from
the UART controller checks. Physical Bluetooth operation remains unqualified.

The board schematic, sheet 18, exposes an alternative UART1 route on PE12/PE13
at header pins 29/31. This route shares the Bluetooth controller and cannot
provide an independent simultaneous serial connection. UART2 PB0/PB1 reaches
header pins 11/13. UART6 PE0/PE1 overlaps SPI3 chip-select/clock at pins 24/23.
Pin ownership must account for those alternatives before changing multiplexers.

The published board device tree leaves UART0 pin names empty. Its UART3 active
route names PI1 while its sleep route names PI11. Those entries do not provide
an accepted pin-programming plan. Console routing and additional exposed routes
require reconciliation against the schematic and pin-function tables.

## Validation covers software behavior

The [focused runner](../checks/a733-uart.py) checks register offsets against the
pinned source and compares the complete configuration write trace with its
source-derived sequence. An AddressSanitizer/UndefinedBehaviorSanitizer model
checks format refusal, update completion, FIFO backpressure, byte budgets,
line errors, receive timeout, modem status, transmit-interrupt disable,
unread-receive retention and sticky failures. A focused capacity case confirms
that an exhausted caller buffer preserves unread line-error flags. A separate focused case verifies
that FIFO empty alone does not complete drain while the shift register is busy.
The controller compiles as freestanding AArch64 C.

Evidence and exact inputs are under
`~/.cache/sunstoneos/evidence/uart-20261016/`. Physical qualification requires
independent recovery, authenticated platform admission, a reference serial peer,
flow-control wiring and a coordinator-owned board test. DMA, RS-485, IrDA,
nine-bit modes, runtime format changes and suspend/resume remain required
controller extensions; they are outside the implemented FIFO path.

[core]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/uart/sunxi-uart-ng-core.c
[header]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/uart/sunxi-uart-ng.h
