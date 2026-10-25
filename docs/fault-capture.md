# Fault capture

Status: **implemented** in the integrated OS image ([os-image.md](os-image.md))
and in the single-core images' exception line. Host and instruction checks
pass. No board has executed the capture, and retention of the records across
a watchdog reset is **unproven**.

The integrated OS image records why each core stopped. Every entry of any core
into the EL3 fault handler stops watchdog renewal, and an EL1 fault or a
deliberate exit also leaves a 128-byte record in a retained slot. The next
boot copies the valid records and the previous resident result into a report
that USB0 returns. The reboot itself is described in [reset.md](reset.md); the
design and its budgets are in
[designs/cpu-fault-reset.md](designs/cpu-fault-reset.md).

## Entry, pending byte and capture

`SUNSTONE_A733_FAULT_PAGE`, which only `cmake/a733-os.cmake` defines, enables
the retained records below. Other cached images use the same synchronous
exception entry without retained slots.

1. **Marker.** Each EL1 vector entry that reports a fault, in the cold table and
   in the runtime table, sets `x2 = 0x100 | slot` before branching to
   `a733_cached_cold_fault`, whose SMC enters EL3. Synchronous exceptions that
   reach the runtime handler first take the slot from `SPSR_EL1.M`: 8 from
   EL0t, 0 from EL1t (the storage tasks run on `SP_EL0`) and 4 from EL1h.
   `a733_cached_smc`, which every deliberate exit uses, and
   `a733_cached_el1_fault` set `x2 = 0`. Bit 8 therefore marks a vector fault,
   whatever status a deliberate exit passes.
2. **Pending byte.** `a733_cached_el3_fault` decodes MPIDR, then stores 1 at
   byte `core` of the EL1 `.bss` word `a733_fault_pending`, followed by
   `DSB SY`. The store comes six instructions after the decode, before the
   state and core checks, so it also happens for an exception taken at EL3, an
   entry that fails the checks, and a second entry of the same core. An MPIDR
   that names no core stores byte 0.
3. **Capture.** For an SMC from EL1 that passes the checks, the stackless leaf
   `a733_fault_capture` ([platform/a733/fault.S](../platform/a733/fault.S))
   fills the core's slot, then the handler publishes its existing EL3 record
   and parks as before. The leaf writes an open marker, the fields, a CRC32C,
   `DMB OSHST` and the final magic, then `DSB SY`.

EL3 reads `pxCurrentTCBs[core]` and the task name through its Nonsecure
write-back alias of image RAM. It accepts a TCB pointer only when it is
nonzero and 8-byte aligned, and reads the 16 name bytes only when they lie
inside `0x41200000`–`0x42000000`. The FreeRTOS task additions export the name
offset as `a733_fault_task_name_offset` and assert that it is 8-byte aligned.

## Synchronous faults do not require a working EL1 stack

The runtime synchronous vector classifies the exception before accessing the
interrupted stack. A translation or alignment fault with an invalid stack
pointer therefore reaches the stackless EL3 report path. Saving registers on
that stack before classification caused another exception at the first store,
preventing capture.

While exception entry masks interrupts, the vector temporarily saves `x16` in
`TPIDR_EL1`. This register normally contains only the validated core index,
which can be recovered from MPIDR affinity level 1. A supervisor call (SVC)
restores `x16`, saves the normal register pair on the stack, and restores the
core index before dispatch. A fatal exception never returns; its EL3 handler
derives the core independently from MPIDR. Both paths retain the existing
branch-history clearing sequence. No emergency stack or additional memory
reservation is introduced.

