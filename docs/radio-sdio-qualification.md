# SDIO bus qualification before radio firmware boot

This procedure is for native-driver and board-test maintainers. The four-bit
25 MHz path is implemented and software-tested. No image or physical result
currently qualifies it. Firmware loading at one bit and 400 kHz needs at least
46.7 seconds of bus time, exceeding the supervised profile's 40-second bootstrap
bound; see [the firmware budget](board-radio.md).

## The native adapter admits both implemented clock settings

The radio platform selects the PERI0 400 MHz branch divided by eight after
enumeration, producing a 25 MHz card clock. Its native adapter accepts the
module register's two implemented encodings, with the module gate either off
or on. It requires the owner task to hold the shared register mutex and refuses
changes to unowned bits. The platform verifies the parent clock and its lease
before changing the module source.

The focused [instruction check](../checks/radio-native-clock.py) executes the
actual native callback and task-context predicate. FreeRTOS observations and
MMIO storage are modeled. The callback accepts both PERI0 encodings in the
gate-off, source-change and gate-on sequence and preserves a nonzero unowned
bit.

The check covers unsupported selectors and divisors, changed unowned bits,
parent-register writes, and refusal without the correct task, mutex or execution
context. Every accepted module write makes one MMIO read, one write and three
`DSB SY` barriers. The identification-clock case executes 106 instructions,
including the fixture wrapper, and the faster-clock case executes 104. The
comparison that permits the second source encoding runs only during clock
configuration.

All 24 cases pass. Inputs, compiler dependencies, the ELF file and results are
preserved under `~/.cache/sunstoneos/evidence/radio-resume-20261024/corrected/`;
the checked ELF SHA-256 is
`9447e25eb021570cd697985c747886cfdd07ad035241ac10b8c022373e6da96a`. These
results establish native admission behavior. Electrical operation and startup timing
remain unqualified.

## The existing returning image needs radio-specific authority

The [single-core returning platform](designs/board-bundles-next.md) supplies
FreeRTOS and a secure timer that returns to ROM after two seconds.
`apps/board-bundles-power/power.c:bb_power_init` supplies an existing composition
of the counter, mutexes, clock leases, pins, rail leases and shared PMIC service.
That constructor authorizes PL0 and PL1 for PMIC access. It does not supply the
radio's required pin or power authority.

Before creating a runnable radio image, preparation must bind these inputs to
the selected board and its current power lifetime:

- **Pin ownership.** PG0–PG5 use function 2; PM0 is an input and PM1 controls
  Wi-Fi enable. The shared pin manager needs authorization for those pins and
  a complete authenticated PM output-latch record. A DATA read of input levels
  cannot establish that record. PM2 and its inherited Bluetooth/reset/oscillator
  behavior must remain stable. The constructor must preserve the PMIC's PL0/PL1
  ownership in the same manager.
- **Supply topology.** Bind VBAT, PG, PM and module VDDIO to the installed
  board's actual rails. The schematic names DCDC8 for the I/O supply, while
  the device tree names BLDO5 and CLDO1 for PG/PM. The admission record must
  resolve that discrepancy. The PMIC service then observes enabled 3.3 V
  VBAT and 1.8 V I/O profiles without voltage or enable writes.
- **Clock prerequisites.** Bind AHB, NSI and MBUS selectors, dividers and rates.
  Verify STORE AHB and MBUS gates, MSI_LITE1 gate/reset, and disabled automatic
  SD-monitor gating. Four-bit operation additionally requires the inherited
  PERI0 400 MHz branch, its request/status bits and a shared parent lease.
  The radio driver observes these shared resources; it cannot enable them.
- **Local startup state.** Admit SDC1 with its local gate off and reset
  asserted. Require accessible Device-mapped PIO, SDC1 and GIC registers, and
  the actual EL1 owner task with IRQs unmasked. The driver masks INTID 194
  and verifies it disabled and inactive before changing radio pins or clocks.
- **Exclusive lifetime and recovery.** Exclude other SDC1, module reset,
  power and Bluetooth-control writers. Retain the platform, card, transport
  and buffers through the result. The timer cannot recover a stalled MMIO
  transaction; the supervised PIO admission requires whole-board power removal
  as the terminal recovery action when needed.

The [platform contract](radio-platform.md) identifies the pinned schematic,
device tree, clock and power sources behind these predicates. Successful
single-core or PMIC operation does not provide the missing radio-specific
authority. No board bundle has been frozen because those inputs and a native
radio application constructor are still missing.

## A bounded debug exchange must exercise CMD53 data transfers

The platform's final CCCR read uses CMD52, which transfers its byte on the
command line. It cannot prove four working data lines. The existing
`aic8800_transport_read_identity` performs a useful bounded data test: a padded
512-byte CMD53 bootstrap write and two padded CMD53 debug-read requests, with
address-correlated confirmations. Its source-defined write is
`0x40500058 = 0x40`; this is a bootstrap operation that requires explicit
admission, rather than a passive register query.

Use the existing transport setup and identity APIs. Admit an awake bootstrap
state, no outstanding debug command or stale confirmation, and uninterrupted
exclusive ownership. Apply the source's pad policy and settle interval. Preserve
both decoded identity values, all successful CMD53 counts, elapsed time, the
controller result and the selected clock/width. Do not attach firmware, runtime,
Wi-Fi, entropy or the network stack for this test.

The smallest procedure supported by the current APIs uses separate power
lifetimes for the one-bit baseline and four-bit comparison:

1. Prepare a single-core image that stops after transport identity completion.
   Give platform setup and identity work one absolute deadline of 1.5 seconds
   after monitor entry, below the existing two-second forced return. This is a
   proposed test bound; no measurement establishes that startup fits it.
2. In the first admitted lifetime, enumerate at one bit and 400 kHz, borrow the
   card, set up the transport and execute the fixed identity exchange. Capture
   the complete result through the returning platform's retained result memory.
3. Remove all board power before the second lifetime. Establish its own DDR,
   startup and radio prerequisites. Request `a733_radio_platform_four_bit`
   before the platform's first poll, then execute the same transport exchange.
   Require `host.wide == 1`, `host.card_hz == 25000000`, and matching identity
   values from successful CMD53 transfers. A low-speed fallback is inconclusive.
4. Stop at the first refusal, timeout, transport failure or mismatch. Preserve
   the result and retain resources through the required recovery. Neither ROM
   return nor a matching identity proves radio shutdown or permits reinitializing
   the retained card in the same lifetime.

The current width request precedes enumeration and borrowing; it cannot switch
an already borrowed card after the baseline exchange. A comparison in one
lifetime would require a reviewed transition API and ownership changes. The
separate-lifetime procedure avoids that additional implementation, at the cost
of a physical power cycle.

Passing this procedure would establish only the tested CMD53 exchanges at the
selected rate. The next test must load the pinned firmware within the original
40-second bootstrap bound and record its start confirmation. Larger transfers,
firmware startup, RF operation and sustained timing remain separate requirements.
