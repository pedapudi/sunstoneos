# DRAM controller and memory-bus priority

The component gives display scan-out a fixed priority on the memory bus and
measures the display port's read latency under CPU load. The thermal policy
also throttles when the DDR temperature channel reaches 85 °C
([thermal policy](thermal.md)). The
[DRAM controller design](designs/dram-controller.md) holds the register
facts, the port table, the window rules, the evidence block layout, the
integration edits and the board evidence that would qualify the component.
The [memory-bus priority design](designs/dram-bus-priority.md) holds the
budgets for the code in `drivers/dram/` and the decode check.

Status:

- **Priority hook, latency window and copy pass: implemented and
  host-modeled; no image links them.** The framebuffer, gpu-display and build
  edits that call them belong to other owners and have not merged. The
  integration host cases in the DRAM controller design are a merge condition
  for that change, and the hook is not recorded as tested before they pass.
- **Decode check: implemented; it refuses; no image links it.** No pinned
  source documents the DRAM controller or firewall registers, so the check
  makes no access and returns 0. Its caller, the cached table builder in
  `platform/a733-smp-cached/memory.c`, links a weak definition with the same
  result in every image.
- **DDR passive trip: implemented and host-modeled** in the thermal policy
  that the OS and thermal images link. Its board cost is unmeasured, and
  connecting THROTTLE to the CPU clock driver waits for the DDR trip
  measurement in the design.
- **Physical behavior: unqualified.** No board run has read the NSI ports,
  written the table or collected a latency window.

DRAM training, frequency scaling, voltage and the boot0 DRAM firewall stay
inside boot0's opaque routine, which the OS depends on. The memory
configuration (type, rank, size and refresh settings) is not decoded.

## Ports 2, 14 and 15 serve display scan-out

