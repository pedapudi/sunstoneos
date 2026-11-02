# Loader admission of compiled graphs

Status: **implemented and board-qualified** for the kinds listed. Neural-network, activation, transpose and
stream-processor operations, the second-input fields, dynamic-input patches and dynamic-fixed-point 16-bit tensors
are in the assembler (`drivers/npu/nbg_assembly.c`). Shader operations are not admitted. The field-by-field meaning is
in [the command formats](../hardware/npu-command-formats.md). The seventeen test graphs of this repository run on a
board through the OS NPU service, and accesses outside the page tables fault (see "Memory containment").

## What the loader establishes

The assembler admits a graph only when every command that reaches the engine is one it has verified and
every address that the graph patches resolves into a buffer the job owns. There is no list of admitted graphs and
no list of admitted programs: a graph that an application compiled is loaded as it is presented, and containment
comes from the page tables (see "Memory containment"). Admission is layered:

1. **Structure.** Container, section sizes, checksums and the operation table
   (`nbg_model.c`, `nbg_checksum.c`).
2. **Commands.** For each operation, the state stream must match the grammar of its kind exactly, every
   register it writes must be on the list for that kind, and every pointer field it or its descriptor
   carries must be named by a patch.
3. **Addresses.** Every patch is relocated by `nbg_relocate`, which refuses a reference that does not lie
   inside the source region it names. Rings are checked as before; second inputs follow the same rule.
   Aliasing, extents and tensor types are checked as listed below.
4. **Integrity of the copy.** The runtime takes the SHA-256 of each model once, when it is prepared, and LOAD hashes
   the copy it stages and compares. This detects a corrupted or replaced copy; it is not an admission rule and needs
   no table. An application that wants its graph authenticated supplies its own digest with the graph.

5. **The low device range is never mapped.** A pointer word of a descriptor that no patch sets is admitted when it is below
   `NBG_ASSEMBLY_LOW_RESERVED` (1 MiB, `A733_NPU_MMU_LOW_RESERVED`). Such words are not addresses of the job's buffers: the
   stream-processor steps of a softmax over 92 positions hold `0x4220` to `0x1cee0`, multiples of the byte size of one
   head's scores or of one row. They are not in the SRAM window either: the window starts at `0x400000` (4 MiB) and has
   the size of the SRAM that the feature database reports (512 KiB on this NPU), so every one of them lies below it. The
   basis is the page tables and nothing else. (a) `placed()` refuses a buffer whose device address is below the bound;
   (b) `a733_npu_mmu_build` refuses a region that starts below it, for every page size; (c) the vendor driver reserves the
   same range: before it assigns any buffer it allocates the device pages from 0 up to the end of the SRAM window and the
   guard pages after it (`vip2/memory/vip_drv_mmu.c:721-729`, with `vip_sram_address = 0x400000` at `:711` and the size
   from `vip2/vip_drv_context.c:337`), so on the vendor's own tables nothing below `0x400000` maps either. Host checks:
   `checks/npu-mmu.c` builds a table for every page size, shows that a region below the bound is refused and that all
   256 pages of 4 KiB below it fault for a read and a write; `checks/os-npu-service.c` walks the tables the runtime built
   for the one-slot, the 31-slot and the one-slot-per-graph layouts and finds no entry below the bound, and every region of
   every slot and the bootstrap buffer lies above it; `checks/nbg-graph-admission.py` shows that a pointer word of `0x4220`
   is admitted and that one of `0x100000` is refused (with a control that removes the rule). If the engine uses such a
   word as an address, the access faults in the page tables and the job fails (the MMU exception of the board runs); it
   cannot reach another client's memory. If it uses it as an offset in its own scratch, no access leaves the engine.
   **What the engine does with these words is unknown**: no source states it, and the board run that completed these
   graphs shows only that no fault occurred. The rule does not depend on the answer, because the page tables contain
   either outcome.

Fields that the loader does not interpret (program words, shader state, some descriptor words) are listed in the
[command formats](../hardware/npu-command-formats.md) as unknown. They can steer the engine only inside the job's
mappings.

## Operation kinds

