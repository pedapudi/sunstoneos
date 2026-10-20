# Supervised native SPI3 write-read exchange

The application connects the existing SPI core, platform and native adapter to
the board providers and copied USB output. One task starts SPI3 on its documented
PE header route, sends one authenticated command and receives a bounded response
under one CS assertion. The application has no assumed peer or opcode. Physical
operation remains unqualified.

## The header route requires 1.8 V electrical admission

The pinned board schematic V1.2 sheet 18 exposes SPI3 MOSI PE2 on header pin 19,
MISO PE3 on pin 21, clock PE1 on pin 23 and CS0 PE0 on pin 24. Sheet 9 identifies
the function-5 mux. CS1 PE4 on pin 26 is excluded. Other SPI3 alternate pad routes
and UART6/LEDC users must also be excluded before the controller is enabled.
The schematic establishes no populated SPI peripheral on this route. SPI0 has
storage/boot conflicts; SPI4 package routing remains unresolved.

Sheet 2 supplies PE from BLDO1 at 1.8 V. The header's 3.3 V power pins do not
establish SPI signal compatibility. Admission must identify the actual peer,
translator if needed, common ground, power domains, inactive CS bias and safe
signal levels. The runtime reserves BLDO1 through the shared rail ledger and
requests an actual read-only PMIC snapshot. Page, chip, revision, enable and
selector checks must establish the programmed 1.8 V setting before controller
startup. That observation proves no measured voltage, settling or wiring.

## One retained owner performs the exchange

The fixed mode is SPI3/CS0, mode 0, MSB first, active-low CS, a 500 kHz maximum
and minimum 2 microsecond setup/hold times. The existing 24 MHz clock planner
selects 500 kHz exactly. The immutable request contains 1–16 command bytes and
requests 1–32 response bytes. WRITE_READ holds CS across both portions; that
direction alone says nothing about a peer command's effects. Image/target-bound
entry records must authorize the exact bytes, selected peer protocol and effects.
A command with persistent or unsafe effects is outside this diagnostic profile.

The complete 832-byte session binds those request bytes and lengths, inherited
and desired pin fields, ownership, deadline and physical evidence. Common and
full application seals retain authenticated copies before any provider use.
Prefix consistency gates pin authority and constructor entry. Private TX/RX
buffers remain allocated for the entire supervised lifetime.

The native adapter enforces the real privileged owner task, scheduling state,
register mutex, selected-controller aperture and FIFO access width. Application
callbacks additionally check independent atomic liveness before and after I/O.
Actual clock-registry tokens reserve SPI3 exclusively and shared parent/fabric
stability; the common pin manager acquires PE0–3. The existing platform verifies
cold gated/reset state, masked interrupt and inactive CS before routing pins.
Its local startup reset sequence is not a terminal recovery mechanism.

Session version 1 polls completion/error status with GIC interrupt ID 59
masked. Version 2 uses deferred interrupt completion on CPU0. Both versions keep
the same authenticated request layout and bounded byte counts. The command fits
the 64-byte TX FIFO; the response fits the 192-byte RX FIFO. The owner preloads
the complete command before exchange, enables transfer-complete and the three
source-defined error interrupts, then unmasks the private level-triggered route.
An interrupt that arrives before unmasking remains latched.

The ISR masks interrupt 59 and notifies the owner. It performs no SPI, FIFO,
clock, rail or pin-manager work. The owner waits for distributor write completion,
masked enable state and inactive interrupt state before disabling controller
interrupts and running the existing bounded error, FIFO and CS hold checks.
Completion requires an actual ISR notification; active polling cannot substitute
for a missed interrupt. Deferred route operations have a 32-attempt bound in
addition to the operation deadline. Private startup rejects inherited enabled,
pending or active interrupt state before common GIC writes. Only version 2 installs
the CPU0 Group1NS level route, with Nonsecure priority 0xb0, and binds the task.

Each service step has a bounded FIFO byte budget. The task reaps only complete
error-free transfers, then copies and hashes the response before success
publication. The operation deadline bounds completion; a completed historical
report remains observable only within the independent supervised lifetime.
Cached platform READY/COMPLETE results are never independent liveness proof.

Cancellation, expiration, ownership loss or access uncertainty closes the atomic
output gate. Only the owner retries clock/rail retention metadata; it never
polls a healthy controller merely to cause a fault. Terminal paths do not close,
reset, restore pins or release resources. Successful output also keeps every
resource and provider borrow retained. Buffers, leases and peer state must not
be reused before the reviewed recovery for every power domain is complete.
Software deadlines cannot recover stalled MMIO.

## Build, preparation and copied output

Configure the pinned board/toolchain dependencies with
`SUNSTONE_BOARD_SUPERVISED=ON` and `SUNSTONE_A733_SPI=ON`; build
`sunstone-a733-spi`. The final manifest is `spi-application/manifest.json`.
`apps/spi/prepare.py` accepts `--image`, `--manifest`, `--admission`, `--template`,
`--output`, `--clang` and `--lld`. It authenticates entry facts and the complete
session before making the immutable SRAM-launch/input bundle.

