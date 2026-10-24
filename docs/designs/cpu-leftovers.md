# Design: drop two redundant table-maintenance loops and read the fault report on the host

Status: **design**; nothing here is implemented. The baseline counts below come
from images built from commit `0fd1b51` and the host checks named in each row.
No figure has been measured on the board. Readers are assumed to know AArch64
data-cache maintenance by virtual address and the Secure and Non-secure
physical address spaces, and to have read
[cpu-table-walks.md](cpu-table-walks.md).

The change has two independent parts:

- **Table maintenance.** The cached eight-core images clean and invalidate
  their translation tables in four places at boot. Two of them now repeat work
  that an earlier or later step already does on every core, and this change
  removes them.
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

1. `a733_cached_memory_prepare()` runs `DC CIVAC` over the section, then
   `DSB SY`, then writes every descriptor and the checksum.
2. `continue.c` runs `DC CVAC` over the same section through
   `a733_cpu_coherence_publish()`.
3. Supervised image only: `a733_supervised_ns_publish()` maps the Non-secure
   alias of DRAM at EL3 and runs `DC IVAC` over image RAM and over the EL1 root
   and level-2 pages, 64 lines for each page.
4. Each core, at EL1 with the MMU off, runs `DC CIVAC` over the EL1 root and
   level-2 pages inside `a733_cpu_coherence_enable_el1()`, reads both pages
   back with Device loads and compares their sum with the recorded checksum
   before it sets `SCTLR_EL1.M`.

### Why steps 2 and 3 find nothing to do

A data-cache line over a table address can exist only after a cacheable access
to that address. Between step 1 and each core's step 4 no such access occurs:

- Core 0 accesses the section with `SCTLR_EL3` M, C and I clear, so every data
  access is Device-nGnRnE and allocates no line. `a733_cached_memory_prepare()`
  refuses to run otherwise.
- After `a733_cpu_coherence_enable_el3()`, EL3 maps the whole table pages
  Normal non-cacheable and the checksum line Device. The eight-core model in
  `checks/a733-smp-cached.py` refuses any CPU access to
  `0x421f0000`–`0x42200000` after a core enables EL3 translation, except the
  step 4 Device reads at EL1 with the MMU off.
- EL1 has no mapping of the section: EL1 level-2 entry 16 is invalid.
- The temporary EL3 map of step 3 runs with `SCTLR_EL3.C` clear and maps the
  Non-secure alias Normal non-cacheable.
- No secondary core runs and no DMA master is admitted before core 0 releases
  the secondaries.

Step 2 therefore cleans lines that step 1 has already removed. A `DC CVAC`
issued under the same conditions as step 1 reaches the same caches, so dropping
it removes no coverage that step 1 lacks.

Step 3 acts on the Non-secure lines that the EL1 walker can read. Step 4 does
the same on every core, immediately before that core's first walk, and checks
the result. It uses `DC CIVAC`, which never discards data; on the Cortex-A55,
`DC IVAC` also cleans a dirty line first (TRM 100442_0200_00, section A6.2.4),
but no Cortex-A76 source is pinned for that behaviour. Step 4 is also the only
maintenance that each core is guaranteed to run on its own caches, whether or
not maintenance issued with the MMU off is broadcast to other cores. Step 3
removes nothing that step 4 would miss.

The covering checks, which stay in place, are:

- `checks/cpu-table-walks.py`: the flush is called before the table builder,
  every flushed line precedes the first table-section write, and
  `enable_el1` issues 128 `DC CIVAC` and 512 `LDP` in the linked image.
- `checks/a733-cpu-coherence.py`: `enable_el1` flushes both EL1 pages before
  any system-register write, and a wrong checksum or a malformed root entry 1
  leaves `SCTLR_EL1.M` clear.

### Changes

- `continue.c`: drop the third `a733_cpu_coherence_publish()` call. The code
  and initial-RAM publications and the stop code `0x102` stay.
- `publication.S`: drop the `.Lel1_root` and `.Lel1_l2` loops and their
  address set-up. The image-RAM loop, the instruction-cache invalidation and
  the control restoration stay.

`continue.S` does not change, so the `continue.S` hash pins in
`cmake/a733-smp-observe.cmake` and `cmake/a733-storage-maintenance.cmake` stay
as they are. No pin covers `continue.c` or `publication.S`.

