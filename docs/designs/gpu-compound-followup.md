# Complete a retained GPU return claim and admit stopped thermal state

Status: implemented host continuation with focused software evidence. The frozen
GPU returning ELF is 6d28a8340c08ba4a3d07eb46a6cd578cb8b8a72f9102c43052e31690f8c89c66.
It is unchanged. This increment makes no physical recognition or return claim.

## Bind the GPU return to the existing compound continuation

The CPU compound host (`apps/os/cpu-return-followup.py`, `authority` and
`returned`) accepts only a completed retained CPU return, its exact successor
ROM and one preapproved claim. It independently decodes CPU retirement and, for
an explicitly reviewed GPU profile, GPU retirement. The continuation supplies
transcript parity, cache/survey helpers, strict CPU rail-helper
capture and final proof under that same claim. It retires the helper's private
18 MiB instead of claiming those bytes were restored.

GPU support adds one explicit `gpu_return` member to the compound review. It
binds the actual linked GPU configuration, the frozen normal-return review hash
and the fixed future `gpu-retirement.bin` capture path. The linked configuration
already includes the 64-byte report address and exact write-back/uncached pool
bounds. The actual GPU-aware CPU host validator must accept the frozen bundle
and its GPU retirement review before the continuation accepts this opt-in.
CPU-only reviews retain their existing admission and receipt checks.

Before any transport request, the continuation reads that copied GPU record,
checks its SHA against the retained receipt, and calls the existing GPU decoder
with the exact epoch and generation. It requires the decoded GPU result to
match the receipt's GPU summary alongside the independently decoded CPU report.
A valid GPU record follows acknowledged firmware OFF, retired queues, reset and
isolation, GPU resource-token release, completed PMIC sample retirement and a
suspended GPU worker in the reviewed native producer. Separate common-context
cleanup command 106 is not part of that producer. Backing stays owned through
OFF and the returning host's RAM restoration.

The local write-back and uncached pools must match the linked configuration
and remain within the already restored 0x41000000 through 0x42200000 reservation.
The original return's backup extents must cover those pool ranges. The same
CPU rail helper subsequently overwrites that private reservation. Its strict
return and supply/readback checks remain unchanged. Successful continuation
records the GPU proof without granting an unrelated device or claiming that
DCDC4 was restored from 960 mV.

## Thermal next entry uses the existing enabled-clock mode

`a733_thermal_stop` disables all five sensors and local alarm/critical interrupt
enables, checks them back, and leaves the GPADC clock and THS gate/reset enabled
(`drivers/thermal/a733_thermal.c`, `a733_thermal_stop`). The CPU return path masks
INTID 71 and observes this STOPPED result before publishing completion. Its
terminal restores the saved GIC fields. Analog conversion may finish after
sensor disable, but no DMA path exists and local interrupts cannot write memory.

The next cold-clock session mode is unsuitable: `clocks(p, 0)` requires the
GPADC enable bit clear when `configure_adc_clock=1`. With that session field
zero, the actual driver instead admits an already enabled 24 MHz GPADC source
and divider. It checks THS sensor, shutdown and interrupt enables all zero,
programs calibration/settings, clears stale pending status and enables sensors
last. Its masked updates do not pulse reset when the gate/reset bits are already
set. `apps/os/thermal.c:construct` already acquires shared stable GPADC ownership
and exclusive THS ownership for this mode. A next GPU image needs no rebuild.

The rail helper is an intervening thermal writer. In
`apps/board-bundles-power/cpu-opp-step.c:thermal_start`, it reads the GPADC enable
bit and selects enabled-clock mode when that bit is already set. It acquires its
own THS/GPADC lease, initializes the same driver, obtains NORMAL samples, executes
the reviewed CPU rail plan and calls `thermal_stop` before COMPLETE. Its alarm
and critical masks are zero; acquisition, period, filter and SID calibration
match the OS driver. Therefore the last stopped predecessor is the **helper**,
not the original CPU return. A new owner reconstructs its own driver and lease
ledger; it inherits hardware register values only.

The stopped-predecessor packet accompanies existing thermal entry evidence. It
records the source contract alongside the existing preparation schema. A
root preparing the next entry must fill and bind all of these facts:

- The original CPU-return receipt and the helper's strict kernel receipt, with
  hashes, board/port, original and returned ROM identities and the retained claim.
  Existing RAM-continuity validation uses these underlying receipts. The compound
  summary does not replace them.