The NSI is the A733 memory-bus interconnect. Each master's port has its own
mode, priority and select words and its own counters, at
`0x02020000 + 0x200 × port`. DE uses port 2, and the IOMMU0 and IOMMU1
page-table walkers, which serve DE's translated reads, use ports 14 and 15.
Their counters run on the MBUS clock. The
[DRAM controller design](designs/dram-controller.md#de-uses-nsi-port-2-and-its-iommu-walkers-use-ports-14-and-15)
derives these numbers and the clock from the pinned vendor and U-Boot
sources.

## The priority hook writes the table once, with read-back

`a733_dram_priority()` in [drivers/dram/a733_dram.c](../drivers/dram/a733_dram.c)
makes every register access through the framebuffer's read and write pair,
which checks ownership, address, Device-nGnRnE mapping and the deadline on
each access. The caller holds the register lock for the entire call. The hook
does not check its arguments, because its one caller passes its own pair and
evidence.

1. Two CCU reads: `NSI_CLK_REG` (`+0x580`) must have gate bit 31 and reset
   bit 30 set, and `NSI_BGR_REG` (`+0x584`) must have reset bit 16 and gate
   bit 0 set. Otherwise the hook records both words, makes no NSI access and
   returns failure with stopping field 0. A failed CCU read returns failure
   with stopping field 10.
2. For each of the nine fields of ports 2, 14 and 15, one read. A field that
   differs from the table is written with every other bit kept and read back
   under the field mask.
3. The first failed access or read-back mismatch stops the hook and records
   the stopping field. On failure the caller takes the framebuffer's failure
   path, which retains the register lock, the clock tokens and the
   framebuffer lease. Nothing accesses the NSI after that.

`SUNSTONE_DRAM_PRIORITY` selects the build: 2 applies the table and 1 makes
the same reads and writes nothing. The source fails to compile without
either value. `a733_dram_address()` admits the eleven words the hook may
access. The hook writes the evidence block directly: it zeroes the block,
fills the hook part and sets the result bits from the build macro and its
own result. The stopping field goes in the block's second `u16`.

## The latency window measures port 2 over 50 ms

[drivers/dram/a733_dram_load.c](../drivers/dram/a733_dram_load.c) compiles
only with `SUNSTONE_GPU_DISPLAY_DRAM_LOAD`. The gpu-display output task calls
`a733_dram_window_step()` once per pass of its 1 ms loop. Each window
programs port 2's counters for 50 ms and then reads six counts. Phase A has
scan-out only. Phase B adds the copy load on all eight cores.

- **MBUS frequency scaling.** The first start of a session reads CCU
  `+0x588` before its six accesses. With `MBUS_DFS_EN` (bit 28) set, the
  MBUS rate can change, so a period written in MBUS cycles would not span
  50 ms. That start then makes no port access, ends the phases early and
  sets the result bit `A733_DRAM_MBUS_SCALING`.
- **Barriers.** The window adds none. The framebuffer's register path ends
  every access with `DSB SY`, so the last NSI access of a phase has completed
  before the counter read (ISB, then `MRS CNTPCT_EL0`) that ends the phase.
- **Phases.** Phase A starts with the first eligible step, which also starts
  a window, and lasts 2 s. Phase B runs until the configured end, `stop_at`
  minus 1 s. Phases change only between windows. The step that begins phase
  B, which returns `A733_DRAM_BEGIN_B`, and the step that ends the phases,
  which returns `A733_DRAM_END`, start no window and make no register access.
- **Monotonic counter.** Every counter sample passes
  `a733_monotonic_advance()`. A sample earlier than the one before it
  refuses the window in flight as late, and a step that is not in a window
  starts nothing.
- **Free-running fallback.** The first window whose two read data values
  differ is refused as free-running, and every later collect first writes
  the saved enable word with bit 0 clear, making seven accesses.
- **Phase CPU time.** `phase_ticks_max` at offset 38 of the evidence block
  holds the longest start or collect in 24 MHz counter ticks, saturating at
  65,535. A start spans its two counter reads. A collect spans the step's
  first counter read to the read after its last access. Neither adds a
  counter read.
- **Abandon.** A step that is no longer eligible, and
  `a733_dram_window_abandon()` at `stop_at` or on the output fault path,
  count a window in flight as abandoned and end the phases with no register
  access. Ending the phases this way sets the result bit for phases ended
  early. During phase B it also reads the counter once to record the end of
  phase B, so the phase B length covers the copy passes. A run with that bit
  set is inconclusive.
- **Failures.** A lock reported busy changes nothing and the next step
  retries. A failed access or a failed `enter` returns a negative value,
  with the lock still held after a failed access, for the caller's retain
  path. The window then stores the ended phase with release, which stops
  the copy tasks. Every later step and abandon returns
  at once, without calling `enter`, the register pair or the counter. A
  synchronous external abort or SError from an NSI access never returns to
  the window; the platform's exception capture ends the image.

`a733_dram_window_block()` adds the window and copy-slot fields to the block
in the load images only, so other images link no packing code for them.

`a733_dram_copy_pass()` is one pass of a copy task. It copies 2 MiB with
`a733_dram_copy_2mib`, two LDP and two STP of Q registers per 64 bytes
([drivers/dram/a733_dram_copy.S](../drivers/dram/a733_dram_copy.S)), because
the C sources build with `-mgeneral-regs-only`. Each pass loads the phase
word and the application state with acquire. It returns 0 and copies
nothing once the phase has ended or the state is not 1; the task then
suspends. Before phase B it returns -1 and the task waits for its
notification.

## The decode check refuses until a register source exists

`int a733_dram_decode_check(const struct a733_dram_range *ranges, unsigned
count)` in
[drivers/dram/a733_dram_decode.c](../drivers/dram/a733_dram_decode.c) is the
EL3 function that the cached table builder in
`platform/a733-smp-cached/memory.c` calls once on core 0, with the MMU off,
before it maps DRAM above the boot image at EL1
([12 GiB EL1 map](designs/memory-map-12g.md)). The builder passes
`0x4800_0000`–`0x7900_0000` (local layout only) and
`0x8000_0000`–`0x3_4000_0000`. A mapped write-back block receives speculative
reads, so the check must refuse any range it cannot prove safe. It may return
1 only when, for every byte of every listed range, this boot's controller
configuration decodes the address linearly from `0x4000_0000` with no alias,
the decoded size reaches `0x3_4000_0000`, and the firewall grants Non-secure
read and write with no protected region overlapping. Any other result returns 0, and the builder then maps none of
the ranges. Its budget is controller and firewall register reads only, no
DRAM access, at most 64 register reads and 2,000 instructions.

No pinned source documents the DRAMC Common, controller, SMC0 or SMC1
registers that would show the decode. The function therefore makes no access
and returns 0. It may return 1 only after a trace of boot0's DRAM
initialization names those registers and an EL3 board read on both boards
matches the trace. No image links this file yet; `memory.c` links a weak
definition that returns 0, and the declaration in `include/a733_dram.h` is
the one both definitions use.

## Host checks and measurements

`checks/dram-controller.py OUTPUT` needs only the pinned sources and host
toolchains. It runs two host programs under AddressSanitizer and
UndefinedBehaviorSanitizer against counting register models:

- `checks/dram-priority.c`, built with the table and reads-only values,
  covers the decode refusal for both ranges the builder passes, the NSI clock gating cases, the access counts,
  kept bits, a difference outside the mask, the subfield rewrite, a stuck
  field at each of the nine positions, a failed access at every position,
  with its own stopping field for a failed CCU read, and the evidence block,
  including the stopping field of a failed hook;
- `checks/dram-window.c` covers the start and collect sequences, the pending
  cost, each refusal rule, the fallback after one free-running window, busy
  and failed locks and accesses, the failure latch after a failure at every
  access of a start and a collect and after a failed `enter`, the
  `MBUS_DFS_EN` refusal at each of the 32 bits of `+0x588`, backward
  counter samples, the phase spans and their saturation, abandon in phase A
  and in phase B, the copy-load stop rule, an 18 s session and the block
  layout.

Each program also prints the register facts its model observed: the CCU
addresses and the bits whose clearing refuses the hook, the address of every
port word in access order, each field's mask and written value, and the
window's access sequences. The script compares those facts with the pinned
`sunxi-nsi.c`, `sunxi-nsi.h`, `sunxi-nsi-bw.c`, the `nsi0` node, the U-Boot
NSI driver and the A733 manual text, together with the port numbering facts
of the DRAM controller design. It then compiles the component for AArch64 with
the image flags, reads its size, barriers and copy loop from the objects, and
runs the linked objects under Unicorn to count instructions outside the
register callbacks.

| Metric | Budget | Result |
| --- | --- | --- |
| Hook accesses | 2 CCU reads; at most 27 NSI accesses; 9 at the table; 0 with the NSI not running | 21 NSI accesses from reset values, 9 at the table, 27 worst case; 9 reads-only; 0 with the NSI not running |
| Window accesses | Start 6 (and one `+0x588` read in the first start), collect 6 (7 after the fallback), pending 0; at most 4,100 per session | As budgeted; 4,009 register accesses over an 18 s session of 40 phase A and 294 phase B windows |
| Accesses, `enter` calls and counter reads after a failure | 0 | 0 |
| Instructions outside the callbacks | Hook 600, start 200, collect 300, pending step 50 | Hook 552 worst case (307 reads-only), start 161, collect 158, pending step 39 |
| `DSB` and `DMB` in the component's objects | 0 | 0 |
| Copy loop | 6 instructions per 64 bytes; 2 acquire loads per pass | 6; 2 |
| Hook code | At most 2 KiB text and 128 B data | 704 B text (588 B reads-only), 0 B data |
| Decode check | At most 16 B | 8 B |
| Load code | Load images only | 3,000 B text in C, 32 B of assembly |

No image links the component, so the gpu-display, HDMI, DisplayPort and
integrated OS images contain no `a733_dram_` symbol and match the integrated
tree's images in size and text. The
[measurement record](designs/dram-controller-measurements.md) gives the
conditions, the per-case figures and the board-only rows. The scan-out
comparison, with its rules on image order and inherited fields, and the DDR
trip measurement are listed in the design's board evidence.
