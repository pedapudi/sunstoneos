# Design: no repeated table publication, and a host reader for the fault report

Status: **implemented and host-tested**; not run on the board. Readers are
assumed to know AArch64 data-cache maintenance by virtual address and the
Secure and Non-secure physical address spaces, and to have read
[cpu-table-walks.md](cpu-table-walks.md).

Two independent parts:

- **Table maintenance.** The cached eight-core images clean and invalidate
  their translation tables in three places at boot; the Non-secure
  invalidation of the EL1 pages in `publication.S` stays until the conditions
  in "Step 3" hold.
- **Fault report.** A host reader requests the previous boot's 1,160-byte
  fault report ([fault-capture.md](../fault-capture.md)) over USB0 operation 5.

## Part 1: table maintenance

Core 0 runs `a733_cached_prepare()` (`platform/a733-smp-cached/continue.c`) at
EL3 with the MMU, data cache and instruction cache off, before any other core
is released. The table section `__tables_start`–`__tables_end` holds the five
EL3 and EL1 pages and the checksum line: 321 lines of 64 bytes in the
diagnostic image and 449 in the supervised image, whose section also holds the
two temporary EL3 pages of `platform/a733-smp-supervised/publication.S`.

- **Step 1.** `a733_cached_memory_prepare()` (`memory.c`) runs `DC CIVAC`
   over the section, `DSB SY`, then writes every descriptor and the checksum.
- **Step 3, supervised image only.** `a733_supervised_ns_publish()` cleans the
   two temporary EL3 pages and enables EL3 translation with `SCTLR_EL3.C`
   clear through a temporary map that makes the Non-secure alias of DRAM
   Normal non-cacheable. Through that map it runs `DC IVAC` over image RAM and
   over the EL1 root and level-2 pages, 64 lines each.
- **Step 4.** Each core, at EL1 with the MMU off, runs `DC CIVAC` over the EL1
   root and level-2 pages inside `a733_cpu_coherence_enable_el1()`, reads both
   pages back with Device loads and compares their sum with the recorded
   checksum before it sets `SCTLR_EL1.M`.

The image issues no clean of the table section after step 1.
`continue.c` publishes only code at `0x41000000` (2 MiB) and initial RAM from
`0x41200000` to `__bss_end`, so `a733_cached_prepare()` contains two
`a733_cpu_coherence_publish()` calls and none names `__tables_start`.

**Why no clean is needed after step 1.** A clean acts only on lines present
when it runs. From step 1's `DC CIVAC` to the publications, core 0 is the only
running core, no DMA master is admitted, and `SCTLR_EL3` M, C and I are clear;
`a733_cached_memory_prepare()` refuses to run otherwise. Every data access is
therefore Device-nGnRnE and allocates no line, and the only other maintenance,
the `DC CVAC` publication of code and initial RAM, allocates nothing. The
tables thus depend on the rule that EL3 data accesses with the MMU off
allocate no cache line. Each eight-core boot gives evidence for it:
`prepare_core(0)` writes `a733_cached_records` after the publications with no
maintenance, and each secondary reads those records at EL3 with the MMU off. If
core 0's stores allocated lines, the secondaries would read stale records and
the boot would stop at the record barrier's deadline. The comments before
`a733_cpu_coherence_enable_el3()` in `platform/a733/cpu_coherence.S` and at the
top of `publication.S` state this rule.

**Step 3 stays.** It is the only Non-secure maintenance that core 0 runs on its
own caches before it releases the secondaries. Core 0's step 4 is not ordered
before any secondary's step 4 or before that secondary sets `SCTLR_EL1.M`, and
no pinned source shows that maintenance issued with the MMU off is broadcast to
other cores. Without step 3, a Non-secure dirty line left in core 0's caches by
the boot loader could be written back after a secondary's checksum passed, and
that secondary would walk corrupted tables undetected. The `.Lel1_root` and
`.Lel1_l2` loops can go once a pinned source proves that MMU-off maintenance is
broadcast, or once the Non-secure dirty-line experiment of
[cold-entry.md](../cold-entry.md) passes on every launch path. The `DC CVAC`
of the two temporary EL3 pages in `publication.S` (128 lines) also stays,
because that experiment uses the temporary mapping.

Checks that cover the step 1 flush: `checks/cpu-table-walks.py` (the flush is
called before the table builder, every flushed line precedes the first
table-section write, `enable_el1` issues 128 `DC CIVAC` and 512 `LDP` in the
linked image, and `a733_cached_prepare()` publishes 0 table lines) and
`checks/a733-cpu-coherence.py` (`enable_el1` flushes both EL1 pages before any
system-register write, and a wrong checksum or a malformed root entry 1 leaves
`SCTLR_EL1.M` clear). The capacity fixtures
(`checks/device-memory-capacity*.c`, `checks/cached-device-reservations.c`)
run the same core-0 sequence as the images.

