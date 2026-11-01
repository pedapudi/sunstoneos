# Admit the complete memory-test map before writing RAM

Status: implemented and host-verified; physical qualification remains open.
Admission inspects every block descriptor covering the 12,224 MiB tested map
before the first write outside the protected 64 MiB window. The previous
three-address check could miss a missing, aliased or user-accessible block.

The existing image uses a 39-bit virtual address space, 4 KiB table pages,
1 GiB and 2 MiB blocks, and identity mappings. The checker requires that
configuration, enabled translation and caches, and the existing memory attributes.
Every tested block must be privileged read/write, Normal write-back and inner
shareable. Execute permission remains permitted because the existing image maps
the lowest 128 MiB with executable blocks. This check does not qualify execution
fault handling or cache visibility on hardware.

Table reads stay inside the protected window. A table descriptor outside that
window, an unsupported descriptor type or an unexpected flag causes the existing
mapping refusal. Before dereferencing a table page, the native reader verifies
its identity translation and memory attributes with the architectural translation
instruction. It reuses that check for consecutive reads of the same page.
Tables and their CPU mappings remain private and immutable during the invocation.
Admission keeps the three architectural write-translation probes as well.

The checker runs once per image invocation. It reads each relevant level-1 entry
once and each relevant level-2 entry once, skips an entire verified block, and
performs no cache maintenance, copying, allocation or MMIO. The production map
needs 12 level-1 reads and 480 level-2 reads; the current baseline makes no
software table reads. The production walk also adds three read translations:
the root page, its child page and the root page again.
The added work replaces an incomplete coverage assumption.
There is no per-word or per-page admission work.

Cache-size admission reads only data or unified levels listed in CLIDR_EL1.
Selecting an absent cache level gives an architecturally unspecified CCSIDR_EL1
value. The three-level model now makes three cache-geometry reads, reduced from
seven, and retains the same 2,392,064-byte cache total.

Host checks independently corrupt every tested descriptor, including blocks
above 4 GiB, and check identity, permissions, attributes, table bounds and atomic
refusal. Instruction emulation enters the actual image admission and verifies
that refusal occurs before a tested-memory write. Existing planner, image-layout
and kernel checks remain applicable. Physical memory patterns, permission faults,
temperature coverage and DMA visibility remain separate board requirements.
Mapping refusal returns the checksummed record and acquires no external resource.

## Host evidence and remaining board work

The [focused check](../../checks/memtest-map-admission.py) rejects 8,347 leaf
corruptions and confirms 492 reads for the complete production map. Eight native
cases refuse before any tested-memory write: an output alias, EL0 access,
read-only memory, a missing access flag, Device memory, an unprotected child
table, disabled table walks and a CPU alias of a table page.

The existing planner and ELF-layout checks pass. The native run, remote-writer
and host round-trip cases also pass, including the three-level cache total.
The reduced-map fixture needs two table-page read translations and preserves
the three write translations. Independent design and code review found no
blocking issues.

Raw inputs, build logs and results are under
`~/.cache/sunstoneos/build/memory-dma-1031/`. The frozen ELF and binary, their
SHA-256 values, linked-input inventory and coordinator procedure are in
`images/memtest/` beneath that directory. The image still uses the legacy
secure-timer monitor. Its owner must supply the WDT0 contract, breadcrumbs and
matching development runner before a board launch. The current development
runner has no memory-test kind.