- The exact helper image, binary, manifest and source closure, its COMPLETE/boot
  clock/DCDC3/DCDC5 readbacks, and its final thermal STOPPED call. The private
  0x41000000 through 0x42200000 reservation is retired and overwritten by this
  helper; no preserved-byte or helper-RAM-restoration claim is allowed.
- The source closure of every executable between helper STOP and next entry,
  including ROM and SRAM helpers. The packet must exclude another THS/GPADC,
  clock or interrupt writer after the helper. A generic assertion about ROM is
  insufficient.
- The next frozen GPU image, session identities, thermal calibration and
  configuration, `configure_adc_clock=0`, and stable enabled GPADC ownership.
  The no-inherited-service fact means no surviving active owner, with explicit
  retirement evidence. It does not mean reset-state registers.
- The observed elapsed lifetime from the helper's validated return to next
  sensor enable, if relied upon. Existing helper receipts do not record a
  monotonic stop timestamp, so they alone cannot substantiate a duration.

The native driver rechecks oscillator/reference/APB and GPADC source/divider,
requires all sensor/data/critical/alarm enables zero before programming, and
verifies enabled THS clocks after startup. On the reviewed retained state its
masked gate/reset updates perform no CCU writes. It can enable a missing THS
gate/reset bit during startup, so the packet must not misstate this as a
pre-start refusal of every altered THS clock word.

### Pending conversion and filter history

The cached A733 manual, sections 7.1.5.1 and 7.1.6, gives a 14-cycle conversion,
48-cycle acquisition and 480-cycle sample divider at 24 MHz for these settings:
0.583 microseconds, 2 microseconds and 20 microseconds respectively. Section
7.1.8.3 gives the programmed period as 4096 times 29 / 24 MHz, about 4.949 ms.
The filter is a four-sample average (7.1.8.11), about 19.797 ms for four periods.
Section 7.1.8.1's high/low acquisition field labels disagree with the programming
example and A733 source; the example and pinned source establish the values.

These timing calculations are not a documented maximum stop-drain or filter
flush bound. CPU/helper STOP does not prove analog drain. A conversion may
finish after disable, and old pending flags or filter history may remain.
`startup` recalibrates from the same SID words, rewrites identical acquisition,
period and filter settings, and W1C-clears all four pending registers before
sensor enable. Cached Linux A733 initialization and resume also calibrate and
reinitialize the enabled clocks; neither source establishes a stronger analog
flush guarantee. The native entry guard still requires valid, fresh NORMAL
observations before CPU performance or GPU recognition; its freshness timestamps
observations; conversions carry no independent generation tag. Physical warm
adoption remains unqualified, including this residual-conversion limitation.

The source packet is saved as `stopped-predecessor.json` beside the focused
check results. Its source hashes and manual sections are concrete; physical
receipt, successor and elapsed-time bindings are explicitly absent. It cannot
serve as a runnable entry authority in that state.

## Budgets and focused evidence

There is no native code change, new scheduler task, reset or bus transaction.
GPU continuation adds one 64-byte record read/hash/decode per existing readiness
check and bounded comparisons against the already loaded geometry/review.
Backup coverage checks use the existing small extent list and allocate no device
memory. CPU-only checks keep their current path. No new continuation phase or
queue state transition is added.

Twenty focused host cases use the actual GPU-aware CPU bundle validator and native
producer records from the accepted GPU return evidence. They cover valid
GPU continuation, missing opt-in, changed geometry/review, absent or corrupted
GPU record, epoch/generation/summary mismatch, and missing pool backup coverage.
The tests call the actual pretransport returned-receipt parser and reach no
transport. The bundle uses nominal modeled clock authority; the unchanged
compound under-test clock check is not bypassed or qualified by these cases.
Existing CPU-only transport/helper checks are reused.

The actual thermal-driver fixture starts and stops the OS, constructs a fresh
helper owner over its registers, stops the helper and constructs a fresh GPU
owner. ASan/UBSan checks pass for enabled-clock mode, stale-status clearing before
sensor enable, unchanged calibration and no CCU writes on that chain. Nine unsafe
predecessor variants refuse before writes: cold mode, invalid GPADC
source/divider/gate, lost reference lock and each inherited local enable.
This fixture models registers. Analog drain is outside its scope.
