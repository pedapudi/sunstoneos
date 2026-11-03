# Admit the complete memory-test map before writing RAM

Status: **host-tested**; physical qualification is open. Admission inspects every block descriptor covering the 12,224 MiB tested map before the first write outside the protected 64 MiB window ([memory-qualification.md](memory-qualification.md)).

The image uses a 39-bit virtual address space, 4 KiB table pages, 1 GiB and 2 MiB blocks, and identity mappings. The checker requires that configuration, enabled translation and caches, and the existing memory attributes. Every tested block must be privileged read/write, Normal write-back and inner shareable. Execute permission is permitted because the image maps the lowest 128 MiB with executable blocks. The check does not qualify execution fault handling or cache visibility on hardware.

Table reads stay inside the protected window. A table descriptor outside that window, an unsupported descriptor type or an unexpected flag causes the mapping refusal. Before dereferencing a table page, the native reader verifies its identity translation and memory attributes with the architectural translation instruction, and reuses that check for consecutive reads of the same page. Tables and their CPU mappings remain private and immutable during the invocation. Admission also keeps the three architectural write-translation probes.

The checker runs once per image invocation. It reads each relevant level-1 entry once and each relevant level-2 entry once, skips an entire verified block, and performs no cache maintenance, copying, allocation or MMIO. There is no per-word or per-page admission work. Mapping refusal returns the checksummed record and acquires no external resource.

Cache-size admission reads only data or unified levels listed in `CLIDR_EL1`, because selecting an absent cache level gives an architecturally unspecified `CCSIDR_EL1` value.

## Qualified figures

Host and instruction-emulation figures only (record `memtest-map-admission-host` in [the evidence file](../evidence/memtest-map-admission.jsonl)).

| Figure | Value |
| --- | --- |
| Table reads for the production map | 12 level-1 and 480 level-2 reads |
| Added read translations | 3 (the root page, its child page, the root page again) |
| Cache-geometry reads (three-level model) | 3, with a cache total of 2,392,064 bytes |
| Leaf corruptions rejected by [the focused check](../../checks/memtest-map-admission.py) | 8,347, including blocks above 4 GiB |
| Native cases refused before any tested-memory write | 8: output alias, EL0 access, read-only memory, missing access flag, Device memory, unprotected child table, disabled table walks, CPU alias of a table page |
| Reduced-map fixture | 2 table-page read translations, 3 write translations |

## Checks

The host checks independently corrupt every tested descriptor and check identity, permissions, attributes, table bounds and atomic refusal. Instruction emulation enters the actual image admission and verifies that refusal occurs before a tested-memory write. The planner, ELF-layout, native run, remote-writer and host round-trip cases pass.

## Open work

1. Physical memory patterns, permission faults, temperature coverage and DMA visibility are board requirements.
2. The image uses the legacy secure-timer monitor. Its owner supplies the WDT0 contract, breadcrumbs and a development runner with a memory-test kind before a board launch.
