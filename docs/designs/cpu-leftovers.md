# Design: drop the repeated table publication and read the fault report on the host

Status: **design**; nothing here is implemented. The baseline counts below come
from images built from commit `0fd1b51` and the host checks named in each row.
No figure has been measured on the board. Readers are assumed to know AArch64
data-cache maintenance by virtual address and the Secure and Non-secure
physical address spaces, and to have read
[cpu-table-walks.md](cpu-table-walks.md).

The change has two independent parts:

- **Table maintenance.** The cached eight-core images clean and invalidate
  their translation tables in four places at boot. One of them, the clean of
  the table section in `continue.c`, repeats work that core 0 has just done,
  and this change removes it. The Non-secure invalidation of the EL1 pages in
  `publication.S` stays until the conditions in "Why step 3 stays" hold.
- **Fault report.** USB0 operation 5 of the integrated OS image returns the
  previous boot's 1,160-byte fault report ([fault-capture.md](../fault-capture.md)),
  and no host program requests it. This change adds one.

## Part 1: table maintenance

### Current sequence

Core 0 runs `a733_cached_prepare()` (`platform/a733-smp-cached/continue.c`) at
EL3 with the MMU, data cache and instruction cache off, before any other core
is released. The table section `__tables_start`–`__tables_end` holds the five
EL3 and EL1 pages and the checksum line: 321 lines of 64 bytes in the
diagnostic image, and 449 in the supervised image, whose section also holds
the two temporary EL3 pages of `platform/a733-smp-supervised/publication.S`.

1. `a733_cached_memory_prepare()` (`memory.c`) runs `DC CIVAC` over the
   section, then `DSB SY`, then writes every descriptor and the checksum.
2. `continue.c` runs `DC CVAC` over the same section through
   `a733_cpu_coherence_publish()`.
3. Supervised image only: `a733_supervised_ns_publish()` cleans the two
   temporary EL3 pages and enables EL3 translation with `SCTLR_EL3.C` clear,
   through a temporary map that makes the Non-secure alias of DRAM Normal
   non-cacheable. Through that map it runs `DC IVAC` over image RAM and over
   the EL1 root and level-2 pages, 64 lines for each page.
4. Each core, at EL1 with the MMU off, runs `DC CIVAC` over the EL1 root and
   level-2 pages inside `a733_cpu_coherence_enable_el1()`, reads both pages
   back with Device loads and compares their sum with the recorded checksum
   before it sets `SCTLR_EL1.M`.

### Why step 2 finds nothing to do

Step 2 acts only on lines present when it runs, so the argument covers only
the window from step 1's `DC CIVAC` to the step 2 call. In that window core 0
is the only running core, no DMA master is admitted, and `SCTLR_EL3` M, C and
I are clear; `a733_cached_memory_prepare()` refuses to run otherwise. Every
data access in the window is therefore Device-nGnRnE and allocates no line.
The only other maintenance in the window is the `DC CVAC` publication of code
and initial RAM, which allocates nothing. Step 2 is issued by the same core in
the same translation state as step 1, so it reaches no cache that step 1
missed, and it cleans lines that step 1 has already removed.

Without step 2, the tables depend on the rule that data accesses at EL3 with
the MMU off allocate no cache line. If that rule failed, a dirty Secure line
over a descriptor could be written back after a core's checksum had passed,
and the failure would not be bounded. The record handshake already depends on
the same rule: `prepare_core(0)` writes `a733_cached_records` after the
publications with no maintenance, and each secondary reads those records at
EL3 with the MMU off.

### Why step 3 stays

Step 3 is the only Non-secure maintenance that core 0 runs on its own caches
before it releases the secondaries. Core 0's step 4 is not ordered before any
secondary's step 4 or before that secondary sets `SCTLR_EL1.M`. No pinned
source shows that maintenance issued with the MMU off is broadcast to other
cores. Without step 3, a Non-secure dirty line left in core 0's caches by the
boot loader could be written back after a secondary's checksum had passed, and
that secondary would then walk corrupted tables undetected. Step 3 forces such
a write-back before any secondary runs, where the secondary's checksum detects
it.

The `.Lel1_root` and `.Lel1_l2` loops can go once a pinned source proves that
MMU-off maintenance is broadcast, or once the Non-secure dirty-line experiment
of [cold-entry.md](../cold-entry.md) passes on every launch path.

The covering checks for step 2, which stay in place, are:

- `checks/cpu-table-walks.py`: the flush is called before the table builder,
  every flushed line precedes the first table-section write, and
  `enable_el1` issues 128 `DC CIVAC` and 512 `LDP` in the linked image.
