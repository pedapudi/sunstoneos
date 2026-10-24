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
- **Decode check: implemented; it refuses.** No pinned source documents the
  DRAM controller or firewall registers, so the check makes no access and
  returns 0. Its only caller, the cached memory layout 2 branch of
  `platform/a733-smp-cached/memory.c`, is compiled out.
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
`0x02020000 + 0x200 × port`.

- The `CONFIG_ARCH_SUN60IW2` branch of `enum nsi_pmu` in the pinned
  `sunxi-nsi.h` gives DE port 2, IOMMU0 port 14 and IOMMU1 port 15. The
  `nsi0` node of the pinned `sun60iw2p1.dtsi` uses the same ids. The IOMMU
  ports carry the page-table walks for DE's translated reads.
- The node sets `sub_node_id_mapping`, so `sunxi-nsi.c` uses each id as the
  register index. Its other mapping subtracts one from the id. That mapping
  needs `NSI_HARDCODED_PORT_MAPPING`, which `sunxi-nsi.h` defines only for
  sun55iw3.
- The pinned U-Boot `drivers/nsi/sunxi_nsi.c` addresses the same words, with
  `SUNXI_NSI_BASE` equal to `0x02020000` in its sun60iw2p1 `cpu_autogen.h`.
- The node lists no clock for ports 2, 14 and 15 in `master_clks`, so their
  counters run on clock index 1, `CLK_MBUS`.

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
   returns failure.
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
access. `a733_dram_block()` fills the hook part of the evidence block. It
takes the result bits from the build macro and from the hook's return value,
and the stopping field goes in the block's second `u16`.

## The latency window measures port 2 over 50 ms

[drivers/dram/a733_dram_load.c](../drivers/dram/a733_dram_load.c) compiles
only with `SUNSTONE_GPU_DISPLAY_DRAM_LOAD`. The gpu-display output task calls
`a733_dram_window_step()` once per pass of its 1 ms loop. Each window
programs port 2's counters for 50 ms and then reads six counts. Phase A has
scan-out only. Phase B adds the copy load on all eight cores.

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
  path. The window then latches failed and stores the ended phase with
  release, which stops the copy tasks. Every later step and abandon returns
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

`a733_dram_decode_check()` in
[drivers/dram/a733_dram_decode.c](../drivers/dram/a733_dram_decode.c) is the
EL3 function that cached memory preparation calls before it maps
`0x8000_0000`–`0xFFFF_FFFF` as write-back
([CPU table walks](designs/cpu-table-walks.md#stage-2-level-1-blocks)). A
mapped write-back block receives speculative reads, so the check may return 1
only when this boot's controller configuration shows a linear decode to
`0x3_4000_0000` with no protected region in that range. No pinned source
documents the DRAMC Common, controller, SMC0 or SMC1 registers that would
show it. The function therefore makes no access and returns 0. It may return
1 only after a trace of boot0's DRAM initialization names those registers and
an EL3 board read on both boards matches the trace.

## Host checks and measurements

`checks/dram-controller.py OUTPUT` needs only the pinned sources and host
toolchains. It runs two host programs under AddressSanitizer and
UndefinedBehaviorSanitizer against counting register models:

- `checks/dram-priority.c`, built with the table and reads-only values,
  covers the decode refusal, the NSI clock gating cases, the access counts,
  kept bits, a difference outside the mask, the subfield rewrite, a stuck
  field at each of the nine positions, a failed access at every position and
  the evidence block, including the stopping field of a failed hook;
- `checks/dram-window.c` covers the start and collect sequences, the pending
  cost, each refusal rule, the fallback after one free-running window, busy
  and failed locks and accesses, the failure latch after a failure at every
  access of a start and a collect and after a failed `enter`, backward
  counter samples, the phase spans and their saturation, abandon in phase A
  and in phase B, the copy-load stop rule, an 18 s session and the block
  layout.

Each program also prints the register facts its model observed: the CCU
addresses and the bits whose clearing refuses the hook, the address of every
port word in access order, each field's mask and written value, and the
window's access sequences. The script compares those facts with the pinned
`sunxi-nsi.c`, `sunxi-nsi.h`, `sunxi-nsi-bw.c`, the `nsi0` node, the U-Boot
NSI driver and the A733 manual text, together with the port numbering facts
in the previous sections. It then compiles the component for AArch64 with
the image flags, reads its size, barriers and copy loop from the objects, and
runs the linked objects under Unicorn to count instructions outside the
register callbacks.

| Metric | Budget | Result |
| --- | --- | --- |
| Hook accesses | 2 CCU reads; at most 27 NSI accesses; 9 at the table; 0 with the NSI not running | 21 NSI accesses from reset values, 9 at the table, 27 worst case; 9 reads-only; 0 with the NSI not running |
| Window accesses | Start 6, collect 6 (7 after the fallback), pending 0; at most 4,100 per session | As budgeted; 4,008 register accesses over an 18 s session of 40 phase A and 294 phase B windows |
| Accesses, `enter` calls and counter reads after a failure | 0 | 0 |
| Instructions outside the callbacks | Hook 600, start 200, collect 300, pending step 50 | Hook 513 worst case (276 reads-only), start 146, collect 158, pending step 39 |
| `DSB` and `DMB` in the component's objects | 0 | 0 |
| Copy loop | 6 instructions per 64 bytes; 2 acquire loads per pass | 6; 2 |
| Hook code | At most 2 KiB text and 128 B data | 728 B text (608 B reads-only), 0 B data |
| Decode check | At most 16 B | 8 B |
| Load code | Load images only | 2,816 B text in C, 32 B of assembly |

No image links the component, so the gpu-display, HDMI, DisplayPort and
integrated OS images contain no `a733_dram_` symbol and match the integrated
tree's images in size and text. The
[measurement record](designs/dram-controller-measurements.md) gives the
conditions, the per-case figures and the board-only rows. The scan-out
comparison, with its rules on image order and inherited fields, and the DDR
trip measurement are listed in the design's board evidence.