## Part 2: fault-report host reader

`a733_fault_boot()` builds `a733_fault_previous` on core 0 at EL3 before any
other core runs, and nothing writes it afterwards. The `DC CVAC` of
`0x41200000`–`__bss_end` in `continue.c` and, in the supervised image, the
image-RAM `DC IVAC` loop of `publication.S` make it visible to EL1. On
operation 5, `apps/os/usb0.c` copies the report once into the USB0 frame and
sends 1,160 bytes with no frame header. A request whose flag, count, offset or
generation field is nonzero stops the board. The report is immutable, so the
USB0 task reads it without a lock or cache maintenance.

`tools/fault-report.py` holds only `fetch(device, same, deadline, retain)`. It
writes the 16-byte request `struct.pack('<BBHIQ', 5, 0, 0, 0, 0)` to endpoint
`0x02` once, then reads endpoint `0x81`, each read requesting the remaining
length rounded up to a multiple of 512 bytes, until it holds 1,160 bytes. It
appends each fragment to one buffer, calls `retain(raw)` with a read-only view
of that buffer to save the raw bytes durably, and returns the same view. It
calls `same()` before every transfer, so a changed USB identity stops the
exchange, and each transfer's timeout is the time left before the deadline,
capped at 500 ms and at least 1 ms. An empty fragment, more than 1,160 bytes, a
transport error or an expired deadline raises an error. It never retries,
because a second request after a partial response would mix two frames. The
same function reads the 104-byte NPU report of operation 6 through its `op`
and `size` parameters.

The module opens no USB device and imports no USB library; it receives a device
that a supervised launcher has already claimed. Callers decode with
`decode_report()` of `checks/fault-decode.py`, the only definition of the
report layout. The supervised OS launcher outside this repository calls
`fetch()` through its optional `--fault-report` flag, once before the capture,
and refuses the flag unless the frozen image defines `a733_fault_previous`.

`checks/fault-report.py` drives `fetch()` against a modeled device that
applies the operation 5 rules of `usb0.c` and returns the response in
fragments of 1, 29 and 512 bytes. It covers the byte-for-byte result after one
`retain()` call, the request bytes and response size against the device and
decoder constants, every refusal (a device that stops without answering, an
extra byte in the completing transfer, an empty fragment, an identity change
before each transfer and before `retain()`, a deadline that expires before the
write, during the reads and after the last read), the shifted next frame that
an extra byte in a later transfer causes, and a static check that the module
imports no USB library and has no command line.

## Qualified figures

All figures come from host models; none describes physical cache or coherence
behaviour.

| Figure | Result | Evidence |
| --- | --- | --- |
| Table-section `DC CVAC` in `a733_cached_prepare()` | 0 lines (321 diagnostic, 449 supervised before) | `cpu-leftovers.jsonl`: `cpu-leftovers-table-publication` |
| Table-line maintenance per boot | 1,345 diagnostic, 1,729 supervised | same |
| Supervised publication lines; EL1 table `DC CIVAC` per core | 42,172; 128 | same |
| `a733_cached_prepare()` size | 260 bytes (288 supervised) | same |
| Host report exchange | 1 request, 3 reads at 512-byte fragments, 0 retries, one copy per byte | `cpu-leftovers.jsonl`: `cpu-leftovers-fault-report-reader` |

The estimated saving is about 0.05 ms per boot in the diagnostic image and
0.06 ms in the supervised image, at an assumed 70 cycles per maintenance
operation at 494 MHz. QEMU and the Unicorn models do not model caches.

## Limitations and open work

- **Unproven.** That EL3 and EL1 with the MMU off address the Secure and the
  Non-secure space, that Device accesses allocate no cache line, and that
  maintenance issued with the MMU off reaches every core. They come from the
  Arm Architecture Reference Manual, which is not pinned. The Cortex-A55
  `DC IVAC` cleaning of a dirty line is in TRM 100442_0200_00, section A6.2.4.
- **Board runs** (record `cpu-leftovers-board-open`): each rebuilt supervised
  and diagnostic image reaches the scheduler on all eight cores with no stop
  `0x102`, `0x108` or EL1 table-check refusal; after a WDT0 reset, `fetch()`
  returns a report whose slots match the deliberate fault.
- No host command opens the board itself; board access belongs to the
  coordinator's supervised launchers.
