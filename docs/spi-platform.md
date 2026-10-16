# SPI acquires shared resources before controller use

The [platform binding](../drivers/spi/a733_spi_platform.c) connects the native
[SPI controller](spi.md) to the common clock registry and banked pin manager.
It acquires actual tokens, programs the selected controller's clocks and reset,
initializes idle chip select, then enables the selected pin route. A successful
close restores inherited pin and clock fields before releasing ownership.
Host composition checks and AArch64 compilation pass. Physical operation remains
unqualified.

## Admitted routes and resource conflicts

| Route | Controller | Mux | Clock / MOSI / MISO | Chip selects |
| --- | --- | --- | --- | --- |
| PC | SPI0 | 5 | PC12 / PC2 / PC4 | CS0 PC3; CS1 PC7 |
| PD | SPI1 | 6 | PD11 / PD12 / PD13 | CS0 PD10; CS1 PD15 |
| PB | SPI2 | 4 | PB1 / PB2 / PB3 | CS0 PB0; CS1 PB4; CS2 PB5; CS3 PB6 |
| PE | SPI3 | 5 | PE1 / PE2 / PE3 | CS0 PE0; CS1 PE4 |
| PL | S_SPI | 4 | PL5 / PL6 / PL7 | CS0 PL4 |
| PM | S_SPI | 3 | PM1 / PM2 / PM3 | CS0 PM0 |

Each request acquires four pins: clock, MOSI, MISO and the selected CS. Pins
must initially be inputs or disabled. The privileged handoff approves their
pull/drive settings, bank voltage and external electrical state. SPI0 requires
specific exclusion of storage and boot users. SPI1 PD pins overlap display
signals. SPI2 overlaps UART2 and I2S; SPI3 overlaps UART6 and LEDC. Always-on
routes overlap debug, UART and I2C functions.

Every alternate pad route and unselected CS output of the selected controller
must also be excluded before enabling it. Subsequent SPI mux writers must honor
the controller's exclusive clock-registry scope. The four-pin lease does not
independently establish this controller-wide exclusion.

SPI4 retains its controller descriptor and exclusive registry scope. Linux
assigns PI2–7/9 mux 4 to SPI4, but the published A733 manual and board schematic
do not establish those package signals. This binding refuses SPI4 pin activation.
Its usable package route remains an explicit research and qualification item.

## Startup and shutdown use actual ownership

The registry supplies an exclusive controller token and shared fabric/parent
stability. Main SPI controllers reserve REF and OSC for SYS_CLK24M. S_SPI
reserves OSC for DCXO. The binding reads the oscillator and reference profile;
it does not retune a shared parent.

A privileged boot handoff separately establishes recovery, mappings, stable
24 MHz input, electrical safety, remote-firmware exclusion and absence of DMA
or old handlers. The native adapter verifies the actual FreeRTOS owner task and
common register-mutex ownership. The boot authority provider remains a platform
prerequisite; numeric handoff identifiers do not establish physical authority.

Startup requires a gated controller with reset asserted and a masked, nonpending
interrupt. It programs the selected clock divider, enables the bus, holds reset
for at least 10 microseconds, then deasserts reset. The source-defined reset
interval comes from the pinned CCU reset implementation.

The existing SPI core initializes while pins remain disconnected. No application
transfer occurs during startup. Complete TCR readback establishes CS ownership,
selector, polarity and mode before the pin manager selects the route. FIFO and
internal-buffer emptiness are also checked. This avoids exposing reset-default
CS polarity to an active-high peripheral.

Application calls use platform submit, poll and reap wrappers. The underlying
controller is private. Each new command validates the live registry token,
pin ownership, pin-manager health and actual native clock/interrupt state.
Idle polling returns cached READY or COMPLETE; it does not extend a completed
operation's deadline. Submit, reap and close supply fresh absolute deadlines.
Successful reap releases borrowed buffers after every fallible admission check.

Close requires READY with no borrowed buffers. It verifies inactive CS and
empty FIFO/internal buffers, restores pins while the controller still drives
its inactive state, then disables and resets the controller. It restores owned
clock/reset fields, clears the owned pending interrupt and releases the registry
token. Neighboring fields remain intact. The original interrupt remains masked.

A timeout, uncertain access, failed pin manager or invalid ownership retains
every unreleased token. Uncertainty while holding the common mutex retains that
mutex too. After a terminal fault, polling retries only registry-retention
metadata; it does not repeat hardware writes. A successful pin release remains
final even if a later step fails. Copied token records do not recreate ownership
already released by their manager.

## Evidence and remaining work

The focused evidence is
`~/.cache/sunstoneos/evidence/spi-platform-20261016/silicon-source/`.
It archives source dependencies, executables, logs, native objects and identities.

The sanitized host composition uses the actual clock registry, banked pin
manager and SPI core. It covers all admitted route entries, one four-byte read,
high-bit controller ownership, clock/pin conflicts, authorization waiting,
CS readback refusal, pin-health failure, uncertain writes, buffer-release timing
and resource restoration. Existing detailed wire/FIFO checks are reused.
The source checker selects the real-silicon pin table explicitly; an initial
FPGA-table selection failed before runtime checks ran.

Both platform objects compile for AArch64 with the actual FreeRTOS headers.
Compilation does not establish native task-state checks or physical MMIO
execution. Source references are the pinned A733 manual, board device tree,
real-silicon pinctrl tables, CCU tables and `ccu_reset.c` identified in the
component evidence and central source lock.

Physical acceptance requires independent watchdog recovery, authenticated boot
and electrical admission, connected peers, pad exclusion and actual pin/clock
restoration. Remaining work includes SPI4 package routing, additional alternate
routes, wider SPI modes, DMA, interrupts, calibrated high rates and power
transitions. The separate SPIF controller remains required. No board access or
benchmark ran for this implementation.
