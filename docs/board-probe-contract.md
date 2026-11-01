# Native probes return through a shared watchdog contract

This contract admits A733 RAM-only diagnostics to `tools/board.py --kind probe`.
USB class diagnostics use the same contract through the runner's
[`usb-class` kind](board-runner.md#usb-class-probes-require-finite-device-cleanup).
It supplies the watchdog marker, session nonce, progress and first-fault records,
result receipt and ROM-return epilogue. It does not qualify a peripheral or
establish ownership from a host declaration. No board was accessed while building
or checking this contract.

The implementation is [sunstone_probe.h](../include/sunstone_probe.h),
[probe.c](../platform/a733/probe.c), [probe.S](../platform/a733/probe.S) and
[the host decoder](../tools/board_probe.py). The common
`platform/a733` monitor has an adapter selected by `SUNSTONE_BOARD_PROBE`.
Its ordinary returning images retain their existing behavior.

## Attach the library and declare the image

A native image must retain symbols and its immutable marker in its ELF. The
runner loads that ELF's segments and compares every upload. A flat binary with
a JSON declaration cannot substitute for the native contract. The old raw-probe
format admits only the intentional watchdog-reset qualification diagnostic.

```cmake
include("${SUNSTONEOS_DIR}/cmake/sunstone-probe.cmake")
sunstone_enable_probe(my_probe)
```

The helper adds the C and assembly library, includes and feature definitions.
Keep the image's normal FreeRTOS, startup, vectors and linker inputs. The common
monitor supplies the SMC adapter and interrupt-restoration hook. A custom monitor
must implement that adapter against its own saved state; linking the library
alone cannot replace a device's existing return or interrupt protocol.

A small image can publish a fixed result record in the conventional result area:

```c
#include "sunstone_probe.h"
SUNSTONE_PROBE_IMAGE(0x42000100, 256, 0, 0, 20);
```

The arguments are payload address, payload capacity, input address, input bytes
and maximum session seconds. The maximum is 600 seconds. This example declares
no input and a 256-byte payload. Use a shorter application work budget so that
startup and cleanup finish before the absolute session deadline.

The host requires a strong `sunstone_probe_input` object for nonempty input,
matching the marker's address and size in writable, non-executable image RAM.
An image-RAM payload similarly needs a strong `sunstone_probe_payload` object.
Payloads in `0x42000100..0x42003fff` use the conventional result reservation.
The runner zeros the payload before entry; preserve no initialized input there.
Input, payload, code, session and receipt allocations must not overlap.

The immutable marker and all required helpers must have real file-backed load
coverage. Host admission rejects writable executable loads, malformed lengths,
MMIO input or payload addresses, missing cleanup hooks and unknown versions.
The ordinary image allocation must stay within `0x41000000..0x41efffff`.
Native USB probes through `usb-class` or its `probe` alias may additionally
reserve the exact `0x42200000..0x42a00000` USB arena. The shared OS host validator
requires an allocated, writable, non-executable `.uncached_pool` NOBITS section,
strong exact linker bounds and one matching standalone load with zero file bytes.
Every other external load is refused. The arena is excluded from uploads and
host initialization; marker input, payload and helper bounds retain their
ordinary limits. This linked geometry establishes neither physical RAM authority
nor a live backup or restoration procedure.

## Begin before cache maintenance and report actual progress

At EL3, call `sunstone_probe_begin()` before invalidating or cleaning the cache
hierarchy. The supplied monitor entry does this before its first cache operation.
Entry requires CPU0 at EL3 with EL3 translation and caches disabled, a valid
session, matching input checksum and the exact host-armed WDT0 mode/configuration.
It retains the full nonce and absolute deadline outside EL1's BSS.

**Physical prerequisite:** all seven secondary CPUs must already have matching
static OFF policy, OFF acknowledgment/status, clear start requests and disabled
GIC wake. The library checks the PCK-600 identity and these fields again before
return. It makes no PPU policy or power-domain writes. Fresh FEL alone does not
establish this prerequisite; no current physical result qualifies that baseline.
A dynamic-OFF sample or a WFE loop does not establish equivalent ownership.

Call `sunstone_probe_progress()` at bounded points of completed work. EL1 uses
SMC `0x734`; EL3 uses the library's direct helper. The common monitor preserves
x1–x30 across renewal, including registers held by assembly memory-test loops.
It restarts WDT0 only before the absolute deadline. A stalled producer or expired
session stops renewal and leaves WDT0 armed. Feeding from an unrelated periodic
interrupt would hide a stalled probe and violates the contract.

WDT0 uses the existing 16-second RTC-clock mode. A probe must preserve that clock,
mode, configuration and reset output. Device-specific clock and voltage checks
remain required. Declare operating-point and storage intent through the existing
[runner policy](board-runner.md#retain-the-electrical-and-storage-checks).

## Retire devices before completing the image

Each image provides a strong `uint64_t sunstone_probe_quiesce(void)` function.
It runs in the normal caller's context before publication. Return
`SUNSTONE_PROBE_READY` only after the device's actual stop/drain operations,
worker retirement, DMA retirement and interrupt-source masking have completed.
Any other result enters the fatal path. Do not release uncertain resources or
replace a driver stop acknowledgment with a constant success value.

Call `sunstone_probe_complete(status, payload_bytes)` from EL1 or EL3 after writing
the payload. Status zero means the application passed; nonzero denotes a
controlled diagnostic mismatch or refusal with equally complete cleanup. Existing
common-platform callers may use `platform_exit(status)`, which completes the
marker-declared payload capacity. Device faults, assertions and exceptions use
`sunstone_probe_fail(site, detail)` at EL1, or `sunstone_probe_fault()` at EL3.

The common EL1 exception vector captures ESR_EL1, ELR_EL1 and FAR_EL1 without
using a task stack or TCB. Its fatal SMC records the first fault and parks with
WDT0 armed. Panic also enters this path before logging or device callbacks.
The generic EL3 vector records its own exception context. Later failures cannot
replace the first retained fault.

Completion publishes the payload through the caller's actual security-state
mapping. The EL1 assembly tail masks interrupts and disables EL1 M/C/I before
SMC `0x733`. EL3 remains uncached. The image also provides a strong EL3
`sunstone_probe_restore_interrupts()` hook; the common monitor implements it with
its real saved interrupt state and bounded restoration. A bare EL3/custom-monitor
image must implement its own real restoration, including timer shutdown.
EL1 M/C/I must be clear on entry to the final EL3 epilogue.

Only successful device cleanup, successful interrupt restoration and a final
secondary-OFF check permit whole-hierarchy cache cleanup. The library then writes
the receipt, verifies the deadline, disarms and reads back WDT0, records
`RETURN_ROM` and executes the existing RVBAR `0x20` / RMR_EL3 `2` return sequence.
A failed check parks with WDT0 armed. Inaccessible hardware still needs watchdog
recovery; software timeouts alone cannot recover a stalled bus transaction.

## Bind the returned bytes to the launched session

| Record | Location and fields |
| --- | --- |
| Immutable marker | `sunstone_probe_marker`, 112 bytes: ABI version, watchdog, maximum seconds, session/receipt geometry, payload/input geometry and return capability. |
| Host session | `0x42004000`, 128 bytes: full epoch, generation, lease, duration and input FNV-1a-64. The image SHA-256 words identify the host upload; native code does not recompute the ELF hash. |
| Completion mailbox | `0x42004080`, 24 bytes: requested payload length, status and the completion sentinel. It cannot replace device or interrupt cleanup. |
| EL3 receipt | `0x42000080`, 128 bytes: nonce, status, payload length/FNV-1a-64, cleanup flags, secondary mask, counters, WDT readback and final sentinel. |
| RTC breadcrumbs | Existing `sunstone_crumb.h` layout: `PROBE_BEGIN`, `PROBE_PROGRESS`, `PROBE_CLEAN`, first fault and `RETURN_ROM`. |

The host classifies WDT0 and breadcrumbs before any DRAM read. After a normal
return it requires the full receipt nonce, completed cleanup/retirement, disabled
watchdog, bounded counters and matching payload checksum. A nonzero diagnostic
status yields `result-mismatch`. A watchdog reset loses DRAM; only retained RTC
records are decoded. See [runner outcomes](board-runner.md#interpret-the-result-and-recovery-state).

## Convert the existing memtest image

The old artifact at `~/.cache/sunstoneos/build/memory-dma-1031/images/memtest/`
lacks this contract. Rebuild it from this checkout. The conversion retains the
mapping-admission changes from the memory stream, the 128-byte contract at
`0x41005000` and the 4 KiB native result at `0x42003000`.

The target now links the probe library. Its CPU-only quiescence hook accepts no
DMA work, and refuses digest, alias-decode and scan stops. The existing four
recovery pages and 1,056 beacon words remain at their original addresses. Added
return code occupies unused space in the protected state page. Its aligned
immutable interval is included in the recovery digest, while mutable monitor
state remains excluded. Finish checks the digest again after the last test
phase. Linker, relocation and stack checks cover the full recovery closure.

```sh
py="$HOME/.cache/sunstoneos/venv/bin/python3"
out="$HOME/.cache/sunstoneos/build/board-probe-example"
"$HOME/.cache/sunstoneos/bin/cfg.sh" "$PWD" "$out/build" \
  -DMIMUS_DIR= -DPython3_EXECUTABLE="$py"
cmake --build "$out/build" --target a733-memtest -j12
"$py" -B apps/memtest/probe-input.py --output "$out/input/contract.bin"
"$py" -B tools/board.py check "$out/build/a733-memtest.elf" \
  --kind probe --input "$out/input/contract.bin" --evidence "$out/admission"
```

Expected: JSON with `admitted: true`, `physical_access: false`, the watchdog and
fresh session identities. Every command above is host-only. Input generation
spends at most 18 seconds of the 20-second session on data/address/signature
work, reserving startup and cleanup time. It establishes no physical authority
or residual acceptance. For later March runs, use `apps/memtest/check.py prepare`
with the actual preceding run log and exclusions; its default work budget is
also 18 seconds. Never reuse the initial input to erase a failed-run exclusion.

The coordinator's eventual command is:

```sh
"$py" -B tools/board.py run "$out/build/a733-memtest.elf" --port 3-1 \
  --kind probe --input "$out/input/contract.bin"
```

After separately establishing the physical prerequisites, the prediction is
`returned`, zero application status and a matching checksummed memory record.
Validate `output.bin` with `apps/memtest/check.py verify --result OUTPUT \
--contract CONTRACT`. Partial coverage is not full-map qualification. A static-OFF
or device-admission refusal instead leaves a retained first fault and a watchdog
reset; do not repeat it without a correction or new evidence.

## Replace parking only where hardware can retire

For a PWM probe, first stop the owned channel, wait for the driver's stopped
condition, mask/ack its interrupt and restore shared pin/clock ownership. Then
complete the probe instead of the final WFE loop. Leaving a waveform running
or merely clearing a DMA request does not satisfy quiescence.

For a crypto single-core probe, use the existing engine completion and drain
proof, retire DMA and IRQ ownership, and restore its monitor's owned state before
completion. The single-core monitor needs the fatal/renewal/return adapter; it
must preserve its own security and interrupt rules. The common adapter cannot
be pasted over those rules.

If the hardware has no demonstrated finite stop or requires whole-board power
removal, retain that restriction. Such an image must not advertise
`SUNSTONE_PROBE_RETURNABLE`; a marker with `SUNSTONE_PROBE_POWER_REMOVAL` is refused
on the host. Watchdog return to FEL alone does not establish that an uncertain
device has retired. The alternative is a source-backed finite stop/restore path,
reviewed and host-tested before advertising normal return.
