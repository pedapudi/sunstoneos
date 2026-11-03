# Design: retained per-core fault records and one watchdog reboot path

Status: **implemented**; host and instruction checks pass. **Open** on the
board: watchdog expiry, slot retention across reset and a real fault's
registers. The component documents are [fault-capture.md](../fault-capture.md)
and [reset.md](../reset.md). Section numbers refer to
[performance-architecture.md](../performance-architecture.md).

In the resident lifetime of the integrated OS image
([os-image.md](../os-image.md)), an EL1 exception on any core takes the
stackless path `a733_cached_cold_fault`, which issues an SMC with the EL1
syndrome in `x1`. The EL3 handler `a733_cached_el3_fault`
([continue.S](../../platform/a733-smp-cached/continue.S)) parks the core. Every
park of any core stops WDT0 renewal, each EL1 fault and deliberate exit leaves a
record in the retained-result reservation, and the next boot reports the
previous boot's records over USB0.

## Data flow and ownership

1. **Mark.** Every EL1 vector entry, in the cold and the runtime tables, sets
   `x2 = 0x100 | slot` before the SMC. `.Lsync_fault` takes the slot from
   `SPSR_EL1.M`: 0 for EL1t, 4 for EL1h, 8 for EL0. `a733_cached_smc`, the
   only deliberate-exit SMC, sets `x2 = 0`, so a deliberate exit cannot produce
   bit 8 whatever status it passes in `x0`.
2. **Stop renewal.** Immediately after the MPIDR decode, before the state and
   core checks and before any other memory access, `a733_cached_el3_fault`
   stores a nonzero byte at index `core` of `a733_fault_pending`, followed by
   `DSB SY`. This runs on every entry, including EL3-origin vector slots. An
   MPIDR that does not decode to a core stores byte 0.
3. **Capture.** For SMC entries that pass validation, the stackless leaf
   `a733_fault_capture` in `platform/a733/fault.S` fills the core's 128-byte
   slot (layout in [a733_fault.h](../../include/a733_fault.h)). The slot holds an open marker
   `A733FLT0`, the kind, status and detail from `x0` and `x1`, the vector
   slot, `CNTPCT_EL0` and the contract epoch, then a CRC32C and the final
   magic `A733FLT1`. Only a vector fault also records `ESR_EL1`, `ELR_EL1`,
   `FAR_EL1`, `SPSR_EL1`, the address of `pxCurrentTCBs[core]` and up to 16
   bytes of its task name. A capture interrupted by a second fault is reported
   as incomplete. EL3-origin entries and entries that fail validation write
   only the pending byte.
4. **Supervisor.** `board_resident_renew` in
   [apps/os/services.c](../../apps/os/services.c) refuses renewal when
   `a733_fault_pending` is nonzero. On a refusal or an expired service window,
   the resident `supervise` task in [apps/board/main.c](../../apps/board/main.c)
   keeps its `board_fail` and Type-C revocation, then calls
   `a733_smp_exit_privileged(5, detail)`. Status 5 means that renewal
   stopped; the detail is one plus the index of the first expired or
   first-expiry-silent service, or 0 for a refused reload.
5. **Reboot.** WDT0, armed by the stage-2 loader in its four-second mode,
   expires and resets the board into a cold boot.
6. **Rotate.** At the next boot, `a733_cached_prepare` (`continue.c`) calls
   `a733_fault_boot()` at EL3 with translation off, after the resident
   result's stage-1 write and before publication and every secondary release.
   It copies into `a733_fault_previous` in EL1 `.bss` the previous resident
   result (which the stage-2 loader moved into bytes 192..319 of its report at
   `0x47008200`, taken only when that report holds its magic and status 6) and
   each slot whose magic and CRC match. It then zeroes the eight slots.
7. **Report.** USB0 operation 5 (`apps/os/usb0.c`; host side
   `tools/fault-report.py`, decoder `checks/fault-decode.py`) returns
   `a733_fault_previous`: an 8-byte header, the 128-byte previous resident
   result and eight slots, 1,160 bytes without a frame header.

A core parks after its first record, so each slot holds at most one record per
boot, and the epoch identifies the boot. Each core writes only its own slot and
pending byte; only `a733_fault_boot`, running single-threaded, writes
`a733_fault_previous`. Capture and rotation are built when
`SUNSTONE_A733_FAULT_PAGE` is defined, which `cmake/sunstone-os-core.cmake`
sets for the OS image. Images without it assemble byte-identical `continue.S`,
`continue.c` and `tasks.c` objects.

The single-core images keep their FEL transport. `platform_exception` in
[platform/a733/platform.c](../../platform/a733/platform.c) builds the same
record on its stack and writes it as one log line (`a733_fault_line`) that the
host decoder mirrors, with `SPSR_EL1`, the core number and the task name beside
the three registers. It does not write the slots.

