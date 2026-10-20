# Prepare and observe retained DisplayPort output

The application generates a fixed 1280×720 RGB565 pattern and drives the
TYPEC24 DisplayPort route at 720p60. TYPEC16 carries USB0 observation and input
power. The documented TYPEC24 route includes four COMB0 lane pairs and AUX on
SBU1/SBU2; its direct HPD ball is unconnected. HPD comes from Type-C negotiation.
The route is documented in [the wiring guide](board-wiring.md#typec24-has-a-documented-displayport-route).
Physical sink output remains unqualified.

`apps/displayport/producer.py` seals a 952-byte session around the existing
592-byte PMIC/USB0 board prefix, using workload 2. The extension holds separate
DP and TYPEC24 ownership, six evidence hashes, inherited clocks, the bank-L latch
handoff, source limits and four initial pin configurations. DP, TYPEC24, PMIC and
USB0 owners must be distinct. No pixels or DMA pointers come from the caller.

The producer reads an explicit JSON profile with these arrays:

- `clocks`: two IOMMU gate words, oscillator, reference PLL, VIDEO1 PLL, SERDES
  configuration, RTC SERDES0 and qualified AUX APB frequency.
- `typec`: owner, qualification and recovery identities.
- `latch`: identity, owner, known mask and value.
- `source`: minimum millivolts, maximum millivolts, milliamps, zero-threshold
  maximum millivolts, FUSB device ID and zero reserved word.
- `pins`: four function/pull/drive triples for PL8, PL9, PL12 and PL13.

The source range stays within 4750–5250 mV and supplies at least 500 mA. The zero
threshold is 1–800 mV. The FUSB device ID is 0x80–0x82. The bank-L latch mask is
0x3fff; initial pin functions are 0 or 15. These fields express the admitted
configuration; they do not establish physical electrical conditions.

`apps/displayport/prepare.py` consumes the actual ELF, image manifest, session
and reviewed entry records. It binds the immutable thirteen-word geometry and
constructs the authenticated 288-byte retained contract. The application uses
extended WB `0x48000000..0x78000000` and NC `0x78000000..0x79000000` ownership.
The actual native allocator must supply the 2 MiB NC allocation, page table,
master-8 mapping and pattern padding/guard proof. Broad reservation or IOVA
values alone do not establish those mappings. The constructor proof records a
zero scan mask because construction precedes IOMMU MMIO. The terminal runtime
observation separately requires the completed master-8 scan mask `0x100`.

The source clock profile uses a 26 MHz oscillator, normalized 24 MHz reference,
1188 MHz VIDEO1 output and 74.25 MHz EDP pixel clock. The AUX APB rate requires
independent physical qualification in the analog/SERDES evidence. An inherited
200 MHz software constant does not establish its physical rate. The admitted
range is 10–400 MHz in whole megahertz steps.

Operational preparation rejects modeled authority. The isolated software fixture
may substitute only the physical-authority predicate; actual image identity,
clock predicates, typed fields, allocations and table validation remain active.
Positive preparation and the single SRAM launcher compilation require the
actual linked image and allocator proof.

## Retain one copied observation

`tools/check-a733-displayport-supervised.py` uses the existing coordinator claim,
sealed bundle and supervised launch. The native lifetime is 20 seconds. One
absolute 19-second host window covers enumeration and bounded observation.
The USB identity is VID `1f3a`, PID `efe8`, revision `0100`, product
`sunstoneos native DisplayPort`, serial `sunstoneos-a733-displayport`.

Each query is byte zero. The host requests 1025 bytes for the fixed 1024-byte
reply, consuming its terminal zero-length USB packet at full or high speed.
Every raw reply becomes durable before validation. A terminal observation is
copied to a durable JSON artifact with its original session, build, lease,
Type-C generation and link diagnostics. Polarity, pin assignment and HPD retain
the negotiated snapshot from before revocation; they do not describe a live port
after shutdown. Transport or persistence uncertainty
stops capture without retry or relaunch. No ACK follows a read.

A healthy terminal record requires a trained link, fresh DE source completion,
a bounded output interval, DP video/TV1 and DE shutdown, and confirmed retained
Type-C revocation. A fault may retain active output when shutdown is uncertain.
All DMA allocations, tables, clocks and provider leases remain retained after
either result. Software shutdown does not prove DMA drain. Recovery follows
the admitted whole-board power-removal plan.

Copied hashes and status words establish their stated software observations.
They do not establish visible pixels, physical link operation, DMA confinement,
recovery or performance.
