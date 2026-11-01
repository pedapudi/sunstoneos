# Experimental GPU traffic measurement through NSI

The optional GPU diagnostic reads hardware traffic counters for the GPU's
master port in the A733 Network System Interconnect (NSI). Software checks
cover its programming and ownership. Board calibration must establish when
the data registers update before their values can be reported as per-job
traffic. No physical traffic measurement is claimed yet.

The intended reader knows memory-mapped I/O and GPU job queues. The counter
boundary is the GPU master port after GPU caches. It includes firmware,
translation and command traffic that uses that port. It does not distinguish
shader loads from those other accesses, or measure DRAM pin transactions.

## Source-backed register identities and units

The pinned A733 device tree assigns GPU master ID 6, NSI base `0x02020000`,
and 16 bytes per input-agent data-counter increment. Its `master_clks`
property assigns GPU master 6 to the NSI clock. The vendor bandwidth reader
multiplies the read and write data counts by that unit. These facts come from
the A733 configuration, rather than another member of the SoC family.

| GPU master register | Offset from NSI base | Used access |
| --- | ---: | --- |
| Counter enable | `0x0cc0` | 32-bit read/write, bit 0 |
| Counter clear | `0x0cc4` | 32-bit read/write, bit 0 |
| Measurement period | `0x0cc8` | 32-bit read/write |
| Read data count | `0x0cd4` | 32-bit read |
| Write data count | `0x0cd8` | 32-bit read |

The identities follow `sunxi-nsi.h` lines 377–385, evaluated at master 6.
The mapping and units are in `sun60iw2p1.dtsi` lines 1470–1517. The vendor
period helper programs cycles as microseconds times the selected master's
clock rate divided by one million. It then disables, clears and enables the
counters and waits 10 microseconds. That delay is the vendor's programming
sequence; it does not establish a documented clear-completion latency.
Pinned URLs, SHA-256 values and licenses are in
[the source record](../vendor-firmware/gpu-nsi-sources.json).

## Diagnostic ownership and bounded access

`SUNSTONE_GPU_NSI_PMU=ON` requires `SUNSTONE_GPU_TIMING=ON`. Both are off
by default. The PMU source and callbacks are omitted when its option is off.
Normal submission code is unchanged.

The diagnostic runs on the existing GPU worker while that worker owns the
active GPU service. It accesses only master 6's counter controls and reads
clock configuration. Every native access checks the Device mapping and uses
32-bit MMIO with DSB SY before and after. The diagnostic writes no clock,
reset, priority, bandwidth-limit or other master's register.

An enabled inherited counter causes a diagnostic refusal with no writes.
For a disabled counter, the diagnostic saves enable, period and data values,
then takes exclusive software ownership of that bank until cleanup. This
assumes the integrated image contains no second NSI PMU writer; the selected
source composition must preserve that assumption. Initial data values are
reported because clearing them cannot preserve their hardware state.

The period is `UINT32_MAX` cycles. Clock decoding requires a stable 24 MHz
PLL reference and an enabled, non-reset NSI clock without dynamic scaling or
a pending update. Integer SYS24, PERI0 and DEPLL3X sources are supported.
DDRPLL and spread-spectrum DE configurations produce an unsupported-clock
record without counter writes. The actual configured rate must be at most
700 MHz. The register fields follow A733 manual sections 4.1.6.61 and
4.1.6.74 and the selected clock driver's NSI and DEPLL definitions.

The pinned boot0's board setup selects NSI mux 3, PERI0_600M, with divider
zero. Its DRAM clock routine selects the same parent and retains that
divider. The DRAM-rate branch changes an MBUS divider before rejoining the
common NSI write. [The binary facts](../vendor-firmware/data/gpu-nsi-clock.json)
record the binary hash, source and decoded addresses. This supports the
existing parent decoder without assuming an NSI rate from the DRAM rate.
Live clock readback still controls diagnostic admission.

The measurement must finish before the programmed period expires. At
700 MHz, the maximum period exceeds six seconds. End-of-job validation uses
the decoded clock rate and architectural counter. Decreasing raw data counts
are classified as nonmonotonic; the diagnostic does not assume a rollover
width or turn a wrapped value into traffic bytes.

Cleanup disables counting and restores the saved disabled enable word and
period, with readback. A cleanup error invalidates the diagnostic and prevents
later counter writes in that boot. It does not turn a completed GPU job into
an application failure. Optional telemetry must not interfere with ordinary
GPU retirement or ROM return.

## Raw observations and calibration criteria

Each job reports inherited counts, counts after the initial 10-microsecond
idle interval, counts after publication, counts at observed completion,
counts immediately after disabling, and counts after a further disabled
10-microsecond interval. `GPUNSI_FIELDS` defines the record layout. Status
`A733_NSI_SAMPLED` means the software sequence completed; it does not certify
the hardware's counting semantics. Saved clock words permit independent rate
decoding. The complete parent chain, NSI gate and clock word must agree at
the two ends of the interval. All counts remain raw in the capture. A clear
word that still has bit 0 set after the vendor delay retains status
`A733_NSI_CLEAR_PENDING` while all six phases are collected. The vendor does
not poll that bit, so it is not treated as a completion acknowledgment.

The first board run must pair an idle control with known GPU work under a
period that cannot expire during either interval. It tests these hypotheses:

1. Read and write data registers expose useful partial-period values before
   the maximum period expires.
2. Clearing and the vendor's settle interval establish a reproducible start.
3. Counts increase with known device traffic and exceed idle-control traffic.
4. Disabling either preserves the live count or exposes a consistent partial
   count; the two disabled observations agree after settling.
5. Enable and period readbacks restore the inherited disabled configuration,
   and configured clocks remain stable through the measurement.

If live counting is established, an eligible interval's observed traffic is
16 times its read/write counter difference, using 64-bit arithmetic. Report
the sampling boundary, idle-control counts and measurement overhead beside
the byte count. A decrease, expired period, changing clock, enabled inherited
owner or failed readback makes the interval ineligible. Raw observations
remain evidence even when the hypotheses fail.

The host fixture models immediate clear and live counting explicitly as
hypotheses. It verifies only the software's sequence, counter refusal,
restoration, configured-rate decoding and invalid-sample handling. It cannot
establish a physical latch, counter width, frequency or counting boundary.

## GPU firmware performance counters remain a separate route

The selected firmware interface exposes HWPerf commands and shared-memory
addresses, while the native initializer leaves performance collection off.
The selected command IDs are 201, 203, 217, 218 and 220. The cached proprietary
DDK uses 300–304 for related operations, so its numbers cannot be copied into
the native command stream.

The pinned HWPerf headers describe programmable groups and bit selections,
and the block table describes which register blocks can exist. Those sources
do not identify a proven BXM-4-64 DRAM-byte or cache-miss event selector.
Generic register headers also name SLC read/write stall counters; stall counts
cannot establish transferred bytes. This is a missing event-definition and
native collection path, rather than evidence that the GPU lacks counters.
