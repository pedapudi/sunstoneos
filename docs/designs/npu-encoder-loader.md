# Loader admission for the whole-encoder graphs

Status: **derivation only; no loader change is made yet.** All results come from the host. The
graphs are those of the mimus study `docs/npu-encoder-offload.md` (mimus branch `work/npu-encoder-1101`),
compiled graphs under `~/.cache/sunstoneos/build/npu-encoder-1101/`. `tools/npu-nbg-census.py` reproduces
every number below from a graph file with the operating system decoder.

## What the loader must establish

The assembler (`drivers/npu/nbg_assembly.c`) admits a graph only when every command that reaches the
engine is one it has verified, and every address that the graph patches resolves into a buffer the job
owns. Today it admits the initialization record (kind 65535), neural-network operations (kind 2) and
neural-transpose steps (kind 8). The study asks for stream-processor and activation operations (kinds 6
and 7) and the shader section (kind 1).

## Sources

| Source | What it establishes | What it does not |
| --- | --- | --- |
| `tools/inspect-nbg.py` (from the vendor SDK enumerations) | Operation kinds: 1 SH, 2 NN, 3 TP, 6 SP, 7 NN_LUT, 8 NT. Patch sources: 0 command, 1 input, 2 output, 3 memory pool, 4 AXI SRAM, 5 generic dynamic, 6 dynamic input, 7 dynamic output, 8 VIP SRAM, 9 kernel bitstream | Field layouts |
| Vendor kernel driver `vip2/` (pinned in `docs/reference-backsolve-npu.md`) | Register sequences for reset, initialization, MMU, submission and interrupts. The driver submits command buffers that the user-space library builds | The load-state packets inside a command buffer: a search of `vip2/`, the Mesa tree and the SDK headers finds no definition of `0x0e5b`, `0x0e5c`, `0x0e5d`, or the shader state registers |
| Mesa etnaviv NPU (`mesa-45b86c99`) | The neural-network descriptor (192 bytes) and its pointer fields for kind 2 | Kinds 6, 7 and 1 |
| The compiled graphs | The wire facts below, which the decoder reads without interpretation | Meaning of any register value |

The kernel driver therefore gives no account of the stream-processor program, the activation table or the
shader state packets. The facts below are observed, not derived from a specification.

## Census of the layer-0 graph (92 frames)

Version `0x20000` directory format, 154 operations, 60 layers, no memory pool, 483,328 bytes of VIP SRAM,
dynamic-input section of 7,040 bytes, one shader instruction section of 1,280 bytes. L7 and L15 have the same
operation mix and the same patch features. The subsampling graphs use only kinds 2 and 8 and pool sources.

| Kind | Operations | State bytes | Descriptor patch destinations (source) |
| --- | ---: | --- | --- |
| INIT | 1 | 40 | 20 (8) |
| NN (2) | 27 | 24, 32 | 20 (5), 24 (1, 8), 28 (8), 76 (8), 84 (8), **136 (8), 140 (8)** |
| NT (8) | 26 | 32 | 24, 28, 76, 84 (8) |
| ACT (7) | 3 | 32 | 20 (5), 24, 28, 76, 84 (8) |
| SP (6) | 94 | 104, 120, 136, 168, 184 | 20 (5), 24 (1, 6, 8), 28 (2, 8), 76, 84 (8), **136 (6, 8), 140 (8)** |
| SH (1) | 3 | 576, 792, 816 | none; command patches inside a 512-byte-class state at 4, 28, 52, 76, 100, 124, 300, 516, 540 (sources 0, 5, 8) |

Findings that change the study's list:

1. **Kind 7 is kind 2.** Its state stream (32 bytes) and its patches are those of a kind 2 operation. The
   existing checks apply unchanged; only the kind test differs.
2. **Seventeen of the 27 kind 2 operations already carry a second input** (descriptor destinations 136 and 140).
   The loader refuses these today (`nbg_assembly.c`, `operation()`, the destination switch), so a graph with a
   batched matrix product or a residual add does not load even before kinds 6 and 7 are considered.
3. **Kind 6 states have a fixed grammar.** The state is `0x08010e5b, v`, then `n` pairs of `0x08010e5c, a` and
   `0x08010e5d, b` (4 to 9 pairs in the 94 operations of L0: 37, 30, 16, 10 and 1 operations with 4, 5, 6, 8 and 9 pairs), then the tail `0x08010e4c, c, 0x08010e5a, d, 0x08010428, 0,
   0x08010429, 0` that kind 2 uses, where the word after `0x08010e5a` is the command patch (descriptor
   address). The pairs are the stream-processor program (instruction words); `0x0e5b` precedes them. No other
   register appears. The value after `0x08010e4c` is `0x04b3f040` in every operation whose descriptor has a second input (17 of kind
   2 and 11 of kind 6) and `0x04b3e040` in every other operation of kinds 2, 6, 7 and 8. All 94 operations
   satisfy this grammar.
