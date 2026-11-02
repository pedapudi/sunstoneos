# Loader admission of compiled graphs

Status: **design and partial implementation.** Neural-network, activation, transpose and stream-processor
operation kinds, the second-input fields, dynamic-input patches and dynamic-fixed-point 16-bit tensors are
in the assembler (`drivers/npu/nbg_assembly.c`) as work in progress. Shader operations are not admitted.
The field-by-field meaning is in [the command formats](../hardware/npu-command-formats.md). Host evidence
only; no board result qualifies any of this.

## What the loader establishes

The assembler admits a graph only when every command that reaches the engine is one it has verified and
every address that the graph patches resolves into a buffer the job owns. Admission is layered:

1. **Digest.** The runtime admits a graph only when its whole-graph SHA-256 is in the reviewed catalogue
   (`npu_admitted_models`, `a733_npu_runtime.c:admit`), built by the pinned compiler. Nothing else loads.
2. **Structure.** Container, section sizes, checksums and the operation table
   (`nbg_model.c`, `nbg_checksum.c`).
3. **Commands.** For each operation, the state stream must match the grammar of its kind exactly, every
   register it writes must be on the list for that kind, and every pointer field it or its descriptor
   carries must be named by a patch.
4. **Addresses.** Every patch is relocated by `nbg_relocate`, which refuses a reference that does not lie
   inside the source region it names. Rings are checked as before; second inputs follow the same rule.
5. **Fields that remain unexplained** are admitted only by the digest of layer 1, and are listed in the
   [command formats](../hardware/npu-command-formats.md) as unknown.

## Operation kinds

| Kind | Admitted | Rule |
| --- | --- | --- |
| INIT (65535) | yes | exact bytes of the initial state, as before |
| NN (2), NT (8) | yes | as before, plus the second input |
| ACT (7) | yes | same state and patches as kind 2; the table is read through patch source 5 |
| SP (6) | yes, directory format | state is `0x08010e5b` and its operand, 1 to 16 pairs of `0x08010e5c` and `0x08010e5d` words, then the 32-byte tail of the other kinds; no other register; one command patch at the operand of `0x08010428`; the `0x0e4c` operand is one of the observed values and its bit 12 equals the presence of a second input |
| SH (1) | no | state streams are not documented enough to verify; see the formats document |
| TP (3), SW (4) | no | not observed |

## Memory containment

Before the digest is trusted to stand in for an unexplained field, every NPU memory access has to be
confined to the job's mappings. The sources establish this much:

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

What the sources do not establish: that no NPU master path bypasses the page tables for instruction,
shader or stream-processor fetches (the vendor driver has no switch for it, and the hardware manual is not
available), and what a hostile program does with the SRAM. A board negative test (a reference to an address
the tables leave unmapped, expecting an MMU fault and recovery) would test the first; fault recovery is
unqualified, so it needs approval before it is submitted.

## Limits that remain

1. **Unexplained program and shader fields.** What the digest covers but the loader does not interpret is
   listed in the formats document. A reviewed graph can still do anything those fields allow inside the job's
   mappings and the shared SRAM.
2. **The SRAM window is shared.** Every operation of a graph without a memory pool uses it, and it is
   addressed physically.
3. **Extents of linear accesses are unresolved** for weights, second inputs and dynamic inputs. Only the
   relocated base and the sizes the descriptor states are checked.
4. **No isolation between slots.** The page-table flush between jobs is unqualified
   (`docs/designs/npu-breadth.md`), so a job could read the previous job's data in the SRAM.
