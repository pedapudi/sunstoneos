# Design: retained per-core fault records and one watchdog reboot path

Status: **design**; no code exists yet. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

In the resident lifetime of the integrated OS image
([docs/os-image.md](../os-image.md)), an EL1 exception on any core takes the
stackless path `a733_cached_cold_fault`, which issues an SMC with the EL1
syndrome (`ESR_EL1`) in `x1`. The EL3 handler `a733_cached_el3_fault`
([continue.S](../../platform/a733-smp-cached/continue.S)) stores `ESR_EL3`,
`FAR_EL3` and `ELR_EL3` in the core's record. For an SMC these describe the
SMC instruction, so the faulting address and instruction are lost. Only core 0
then calls `a733_smp_stop`. The record lives in monitor `.bss`, which the next
boot clears, and no image reports it over USB0. A fault on a core that runs
only unsupervised tasks, such as an idle task, parks that core while the
supervising task keeps renewing WDT0, so the board never reboots.

This package records each EL1 fault in a page that survives the reboot, makes
every recorded fault stop watchdog renewal, and reports the previous boot's
records over USB0.

## Data flow and ownership

1. **Capture.** The EL1 vector slot number travels in `x2` through the
   existing stackless path and SMC. At EL3, `a733_fault_capture_el3(core,
   slot)` in `platform/a733/fault.c` runs on every core. It first writes byte
   `core` of the pending word, then fills the core's 128-byte slot:
   `ESR_EL1`, `ELR_EL1`, `FAR_EL1`, `SPSR_EL1`, the vector slot,
   `CNTPCT_EL0`, the contract epoch, a per-page fault sequence number, the
   address of `pxCurrentTCBs[core]` and up to 16 bytes of its task name. A
   CRC32C over the slot and the magic number are written last. The SMC does
   not change the EL1 registers, so EL3 reads them directly.
2. **Stop renewal.** `board_resident_renew` in
   [apps/os/services.c](../../apps/os/services.c) refuses renewal when the
   pending word is nonzero. It then runs the existing close path:
   `board_fail`, Type-C revocation and park.
3. **Reboot.** WDT0, armed by the stage-2 loader in its four-second mode,
   expires and resets the board into a cold boot.
4. **Rotate.** At the next boot, core 0 at EL3 calls `a733_fault_boot()`
   before it releases the other cores. Each current slot whose magic and
   CRC match moves to the previous half of the page; the current half and the
   pending word are cleared.
5. **Report.** A USB0 request byte of 1 returns the previous half: an 8-byte
   header, the 16-byte reboot reason and eight slots, 1,048 bytes in all.

The page is 4 KiB at physical address `0x421fe000`, inside the
`0x42000000`–`0x42200000` monitor block. Each core writes only its own slot
and its own pending byte. The supervising task writes only the reason word,
which records the service whose window expired or a refused renewal.
`a733_fault_boot` runs single-threaded. The page belongs to images that define
`SUNSTONE_A733_FAULT_PAGE`; the OS image is the only one.

The single-core images keep their FEL transport. `platform_exception` in
[platform/a733/platform.c](../../platform/a733/platform.c) builds the same
record on its stack and writes it as one log line through a formatter that the
host decoder mirrors. It adds `SPSR_EL1`, the core number and the task name to
the three registers printed today. It does not write the page, because a
FEL-launched image never rotates it. The edit stays inside the function body,
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

Both EL1 and EL3 map the monitor block as Normal Non-cacheable
([a733_cpu_coherence.h](../../include/a733_cpu_coherence.h), lines 54–58), so
the page needs no cache maintenance. EL3 reads the TCB name through its own
mapping of image RAM. The header states that those descriptors select the
same nonsecure physical addresses as EL1; the primary agent confirms that
they also carry the EL1 write-back attributes, which the read needs for
coherence with EL1 stores. The capture accepts a
TCB pointer only when it is 8-byte aligned and the name lies inside
`0x41200000`–`0x42000000` (`a733_extent_within` from
[a733_range.h](../../include/a733_range.h)).

The capture is a stackless leaf, because the faulting core's stack and EL3 stack
state are not trusted. Barriers follow section 1.2:

- `DMB OSHST` between the slot fields and the magic number;
- `DSB SY` after the pending byte and after the magic number, as completion
  before the parking `WFE`;
- none on the reader, which makes one 64-bit load of the pending word per pass.

Byte stores to distinct bytes of one word need no atomic read-modify-write,
which is avoided because exclusives on Non-cacheable memory are
implementation-defined. The pending byte is written before the slot, so a
second fault while reading the TCB still stops renewal; that slot then fails
its CRC and is reported as incomplete.