**Interrupts, memory placement and synchronization.** Nothing changes. Both
steps ran on core 0 with exceptions masked, before any secondary release; the
tables keep their placement and attributes; the record barrier in `continue.c`
still orders table completion before every secondary's entry.

## Part 2: fault-report host tool

### Data flow and ownership

`a733_fault_boot()` builds `a733_fault_previous` on core 0 at EL3 before any
other core runs, and nothing writes it afterwards. On operation 5,
`apps/mimus-board/usb0.c` copies it once into the USB0 frame and sends exactly
1,160 bytes with no frame header. A request whose flag, count, offset or
generation field is nonzero stops the board through `stop()`.

The new `tools/fault-report.py` holds:

- `fetch(device, same, deadline, retain)`. It writes the 16-byte request
  `struct.pack('<BBHIQ', 5, 0, 0, 0, 0)` to endpoint `0x02` once. It then reads
  endpoint `0x81` until it holds 1,160 bytes, calls `retain(raw)` to save the
  raw bytes durably, and returns them. It calls `same()` before every transfer,
  as `device_exchange()` in `apps/mimus-board/host.py` does, so a changed USB
  identity stops the exchange. An empty fragment, more than 1,160 bytes, a
  transport error or an expired deadline raises an error. The function never
  retries, because a second request after a partial response would mix two
  frames.
- `decode(data)`, which calls `decode_report()` of `checks/fault-decode.py`.
  The decoder stays the only definition of the report layout.
- A command line that decodes a saved report: `fault-report.py REPORT.bin`
  prints one line per record in the `a733_fault_line()` format, and `--json`
  prints the full decode.

The tool opens no USB device and imports no USB library. `fetch()` receives a
device that a supervised launcher has already claimed. The launcher call is the
edit to `tools/check-a733-os-supervised.py` listed below, which only the
coordinator runs.

**Interrupts and polling.** The device side keeps the shared USB0 loop, which
the OS image services once per 1 ms tick. On the host each read blocks in the
USB stack with a timeout of at most 500 ms, bounded by the deadline; there is
no sleep loop.

**Memory, cache policy and cores.** Nothing changes on the device. The report
is immutable after core 0 builds it, so the USB0 task reads it without a lock
or cache maintenance.

### Test

`checks/fault-report.py` drives `fetch()` against a modeled device that applies
the operation 5 rules of `usb0.c` and returns the response in fragments of 1,
29 and 512 bytes. It needs no image and no USB access. Its cases are:

- a report with valid, incomplete and empty slots, decoded field by field, with
  `retain()` called before the decode;
- the request bytes, compared with `FAULT_OP` and the zero-field rule parsed
  from `apps/mimus-board/usb0.c`, and the response size, compared with
  `REPORT_BYTES` in the decoder;
- refusals: a device that stops without answering (one write, no retry), an
  extra byte after the report, an empty fragment, an identity change, and a
  deadline that expires before the write and during the reads;
- a report with a wrong magic or a valid-slot mask that disagrees with a slot,
  which the decoder refuses after the raw bytes are retained;
- every report that `checks/fault-capture.py` saves, when its output directory
  is given; those reports come from the linked `a733_fault_boot()`;
- `sys.modules` after the import holds no `usb` module.

## Performance budgets

| Metric | Baseline (`0fd1b51`) | Budget | Measured by |
| --- | --- | --- | --- |
| Table-section `DC CVAC` in `a733_cached_prepare()` | 321 lines (diagnostic), 449 (supervised); one publish call | 0 lines; no call names `__tables_start` | `checks/cpu-table-walks.py`, from the disassembly of `a733_cached_prepare()` |
| Table-line maintenance per boot, eight-core diagnostic image | 1,666 (321 + 321 + 8 × 128) | 1,345 | `checks/cpu-table-walks.py` counts |
| Table-line maintenance per boot, supervised image | 2,178 (449 + 449 + 128 + 128 + 8 × 128) | 1,601 | the same counts and the publication check |
| Lines that `a733_supervised_ns_publish()` maintains | 42,172 | 42,044 | `checks/a733-supervised-publication.py`, `maintained_lines` |
| EL1 table `DC CIVAC` per core before `SCTLR_EL1.M` | 128 | 128 | `checks/cpu-table-walks.py`; a new per-core case in the publication check |
| Pool translations; EL1 map | 392 `AT`; golden map | unchanged | `checks/device-memory-capacity.py` |
| Operation 5 response on the device | 1,160 bytes in 1 tick | unchanged | `checks/usb0-service-burst.py` |
| Host transfers per report | none | 1 request, at most 3 reads with 512-byte fragments, 0 retries | `checks/fault-report.py` |
| Copies per report byte on the host | none | one: the fragments are appended to one buffer | `checks/fault-report.py` |

