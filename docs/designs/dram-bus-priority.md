# Design: memory-bus priority for scan-out and DRAM latency under copy load

Status: design only. No code for this design is on the branch yet. Section
numbers refer to [performance architecture](../performance-architecture.md).
The [DRAM controller design](dram-controller.md) remains the full
specification for the port table, the window rules, the evidence block layout
and the board session. This document covers how this package implements that
specification inside its own files. It also adds the
`a733_dram_decode_check()` function that
[CPU table walks](cpu-table-walks.md#stage-2-level-1-blocks) requires.

## What the package delivers

The package adds these files and no others:

- `include/a733_dram.h`: one header for every declaration below.
- `drivers/dram/a733_dram.c`: the priority hook, its address set and the hook
  part of the evidence block.
- `drivers/dram/a733_dram_load.c` and `a733_dram_copy.S`: the latency window,
  the copy pass and the window part of the evidence block. The C file compiles
  only with `SUNSTONE_GPU_DISPLAY_DRAM_LOAD`.
- `drivers/dram/a733_dram_decode.c`: the decode check.
- `checks/dram-priority.c`, `checks/dram-window.c` and
  `checks/dram-controller.py`: host cases, the source comparison, the
  instruction counts and code size; `docs/dram.md`.

An implementation of the hook, window and copy pass already exists in commit
`02154a1`, which was never merged. The package moves it onto the current
integrated tree and makes the three changes listed under "Reuse and removal".
No image links the new objects until the framebuffer, gpu-display and build
edits listed in the DRAM controller design merge. Those files belong to other
owners. The package's own checks need none of them, so the package delivers
no patch outside its files.

## Register sources and port numbering

- **Port numbers.** For `CONFIG_ARCH_SUN60IW2`, the pinned `sunxi-nsi.h` enum
  gives DE port 2, IOMMU0 port 14 and IOMMU1 port 15. The `nsi0` node of the
  pinned `sun60iw2p1.dtsi` uses the same ids and sets `sub_node_id_mapping`.
  In `sunxi-nsi.c` that mapping makes a port's register index equal to its id
  (`ia_index = i`). The other path subtracts one from the id and sends port 0
  to a CPU channel. It runs only when `NSI_HARDCODED_PORT_MAPPING` is defined,
  and `sunxi-nsi.h` defines that macro only for sun55iw3. The pinned U-Boot
  `drivers/nsi/sunxi_nsi.c` agrees: it addresses `SUNXI_NSI_BASE + 0x200 × id`,
  with `SUNXI_NSI_BASE` equal to `0x02020000` in
  `plat-sun60iw2p1/cpu_autogen.h`. Ports 2, 14 and 15 are therefore at
  `0x02020400`, `0x02021C00` and `0x02021E00`.
- **Field writes.** `nsi_port_setmode`, `nsi_port_setpri` and `nsi_port_setio`
  in `sunxi-nsi.c` read, modify and write bits [1:0] of `+0x10`, bits [3:0] of
  `+0x14` (value `(p << 2) | p`) and bit 0 of `+0x18`. The DT table sets mode
  0 and select 1, with priority 2 for DE and 3 for both IOMMU ports. U-Boot
  writes whole words, and its select case writes `+0x10` instead of `+0x18`.
  The hook follows the Linux read-modify-write sequence.
- **Counter clock.** With `topology_type = <2>`, `__rate_for_master` takes
  each port's clock from `master_clks`. Ports 2, 14 and 15 are absent from that
  list and default to clock index 1, `CLK_MBUS`. The window period is
  therefore written in MBUS cycles.
- **Counter sequence.** `__nsi_pmu_timer_store_v2` writes the period, clears
  enable bit 0 of `+0xc0`, sets clear bit 0 of `+0xc4` and then sets enable
  bit 0. `sunxi-nsi-bw.c` divides the latency sum (`+0xdc`) by read requests
  (`+0xcc`).
- **Power domain.** `sunxi_set_nsi_qos_params` takes every master's power
  domain before it writes port fields, and DE's domain is DE_SYS. The hook
  therefore runs after the framebuffer has powered DE_SYS.
- **CCU words.** Manual sections 4.1.6.74 and 4.1.6.75 place `NSI_CLK_REG` at
  CCU `+0x580` (gate bit 31, reset bit 30) and `NSI_BGR_REG` at `+0x584` (reset
  bit 16, gate bit 0). In each field, 1 means the gate is open or the reset
  is deasserted.

Unproven on the board: Non-secure access to the NSI ports and to `+0x584`,
whether the clear bit clears itself, whether the counters hold at the end of
the period, and the unit of the latency sum. The window's refusal rules cover
the last three cases. An idle refusal shows a stuck clear bit. The
free-running fallback handles counters that keep running. The statistic
compares the two load images at the same recorded MBUS rate, so the unit of
the latency sum cancels.

## Data flow and ownership

- **Hook.** The framebuffer calls `a733_dram_priority(io, evidence)` once, in
  prepare stage 4, under one register-lock acquisition. Every access goes
  through the framebuffer's `raw_read` and `raw_write` pair. The hook reads
  the two CCU words first. When the NSI is not running, it makes zero NSI
  accesses. Otherwise it reads each of the nine fields. In the table image, it
  writes and reads back only the fields that differ from the table.
- **Window.** The gpu-display output task calls `a733_dram_window_step()` once
  per pass of its existing 1 ms loop. A window programs port 2's counters for
  P = 50 ms and then collects six counts. Per-phase sums of the latency
  statistic go into the evidence block, which the output task copies once into
  the observation reply (136 bytes).
- **Copy load.** Eight tasks, one pinned to each core, call
  `a733_dram_copy_pass()` in a loop. Each call copies 2 MiB inside a 32 MiB
  lease that stays in `DEVICE_MEMORY_CPU` until power-off. No device is ever
  granted the lease, so its dirty lines never need cleaning.
- **Single writers.** The output task is the only writer of NSI ports 2, 14
  and 15 and of the window state. The phase word has one writer, the output
  task. Each copy task writes only its own 64-byte pass slot.

## Interrupts, memory, barriers and cores

- **Polling.** The NSI interrupt (SPI 43) has no qualified delivery, and a
  50 ms period spans many ticks. Section 1.3 permits a tick wait for that
  case. The window adds no task, timer or interrupt. Its pending check is one
  counter read and one compare per tick.
- **Copy tasks.** Each task blocks on a task notification until phase B
  begins. It calls `vTaskSuspend(NULL)` once the phase word reads ended or
  the application state is not 1. It never returns.
- **Memory.** Window state, evidence and pass slots are normal write-back
  memory that no device reads, so they need no cache maintenance. The write-back
  pool is mapped at EL1 with 2 MiB blocks, which meets section 2.8 for the
  lease.
- **Barriers.** The new code adds no `DSB` or `DMB`. The framebuffer's raw
  path keeps its existing `DSB SY` before and after each access. That pair also
  completes the last NSI access of a phase before the counter read that ends
  the phase, the case section 1.2 names. Timestamps are an `ISB` followed by
  `MRS CNTPCT_EL0`. Every counter sample passes `a733_monotonic_advance()` from
  `a733_range.h`, and a sample that goes backwards refuses the window as late.
- **Cross-core order.** The output task stores the phase word with release.
  Each copy task loads the phase word and the application state with acquire
  once per 2 MiB pass. The output task reads the pass slots with acquire after
  it stores the end phase. The package adds no lock.

## Failure classes

After a failed access the device state is unknown, and the package makes no
further access to it.

| Failure | Class (section 1.8) | Retained | Later accesses |
| --- | --- | --- | --- |
| NSI reset asserted or gate closed | Refused before any NSI access; the framebuffer's existing stage 4 failure path still retains its resources | Framebuffer lease and clock tokens | 0; the stage never repeats |
| Failed hook access, or read-back mismatch | Unknown | Register lock if held, clock tokens, framebuffer lease | 0 |
| Failed window access, or `enter` failure | Unknown | As above, through the load accessor | 0: the window latches failed, stores the ended phase with release and returns at once from every later step and abandon, without calling `enter`, the I/O pair or the counter |
| Window refused or abandoned | Refused | Nothing | Abandon makes 0 register accesses |
| Decode check refusal | Refused before any access | Nothing | Not applicable |

## Reuse and removal

The package reuses the framebuffer's register path, lock, failure path,
fabric clock compare and fabric exclusion. It also reuses
`a733_cpu_clock_report`, the device-memory allocator,
`xTaskCreateStaticAffinitySet`, the output task's loop and its observation
reply, and `a733_monotonic_advance()` from `a733_range.h`. It adds no
register path, mapping check or failure class, and removes no existing code.

Three changes are made to the unmerged implementation:

1. The failure latch in the table above. Before this change, a step after a
   failed access could call `enter` again.
2. The monotonic counter check.
3. The decode check below.

## The decode check refuses until a register source exists

`a733_dram_decode_check()` is the EL3 function that `memory.c` calls before
it maps `0x8000_0000`–`0xFFFF_FFFF` as write-back. It must return 1 only when
this boot's controller configuration shows a linear decode to `0x3_4000_0000`
and no protected region inside that range. No pinned source documents the
needed registers. Manual section 11.1 lists DRAMC features only, and its
memory map names the SMC0 and SMC1 firewalls (`0x0A000000`, `0x0A010000`),
DRAMC Common and the two controllers with no register descriptions. Appendix A
of the [memory qualification design](memory-qualification.md) also finds no
SMC configuration call in the pinned boot0. That finding conflicts with the
"120 MiB secure region" that the table-walk design attributes to boot0, and
no source in the repository supports that region.

The function therefore makes no access and returns 0, so stage 2 stays refused.
Its sequence is **unproven**. Converting it requires two steps:

1. A host trace of the pinned boot0's DRAM initialization, from the DDR
   wrapper calls at `0x48fad` and `0x49bdd`, listing its writes to DRAMC Common,
   the controllers, SMC0 and SMC1.
2. A board read of those registers at EL3 on both boards. The read must match
   the traced values, and the memory qualification's address-line result must
   confirm the decode that the values imply.

The function's prototype is `int a733_dram_decode_check(void)`, the
declaration that `memory.c` already uses.

## Performance budgets

The baseline is the integrated tree, which has no hook, window or copy code.
Figures for the unmerged code come from its own host check. They
become measured baselines when the package's check reproduces them.

| Metric | Baseline | Unmerged code | Budget | Measured by |
| --- | --- | --- | --- | --- |
| Hook accesses | 0 | 2 CCU reads; 21 NSI accesses from reset fields, 9 at the table, worst 27 | 2 CCU reads; at most 27 NSI; 9 at the table; 0 when the NSI is off; 1 lock pair | Counting register model, `checks/dram-priority.c` |
| Window accesses per phase | 0 | Start 6; collect 6, or 7 after the fallback; pending 0 | Same; at most 4,100 per 20 s session (unmerged code: 4,008) | Counting model, `checks/dram-window.c` |
| Accesses, `enter` calls and counter reads after a failure | Not modeled | Not modeled | 0 | New case in `checks/dram-window.c` |
| Instructions outside the I/O callbacks | None | Not counted | Hook at most 600; start at most 200; collect at most 300; pending step at most 50 | Unicorn run of the AArch64 objects built with image flags, with counting stubs for the callbacks |
| `DSB` and `DMB` in package objects | 0 | 0 | 0 | `llvm-objdump` scan |
| Copy kernel | None | 6 instructions per 64 bytes; 2 acquire loads per 2 MiB pass | Same | Disassembly count |
| Code size | 0 | Hook 863 B text; reads-only hook 723 B; window 2,452 B; copy 32 B; 0 B data | Outside the load mode, at most 2 KiB text and 128 B data; decode check at most 16 B | `llvm-size` |
| Added tasks, timers, interrupts and tick wake-ups in HDMI, DisplayPort and steady-state images | 0 | 0 | 0 | `llvm-nm` on the default images shows no `a733_dram_` symbol |
| CPU time per window phase | None | Not measured | At most 20 µs. Each raw access costs an estimated 1–2 µs at 494 MHz, mostly its `AT` translation and two `DSB SY` | Board counter around each phase |
| Scan-out latency | Not measured | Not measured | The scan-out row of the DRAM controller design | Both load images in one board session |

QEMU is not used: no case depends on the scheduler or the translation
tables.

## Left out

- A bandwidth gate on total DRAM traffic. The NSI target-agent ports do not
  count CPU direct-channel traffic. Contention is instead shown by comparing
  the latency of phase A with that of phase B.
- Removing the raw path's per-access `AT` and `DSB SY` pair (sections 1.2 and
  1.5). The framebuffer owns that path, and the change needs barrier
  qualification.
- Other masters, limiters, runtime priority changes and the hook in HDMI and
  DisplayPort images, for the reasons in the DRAM controller design.

## Board evidence

- The gpu-display images return the inherited and final fields and the two CCU
  words. These show Non-secure NSI access and whether boot0 already sets the
  table.
- The no-table and table load images run in one session, no-table first. They
  give the scan-out row, the collect-order bit, the CPU clocks and the passes
  per core.
- The decode check needs the boot0 trace and the EL3 register reads described
  above before it may return 1.
