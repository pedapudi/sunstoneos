# Design: memory-bus priority for scan-out and DRAM latency under copy load

Status: **implemented and host-tested**; no image links the code. The measured host figures are in [dram-bus-priority.jsonl](../evidence/dram-bus-priority.jsonl). Non-secure EL1 reads of NSI registers are **qualified on the board** by the [DRAM bandwidth](dram-bandwidth.md) study; the hook's writes, the window and the decode check are not run on a board. Section numbers refer to [performance architecture](../performance-architecture.md).

The [DRAM controller design](dram-controller.md) is the specification for the port table, the window rules, the evidence block layout and the board session. This document covers how `drivers/dram/` implements it, and the `a733_dram_decode_check()` function that [CPU table walks](cpu-table-walks.md#stage-2-blocks-above-the-boot-image) requires.

## Files

- `include/a733_dram.h`: one header for every declaration.
- `drivers/dram/a733_dram.c`: the priority hook and its address set. The hook writes the hook part of the evidence block directly.
- `drivers/dram/a733_dram_load.c` and `a733_dram_copy.S`: the latency window, the copy pass and the window part of the evidence block. The C file compiles only with `SUNSTONE_GPU_DISPLAY_DRAM_LOAD`.
- `drivers/dram/a733_dram_decode.c`: the decode check.
- `checks/dram-priority.c`, `checks/dram-window.c` and `checks/dram-controller.py`: host cases, the source comparison (including the port numbering, `sub_node_id_mapping`, the counter clock, the U-Boot base and stride, and the `MBUS_DFS_EN` bit against the manual), the instruction counts and code size; [docs/dram.md](../dram.md).

No image links the objects until the framebuffer, gpu-display and build edits listed in the DRAM controller design merge. Those files belong to other owners.

## Register sources

- **Field writes.** `nsi_port_setmode`, `nsi_port_setpri` and `nsi_port_setio` in `sunxi-nsi.c` read, modify and write bits [1:0] of `+0x10`, bits [3:0] of `+0x14` (value `(p << 2) | p`) and bit 0 of `+0x18`. The DT table sets mode 0 and select 1, with priority 2 for DE and 3 for both IOMMU ports. U-Boot writes whole words, and its select case writes `+0x10` instead of `+0x18`. The hook follows the Linux read-modify-write sequence.
- **Counter sequence.** `__nsi_pmu_timer_store_v2` writes the period, clears enable bit 0 of `+0xc0`, sets clear bit 0 of `+0xc4` and then sets enable bit 0. `sunxi-nsi-bw.c` divides the latency sum (`+0xdc`) by read requests (`+0xcc`).
- **Power domain.** `sunxi_set_nsi_qos_params` takes every master's power domain before it writes port fields, and DE's domain is DE_SYS, so the hook runs after the framebuffer has powered DE_SYS.
- **CCU words.** Manual sections 4.1.6.74 and 4.1.6.75 place `NSI_CLK_REG` at CCU `+0x580` (gate bit 31, reset bit 30) and `NSI_BGR_REG` at `+0x584` (reset bit 16, gate bit 0); a set bit means the gate is open or the reset is deasserted. Section 4.1.6.76 places `MBUS_CLK_REG` at `+0x588`, with `MBUS_DFS_EN` at bit 28.

Board facts: Non-secure EL1 reads the nine boot0 NSI target-agent words, the mode, priority, select and limit words of all 21 initiator ports, `NSI_CLK_REG` (`0xc3000000`), `NSI_BGR_REG` (`0x10001`) and `MBUS_CLK_REG` without an abort, and the nine target-agent words accept writes that read back. Every initiator port reads priority 0 in the boot path, which differs from the table, so a no-table image can show contention.

Not yet shown on the board:

- Writes to the priority fields of ports 2, 14 and 15.
- Whether `MBUS_DFS_EN` is clear. With it set, the MBUS rate can change, so a period written in MBUS cycles would not span 50 ms; the first start of a session reads `+0x588` and refuses the session when the bit is set.
- Whether the clear bit clears itself, whether the counters hold at the end of the period, and the unit of the latency sum. The window's refusal rules cover the clear bit (an idle refusal shows a stuck bit) and the counter hold (the free-running fallback); the statistic compares the two load images at the same MBUS rate, so the unit cancels.

A Secure-only NSI would answer a Non-secure access with an external abort, and one that ignores Non-secure writes shows as a read-back mismatch. An `NSI_CLK_REG` that reads zero from the Non-secure side refuses the hook before any NSI access.

## Data flow and ownership

- **Hook.** The framebuffer calls `a733_dram_priority(io, evidence)` once, in prepare stage 4, under one register-lock acquisition. Every access goes through the framebuffer's `raw_read` and `raw_write` pair. The hook zeroes the evidence block and reads the two CCU words first (a failed read stores stopping field 10). When the NSI is not running, it makes zero NSI accesses and leaves the stopping field 0. Otherwise it reads each of the nine fields and, in the table image, writes and reads back only the fields that differ from the table.
- **Window.** The gpu-display output task calls `a733_dram_window_step()` once per pass of its 1 ms loop. A window programs port 2's counters for P = 50 ms and then collects six counts. Per-phase sums of the latency statistic go into the evidence block, which the output task copies once into the observation reply (136 bytes). The `u16` at offset 38 is `phase_ticks_max`: the longest start or collect in 24 MHz ticks, saturating at 65,535 (2.7 ms). Start measures t1 − t0, and collect measures t_end minus the counter value that the step read before calling it. Both spans include `enter` and every access but not `leave`.
- **Duty-cycle loss.** A collect and the next start run in consecutive 1 ms steps, so each window counts 50 ms of about 51 ms and about 2% of each phase goes uncounted. The statistic is a mean per read request, so the gap lowers the number of windows and leaves each window's value unchanged. Starting the next window inside the collect step would remove the gap, but that step would run about 306 instructions (collect 158 and start 148), above the collect budget of 300.
- **Copy load.** Eight tasks, one pinned to each core, call `a733_dram_copy_pass()` in a loop. Each call copies 2 MiB inside a 32 MiB lease that stays in `DEVICE_MEMORY_CPU` until power-off. No device is granted the lease, so its dirty lines never need cleaning.
- **Single writers.** The output task is the only writer of NSI ports 2, 14 and 15 and of the window state, and of the phase word. Each copy task writes only its own 64-byte pass slot.
- **Failure latch.** After a failure the ended phase makes every later step and abandon return at once, without calling `enter`, the I/O pair or the counter. Every counter sample passes `a733_monotonic_advance()` from `a733_range.h`, and a sample that goes backwards refuses the window as late.

## Interrupts, memory, barriers and cores

- **Polling.** The NSI interrupt (SPI 43) has no qualified delivery, and a 50 ms period spans many ticks. Section 1.3 permits a tick wait for that case. The window adds no task, timer or interrupt; its pending check is one counter read and one compare per tick.
- **Copy tasks.** Each task blocks on a task notification until phase B begins. It calls `vTaskSuspend(NULL)` once the phase word reads ended or the application state is not 1, and never returns.
- **Memory.** Window state, evidence and pass slots are normal write-back memory that no device reads, so they need no cache maintenance. The write-back pool is mapped at EL1 with 2 MiB blocks, which meets section 2.8 for the lease.
- **Barriers.** The component adds no `DSB` or `DMB`. The framebuffer's raw path keeps its `DSB SY` before and after each access: up to 58 for the hook's 29 accesses, 12 per start (14 for the first start of a session) and 12–14 per collect. That pair also completes the last NSI access of a phase before the counter read that ends the phase, the case section 1.2 names. Timestamps are an `ISB` followed by `MRS CNTPCT_EL0`.
- **Barriers after the raw path moves to plain accessors.** Each sequence reaches one peripheral, which Device-nGnRnE already orders. The hook needs no barrier: it runs before any master on ports 2, 14 and 15 starts, and every write is followed by a read-back that the hook compares. Start needs one `a733_mmio_dsb_sy()` before the `ISB` and counter read that give t1, because that read is not a memory access. Collect needs one before the t_end read. The pending check and the t0 read follow no NSI access and need none.
- **Cross-core order.** The output task stores the phase word with release. Each copy task loads the phase word and the application state with acquire once per 2 MiB pass. The output task reads the pass slots with acquire after it stores the end phase. No lock.

## Failure classes

After a failed access the device state is unknown, and the component makes no further access to it.

| Failure | Class (section 1.8) | Retained | Later accesses |
| --- | --- | --- | --- |
| NSI reset asserted or gate closed | Refused before any NSI access; the framebuffer's stage 4 failure path still retains its resources | Framebuffer lease and clock tokens | 0; the stage never repeats |
| Failed hook access (a CCU read stores stopping field 10), or read-back mismatch | Unknown | Register lock if held, clock tokens, framebuffer lease | 0 |
| Failed window access, or `enter` failure | Unknown | As above, through the load accessor | 0: the failure latch |
| `MBUS_DFS_EN` set at the first start | Refused after one CCU read and no port access | Nothing | 0: the phases end early and a result bit records the refusal |
| Synchronous external abort or SError from an NSI or `+0x584` access | Unknown; the access never returns to the failure path | Everything; the platform's exception capture records ESR, ELR and FAR and ends the image | 0 |
| Window refused or abandoned | Refused | Nothing | Abandon makes 0 register accesses |
| Decode check refusal | Refused before any access | Nothing | Not applicable |

## The decode check refuses until a register source exists

`a733_dram_decode_check()` is the EL3 function that the cached table builder in `memory.c` calls before it maps DRAM above the boot image as write-back at EL1. The builder passes the ranges it is about to map: `0x4800_0000`–`0x7900_0000` in the local layout and `0x8000_0000`–`0x3_4000_0000` in both layouts ([12 GiB EL1 map](memory-map-12g.md)). The check must return 1 only when, for every byte of every listed range, this boot's controller configuration decodes the address linearly from `0x4000_0000` with no alias, the decoded size reaches `0x3_4000_0000`, and the firewall grants Non-secure read and write with no protected region overlapping. Its budget is controller and firewall register reads only, no DRAM access, at most 64 register reads and 2,000 instructions.

No pinned source documents the needed registers. Manual section 11.1 lists DRAMC features only, and its memory map names the SMC0 and SMC1 firewalls (`0x0A000000`, `0x0A010000`), DRAMC Common and the two controllers with no register descriptions. Appendix A of the [memory qualification design](memory-qualification.md) finds no SMC configuration call in the pinned boot0. That conflicts with the "120 MiB secure region" that the table-walk design attributes to boot0, and no source in the repository supports that region.

The function therefore makes no access and returns 0, so the builder maps nothing above the boot image. Its sequence is unproven. Converting it requires a host trace of the pinned boot0's DRAM initialization (from the DDR wrapper calls at `0x48fad` and `0x49bdd`) listing its writes to DRAMC Common, the controllers, SMC0 and SMC1. It also requires a board read of those registers at EL3 on both boards that matches the traced values, with the memory qualification's address-line result confirming the decode that the values imply.

The prototype is `int a733_dram_decode_check(const struct a733_dram_range *ranges, unsigned count)`, declared once in `include/a733_dram.h`, which `include/a733_device_memory_layout.h` includes. No image links `drivers/dram/a733_dram_decode.c`, so `memory.c` keeps a weak definition with the same result, and each image links one definition.

## Performance budgets

The baseline is a tree without the component. The measured values are in `dram-bus-priority-host-measurement`.

| Metric | Budget | Measured by |
| --- | --- | --- |
| Hook accesses | 2 CCU reads; at most 27 NSI; 9 at the table; 0 when the NSI is off; 1 lock pair | Counting register model, `checks/dram-priority.c`; the lock pair belongs to the framebuffer caller |
| Window accesses per phase | Start 6, plus one `+0x588` read in the first start; collect 6, or 7 after the fallback; pending 0; at most 4,100 per 20 s session | Counting model, `checks/dram-window.c` |
| Accesses, `enter` calls and counter reads after a failure | 0 | `checks/dram-window.c` |
| Instructions outside the I/O callbacks | Hook at most 600; start 200; collect 300; pending step 50 | Unicorn run of the AArch64 objects with counting stubs |
| `DSB` and `DMB` in objects | 0 (2 after the raw path moves to plain accessors) | `llvm-objdump` scan |
| `DSB SY` per sequence, raw path included | Hook up to 58; start 12 (14 for the first start); collect 12–14. After the raw path moves to plain accessors: hook 0, start 1, collect 1 | Derived: two per access times the modeled access counts |
| `phase_ticks_max` | Largest span, saturating at 65,535 | `checks/dram-window.c` |
| Copy kernel | 6 instructions per 64 bytes; 2 acquire loads per 2 MiB pass | Disassembly count |
| Code size | Outside the load mode at most 2 KiB text and 128 B data; decode check at most 16 B | `llvm-size` |
| Added tasks, timers, interrupts and tick wake-ups in HDMI, DisplayPort and steady-state images | 0 | `llvm-nm`: no `a733_dram_` symbol |
| CPU time per window phase | At most 20 µs (480 ticks); each raw access costs an estimated 1–2 µs at 494 MHz | `phase_ticks_max` from both load images |
| Scan-out latency | The scan-out row of the DRAM controller design | Both load images in one board session |

## Left out

- A bandwidth gate on total DRAM traffic: the NSI target-agent ports do not count CPU direct-channel traffic, so contention is shown by comparing the latency of phase A with that of phase B.
- Removing the raw path's per-access `AT` translation (section 1.5), `DSB SY` pair (section 1.2) and two `timely()` calls per access, each of which takes the counter clock's ticket lock with DAIF masked. The framebuffer owns that path, and the barrier change needs the barrier qualification in `docs/shared-helpers.md`. The barrier placement above makes this component's part of that migration mechanical.
- Other masters, limiters, runtime priority changes and the hook in HDMI and DisplayPort images, for the reasons in the DRAM controller design.

## Open work

1. Run the no-table load image first (built reads-only, `SUNSTONE_DRAM_PRIORITY=1`) to record the inherited fields and CCU words, then the table load image to show that writes stick. Together they give the scan-out row, the collect-order bit, `phase_ticks_max`, the CPU clocks and the passes per core. The normal gpu-display image defines `SUNSTONE_DRAM_PRIORITY` as 2 only after both results exist.
2. Obtain the boot0 trace and the EL3 register reads that let the decode check return 1.