4. **Patch source 6 (dynamic input) appears in 10 kind 6 operations**, at descriptor destinations 24 and 136,
   with source offsets 0 and 352 and table indexes 0 to 9. The 7,040-byte dynamic-input section is 20 records of
   352 bytes. The assembler has no allocation for this section, so it needs a new read-only buffer filled from
   the model.
5. **Shader operations carry 576 to 816 bytes of load-state packets** with at least 14 distinct register
   addresses (`0x5580`, `0x0e02`, `0x4d800` to `0x4d82c`), pointers to VIP SRAM and coefficients inside the
   stream, and the shader program in the instruction section. `nbg_sh_nt.c` admits one fixed 3,976-byte graph
   by exact comparison of every state, patch and the program.

## Admission rules, field by field

Rule 1 applies to every kind: a patch is admitted only when its destination is a field the loader names, its
source is a region the job maps, and its relocated address plus the extent the field implies lies inside that
region. A graph that carries a patch the loader does not name is refused.

| Item | Rule | Basis |
| --- | --- | --- |
| Kind 7 | Same state bytes, patch set and ring checks as kind 2 | Observed identical wire form (finding 1); LUT coefficient through source 5 as kind 2 |
| Kind 2 second input (136, 140) | Admit as an input pointer with its end pointer (source 8 only), and require the `0x0e4c` value `0x04b3f040` exactly when 136 is patched; the end must equal the start plus the ring size of the descriptor word that names it, as for the first input | Mesa documents the first input ring; the second-input words are observed, not documented |
| Kind 6 state | Exact grammar of finding 3; every packet header is one of the seven listed; `0x0e5c` and `0x0e5d` alternate; at most 16 pairs (a bound of this loader, not a hardware limit); the tail is the kind 2 tail; one command patch, at the word after `0x08010e5a`; no patch falls inside the program words | Observed grammar; the program words cannot be interpreted (security limit 1) |
| Source 6 | New read-only buffer of the dynamic-input section size; offset plus 352 bytes must lie inside it; index below the record count | Finding 4; vendor source enumeration |
| Kind 1 (shader) | Not admitted by a general rule. A shader graph is admitted only through an exact-artifact template of its states, patches and program, as `nbg_sh_nt.c` does, and the template lists every destination offset of the packet stream that a patch fills | No documented register or program format (security limit 2) |

## Security limits that cannot be closed from the available sources

1. **The stream-processor program is uninterpreted.** The loader can ensure that the only registers a state
   writes are the seven listed, and that every address the descriptor holds is relocated. It cannot ensure what
   a program word makes the engine do with those addresses. Containment rests on the NPU page tables (the job's
   buffers only) and on the digest of the whole graph in the session catalogue. A program that reads or writes
   VIP SRAM outside the job's use is not detectable. VIP SRAM at 0x400000 is addressed physically, and every
   operation of a layer graph uses it (no memory pool).
2. **Shader state streams are uninterpreted.** Fourteen or more register addresses, constants and pointer words
   are accepted only by exact match of an authenticated artifact. A new shader graph needs a reviewed template.
3. **Extents of linear accesses are unresolved** for weights, second inputs and dynamic inputs, as in the
   current loader (comment before `r.referenced_bytes = 1`). Only the relocated base and the ring sizes are
   checked.
4. **No isolation between slots.** The NPU page-table flush between jobs is unqualified (docs/designs/
   npu-breadth.md), so a job could read the previous job's data in the shared SRAM.

## Consequence for the board plan

The three changes in the study are not enough. Loading layer 0 needs, in this order: second-input fields of
kind 2, kind 7, the kind 6 grammar, the dynamic-input buffer, a shader template for the three shader operations,
and the catalogue support for dynamic-fixed-point 16-bit tensors. Each change needs host cases against the
compiled graphs and refusal cases, and the exact-artifact basis for the shader operations means that the layer
graphs of the 16 layers need either one template per distinct shader program or a reviewed rule. The study
reports the three shader operations of each layer as the same kind of operation; their program sections should
be compared across the layers before the number of templates is known.