## Reuse and removal

The design reuses the stackless EL1 fault path and its EL3 core validation,
the supervisor's close path, the existing monitor-block mapping, the USB0
exchange and its 1,160-byte response buffer, and
`checks/os-image-model.c`'s modeled watchdog. It removes `platform_exception`'s
separate printing in favor of the shared record. The EL3 `a733_cached_records`
fields stay, because the FEL host checks read their layout. No MMIO helper is
needed. The page mapping is checked once at OS admission with
`a733_mapping_check` (Non-cacheable, privileged read and write).

## Budgets

| Metric | Baseline | Budget | Measured by |
| --- | --- | --- | --- |
| EL1 faults that end in a reboot | Faults that stop a supervised service, or on core 0 (counted) | Every fault, on each of the 8 cores | `checks/reset-latency.c`: `services.c` with the modeled watchdog, one injected fault per core |
| Request to watchdog expiry | Unbounded for a core without a supervised service | At most 4.02 s (one 20 ms pass plus 4 s) | Same model, fault at each phase of the renewal second |
| Fields per fault | `ESR_EL1` only (counted) | Nine fields and the task name, listed above | `checks/fault-capture.py` decodes the slot |
| Capture cost | Existing EL3 report, validation to publication: about 36 instructions (counted) | At most 250 instructions; no stack, MMIO or lock; writes only the core's slot and byte | Unicorn with modeled system registers; memory-write log |
| Supervisor cost per 20 ms pass | 0 | One 64-bit load; no lock or barrier | Disassembly of `board_resident_renew` in the same check |
| Boot rotation | None | 1 KiB read and 2 KiB written, once per boot; no cache maintenance | Unicorn access count |
| USB0 report | None | One 1,048-byte response in 1 tick | `checks/usb0-service-burst.py`, 1,160-byte row |
| Single-core fault line | 3 registers | At most 160 bytes; patch applies with fuzz 0 | Build of every target that compiles `platform.c` |
| Memory | None | One 4 KiB page; code at most 1 KiB | Linker assertion; `nm` sizes |

No hot path changes apart from the supervisor's load.

## Failure classes

All faults are section 1.8 class 3: the faulting core parks at EL3 and keeps
every lock, buffer and DMA job until reset. Other cores run for at most 4.02 s;
the OS image writes no storage. A stalled supervisor or a bus hang stops renewal
already and leaves no reason recorded, which the next boot reports as an
unexplained reset. A cold boot and a watchdog reset cannot be distinguished,
because no retained reset-cause register is known
([docs/watchdog-reset-evidence.md](../watchdog-reset-evidence.md)).

## Changes outside the owned files

These go into one patch for the primary agent:

- `platform/a733-smp-cached/continue.S`: pass the vector slot in `x2`; call
  the capture for every core in fault-page builds.
- `platform/a733-smp-cached/boot.c`: call `a733_fault_boot()` before secondary
  release.
- `platform/a733-smp-cached/link.ld` and `link-devices.ld`: a `NOLOAD` page at
  `0x421fe000` and an assertion that the tables end below it.
- The FreeRTOS task additions header: export `offsetof(TCB_t, pcTaskName)`.
- `apps/os/services.c` and `apps/board/usb0.c`: the renewal refusal, reason
  word and request byte 1.

## Unproven and left out

**Not proven by a pinned source:** that an SMC leaves `ESR_EL1`, `ELR_EL1`,
`FAR_EL1` and `SPSR_EL1` unchanged, and the single-copy atomicity of byte
stores. Both come from the Arm Architecture Reference Manual (DDI 0487), which
is not pinned. TF-A `0892ad4d` `arch.h` (pinned under `cpu-entry/`) supplies the
`ESR_EC_SHIFT` and `SPSR_M` field positions the decoder uses.

Left out: an SGI to the supervisor, a USB0 reboot command, capture of EL3
faults (they keep the existing report), stack or register dumps, and fault
capture in the QEMU platforms. None has a consumer.

## Board evidence

Each step needs a scheduled board session under
[docs/validation.md](../validation.md):

1. WDT0 expiry in the resident lifetime reboots into a cold boot (shared with
   the watchdog expiry trial).
2. The page at `0x421fe000` survives WDT0 reset and boot0's DRAM
   initialization with a valid CRC. The stage-2 loader's copy of the previous
   result relies on the same unproven retention.
3. A deliberate load from an unmapped address in a test task, once on an A55
   and once on an A76, produces the expected `ESR_EL1`, `ELR_EL1` and
   `FAR_EL1`.
4. The previous-boot report arrives over USB0 after the reboot.