At 494 MHz and at most 70 cycles per maintenance operation, the removed 321
operations save about 0.05 ms per boot in the diagnostic image and the removed
577 save about 0.08 ms in the supervised image. Both are estimates. QEMU and
the Unicorn models do not model caches, so they show only that each image still
reaches cached EL1 on all eight cores with unchanged tables.

## Left out

- The `DC CVAC` of the two temporary EL3 pages in `publication.S` (128 lines)
  is redundant by the same argument as step 2. It stays because the
  Non-secure dirty-line experiment in [cold-entry.md](../cold-entry.md) uses
  that temporary mapping, and the harness should not change before the
  experiment runs.
- A `fetch` command that opens the board itself. Board access belongs to the
  coordinator's supervised launchers.
- Changes to `usb0.c` or the report layout. The device side already meets its
  budget.

## Board evidence required

Run only in scheduled board sessions:

- **Supervised and diagnostic boot.** Each rebuilt image reaches the scheduler
  on all eight cores with no stop `0x102`, `0x108` or EL1 table-check refusal.
  A refusal at step 4 would show a stale Non-secure line that step 3 used to
  write back earlier; it stops that core before translation, which is a
  bounded failure. The dirty-line experiment of
  [cpu-table-walks.md](cpu-table-walks.md) still decides whether such lines
  occur.
- **Report after reset.** After a WDT0 reset, `fetch()` returns a report whose
  slots match the deliberate fault, which is item 3 of the board evidence in
  [fault-capture.md](../fault-capture.md).

## Sources and unproven steps

- `DC IVAC` cleaning a dirty line on the Cortex-A55: TRM 100442_0200_00,
  section A6.2.4, pinned as recorded in [cpu-table-walks.md](cpu-table-walks.md).
- **Unproven:** that EL3 and EL1 with the MMU off address the Secure and the
  Non-secure space, that Device accesses allocate no cache line, and that
  maintenance issued with the MMU off reaches every core. These come from the
  Arm Architecture Reference Manual, which is not pinned. The removal adds no
  new dependence on them: steps 1 and 4 already rely on the same rules, and
  steps 2 and 3 ran under the same conditions.
- This change adds no hardware register sequence.

## Changes outside the owned files

These go to the primary agent in `primary.patch`:

- `checks/a733-supervised-publication.py`: remove the EL1 pages from the
  expected maintenance set, assert that publication issues no maintenance on
  them, and, in `--all-core-transition-only`, assert that each core flushes
  both EL1 pages at EL1 with the MMU off before it sets `SCTLR_EL1.M`.
- `checks/cpu-table-walks.py`: measure the table publication from the
  disassembly of `a733_cached_prepare()` and require 0 lines.
- `checks/device-memory-capacity.c`, `checks/device-memory-capacity-local.c`
  and `checks/cached-device-reservations.c`: drop the table publication from
  each fixture's copy of the core-0 sequence, so the capacity checks run the
  sequence the images run.
- `checks/fault-capture.py`: write the two reports that its rotation cases
  read back from `a733_fault_previous` to its output directory, for the
  saved-report case of `checks/fault-report.py`.
- `tools/check-a733-os-supervised.py`: an optional `--fault-report` flag that
  calls `fetch()` once before the capture and saves the raw and decoded report
  in the evidence directory.
- `checks/index.json`, regenerated by `checks/check-index.py` for the new check.
- `docs/cpu-coherence.md` (maintenance table and totals),
  `docs/a733-smp-cached.md` (boot step 1) and `docs/a733-smp-supervised.md`
  (the publication paragraph): state the sequence after the change.