`apps/spi/capture.py` reuses the existing supervisor, queue claim, port lock,
USB access/discovery and failure retention. Its arguments are `--bundle`, `--fel`,
`--evidence`, `--database`, `--claim`, `--owner` and
`--supervised-manual-reconnect`. USB PID `0xefe8` has a distinct SPI product and
serial. The 128-byte frame header binds the full session and copied payload by
SHA-256. A successful 96-byte payload reports observation time, selected rate,
counts, mode selection, exact transmitted command and actual received bytes.
The metadata word at payload offset 28 is zero for polling and contains the actual
IRQ notification count, from 1 through 32, for interrupt completion. CS0 remains
fixed by the authenticated profile; the full session hash binds the selected mode.
Pending/failure frames contain no output. The device checks eligibility before
snapshot, after copying and before queued transmission; host success requires
durable raw frame and copied payload.

## A physical attempt requires an identified peer and recovery

The coordinator may prepare one RAM-only attempt after board quarantine and
recovery are resolved. Required records establish the actual board/header,
peer/translator/circuit, exact request effects, stable BLDO1 and external supplies,
1.8 V levels, idle CS, no inherited DMA/handlers, controller-wide pad exclusion,
remote/secure-writer exclusion, Device/security mappings, stable clock parents
and cold controller state. Existing board CPU/memory/coherence/USB/PMIC/counter
conditions also apply. Numeric authorities do not prove those facts.

Expected output is the copied response or a retained failure. No peripheral
acknowledgement or protocol correctness is inferred merely from SPI completion.
There is no automatic retry. Keep the failed claim and backing until qualified
full board power removal and the peer's separately reviewed power/reset sequence.
Cable reconnection alone establishes no safe resource reuse or peer reset.

DMA, interrupt-driven transfers beyond this FIFO-sized profile, broader wire
modes and rates, slave operation,
SPI1 DBI, SPI4 routing, flash protocols, power transitions and shutdown remain
required SPI work. The separate SPIF controller remains in scope. This initial
application does not claim complete SPI support.

## Software evidence covers composition and copied output

The frozen batch in `~/.cache/sunstoneos/evidence/spi-application-20261018/`
contains one supervised native link, sanitizer-checked production admission/full
sealing, thirteen linked application cases, four host groups and a nondeployable
offline launch bundle. Before native configuration, the actual platform initializer
validates the nominal route, clock, format and pin fields. The actual PMIC profile
and every BLDO1 selector are checked against the pinned source-defined behavior.
BLDO1 uses enable register `0x20` bit 6, selector `0x2a` masked by `0x1f`, and
500–3400 mV in 100 mV steps. These fields follow U-Boot revision
`b791be842935b27268ae3d00e943a9075495f30a`, its AXP8191 control table and header.
The extension adds no PMIC voltage write.

Linked cases execute actual sealing, application sequencing, native callback
filters and copy instructions. They cover unsealed/mixed-prefix refusal,
wrong-width/port access, rail refusal, fault hidden by BUSY, partial failure,
reap expiry and publication/copy revocation. A successful historical result stays
eligible after the operation deadline only while the supervised lifetime remains
live, with no additional provider calls. Host cases cover durable copied data,
session/frame integrity and real shared selector/access/discovery routing.

Platform/controller/PMIC/rail returns, shared-manager initial state, translations,
time, RTOS scheduling and USB transport are modeled. Existing wire, platform and
provider evidence is reused. These results establish no physical exchange, signal
compatibility, peer protocol behavior or stalled-MMIO recovery. The bundle names
no physical port and cannot authorize a device attempt.

## Deferred interrupt software evidence

The frozen batch in `~/.cache/sunstoneos/evidence/spi-irq-20261020/` contains
one affected native image, one offline SRAM launcher, five linked leaf cases,
three focused host groups and fourteen sanitized clock-reference cases. The image
SHA-256 is `149f7635fb8da3a5b6dc20cf72223a373ff0e5f5e929b7e3e3e6317d6f5d474e`.
The leaf cases execute the actual application, ISR, native ownership checks,
controller core, platform, clock acquisition ledger and pin manager. They cover
interrupt success and copied output, missing-interrupt expiry, active-source
deferral exhaustion, controller error and an unexpected interrupt. The root
checks separately execute seven route/CPU-dispatch cases and five private boot
quiet-state cases on that same image. No second image was built.

SPI/GIC register effects, shared pin callbacks, PMIC/rail replies, scheduling and
USB transport are modeled. The host reuses an existing polling frame to verify
ABI compatibility without repeating its native batch. Model setup initially used
the wrong scheduler constant, passed a bytearray to Unicorn's byte-only memory
API and initialized the wrong PE mux address. The reusable check now uses the
selected FreeRTOS constant, explicit bytes and the source-derived PE address.
Those failed attempts establish no completed case. An exploratory host fixture
also showed that an expired mutex admission returns BUSY; the complete linked
application correctly converts its independent deadline expiry to retained failure.
The final image retains the inherited board main from its recorded source base;
a later independent board-progress correction is not part of this image.

The IRQ mask follows the pinned Linux `spi-sunxi.h` definitions for transfer
complete and `SUNXI_SPI_INT_ERR`: bits 12, 10, 9 and 8. TX underrun bit 11 is
excluded by that source. A733 manual sections 18.8.8.3 and 18.8.8.4 define the
controller enable and status registers. These checks establish no physical IRQ
delivery, signal timing, peer response, electrical compatibility or recovery.