The [Arm Cortex-A55 r2p0 manual](https://documentation-service.arm.com/static/5e7e09f6a3736a0d2e862d2f),
revision `100442_0200_00_en`, section B2, identifies `TPIDR_EL1` as a software
thread-ID register. The pinned
[Linux v6.17 entry code](https://github.com/torvalds/linux/blob/e5f0a698b34ed76002dc5cff3804a61c80233a7a/arch/arm64/kernel/entry.S)
checks for stack overflow before saving general registers and uses thread-ID
registers as temporary storage on its fatal path. SunstoneOS needs only terminal
capture, so it reuses its existing stackless report path.

The budget for this change is zero EL1 data accesses on a synchronous fault
and at most eight additional instructions per SVC. The linked OS image takes
94 instructions from the vector to SVC dispatch, versus 89 before the change.
Its four loads and four stores are unchanged; it adds no lock, barrier,
translation, copy or wait. The interrupt path and idle hook are unchanged.

The extended [fault model](../checks/fault-capture.py) checks unmapped and
misaligned stack pointers on all eight cores from both EL1h and EL0 vectors.
All 32 cases capture the original syndrome, return address and fault address
without an EL1 data access. Another 32 cases preserve every general register,
the stack pointer and core index at normal SVC and exit dispatch. The observer
image passes 48 cases including its private SVC 120. The existing 72 park and
capture cases and the eight-core scheduler model also pass. The original image
fails the unmapped-stack case at its first store. Evidence, image identities,
instruction counts and source pins are under
`~/.cache/sunstoneos/build/cpu-runtime-robustness-1024/`.

These checks model exception entry and system registers. They do not establish
physical sustained scheduling, A55/A76 cache coherence, all-core idle wake-up,
fault retention across reset, or cleanup after active peripheral interrupts.
Runtime stop continues to retain CPUs, device state and memory until the
externally owned recovery path completes; it does not prove quiescence or
authorize reuse. A stalled memory access can still prevent capture.

Complete CPU power and thermal support remains outstanding. Idle uses `WFI`
with caches powered; runtime core removal, restart, retention and cluster
power-down are unimplemented. The [clock driver](cpu-clock.md) supports the
pinned 26 MHz operating-point tables, up to 1794 MHz for the A55 cluster and
2002 MHz for the A76 cluster where the speed bin permits them. Voltage follows
the selected bin and retains the inherited rail floor; the shared A55/DSU rail
also constrains changes. Thermal throttling targets at most 1014 MHz, whose
table voltage is 800 mV in every bin. Physical transitions still require a
session grant and approval for the specific change. Sustained eight-core load,
voltage/frequency transitions, thermal response and recovery require physical
qualification; this fault correction qualifies none of them.

## Slot layout

Eight 128-byte slots occupy `0x42001000`–`0x42001400`, inside the 16 KiB
retained `.result` reservation. EL3 maps that page as Secure Device memory and
EL1 has no mapping of it, so every slot access is an aligned 64-bit access and
needs no cache maintenance. `include/a733_fault.h` defines the layout.

| Word | Field |
| --- | --- |
| 0 | `A733FLT1` once complete; `A733FLT0` while the capture runs |
| 1 | CRC32C of words 2..15 (initial value and final XOR `0xffffffff`) |
| 2 | Kind in bits 0..7 (1 vector fault, 2 deliberate exit), vector slot in bits 8..15, core in bits 16..23 |
| 3, 4 | Status and detail: `x0` and `x1` of the SMC (status 4 and `ESR_EL1` for a vector fault) |
| 5, 6 | `CNTPCT_EL0` at capture and the contract epoch |
| 7..10 | `ESR_EL1`, `ELR_EL1`, `FAR_EL1`, `SPSR_EL1` (vector fault only) |
| 11 | `pxCurrentTCBs[core]` (vector fault only) |
| 12, 13 | First 16 bytes of the task name (vector fault only) |
| 14, 15 | Zero |

A core parks after its first record, so each slot holds at most one record per
boot. Records are ordered by the counter in word 5.

## Previous-boot report

`a733_cached_prepare` calls `a733_fault_boot` first, on core 0 at EL3 with
translation off, before image RAM is published to the other cores. The
function makes only aligned 64-bit volatile accesses. It builds
`a733_fault_previous` (`struct a733_fault_report`, 1,160 bytes):

- a header word: `FLT1` in bits 0..31, the valid-slot mask in bits 32..39, the
  incomplete-slot mask (open marker without a complete record) in bits 40..47,
  and bit 48 when the previous resident result was copied;
- the previous resident result, which the stage-2 loader moved into words
  24..39 of its report at `0x47008200` before clearing it; it is copied only
  when that report holds the loader's magic and status 6 (entered);
- the eight slots, each copied when its magic and CRC match and zero otherwise.

It then zeroes the eight slots.

USB0 operation 5 of the OS image's 16-byte request (count, offset and
generation zero) returns the report alone, without the 64-byte frame header.
[checks/fault-decode.py](../checks/fault-decode.py) decodes a saved report:

```sh
python3 checks/fault-decode.py report.bin
```

On the host, `fetch()` in [tools/fault-report.py](../tools/fault-report.py)
sends that request once on a device that a supervised launcher has already
claimed. It reads until it holds 1,160 bytes, each read asking for the
remaining length rounded up to 512 bytes, saves the raw report through the
caller's `retain()` and returns it. An empty fragment, more than 1,160 bytes,
a changed USB identity, an expired deadline or a transport error stops the
exchange, which is never retried. The module opens no device and has no
command line. The supervised OS launcher
[check-a733-os-supervised.py](../tools/check-a733-os-supervised.py) calls it
with `--fault-report`, once before the result capture, and saves
`fault-report.bin` and its `decode_report()` result `fault-report.json` in the
evidence directory. The launcher refuses the flag before any device access
when the frozen image does not define `a733_fault_previous`, and a `fetch()`
error ends the run before the capture.

## Single-core images

The single-core images keep their FEL transport and do not use the slots.
`platform_exception` in [platform/a733/platform.c](../platform/a733/platform.c)
builds the same record on its stack, with the vector slot recorded as `0xff`
(not known), and writes one log line through `a733_fault_line`:

```
FAIL exception core=0 ESR=0x... ELR=0x... FAR=0x... SPSR=0x... task=NAME
```

The line holds at most 137 characters. It adds `SPSR_EL1`, the core number
and the task name to the three registers printed before. The name is read only
when it lies inside `0x41000000`–`0x42000000`. The includes sit directly above
the function so that every `platform.patch` built on this file still applies
without fuzz.

## Checks

- `checks/fault-capture.py --image OS.elf [--baseline OLD.elf] --output DIR`
  executes the linked image under Unicorn with modeled system registers. It
  runs nine entries on each of the eight cores. Five are faults: through the
  EL1h, EL0 and EL1t synchronous entries, a direct runtime fault vector and a
  cold vector. The others are a deliberate exit with a stale `x2 = 0x1ff`, an
  EL3-origin exception, a failed state check and an MPIDR that names no core.
  Each entry
  stores its pending byte, captures write only their own slot and record, and
  each slot decodes as expected. It also runs `a733_fault_boot` over valid,
  incomplete, corrupt and empty slots, checks the supervisor's added load by
  disassembly, and compares the C line formatter with the decoder.
- `checks/reset-latency.py` is described in [reset.md](reset.md).
- `checks/fault-report.py --output DIR` drives `fetch()` against a modeled
  device that applies the operation 5 rules parsed from
  `apps/mimus-board/usb0.c` and answers in fragments of 1, 29 and 512 bytes.
  It checks the byte-for-byte result and the request bytes, the refusals of
  a stopped device, an extra byte, an empty fragment, an identity change and
  an expired deadline, and that the module imports no device library and has
  no command line.

The resident and supervised radio images pass. Measurements against the
merge base are in the design document.

## Board evidence still required

1. The slots survive a WDT0 reset and boot0's DRAM initialization with a
   valid CRC; the loader's copy of the previous result relies on the same
   retention.
2. A load from an unmapped address in a test task, once on an A55 and once on
   an A76, produces the expected `ESR_EL1`, `ELR_EL1` and `FAR_EL1`.
3. Operation 5 returns the report after the reboot.

That an SMC leaves `x2` and the four EL1 registers unchanged, and that byte
stores are single-copy atomic, come from the Arm Architecture Reference Manual
(DDI 0487), which is not pinned.
