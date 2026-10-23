# Design: retained per-core fault records and one watchdog reboot path

Status: **implemented**; host and instruction checks pass, board evidence is
outstanding. The component documents are [fault-capture.md](../fault-capture.md)
and [reset.md](../reset.md); the implementation departs from this design where
[Implementation and measurements](#implementation-and-measurements) says so.
Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

In the resident lifetime of the integrated OS image
([docs/os-image.md](../os-image.md)), an EL1 exception on any core takes the
stackless path `a733_cached_cold_fault`, which issues an SMC with the EL1
syndrome (`ESR_EL1`) in `x1`. The EL3 handler `a733_cached_el3_fault`
([continue.S](../../platform/a733-smp-cached/continue.S)) stores `ESR_EL3`,
`FAR_EL3` and `ELR_EL3` in the core's record. For an SMC these describe the
SMC instruction, so the faulting address and instruction are lost. Only core 0
then calls `a733_smp_stop`. The record lives in monitor `.bss`, which the next
boot clears, and no image reports it over USB0. Several parks leave WDT0
renewal running: a fault or deliberate exit on a core without a supervised
service, an exception taken at EL3, and an entry that fails the handler's
state and core checks. The board then never reboots.

This package makes every park of any core stop renewal, records each EL1
fault and deliberate exit in the existing retained-result reservation, and
reports the previous boot's records over USB0.

## Data flow and ownership

1. **Mark.** Every EL1 vector entry, in the cold and the runtime tables, sets
   `x2 = 0x100 | slot` before the SMC. `.Lsync_fault` takes the slot from
   `SPSR_EL1.M`: 4 for EL1h, 8 for EL0. `a733_cached_smc`, the only
   deliberate-exit SMC, sets `x2 = 0`. A deliberate exit therefore cannot
   produce bit 8, whatever status it passes in `x0`.
2. **Stop renewal.** Immediately after the MPIDR decode, before the state and
   core checks and before any other memory access, `a733_cached_el3_fault`
   stores a nonzero byte at index `core` of `a733_fault_pending`, followed by
   `DSB SY`. This runs on every entry, including EL3-origin vector slots. An
   MPIDR that does not decode to a core stores byte 0. Every park of every
   core therefore stops renewal.
3. **Capture.** After the existing validation, for SMC entries (EL3 vector
   slot 8), the stackless assembly leaf `a733_fault_capture` in
   `platform/a733/fault.S` fills the core's 128-byte slot. The slot holds the
   kind, the status and detail from `x0` and `x1`, the vector slot,
   `CNTPCT_EL0` and the contract epoch. The kind is a vector fault when `x2`
   bit 8 is set, and a deliberate exit otherwise. Only a vector fault also records `ESR_EL1`, `ELR_EL1`, `FAR_EL1`,
   `SPSR_EL1`, the address of `pxCurrentTCBs[core]` and up to 16 bytes of its
   task name. A CRC32C over the slot and the magic number are written last.
   EL3-origin entries and entries that fail validation write only the
   pending byte.
4. **Supervisor.** `board_resident_renew` in
   [apps/os/services.c](../../apps/os/services.c) refuses renewal when
   `a733_fault_pending` is nonzero. On a refusal or an expired service window,
   the resident `supervise` task in [apps/board/main.c](../../apps/board/main.c)
   keeps its `board_fail` and Type-C revocation, then calls
   `a733_smp_exit_privileged(5, detail)` instead of suspending itself. Status
   5 means that renewal stopped; the detail is one plus the index of the
   first expired service, or 0 for a refused reload. The reason thus reaches
   the supervisor core's slot as a deliberate exit, and on core 0 the existing
   `a733_smp_stop` also writes it to the resident result.
5. **Reboot.** WDT0, armed by the stage-2 loader in its four-second mode,
   expires and resets the board into a cold boot.
6. **Rotate.** At the next boot, `a733_smp_boot` on core 0 calls
   `a733_fault_boot()` at EL3 with translation off. The call follows the
   resident result's stage-1 write and precedes `a733_cached_prepare` and
   every secondary release. It copies two things into `a733_fault_previous`
   in EL1 `.bss`:
   - the previous resident result, which the stage-2 loader moved into bytes
     192..319 of its report at `0x47008200` before clearing the result magic,
     taken only when that report holds its magic and status 6 (entered);
   - each slot whose magic and CRC match.

   It then zeroes the eight slots.
7. **Report.** A USB0 request byte of 1 returns `a733_fault_previous`: an
   8-byte header, the 128-byte previous resident result and eight slots,
   1,160 bytes in all.

The stage-2 loader clears the resident result before the OS image starts, and
the OS image does not otherwise serve the stage-2 report, so step 6 extends
that handoff instead of adding a reason word. Records are ordered by
`CNTPCT_EL0`. A core parks after its first record, so each slot holds at most
one record per boot, and the epoch identifies the boot. No sequence number is
kept.

Each core writes only its own slot and its own pending byte. Only
`a733_fault_boot`, running single-threaded, writes `a733_fault_previous`.
Capture and rotation are built when `SUNSTONE_A733_FAULT_PAGE` is defined; the
OS image is the only image that defines it.

The single-core images keep their FEL transport. `platform_exception` in
[platform/a733/platform.c](../../platform/a733/platform.c) builds the same
record on its stack and writes it as one log line through a formatter that the
host decoder mirrors. It adds `SPSR_EL1`, the core number and the task name to
the three registers printed today. It does not write the slots, because a
FEL-launched image never rotates them. The edit stays inside the function body,
so `apps/storage-read-single-core/platform.patch` still applies with fuzz 0.

## The reboot path is watchdog expiry

The OS image has one reboot path: stop renewal and let the armed WDT0 expire.
Fault capture and the supervising task both enter it, and it writes no
watchdog register. The resident contract therefore stays as specified: one
WDT0 writer, mode `0x41`, configuration `1`, output `2`, and no disarm. The
A733 User Manual V1.00, section 8.3.6.5 (configuration `01` resets the whole
system), and the loader sequence in [docs/boot-slots.md](../boot-slots.md)
define the armed state. Expiry and cold reboot are **unproven** on the board
([docs/watchdog.md](../watchdog.md)).

Two faster sequences are left out:

- **Short interval.** The pinned board kernel `sunxi_wdt.c` (`wdt_v103`,
  `sunxi_wdt_restart`) writes mode interval 0 (0.5 s) with enable, then
  restarts. This adds a second WDT0 writer and a mode value that the contract
  checks reject, to save at most 3.5 s on a path followed by a multi-minute
  slot load.
- **Software reset.** The pinned bootloader `b791be84`
  `arch/arm/mach-sunxi/board_sun60iw2.c` (`sunxi_board_reset_cpu`) writes
  `WDT_SRST_REG`. Manual section 8.3.6.3 requires the watchdog timer to be
  disabled for that use, which the resident contract forbids.

No PRCM system-reset register appears in the manual. The RTC general-purpose
registers (section 9.1.7.14) survive reset, but bootloader `rtc.c` uses
indices 0, 2 and 6, and the boot0 owners of the remaining indices are not
known. They are not used.

## Interrupts, memory and synchronization

No interrupt is added. The supervising task already polls every 20 ms for
renewal, which section 3 permits; an SGI would shorten a 4 s path by at most
20 ms.

**Slots.** The eight slots occupy `0x42001000`–`0x42001400`, inside the 16 KiB
`.result` reservation at `0x42000000` in
[link.ld](../../platform/a733-smp-cached/link.ld). No OS-image record uses that
page: the resident result ends at `0x42000080`, the native startup records
start at `0x42002000` and memtest uses `0x42003000`. EL1 keeps no mapping of
the monitor block ([memory.c](../../platform/a733-smp-cached/memory.c) clears
`a733_cached_el1_l2[16]`). EL3 sees the page as Secure Device-nGnRnE
(attribute index 0, NS clear) with translation on, and as Secure Device
memory with translation off. Every slot access is therefore uncached and in
one physical address space, and needs no cache maintenance. Device memory
faults on unaligned accesses and `DC ZVA`, so every slot and stage-2 report
access is an aligned 64-bit load or store: assembly in `fault.S`, and
`volatile uint64_t` loops in `fault.c`, built with `-mstrict-align` and
without `memset` or `memcpy`.

**Pending word and previous-boot buffer.** Both are aligned EL1 `.bss`
objects: the 64-bit `a733_fault_pending` and the 1,160-byte
`a733_fault_previous`. [start.S](../../platform/a733-smp-cached/start.S)
clears `.bss` with translation off, and `a733_fault_boot` writes the buffer
before `a733_cached_prepare`, whose existing `a733_cpu_coherence_publish` of
`0x41200000`..`__bss_end` publishes both objects before any core enters EL1.
At run time EL3 stores the pending byte, and reads the task name, through its
Nonsecure write-back alias of image RAM (`a733_cpu_coherence_tables(..., 3)`),
which is coherent with EL1. An EL3 entry with translation still off precedes
the core's state 3 in [continue.c](../../platform/a733-smp-cached/continue.c);
no core enters EL1 before all eight reach state 3, so no renewal exists yet
and the startup deadline stops the boot. The capture accepts a TCB pointer only when it is 8-byte aligned
and the name lies inside `0x41200000`–`0x42000000` (`a733_extent_within` from
[a733_range.h](../../include/a733_range.h)).

**Barriers** follow section 1.2:

- `DSB SY` after the pending byte, before validation;
- `DMB OSHST` between the slot fields and the magic number;
- `DSB SY` after the magic number, before the parking `WFE`;
- none on the reader, which makes one relaxed 64-bit atomic load of the
  pending word per pass.

Byte stores to distinct bytes of one word need no atomic read-modify-write.
Every writer stores a nonzero value, so two writers of byte 0 cannot clear
each other. The pending byte precedes the slot, so a second fault while
reading the TCB still stops renewal; that slot then fails its CRC and is
reported as incomplete.

## Reuse and removal

The design reuses these parts:

- the stackless EL1 fault path and its EL3 core validation;
- the retained-result reservation and the stage-2 handoff of the resident
  result;
- the existing `.bss` clear and image-RAM publication;
- the supervisor's close path;
- the USB0 exchange and its 1,160-byte response buffer;
- `checks/os-image-model.c`'s modeled watchdog.

It removes `platform_exception`'s
separate printing in favor of the shared record. The EL3 `a733_cached_records`
fields stay, because the FEL host checks read their layout. No MMIO helper or
mapping check is needed, because no mapping is added.

## Budgets

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| Parks that end in a reboot | EL1 faults that stop a supervised service, or on core 0 (counted) | Every park on each of the 8 cores: vector fault, deliberate exit, EL3-origin entry, failed validation | `checks/reset-latency.c`: `services.c` with the modeled watchdog, one injected case of each kind per core |
| Request to watchdog expiry | Unbounded for a core without a supervised service | At most 4.02 s (one 20 ms pass plus 4 s) | Same model, park at each phase of the renewal second |
| Fields per record | `ESR_EL1` only (counted) | Kind, status, detail, vector slot, counter, epoch; for vector faults also four EL1 registers and the task name | `checks/fault-capture.py` decodes the slot; a deliberate exit with a stale `x2` decodes as an exit |
| Capture cost | Existing EL3 report, validation to publication: about 36 instructions (counted) | Pending byte within 6 instructions of the MPIDR decode; capture at most 250; no stack, MMIO or lock; writes only the core's slot and byte | Unicorn with modeled system registers; memory-write log |
| Supervisor cost per 20 ms pass | 0 | One 64-bit load; no lock or barrier | Disassembly of `board_resident_renew` in the same check |
| Boot rotation | None | 1,152 bytes read, 1,160 bytes written to RAM and 1 KiB cleared, once per boot; no added cache maintenance | Unicorn access count |
| USB0 report | None | One 1,160-byte response in 1 tick | `checks/usb0-service-burst.py`, 1,160-byte row |
| Single-core fault line | 3 registers | At most 160 bytes; patch applies with fuzz 0 | Build of every target that compiles `platform.c` |
| Memory | None | No new reservation; 1 KiB of `.result` and 1,168 bytes of `.bss`; code at most 1 KiB | `nm` sizes |

No hot path changes apart from the supervisor's load.

## Failure classes

All parks are section 1.8 class 3: the core parks at EL3 and keeps every
lock, buffer and DMA job until reset. Other cores run for at most 4.02 s; the
OS image writes no storage. A stalled supervisor or a bus hang stops renewal
already and leaves no slot, as do an EL3-origin entry and a failed
validation; the next boot reports these resets as unexplained. A cold boot and a watchdog reset cannot be distinguished, because
no retained reset-cause register is known
([docs/watchdog-reset-evidence.md](../watchdog-reset-evidence.md)).

## Changes outside the owned files

These go into one patch for the primary agent:

- `platform/a733-smp-cached/continue.S`: the `x2` marker in both EL1 vector
  tables, `.Lsync_fault` and `a733_cached_smc`. In `a733_cached_el3_fault`,
  keep the incoming `x2`, store the pending byte after the MPIDR decode, and
  call `a733_fault_capture` after validation in fault-page builds.
- `platform/a733-smp-cached/boot.c`: call `a733_fault_boot()` before
  `a733_cached_prepare`.
- The FreeRTOS task additions header: export `offsetof(TCB_t, pcTaskName)`.
- `apps/os/services.c`, `apps/board/main.c` and `apps/board/usb0.c`: the
  renewal refusal and expired-service index, the resident supervisor's
  status-5 exit, and request byte 1.
- `cmake/a733-os.cmake`: add `fault.S` and `fault.c`, the latter with
  `-mstrict-align`.

The linker scripts and `memory.c` do not change.

## Unproven and left out

**Not proven by a pinned source:** that an SMC leaves `x2`, `ESR_EL1`,
`ELR_EL1`, `FAR_EL1` and `SPSR_EL1` unchanged, and the single-copy atomicity of
byte stores. Both come from the Arm Architecture Reference Manual (DDI 0487),
which is not pinned. TF-A `0892ad4d` `arch.h` (pinned under `cpu-entry/`)
supplies the `ESR_EC_SHIFT` and `SPSR_M` field positions the decoder uses.

Left out: an SGI to the supervisor, a USB0 reboot command, slot capture of
EL3-origin exceptions (they stop renewal and keep the existing monitor
report), stack or register dumps, and fault capture in the QEMU platforms.
None has a consumer.

## Board evidence

Each step needs a scheduled board session under
[docs/validation.md](../validation.md):

1. WDT0 expiry in the resident lifetime reboots into a cold boot (shared with
   the watchdog expiry trial).
2. The slots at `0x42001000` survive WDT0 reset and boot0's DRAM
   initialization with a valid CRC. The stage-2 loader's copy of the previous
   result relies on the same unproven retention.
3. A deliberate load from an unmapped address in a test task, once on an A55
   and once on an A76, produces the expected `ESR_EL1`, `ELR_EL1` and
   `FAR_EL1`.
4. The previous-boot report arrives over USB0 after the reboot.

## Implementation and measurements

The implementation departs from the design in five places:

- **USB0 request.** The OS image's USB0 exchange is the mimus board's
  16-byte request (`apps/mimus-board/usb0.c`), whose operation 1 already
  copies transcript text. The report is therefore operation 5, with zero
  count, offset and generation. Its response is the 1,160-byte report without
  the 64-byte frame header, and the frame buffer grows from 1,088 to 1,160
  bytes. `apps/board/usb0.c` is unchanged.
- **Rotation call.** `a733_cached_prepare` (`continue.c`) calls
  `a733_fault_boot` first. A call in `boot.c` would change the context lines
  of `apps/board/boot.patch`. The order is the same: after the resident
  result's stage-1 write, before publication and every secondary release.
- **EL1t slot.** In the OS image the storage patch routes EL1t synchronous
  exceptions to the runtime handler, so `.Lsync_fault` also records slot 0
  for `SPSR_EL1.M` = EL1t.
- **Open marker.** The capture writes `A733FLT0` to word 0 before the fields.
  A capture interrupted by a second fault is then reported as incomplete, and
  random DRAM after power-on is not.
- **Stop detail.** A service still silent at the first expiry also gives one
  plus its index. The watchdog expires before that pass in the model, so the
  value is reachable only if the reset fails.

`continue.S` keeps every line that `platform/a733-storage-maintenance/continue.patch`
and `platform/a733-smp-observe/continue.patch` use as context: the vector
macro records the slot with an assembler counter. Both patches apply without
fuzz, and the three review hashes of `continue.S` in `cmake/` name the new
file. Images without `SUNSTONE_A733_FAULT_PAGE` assemble byte-identical
`continue.S`, `continue.c` and `tasks.c` objects.

Measurements compare the resident OS image (`-DSUNSTONE_A733_OS=ON
-DSUNSTONE_A733_OS_RESIDENT=ON`) built from the branch's merge base with the
changed image.

| Metric | Merge base | Changed | Budget | Source |
| --- | --- | --- | --- | --- |
| Modeled parks that end in a reboot | 1,000 of 8,000 (core 0) | 8,000 of 8,000 | All | `checks/reset-latency.py` |
| Park to watchdog expiry | Unbounded on cores 1..7 | At most 3.999 s | 4.02 s | Same |
| Executed park entries that store the pending byte | 0 | 72 of 72 (9 kinds, 8 cores) | All | `checks/fault-capture.py` |
| Fields per record | `ESR_EL1` (as detail) | 11 nonzero for a vector fault | As designed | Same |
| Decode to pending store | None | 6 instructions | 6 | Same |
| Capture leaf | None | 85 instructions | 250 | Same |
| EL3 entry to record publication, deliberate exit | 122 instructions | 203 instructions | No budget | Same |
| Supervisor pass | 40 instructions, no pending load | 52 instructions; one plain `ldr`, no barrier or atomic | One load | Disassembly in the same check |
| Rotation | None | 1,875 instructions; reads 968 bytes with no valid slot, 1,384 in the check's mix; writes 1,160 bytes and clears 1,024 | 1,152 read, 1,160 written | Same |
| USB0 report | None | 1,160 bytes in 1 tick | 1 tick | `checks/usb0-service-burst.py` |
| Single-core line | 84 bytes, 3 registers | At most 137 bytes, 4 registers, core and task | 160 | `checks/fault-capture.py` |
| `.text` / `.bss` | | +1,024 / +1,280 bytes | 1 KiB / 1,168 | `size -A` |

The rotation reads two report header words and, for each valid slot, the
slot a second time while copying it; with valid slots it exceeds the
1,152-byte read budget by those words. The `.bss` growth exceeds its budget by
the 72-byte larger USB0 frame, the stop detail word and alignment. The EL3
path to publication grows by the capture and the pending store. None of
these runs outside a fault, a boot or a report request.