- `checks/a733-cpu-coherence.py`: `enable_el1` flushes both EL1 pages before
  any system-register write, and a wrong checksum or a malformed root entry 1
  leaves `SCTLR_EL1.M` clear.

### Changes

- `continue.c`: drop the third `a733_cpu_coherence_publish()` call. The code
  and initial-RAM publications and the stop code `0x102` stay.
- `publication.S`: no change.

`continue.S` does not change, so the `continue.S` hash pins in
`cmake/a733-smp-observe.cmake` and `cmake/a733-storage-maintenance.cmake` stay
as they are. No pin covers `continue.c`.

**Interrupts, memory placement and synchronization.** Nothing changes. Step 2
ran on core 0 with exceptions masked, before any secondary release; the
tables keep their placement and attributes; the record barrier in `continue.c`
still orders table completion before every secondary's entry.

## Part 2: fault-report host tool

### Data flow and ownership

`a733_fault_boot()` builds `a733_fault_previous` on core 0 at EL3 before any
other core runs, and nothing writes it afterwards. Two publications make it
visible to EL1: the `DC CVAC` of `0x41200000`–`__bss_end` in `continue.c`, and
in the supervised image the image-RAM `DC IVAC` loop of `publication.S`. On
operation 5, `apps/mimus-board/usb0.c` copies the report once into the USB0
frame and sends 1,160 bytes with no frame header. A request whose flag, count,
offset or generation field is nonzero stops the board through `stop()`.

The new `tools/fault-report.py` holds only
`fetch(device, same, deadline, retain)`, a short copy of `device_exchange()` in
`apps/mimus-board/host.py`, which a comment names. It writes the 16-byte
request `struct.pack('<BBHIQ', 5, 0, 0, 0, 0)` to endpoint `0x02` once. It
then reads endpoint `0x81`, each read requesting the remaining length rounded
up to a multiple of 512 bytes, until it holds 1,160 bytes; it calls
`retain(raw)` to save the raw bytes durably and returns them. As in
`device_exchange()`, it calls `same()` before every transfer, so a changed USB
identity stops the exchange, and each transfer's timeout is the time left
before the deadline, capped at 500 ms and at least 1 ms. An empty fragment,
more than 1,160 bytes, a transport error or an expired deadline raises an
error. The function never retries, because a second request after a partial
response would mix two frames.

Callers decode with `decode_report()` of `checks/fault-decode.py`, which stays
the only definition of the report layout and the only command line for saved
reports. `docs/fault-capture.md` keeps pointing at it.

The tool opens no USB device and imports no USB library. `fetch()` receives a
device that a supervised launcher has already claimed: the optional
`--fault-report` flag of `tools/check-a733-os-supervised.py`, listed below,
which only the coordinator runs.

**Interrupts and polling.** The device side keeps the shared USB0 loop, which
the OS image services once per 1 ms tick. On the host each read blocks in the
USB stack; there is no sleep loop.

**Memory, cache policy and cores.** Nothing changes on the device. The report
is immutable after core 0 builds it, so the USB0 task reads it without a lock
or cache maintenance.

### Test

`checks/fault-report.py` drives `fetch()` against a modeled device that applies
the operation 5 rules of `usb0.c`, returns no more than each read requests, and
returns the response in fragments of 1, 29 and 512 bytes. It needs no image
and no USB access. Its cases are:

- a non-empty 1,160-byte report that `fetch()` returns unchanged byte for
  byte, after one `retain()` call with the same bytes;
- the request bytes, compared with `FAULT_OP` and the zero-field rule parsed
  from `apps/mimus-board/usb0.c`, and the response size, compared with
  `REPORT_BYTES` in the decoder;
- refusals: a device that stops without answering (one write, no retry), an
  extra byte after the report, an empty fragment, an identity change, and a
  deadline that expires before the write and during the reads;
- a static check of the syntax tree of `tools/fault-report.py`: it imports no
  `usb`, `usb1`, `libusb`, `serial` or `subprocess`, its text does not
  contain `/dev/bus/usb` or `usb-access`, and it has no `__main__` block or
  argument parser that could call `fetch()`.

## Performance budgets

