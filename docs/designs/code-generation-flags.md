# Code generation flags of the OS image

The relaxed units and the application objects are compiled for the Cortex-A55 (`-mtune=cortex-a55`; six of the eight cores are A55 cores, and the A76 cores run the same code out of order). The strict units, boot and device code, are not: the tuning unrolls their loops and adds 9 percent to their instructions (206,545 against 189,587 for the strict units of the encoder image) with no measured gain. The strict memory routines `runtime/memory.c` and `memmove.c` are the exception: the tuned loops copy 1.7 times faster (8.0 against 4.7 GB/s aligned and 1.0 against 0.6 GB/s with differing alignments, in `apps/memcpy-bench`). Whether a unit is compiled for strict alignment (`-mstrict-align`) depends on where and when its code runs. `cmake/codegen-align.cmake` holds the two lists that decide it, and `checks/codegen-alignment.py` enforces them on the compile database of an OS build.

## Why alignment matters

A data access made with the MMU off is a Device access, and so is an access to a Device or non-cacheable monitor page. An unaligned access to such memory faults. After the MMU maps DRAM as Normal write-back memory and `SCTLR_EL1.A` is clear, a Cortex-A55 or A76 performs an unaligned access to that memory in hardware. A compiler that must assume strict alignment builds a halfword, word or doubleword from bytes and splits every wide copy of unknown alignment. For the 80 NPU graphs of the encoder, the operation inspection of admission executes 13.7 million instructions with strict alignment and 9.2 million without.

## The two lists

- **Strict.** Code that runs before the MMU is on or at EL3: `platform/`, the generated boot, continuation and return units, and the session decoder and digest that the EL3 boot calls (`apps/os/session.c`, `grants.c`, `capture.c`, `npu-declaration.c`, `drivers/npu/npu_declaration.c`, `drivers/storage/storage_digest*`). Also every unit that accesses registers, controller queues or descriptors in Device or non-cacheable memory: the `*_native` units, the USB, GPU, power, clock, crypto, pin and thermal drivers, the NPU controller, memory, MMU and sequence units, and the memory routines `runtime/memory.c` and `memmove.c`.
- **Relaxed.** Units that run at EL1 or EL0 on Normal write-back memory only: the scheduler and the unified memory ledger, the OS services and tasks, the NPU graph decoder and assembler and the runtime that places the graphs, the radio protocol and file system parsers, the network stack and the supplicant, and the application objects.

A unit on neither list stays strict. A unit on both is refused when CMake configures.

## The memory routines

A relaxed unit is compiled with `include/sunstone_relaxed_memory.h` force-included, which renames its `memcpy`, `memmove` and `memset` calls to `ss_memcpy_unaligned`, `ss_memmove_unaligned` and `ss_memset_unaligned` (`runtime/memory_unaligned.c`). Those routines load unaligned and store 16-byte-aligned pairs. A strict unit keeps the routines of `runtime/memory.c` and `memmove.c`, which copy a byte at a time when source and destination differ in alignment. A call that the compiler makes by itself, such as the copy of a large structure, links the strict routine. The link check (`checks/sunstone-image-link.py`) lists in `link-check.json` which objects call which version.

## Every core must have `SCTLR_EL1.A` clear

The continuation code writes the whole register for every core, and the entry of each core reads it back before any task runs and stops the board with detail `0x207` if `A` is set (`platform/a733-smp-cached/continue.c`). The admission record of a run holds the value for core 0.

## What the check requires

- Every relaxed and application unit has `-mtune=cortex-a55`, and no strict unit has it except the strict memory routines.
- Every unit on the strict list ends with `-mstrict-align`, and every unit on the relaxed list with `-mno-strict-align` and the force-included header.
- No function of a relaxed unit is reachable, by direct calls, from the EL3 roots `a733_smp_boot` and the return terminal's restore. Calls through function pointers are not followed, so a unit that EL3 reaches only through a pointer must be placed on the strict list by hand.
- Mutations (a strict unit built without `-mstrict-align`, a strict unit given `-mno-strict-align`, a relaxed unit built strict, and a relaxed unit that EL3 calls) are refused.

## Stores in address order

A Cortex-A55 recognizes a stream of stores that fill whole cache lines at rising addresses and then stops fetching the lines it is about to overwrite. The unaligned routines therefore store at rising addresses and never store a byte twice: a ladder of 1, 2, 4 and 8 bytes brings the destination to a 16-byte boundary, 64-byte blocks follow, and a ladder of 32 down to 1 bytes ends the copy. Loads may be unaligned and overlap. A first version stored the head and the tail of each copy again after the loop. In `apps/memcpy-bench`, 176 rows of 382 bytes written into lines that are not in the cache took 219 ns a row that way and 120 ns in address order (the strict routines: 342 ns). In the encoder it made the position window, which writes into an input buffer that the queue has just cleaned and invalidated, 5.3 ms slower than with the strict routines; in address order it is 5.2 ms faster. `checks/memory-unaligned.py` requires address order for every copy and fill above 64 bytes (above 32 for `memset`).

## Measurements

The encoder on the board (CPU-return image, A55 at the granted clock) improved from 656.3 ms to 610.3 ms in total. Admission's preparation went from 83.9 ms to 58.0 ms (layout 51.6 to 31.8 ms, copy 14.6 to 10.7 ms, decode 7.0 to 4.9 ms). The position window went from 26.7 to 21.5 ms. The encoder application's table-driven CRC of its results took 140.9 ms instead of 172.8 ms. `apps/memcpy-bench` measures the routines at 1794 MHz against the tuned strict routines: copies whose source and destination differ in alignment run 5 to 6 times faster for 1 KiB to 256 KiB and 3 times faster from DRAM; aligned copies run 1.1 to 1.2 times faster in the caches and equally from DRAM; `memset` of 4 KiB runs 1.6 times faster aligned and 2.4 times faster off by 3.
