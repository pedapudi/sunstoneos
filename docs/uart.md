# A733 UART controller

The native UART controller implements bounded configuration, FIFO receive and
transmit, automatic RTS/CTS flow control, line-error reporting, interrupt
service, a finite DMA lease on every port, and IrDA SIR, RS-485 9-bit and
internal loopback line modes. Host checks and an AArch64 compile pass. Physical serial operation,
Bluetooth HCI transport and platform resource binding remain unqualified on hardware.
The [platform binding](uart-platform.md) implements real clock/pin ownership,
local gate/reset control and a native adapter for polling or deferred IRQ service.
The [finite DMA application](uart-application.md#finite-dma-transfers) uses DMA0
request 16 with bounded caller inputs up to 1024 bytes. Software checks execute
the actual driver, native MMIO policy, DMA service/controller and copied result.
Physical UART DMA remains unqualified. DMA release is terminal for the
controller object: after release only drain and close are accepted, because
interrupt enables stay cleared and the FIFO stays in DMA mode.
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

Every FIFO control write uses one fixed receive trigger per port. UART1–6 have
256-byte FIFOs and trigger at half full (FCR 0x81, or 0x87 with both FIFO
resets). UART0 and S_UART0–1 have 64-byte FIFOs and keep the one-character
trigger (FCR 0x01 or 0x07). The pinned Linux driver also uses the half-full
trigger above 9600 baud. With automatic flow control, the controller deasserts
RTS when the receive FIFO reaches the trigger level. A one-character trigger
would therefore stop the Bluetooth module after almost every byte at
1,500,000 baud. In interrupt mode a burst shorter than the trigger level ends
with a receive-timeout interrupt (IIR cause 12), which the driver services as
received data. The native binding accepts only the planned values for the
port, plus the DMA-mode value 0x09. The trigger behavior of this
controller instance has not been measured on the board.

Bluetooth uses UART1 without DMA. Each H4 transport step reads at most 64
bytes, and the Bluetooth task polls once per 1 ms tick. Identification,
configuration and pairing step the transport once per poll, about 64 KB/s.
Service discovery and hands-free control step it twice per poll, and
synchronous audio three times, up to about 192 KB/s. The line rate at
1,500,000 baud is 150 KB/s. Received audio needs about 16 KB/s for CVSD and
8 KB/s for mSBC. UART1 RTS/CTS is wired to PG8/PG9, and the Bluetooth profile
requires automatic flow control, so a slower drain throttles the module instead
of overflowing the receive FIFO. The driver permits a DMA lease on UART1, but
the Bluetooth application does not use it: its receive path has no current
throughput requirement. Physical throughput has not been measured.

`exchange` services FIFO status directly. `interrupt` handles one interrupt
cause: received data, receive timeout, line status, transmitter empty or modem
status. Each call moves at most its specified budget of receive bytes and the
same budget of transmit bytes; the maximum budget is 256.
`tx_irq` arms THRE without transmitting, so a native application can start TX
from an actual interrupt notification. The caller owns pending buffers and
advances them by the returned counts.
The controller stores no buffer pointers and has no internal transmit queue.

Transmit counts report the bytes each FIFO batch moved. A batch that finishes
after the deadline still reports its bytes and then faults the controller.
The counts do not establish transmission on the wire. The driver arms the transmitter
interrupt while the supplied span remains and disables it when that span is
consumed. A failed access or a late batch may already have transferred its final byte;
the fault prevents automatic retransmission of uncertain data.

Receive processing captures line status before reading each byte. Each receive
or transmit FIFO batch checks the deadline once before and once after the batch
rather than around every register access; a failed access still faults at once.
The register filter has no access to the receive FIFO level register, so the
driver still reads line status once per received byte. Returned
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

## DMA leases use each port's request line

`a733_uart_dma_prepare` accepts any of the nine controllers. Manual table 13-2
assigns DMA0 requests 14–20 to UART0–6 and 50–51 to S_UART0–1;
`a733_uart_dma_request` returns them, and the board device tree matches for
UART1–6 and S_UART0–1. The device tree gives UART0 no DMA request, so its value
rests on the manual alone. Every port uses the same sequence: requests
disabled and read back, interrupts disabled, handshake mode `0xe5`, FIFO
control 0x09 (DMA mode with the one-character receive trigger) and HALT `0xc2`.
With automatic flow control, the one-character trigger deasserts RTS while a
received byte waits for DMA, so the peer pauses between bytes until the channel
reads. Only the [UART2 application](uart-application.md#finite-dma-transfers)
integrates DMA channels; another port needs its own channel, memory and request
admission.

## Line modes change only on a quiet line

`a733_uart_mode` selects one of four line modes in READY, without a DMA lease
and with the transmit interrupt disarmed. It first requires an empty transmit
FIFO and shift register, an empty receive FIFO and an idle controller; a busy
line leaves the mode unchanged and returns READY. It writes and reads back the
modem control register, and for RS-485 the RS-485 control register at `0xc0`.
A readback mismatch faults. The mode persists until another call or the
external reset at close.

- **UART** restores the planned modem control (`0x22` with flow control, else 0).
- **Loopback** adds MCR bit 4. The transmitter output stays idle and data,
  RTS and DTR loop back internally, which permits a self-test without a peer.
- **IrDA SIR** writes MCR function 1 (`0x40`). It requires a plan without
  flow control and an actual rate of at most 115200 baud within the 0.87% SIR
  tolerance; 115200 from 24 MHz gives 115384 baud. The external infrared
  transceiver is outside the driver.
- **RS-485** writes MCR function 2 with automatic flow control (`0xa2`) and
  RS-485 control `0x18`: half duplex (bit 4) and the driver's RXBFA bit
  (bit 3). This is the pinned `sunxi-uart-ng` hardware 9-bit configuration. It requires
  UART1–6 with RTS/CTS routed, eight data bits and parity. The RTS output
  drives the transceiver enable. The readback compares bits 7, 4 and 2–0 of
  the RS-485 control register. It leaves out the write-one-to-clear
  address-detect status bits 6 and 5 and the reserved bit 3, which may read
  back zero on silicon. The parity select supplies the ninth transmit
  bit: even parity writes THR bit 8 set, odd parity leaves it clear, as the
  pinned driver does. Received bytes return RBR bit 8 in status bit 0
  (`A733_UART_NINTH_BIT`).

Two sources disagree about RS-485 and 9-bit support. The manual lists both, but
the board device tree declares `allwinner,uart-v100`, whose pinned driver data
disables hardware RS-485 control and 9-bit mode; that driver would use a GPIO
transceiver enable instead. The manual also marks RS-485 control bit 3 as
reserved. Physical qualification must therefore establish the RS-485 mode,
including whether parity errors accompany address bytes, before an application
relies on it. IrDA and loopback have no such conflict but are also physically
unqualified.

## Platform ownership must precede register access

The controller takes ordered 32-bit register callbacks and a monotonic
microsecond clock. Each configuration and status access checks the absolute
deadline before and after the callback; FIFO data batches check it around the
batch. Configuration and drain keep their original deadline across polls.
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

The [focused runner](../checks/a733-uart.py) checks register offsets and
line-mode values against the pinned source and compares the complete
configuration write trace with its source-derived sequence. An AddressSanitizer/UndefinedBehaviorSanitizer model
checks format refusal, update completion, FIFO backpressure, byte budgets,
line errors, receive timeout, modem status, transmit-interrupt disable,
unread-receive retention and sticky failures. A focused capacity case confirms
that an exhausted caller buffer preserves unread line-error flags. A separate focused case verifies
that FIFO empty alone does not complete drain while the shift register is busy.
A line-mode model covers RS-485 with both parity selects, the ninth bit in
both directions, the return to UART mode, loopback, IrDA at 115200 baud, every
refusal and a busy line, and readback faults. The DMA lease model runs on UART1,
UART2 and S_UART1. The controller compiles as freestanding AArch64 C. The
linked native write policy, including the ninth-bit rule that follows the last
modem control and line control writes, is checked separately on the UART DMA
image by [uart-native-fcr.py](../checks/uart-native-fcr.py). The
[platform composition check](uart-platform.md#validation-composes-the-real-software-components)
runs the line modes and the DMA lease on UART1 and S_UART0 through the
binding's register filter.

Evidence and exact inputs are under
`~/.cache/sunstoneos/evidence/uart-20261016/`. Physical qualification requires
independent recovery, authenticated platform admission, a reference serial peer,
flow-control wiring and a coordinator-owned board test. DMA applications on
ports other than UART2, physical RS-485 and IrDA transceivers, runtime format
changes and suspend/resume remain required.

[core]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/uart/sunxi-uart-ng-core.c
[header]: https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/uart/sunxi-uart-ng.h