| Metric | Baseline (`0fd1b51`) | Budget | Measured by |
| --- | --- | --- | --- |
| Table-section `DC CVAC` in `a733_cached_prepare()` | 321 lines (diagnostic), 449 (supervised); one publish call | 0 lines; no call names `__tables_start` | `checks/cpu-table-walks.py`, from the disassembly of `a733_cached_prepare()` |
| Table-line maintenance per boot, eight-core diagnostic image (derived) | 1,666 (321 + 321 + 8 × 128) | 1,345 | sum of the step 1 count and the step 2 count in `checks/cpu-table-walks.py`, and eight times its `enable_el1` count |
| Table-line maintenance per boot, supervised image (derived) | 2,178 (449 + 449 + 128 + 128 + 8 × 128) | 1,729 | the same counts, plus the two 128-line step 3 terms from `checks/a733-supervised-publication.py` |
| Lines that `a733_supervised_ns_publish()` maintains | 42,172 | 42,172 | `checks/a733-supervised-publication.py`, `maintained_lines` |
| EL1 table `DC CIVAC` per core before `SCTLR_EL1.M` | 128 | 128 | `checks/cpu-table-walks.py` |
| Pool translations; EL1 map | 392 `AT`; golden map | unchanged | `checks/device-memory-capacity.py` |
| Operation 5 response on the device | 1,160 bytes in 1 tick | unchanged | `checks/usb0-service-burst.py` |
| Host transfers per report | none | 1 request, at most 3 reads with 512-byte fragments, 0 retries | `checks/fault-report.py` |
| Copies per report byte on the host | none | one: the fragments are appended to one buffer | `checks/fault-report.py` |

The line counts are the budgets. The time saved is an estimate: no source is
pinned for the cost of one maintenance operation. At 494 MHz and an assumed 70
cycles per operation, the 321 removed operations save about 0.05 ms per boot
in the diagnostic image, and the 449 removed operations about 0.06 ms in the
supervised image.
QEMU and the Unicorn models do not model caches, so they show only that each
image still reaches cached EL1 on all eight cores with unchanged tables.

## Left out

- The removal of the `.Lel1_root` and `.Lel1_l2` loops, which the scope
  names, for the reasons in "Why step 3 stays".
- The `DC CVAC` of the two temporary EL3 pages in `publication.S` (128 lines).
  The Non-secure dirty-line experiment in [cold-entry.md](../cold-entry.md)
  uses that temporary mapping, and the harness should not change before the
  experiment runs.
- A supervised per-core flush assertion in
  `checks/a733-supervised-publication.py`. With step 3 kept, the supervised
  image's EL1-page maintenance is unchanged, and such an assertion covers no
  case that `checks/cpu-table-walks.py` and `checks/a733-cpu-coherence.py`
  miss.
- A host command that opens the board itself. Board access belongs to the
  coordinator's supervised launchers.
- Changes to `usb0.c` or the report layout. The device side already meets its
  budget.

## Board evidence required

Run only in scheduled board sessions:

- **Supervised and diagnostic boot.** Each rebuilt image reaches the scheduler
  on all eight cores with no stop `0x102`, `0x108` or EL1 table-check refusal.
- **Report after reset.** After a WDT0 reset, `fetch()` returns a report whose
  slots match the deliberate fault, which is item 3 of the board evidence in
  [fault-capture.md](../fault-capture.md).

## Sources and unproven steps

- `DC IVAC` cleaning a dirty line on the Cortex-A55: TRM 100442_0200_00,
  section A6.2.4, pinned as recorded in [cpu-table-walks.md](cpu-table-walks.md).
- **Unproven:** that EL3 and EL1 with the MMU off address the Secure and the
  Non-secure space, that Device accesses allocate no cache line, and that
  maintenance issued with the MMU off reaches every core. These come from the
  Arm Architecture Reference Manual, which is not pinned. Removing step 2
  makes the tables depend on the second rule, on which the record handshake
  already depends; keeping step 3 avoids a new dependence on the third.
- This change adds no hardware register sequence.

## Changes outside the owned files

These go to the primary agent in `primary.patch`:

- `checks/cpu-table-walks.py`: measure the table publication from the
  disassembly of `a733_cached_prepare()` and require 0 lines.
- `checks/device-memory-capacity.c`, `checks/device-memory-capacity-local.c`
  and `checks/cached-device-reservations.c`: drop the table publication from
  each fixture's copy of the core-0 sequence, so the capacity checks run the
  sequence the images run.
- `tools/check-a733-os-supervised.py`: an optional `--fault-report` flag that
  calls `fetch()` once before the capture and saves the raw report and its
  `decode_report()` result in the evidence directory. The launcher refuses the
  flag before any device access unless the frozen `image.elf` defines
  `a733_fault_previous`, which only the OS image, built with
  `SUNSTONE_A733_FAULT_PAGE`, links. A `fetch()` error ends the run before any
  capture exchange.
- `checks/os-image-launch.py`: the flag is refused for an image without
  `a733_fault_previous`, and with the flag the host mode still opens no device
  and never calls the transport.
- `checks/index.json`, regenerated by `checks/check-index.py` for the new check.
- `docs/cpu-coherence.md` (maintenance table and totals) and
  `docs/a733-smp-cached.md` (boot step 1): state the sequence after the change.
