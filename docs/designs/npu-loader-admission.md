# Loader admission of compiled graphs

Status: **qualified on the board** for the listed operation kinds; implemented in `drivers/npu/nbg_assembly.c`. Neural-network, activation, transpose, stream-processor and shader operations, the second-input fields, dynamic-input patches and dynamic-fixed-point 16-bit tensors are admitted. The field-by-field meaning is in [the command formats](../hardware/npu-command-formats.md). The seventeen test graphs of this repository run on a board through the OS NPU service, and accesses outside the page tables fault ([record](../evidence/npu-loader-admission.jsonl), `npu-loader-admission-mmu-containment`).

## What the loader establishes

The assembler admits a graph only when every command that reaches the engine is one it has verified and every address that the graph patches resolves into a buffer the job owns. There is no list of admitted graphs or programs: a graph that an application compiled is loaded as presented, and containment comes from the page tables. Admission has five layers:

1. **Structure.** Container, section sizes, checksums and the operation table (`nbg_model.c`, `nbg_checksum.c`).
2. **Commands.** For each operation, the state stream matches the grammar of its kind exactly, every register it writes is on the list for that kind, and every pointer field it or its descriptor carries is named by a patch.
3. **Addresses.** `nbg_relocate` relocates every patch and refuses a reference that does not lie inside the source region it names. Rings, second inputs, aliasing, extents and tensor types are checked.
4. **Integrity of the copy.** The runtime takes the SHA-256 of each model once, when it is prepared, and LOAD hashes the copy it stages and compares. This detects a corrupted or replaced copy; it is not an admission rule and needs no table. An application that wants its graph authenticated supplies its own digest.
5. **The low device range is never mapped.** A pointer word of a descriptor that no patch sets is admitted when it is below `NBG_ASSEMBLY_LOW_RESERVED` (1 MiB, `A733_NPU_MMU_LOW_RESERVED`). Such words are not addresses of the job's buffers: the stream-processor steps of a softmax over 92 positions hold `0x4220` to `0x1cee0`, multiples of the byte size of one head's scores or of one row. The basis is the page tables: `placed()` refuses a buffer whose device address is below the bound, `a733_npu_mmu_build` refuses a region that starts below it for every page size, and the vendor driver reserves the same range. If the engine uses such a word as an address, the access faults and the job fails; as an offset in its own scratch, no access leaves the engine. What the engine does with these words is unknown, and the rule does not depend on the answer. Host checks are in `npu-loader-admission-low-range-host-checks`.

Fields that the loader does not interpret (program words, shader state, some descriptor words) are listed in the command formats as unknown. They can steer the engine only inside the job's mappings.

## Operation kinds

| Kind | Admitted | Rule |
| --- | --- | --- |
| INIT (65535) | yes | exact bytes of the initial state |
| NN (2), NT (8) | yes | state grammar of the kind, plus the second input |
| ACT (7) | yes | same state and patches as kind 2; the table is read through patch source 5 |
| SP (6) | yes, directory format | state is `0x08010e5b` and its operand, 1 to 16 pairs of `0x08010e5c` and `0x08010e5d` words, then the 32-byte tail of the other kinds; no other register; one command patch at the operand of `0x08010428`; the `0x0e4c` operand is one of the observed values and its bit 12 equals the presence of a second input; the `0x0e5b` operand (position in the 256-entry program memory) is at most 255. The program words are not interpreted: any program is admitted |
| SH (1) | yes, directory format | the shader specification of the formats document: packet grammar (S1), register-set membership (S2a: the MMU, flush, command-fetch and control registers are refused; the flush and synchronization writes are pinned), uniform bank (S4), program length (S5), patches and regions (S6), descriptor extents (S7, S8) and the shader table (S9). The program is copied into the slot's read-only region at a multiple of 256 bytes. A stream has no length limit of its own; it is refused only when it cannot share a command segment with its link, that is when it is longer than 524,208 bytes. The loader keeps no copy of a stream, so the length costs no stack, and the number of patches is bounded by the stream itself. A tensor descriptor's height is the channel stride over the row stride, and a width or height of 65,536 or more is refused with `NBG_REFUSED_DIMENSION`, because the engine's 16-bit shape fields cannot hold it. Program words and uniform constants are not interpreted |
| TP (3), SW (4) | no | the board's NPU reports zero tensor-processor engines, the compiler emits neither kind for this target, and a software operation names work for the vendor runtime on a CPU |

## Memory containment

Because the loader does not interpret every field, the hardware confines every NPU memory access to the job's mappings:

- The NPU runs with its MMU enabled. The vendor driver enables it for every core before any job, and the controller of this repository checks `+0x388 == 0x21` before every start and before open completes. Command fetch, descriptor fetch, weights and tensors are addressed by device virtual address through the page tables that `a733_npu_mmu.c` builds from exactly the registered buffers. An absent entry holds the exception-enabled value ([MMU notes](../npu-mmu.md)).
- The one window outside the page tables is the internal SRAM, the 512 KiB of shared SRAM that the boot code routes to the NPU, at device address `0x400000`. A reference into that window reaches only that SRAM. The memory layer refuses every buffer that overlaps `0x400000` to `0x480000`.

Board result ([reference](../reference-backsolve-npu.md#npu-mmu-exceptions)): five graphs, each with one descriptor pointer aimed at an unmapped address, failed one by one with an NPU MMU exception at that address; the DRAM canary stayed unchanged and the service recovered each time.

Not established: that every NPU master path goes through the page tables for instruction, shader or stream-processor program fetches (the programs in the run were compiler products), and what a program does with the shared SRAM. A compiled graph is therefore as trusted as its application: the page tables confine it to the job's buffers and the shared SRAM, not to a correct computation.

## Tensor representation: fixed position of a dynamic-fixed-point tensor

A 16-bit signed dynamic-fixed-point graph tensor carries a fixed position in its record: the values are the codes scaled by 2^-position. The field is host-side metadata (the engine's descriptors hold their own scales and shifts), and a tensor whose values are large has a negative position; the convolution module of a conformer layer compiled for 368 frames has position -2, a step of 4. The loader admits every value and nothing computes with it. A board run of that module (three consecutive jobs, one slot) returned the simulator's output byte for byte (`npu-loader-admission-fixed-position-board`).

## Limits

1. **Unexplained program and shader fields.** A graph can do anything those fields allow inside the job's mappings and the shared SRAM.
2. **The SRAM window is shared.** Every operation of a graph without a memory pool uses it, and it is addressed physically.
3. **Extents of linear accesses are unresolved** for weights, second inputs and dynamic inputs. Only the relocated base and the sizes the descriptor states are checked.
4. **No isolation between slots.** The page-table flush between jobs is unqualified ([NPU runtime](npu-breadth.md)), so a job could read the previous job's data in the SRAM.