## Constraint: the reboot path is watchdog expiry

The OS image has one reboot path: stop renewal and let the armed WDT0 expire.
It writes no watchdog register, so the resident contract holds: one WDT0
writer, mode `0x41`, configuration `1`, output `2`, no disarm. A short
interval or a software reset would add a second writer or require disarming
the watchdog, which the contract forbids. No PRCM system-reset register is
documented, and the RTC general-purpose registers are not used because their
boot0 owners are unknown. The armed state is defined by the A733 User Manual
V1.00, section 8.3.6.5, and [boot-slots.md](../boot-slots.md). Expiry and cold
reboot are unproven on the board ([watchdog.md](../watchdog.md)).

## Interrupts, memory and synchronization

No interrupt is added: the supervising task polls every 20 ms for renewal,
which section 3 permits.

- **Slots.** The eight slots occupy `0x42001000`–`0x42001400` inside the 16 KiB
  `.result` reservation at `0x42000000` ([link.ld](../../platform/a733-smp-cached/link.ld)).
  No other OS-image record uses that page. EL1 keeps no mapping of it. EL3 sees
  it as Secure Device-nGnRnE with translation on and as Secure Device memory
  with it off, so every access is uncached and needs no cache maintenance.
  Device memory faults on unaligned accesses and `DC ZVA`, so every slot and
  stage-2 report access is an aligned 64-bit load or store (`fault.S`;
  `volatile uint64_t` loops in `fault.c`, built with `-mstrict-align` and
  without `memset` or `memcpy`).
- **Pending word and previous-boot buffer.** Both are aligned EL1 `.bss`
  objects. `a733_fault_boot` writes the buffer before `a733_cached_prepare`
  publishes `0x41200000`..`__bss_end`, so both reach EL1 coherently. At run
  time EL3 stores the pending byte, and reads the task name, through its
  Nonsecure write-back alias of image RAM. The capture accepts a TCB pointer
  only when it is 8-byte aligned and the name lies inside
  `0x41200000`–`0x42000000` (`a733_extent_within`).
- **Barriers.** `DSB SY` after the pending byte; `DMB OSHST` between the slot
  fields and the magic; `DSB SY` after the magic, before the parking `WFE`;
  none on the reader, which makes one relaxed 64-bit load of the pending word
  per pass. Every writer stores a nonzero value, so two writers of byte 0
  cannot clear each other. The pending byte precedes the slot, so a second
  fault while reading the TCB still stops renewal; that slot then fails its CRC.

## Qualified figures

All figures come from host and instruction-emulation checks; no board ran the
capture.

| Figure | Budget | Result | Evidence |
| --- | --- | --- | --- |
| Modeled parks that end in a reboot | every park on 8 cores | 8,000 of 8,000 | `cpu-fault-reset.jsonl`: `cpu-fault-reset-host-budgets` |
| Park to watchdog expiry | 4.02 s | at most 3.999 s | same |
| Decode to pending store; capture leaf | 6; 250 instructions | 6; 85 | same |
| Supervisor pass | one load, no barrier | 52 instructions, one plain `ldr` | same |
| USB0 report | 1 tick | 1,160 bytes in 1 tick | same |
| Rotation bytes read | 1,152 | 1,248 to 1,384 (reads each valid slot twice; once per boot) | same |
| `.text`; `.bss` growth | +1 KiB; +1,168 bytes | +1,024; +1,280 | same |

## Limitations and open work

All parks are section 1.8 class 3: the core parks at EL3 and keeps every lock,
buffer and DMA job until reset. Other cores run for at most 4.02 s; the OS image
writes no storage. A stalled supervisor, a bus hang, an EL3-origin entry and a
failed validation stop renewal but leave no slot, and the next boot reports
those resets as unexplained. A cold boot and a watchdog reset cannot be
distinguished, because no retained reset-cause register is known
([watchdog-reset-evidence.md](../watchdog-reset-evidence.md)). That an SMC
leaves `x2` and the EL1 syndrome registers unchanged, and the single-copy
atomicity of byte stores, come from Arm DDI 0487, which is not pinned.

Board runs (record `cpu-fault-reset-board-retention`):

1. WDT0 expiry in the resident lifetime reboots into a cold boot.
2. The slots survive WDT0 reset and boot0's DRAM initialization with a valid
   CRC.
3. A deliberate load from an unmapped address in a test task, once on an A55
   and once on an A76, produces the expected `ESR_EL1`, `ELR_EL1` and `FAR_EL1`.
4. The previous-boot report arrives over USB0 after the reboot.

Not built, for lack of a consumer: an SGI to the supervisor, a USB0 reboot
command, slot capture of EL3-origin exceptions, stack or register dumps, and
fault capture in the QEMU platforms.
