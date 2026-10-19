# Finite native UART2 header exchange

The UART2 application implements one finite transmit and receive operation on
header pins 11 and 13, connected by a passive wire between PB0/TX and PB1/RX.
It uses the existing native UART controller, clock registry, pin manager,
FreeRTOS task adapter and deferred interrupt service. The implementation and modeled checks pass; physical
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

The task services one UART interrupt cause with a budget of 32 bytes per call.
Its fixed
operation deadline is 250 milliseconds through ten seconds from the board
session start. Each operational callback checks the board lifetime and shared
clock health before register access. Reads, writes and time observations also
check health after the callback returns. The independent board lifetime bounds
later observation of a completed result.

A mask-only ISR handles UART2 INTID 36 through the existing FreeRTOS
dispatcher. It masks the owned source and publishes atomic pending metadata.
The designated task waits for completed disable propagation and an inactive
handler before reading IIR and servicing RX or TX. It arms THRE without writing
the FIFO, so the first transmit requires an actual interrupt notification.
There is no FIFO polling fallback. After service, the task rearms the source
under the same kernel critical section used by the ISR. A newly published
notification defers rearm without discarding work. At most 128 notifications
are admitted; exceeding that bound retains resources.

The passive wire has no independent transmitter. After all requested bytes
have returned without line errors and match the copied request, the task stops
submissions and requests close. The platform authenticates STOP admission,
checks FIFO and shift-register drain, asserts reset, removes the gate, restores
the pins and releases the clock token. Only completed close permits success.
Close stops IRQ rearm and again requires completed masking and handler
synchronization before drain. The software checks exercise this IRQ path;
physical interrupt delivery remains unqualified.

Uncertain access, lost authority, absent or incorrect echo, line errors and
incomplete drain retain every unreleased UART resource. The task never repeats
an uncertain write or attempts hardware recovery. Metadata retention remains
available to the actual owner after operational health loss. A failed result
contains no success payload; completed byte counts remain in private application
storage for diagnosis.

## USB capture returns owned copies

The application copies its final observation into 144 private bytes: observation
time, actual baud, completed transmit and receive counts, aggregate line errors,
port and closed status. It also includes 32 request bytes, 32 received bytes,
32 per-byte status values, and notification, TX, RX and other interrupt-cause counts.
Success requires at least one TX and one RX interrupt service and an exact
match between notification and serviced-cause totals. A SHA-256 digest binds
that payload to a 128-byte USB response header with wire version 2.
The header also binds the session digest, epoch and generation.

[The preparation tool](../apps/uart/prepare.py) seals the existing supervised
board inputs and builds the existing SRAM launcher with UART session geometry.
[The capture tool](../apps/uart/capture.py) uses the existing supervised host
transport. It preserves each frame before decoding, accepts success only with
matching identities, exact byte and IRQ counts, correct echo, zero line errors and closed
status, and stops after the first transport failure. The native USB PID remains
`1f3a:efe8`, with UART-specific product and serial strings.

Successful UART teardown releases its local resources. The inherited board USB
and shared providers retain the supervised board session until its reviewed
whole-board recovery condition. Software fixtures identify themselves as models;
their admitted fields do not establish actual board or electrical authority.

## Offline evidence and remaining qualification

Evidence for deferred IRQ service is under
`~/.cache/sunstoneos/evidence/uart-irq-20261019/`.
The linked native ELF SHA-256 is
`7d112182035d38de6c8e448f9107e55f24adead928e5f69abad820165d33c4b5`.
The existing snapshot tool preserves compiled dependencies, native artifacts
and selected check outputs under `frozen/`.

The added checks cover:

- Actual platform/clock/pin synchronization under AddressSanitizer and
  UndefinedBehaviorSanitizer: GIC disabled, inactive and RWP observations,
  THRE arm without TX, IIR TX/RX-timeout/overrun service and retained failures.
- The actual ISR, deferred task and FIFO driver with modeled delivery: no
  polling fallback, active-handler/RWP deferral, exact copied IRQ counts,
  corrupt/absent echo, line errors, uncertain TX, bounded interrupt storms
  and retained close/rearm failure.
- The rearm race found during focused Astra review: an IRQ published between
  the task's pending check and rearm defers service without false failure.
  The correction passed its focused sanitizer case and review to closure.
- Production preparation and durable host capture for wire version 2, rejecting
  polling responses, absent TX/RX IRQs and inconsistent notification totals.
- Native linking and root-owned instruction checks of the fixed IRQ route and
  dispatcher. Every one of the eleven selected cases passes. Those records
  are under `root/linked-check/`; they model execution rather than physical delivery.

Prior polling, session-copy, native callback lifetime, clock-reference and
pin/controller evidence remains valid for unchanged behavior under
`uart-native-20261018/`. The selected IRQ checks do not repeat those workloads.
The default Bluetooth binding retains polling mode. Embedding consumers must
rebuild against the changed platform configuration header.

The checks model device and USB observations. They do not establish electrical
compatibility, physical baud, drain timing, interrupt operation, multicore
startup, cache visibility, recovery or serial transfer on either board. No
hardware, USB or queue operation was performed for this work.

Physical qualification requires the passive PB0/PB1 connection, verified header
routing and stable bank voltage, exclusion of UART0/SPI2/I2S and remote writers,
a cold UART2 controller, an admitted clock profile and identity Device mappings.
Qualification also requires exclusive Group1NS level IRQ36 routed to the boot
CPU at configured priority, qualified inherited startup and a reviewed
independent recovery procedure.
These conditions remain accessory- and board-dependent; no launch-admitted
physical bundle is claimed.