| Kind | Admitted | Rule |
| --- | --- | --- |
| INIT (65535) | yes | exact bytes of the initial state, as before |
| NN (2), NT (8) | yes | as before, plus the second input |
| ACT (7) | yes | same state and patches as kind 2; the table is read through patch source 5 |
| SP (6) | yes, directory format | state is `0x08010e5b` and its operand, 1 to 16 pairs of `0x08010e5c` and `0x08010e5d` words, then the 32-byte tail of the other kinds; no other register; one command patch at the operand of `0x08010428`; the `0x0e4c` operand is one of the observed values and its bit 12 equals the presence of a second input; the `0x0e5b` operand (the position in the 256-entry program memory) is at most 255. The program words themselves are not interpreted and not listed: any program is admitted |
| SH (1) | yes, directory format | the shader specification of the formats document: packet grammar (S1), register-set membership (S2a: the MMU, flush, command-fetch and control registers are refused; the flush and synchronization writes are pinned), uniform bank (S4), program length (S5), patches and regions (S6), descriptor extents (S7, S8) and the shader table (S9). The program is copied into the slot's read-only region at a multiple of 256 bytes. A stream has no length limit of its own (rule 7; the examined graphs hold up to 1136 bytes): it is refused only when it cannot share a command segment with its link, that is when it is longer than 524,208 bytes. The loader keeps no copy of a stream and no table of its packets, so the length costs no stack, and the number of patches is bounded by the stream itself (each names a distinct operand word). A tensor descriptor's height is the channel stride over the row stride, and a width or height of 65,536 or more is refused with the reason `NBG_REFUSED_DIMENSION`, because the engine's 16-bit shape fields cannot hold it (S7 in the formats document). The program words and the uniform constants are not interpreted |
| TP (3), SW (4) | no | refused: the board's NPU reports zero tensor-processor engines, the compiler emits neither kind for this target, and a software operation names work for the vendor runtime on a CPU |

## Memory containment

Because the loader does not interpret every field, every NPU memory access has to be confined to the job's
mappings by the hardware. The sources establish this much:

- The NPU is brought up with its MMU enabled: the vendor driver sets up and enables it for every core
  before any job (`vip2/vip_drv_hardware.c:237-275`, `:380-422`, `:462-466`), and the controller of this
  repository checks `+0x388 == 0x21` before every start and before open completes
  (`drivers/npu/a733_npu_controller.c:98-99`). Command fetch, descriptor fetch, weights and tensors are
  addressed by device virtual address through the page tables that `a733_npu_mmu.c` builds from exactly the
  registered buffers; an absent entry holds the exception-enabled value (`docs/npu-mmu.md`).
- The one window outside the page tables is the internal SRAM. The vendor driver reserves device virtual
  addresses from `0x400000` for it and fills no page-table entries for it
  (`vip2/memory/vip_drv_mmu.c:681-731`; `docs/reference-backsolve-npu.md`, "internal SRAM base is the
  virtual address `0x400000`"). It is the 512 KiB of shared SRAM that the boot code routes to the NPU
  (`docs/reference-backsolve-npu.md`, rows 1 and 218-236). A reference into that window reaches only that
  SRAM. The memory layer refuses every buffer that overlaps `0x400000` to `0x480000`
  (`drivers/npu/a733_npu_memory.c`, `nbg_assembly.c:placed`).

**Board evidence** (3-1, 852 MHz at 800 mV, [the reference](../reference-backsolve-npu.md#npu-mmu-exceptions)). Five
graphs, each with one descriptor pointer aimed at an address the page tables do not map (a canary buffer in DRAM, or
page 0), failed one by one with an NPU MMU exception at that address, and the canary stayed unchanged after every case.
The cases cover an input read and an output write of activation operations, a first-input read of a stream-processor
operation and an output write of another, and the page-0 address. The service cancelled and reset the NPU after each
and the session went on. So for the operation kinds tested, a pointer outside the tables is stopped by the MMU.

What this does not establish: that every NPU master path goes through the page tables for instruction, shader or
stream-processor program fetches (a hostile program is not tested; the programs in the run are ones the compiler
produced), and what a program does with the shared SRAM at `0x400000`. A graph that an application compiled is
therefore as trusted as the application: the page tables confine it to the job's buffers and the shared SRAM, not to
a correct computation.

## Limits that remain

1. **Unexplained program and shader fields.** What the loader does not interpret is listed in the formats
   document. A graph can do anything those fields allow inside the job's mappings and the shared SRAM.
2. **The SRAM window is shared.** Every operation of a graph without a memory pool uses it, and it is
   addressed physically.
3. **Extents of linear accesses are unresolved** for weights, second inputs and dynamic inputs. Only the
   relocated base and the sizes the descriptor states are checked.
4. **No isolation between slots.** The page-table flush between jobs is unqualified
   (`docs/designs/npu-breadth.md`), so a job could read the previous job's data in the SRAM.

## Tensor representation: the fixed position of a dynamic-fixed-point tensor

A 16-bit signed dynamic-fixed-point graph tensor carries a fixed position in its record: the values are the codes scaled by
2^-position. The field is host-side metadata (the engine's descriptors hold their own scales and shifts; the loader never
derives anything from it), and a tensor whose values are large has a negative position: the output of the convolution module
of a conformer layer compiled for 368 frames has position -2, a step of 4. The loader admits positions from -16 to 31 and
refuses beyond (`checks/nbg-graph-admission.py` admits -2 and -16 and refuses -17 and 32, with controls). A board run of that
convolution module (three consecutive jobs, one slot) returned the simulator's output byte for byte and the same bytes in every
job, so the engine runs the representation as compiled.
