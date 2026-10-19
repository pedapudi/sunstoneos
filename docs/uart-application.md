# Finite native UART2 header exchange

The UART2 application implements one finite transmit and receive operation on
header pins 11 and 13, connected by a passive wire between PB0/TX and PB1/RX.
It uses the existing native UART controller, clock registry, pin manager and
FreeRTOS task adapter. The implementation and modeled checks pass; physical
serial operation remains unqualified.

The application serves platform developers. Its private build entry is
[apps/uart/CMakeLists.txt](../apps/uart/CMakeLists.txt), which includes
[the UART build fragment](../cmake/a733-uart.cmake). It inherits the existing
supervised cached board application and its eight-core startup. A single UART
task does not remove that physical multicore qualification dependency. The
application adds no CPU startup or recovery implementation.

## One task owns the complete finite operation

A sealed 832-byte session contains the board prefix, distinct UART owner and
handoff identities, four physical-evidence digests, expected and desired pin
settings, and a request of one through 32 bytes. The receive count must equal
the transmit count. Only PB0 and PB1 are admitted; unused pin fields and request
padding must be zero. The baud is fixed at 115200, with eight data bits, no
parity, one stop bit and no automatic flow control. The 24 MHz clock produces
115384 baud through divisor 13, within the admitted 2000 ppm error limit.

Startup validates the complete session and contract digest, then copies the
session before constructing providers. The board pin handoff authorizes PB0
and PB1. The real platform binding reserves UART2 and its APB_UART/reference
ancestry, authenticates the designated EL1 task and physical admission, masks
interrupt 36, acquires both pins and configures the controller.

The task polls FIFO status with a budget of 32 bytes per call. Its fixed
operation deadline is 250 milliseconds through ten seconds from the board
session start. Each operational callback checks the board lifetime and shared
clock health before register access. Reads, writes and time observations also
check health after the callback returns. The independent board lifetime bounds
later observation of a completed result.

The passive wire has no independent transmitter. After all requested bytes
have returned without line errors and match the copied request, the task stops
submissions and requests close. The platform authenticates STOP admission,
checks FIFO and shift-register drain, asserts reset, removes the gate, restores
the pins and releases the clock token. Only completed close permits success.
The UART interrupt remains masked throughout; this path does not qualify an
interrupt-driven application.

Uncertain access, lost authority, absent or incorrect echo, line errors and
incomplete drain retain every unreleased UART resource. The task never repeats
an uncertain write or attempts hardware recovery. Metadata retention remains
available to the actual owner after operational health loss. A failed result
contains no success payload; completed byte counts remain in private application
storage for diagnosis.

## USB capture returns owned copies

The application copies its final observation into 128 private bytes: observation
time, actual baud, completed transmit and receive counts, aggregate line errors,
port, closed status, 32 request bytes, 32 received bytes and 32 per-byte status
values. A SHA-256 digest binds that payload to a 128-byte USB response header.
The header also binds the session digest, epoch and generation.

[The preparation tool](../apps/uart/prepare.py) seals the existing supervised
board inputs and builds the existing SRAM launcher with UART session geometry.
[The capture tool](../apps/uart/capture.py) uses the existing supervised host
transport. It preserves each frame before decoding, accepts success only with
matching identities, exact counts, correct echo, zero line errors and closed
status, and stops after the first transport failure. The native USB PID remains
`1f3a:efe8`, with UART-specific product and serial strings.

Successful UART teardown releases its local resources. The inherited board USB
and shared providers retain the supervised board session until its reviewed
whole-board recovery condition. Software fixtures identify themselves as models;
their admitted fields do not establish actual board or electrical authority.

## Offline evidence and remaining qualification

Evidence is under `~/.cache/sunstoneos/evidence/uart-native-20261018/`.
The linked native ELF SHA-256 is
`15fb2b6eedfbe33e454ecf225f21b586cc72cc5c6be73a77626c63eb8c1ecf19`.
The linked-object dependency record covers 165 objects and 419 compiled inputs.
The existing snapshot tool preserves those inputs, native artifacts and selected
check outputs under `frozen/`.

The selected checks cover:

- Actual platform, clock, pin and FIFO composition under AddressSanitizer and
  UndefinedBehaviorSanitizer, including normalized 19.2 and 26 MHz oscillator
  profiles and refusal of a non-24 MHz direct S_UART source.
- The finite application with the actual FIFO driver: copied completion after
  close, absent and corrupt echo, line errors, uncertain write without retry,
  and retained close failure.
- Actual native application callbacks with board failure during a write and
  clock failure during a read, blocking further MMIO while preserving metadata
  retention.
- Actual session and contract validators, full digest refusals, copied-session
  lifetime and publication refusal, plus production launcher preparation,
  bounded-result decoding, durable capture and first-transport-failure stop.
- Native linking against the inherited board composition. Focused Astra review
  confirmed the per-access lifetime checks and asynchronous STOP admission.

The checks model device and USB observations. They do not establish electrical
compatibility, physical baud, drain timing, interrupt operation, multicore
startup, cache visibility, recovery or serial transfer on either board. No
hardware, USB or queue operation was performed for this work.

Physical qualification requires the passive PB0/PB1 connection, verified header
routing and stable bank voltage, exclusion of UART0/SPI2/I2S and remote writers,
a cold UART2 controller, an admitted clock profile, identity Device mappings,
qualified inherited startup and a reviewed independent recovery procedure.
These conditions remain accessory- and board-dependent; no launch-admitted
physical bundle is claimed.
