# SPI storage depends on an identified board connection

The board manual V1.0, section 3.16.3, describes SPI3 header loopback.
Section 5.10.3 demonstrates a W25Q64 module that the reader first connects to
SPI3. This example does not establish fitted onboard SPI flash.

The pinned board device tree disables SPI0 while declaring a SPI NOR child
under it. It also disables SPI3 and provides two generic userspace children.
Controller and child status describe a software configuration; they do not
establish component population. No present board observation identifies SPI
storage, its supply, capacity or electrical compatibility.

The existing native SPI3 application uses PE0–3: CS0, clock, MOSI and MISO.
PE4 provides the alternate CS1 route. The platform's existing BLDO1 admission
requires 1.8 V. UART6 shares PE0/PE1, and TWI2/TWI3 share PE1–4. Electrical
testing needs the actual peer or passive loopback, a verified voltage domain,
and the exclusive pin lease. A module intended for 3.3 V requires an appropriate
electrical interface before connection.

The board device tree gives SPI3 a 100 MHz controller ceiling and limits each
generic child to 500 kHz. The effective vendor driver selects the peer or
transfer rate in `sunxi_spi_bus_setup` and retunes the module clock through
`sunxi_spi_set_clk`. The native application's 500 kHz profile matches that
generic child's limit. It does not establish a fitted storage device's top rate.
The native planner uses a normalized 24 MHz parent and caps requests there;
a faster identified peer requires a source-backed extension and coordinator
clock-point review before a run.

The DMA profile sends an authenticated command of 1–16 bytes and receives
4096 bytes. Success requires an actual SPI IRQ notification, controller wire
completion, request shutdown, both DMA channel drains, RX synchronization and
a copied result matching an independent peer reference. A GPIO wire joining
MOSI and MISO does not supply the command-response behavior of an identified
storage device.

Before adding a storage protocol, the coordinator must establish an actual
device or exposed connection, its identity, voltage, command semantics and
recovery boundary. A JEDEC identification command is specific to a compatible
peer; its opcode is not general authority to drive an unidentified device.
Storage writes require the accepted storage-preservation policy and explicit
session authority.

The references are `board-manual`, `board-schematic`, and
`board-gpu-power-sun60i-a733-orangepi-zero3w-dts` in
[the source lock](../../sources.lock.json). The manual sections and device-tree
nodes were checked against the cached files with the recorded SHA-256 values.
The effective clock sequence comes from the same lock's `a733-spi-ng-c` entry.
The documents and dual-licensed GPL/MIT device tree are host-side development references.
Physical population and PCB revision remain unresolved for each board.
