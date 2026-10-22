# Measurements: DMA address-width rules

These measurements compare the DMA address-width change with its parent
against the budgets that the change's design set and the DMA defaults of the
performance architecture. The rules themselves are recorded in
[docs/dma-address-width.md](../dma-address-width.md). Every figure comes from
a host model, AArch64 instruction emulation or the object files of a target
build. No board was used.

## Inputs

- **Baseline:** commit `3d576cb`, the integration commit that the change
  branches from.
- **Change:** commit `62c62db`. It contains the address-width rules
  (`4b1fce4`) and a rewrite of the DMA0 conflict scan that walks the channel
  array by pointer.
- **Builds:** the baseline was exported with `git archive` and the change was
  built from a clean checkout. Both were configured with the project toolchain
  (clang, `cmake/aarch64-clang.cmake`). The targets were `dma-service-model`,
  `dma-native-model` and `i2s-stream-model` in the default configuration,
  `sunstone-a733-hdmi` with `SUNSTONE_HDMI_FRAMEBUFFER=ON` (links the IOMMU
  driver), and `sunstone-a733-hdmi` with `SUNSTONE_HDMI_AUDIO=ON` (links the
  DMA0 controller, descriptor builder and DMA memory policy).
- **Instruction emulation:** a freestanding harness links
  `a733_dma_controller.c`, `a733_dma.c` and `a733_iommu.c` from each commit
  with the image's C flags (`-O2 -march=armv8-a -mstrict-align
  -mgeneral-regs-only -ffreestanding -fno-builtin`). Unicorn 2.1.4 runs one
  call per scenario and counts every executed AArch64 instruction, callbacks
  included. The callbacks count register reads and writes, barriers, cache
  operations and validation calls. The `submit` code in the harness has the
  same instructions as the code in the audio image's object; only addresses
  and the order of two prologue instructions differ.
- **Host checks:** `checks/dma-descriptors.c`, `checks/dma-cyclic.c`,
  `checks/dma-controller.c`, `checks/dma-memory.c`, `checks/a733-iommu.c`,
  `checks/iommu-camera-supervised.c`, `checks/dma-address-width.c` and
  `checks/uart-dma-conflict.c`, built with AddressSanitizer and
  UndefinedBehaviorSanitizer. All pass on the change. The address-width check
  also passes against the baseline IOMMU driver, with the new header supplied
  separately.

Builds, harness, scripts and raw records are under
`~/.cache/sunstoneos/build/dma-address-width-measure/`. The change's harness,
audio image and check records are in its `fix/` directory, and
`harness/build.sh SRC OUT` builds the harness from a source tree.

## Scenarios

The successful scenarios each submit a finite transfer on a claimed, quiet
channel 0, with no other channel active:

- **Copy:** one 4 KiB memory-to-memory segment, one source and one
  destination mapping.
- **Four-segment copy:** four 4 KiB memory-to-memory segments over the same
  two mappings.
- **FIFO:** one 4 KiB memory-to-FIFO segment and one source mapping.

The refused scenarios place the descriptor mapping at bus `0xfffff7c0` with a
two-descriptor chain, so the second descriptor crosses the end-of-chain
marker `0xfffff800`:

- **Bad chain:** the channel is quiet.
- **Bad chain, busy channel:** the channel's mode register is nonzero, so the
  channel is not quiet.

The IOMMU scenario validates a table that maps one 307,200-byte plane at
physical `0x1_8000_0000`.

## Results

| Metric | Budget | Baseline | Change | Within budget |
| --- | --- | --- | --- | --- |
| Width checks per DMA0 submit (two compares each) | One per descriptor chain; others unchanged | Two per mapping, two per chain, two per FIFO endpoint, one per memory endpoint | Two per mapping, one per chain, two per FIFO endpoint, one per memory endpoint | Yes |
| Width checks on GPU, NPU, IOMMU or crypto job paths | Zero added | None on job paths | None added; the IOMMU changes run only in table validation and initialization | Yes |
| AArch64 instructions per successful submit: copy, four-segment copy, FIFO | No budget; must not be worse than baseline | 1,148; 2,822; 1,017 | 1,121; 2,795; 990 | Yes: 27 fewer in each |
| AArch64 instructions per refused submit: bad chain, bad chain with busy channel | Design accepts a later refusal | 136 (`INVALID`); 136 (`INVALID`) | 710 (`INVALID`); 667 (`RECOVERY`) | Yes, documented |
| Register reads per refused submit: bad chain, busy channel | Design accepts side-effect-free status reads | 0; 0 | 10; 8 | Yes, documented |
| Register writes, barriers and cache operations per successful submit | Unchanged | 3 writes, 3 barriers, 3 cache operations (copy) | Same in every successful scenario | Yes |
| Register writes, cache operations and descriptor bytes written per refused submit | Zero | Zero | Zero | Yes |
| Interrupts and task wake-ups per transfer | Unchanged | One interrupt when bound | Unchanged; no new event | Yes |
| Locks taken per submit | Unchanged | None in the controller | None | Yes |
| Copies per byte for a buffer above 4 GiB | Zero | No such buffers | Zero; no bounce path exists | Yes |
| AArch64 instructions to validate an IOMMU table for one plane | No budget | 154,159 | 154,159 | Yes |
| IOMMU leaf words per 307,200-byte plane at `0xffff_f000` and `0x1_8000_0000` | 75 | 75 and 75 | 75 and 75 | Yes |
| DRAM below 4 GiB that width rules require | 184,320 bytes once the native camera uses fixed IOVAs | 847,872 bytes plus one descriptor page per DMA0 job table | 847,872 bytes plus one descriptor page per DMA0 job table | No; the camera change belongs to its owner |
| Master paths that can reach a wrong address without an error | Zero | One: NVMe | One: NVMe | No; the NVMe refusal belongs to the PCIe owner |
| Code size, `a733_dma_controller.c` `.text` | No budget | 7,248 bytes | 7,200 bytes | Yes |
| Code size, `a733_dma.c` and `dma_memory.c` `.text` | No budget | 1,908 and 1,588 bytes | Identical instructions | Yes |
| Code size, `a733_iommu.c` `.text` | No budget | 6,516 bytes | 6,504 bytes | Yes |
| Image `.text`, `.rodata`, `.data`, `.bss` | No budget | HDMI audio image: 186,608 `.text` | 186,560 `.text`; other sections identical in every image | Yes |

## Findings

**Successful submits execute 27 fewer instructions.** All 27 are in
`submit`, which executes 442 instructions for a one-segment copy against 469
in the baseline. The conflict scan over the 16 channels accounts for 16 of
them, and the rest of `submit` for 11, mostly the descriptor-limit compares
and their loads that `validate_transfer` no longer runs. The scan now walks
`c->channels` with a pointer. Indexing the array cost a multiply per channel.
With the indexed scan and without the descriptor-limit compares, the compiler
also spilled the array base and reloaded it on every channel, so that version
executed seven more instructions per submit than the baseline. The pointer
walk removes both the multiply and the reload of the array base. The scan body
that runs for each idle channel:

| Source | Instructions per idle channel | Stack loads per idle channel |
| --- | --- | --- |
| Baseline, indexed scan | 7 | 0 |
| Address-width rules with the indexed scan | 8 | 1: the array base |
| Change, pointer walk | 6 | 1: the controller pointer |

The pointer walk still loads the controller pointer from the stack once per
channel, because `submit` keeps all ten callee-saved registers occupied. A
non-inlined scan function keeps every scan value in a register, but its call
and register saves cost more than the 16 loads. With the scan in a separate
function the three successful submits execute 1,136, 2,807 and 1,003
instructions, 15 more than the pointer walk in each. The pointer walk is
therefore the cheapest form measured. It changes no result: the scan visits
the channels in the same order and returns `A733_DMA_CONFLICT` at the first
overlap.

**Refused bad chains cost more and can enter recovery.** A chain that crosses
the end-of-chain marker is now refused in `a733_dma_build` instead of at the
start of `validate_transfer`. The refusal runs after mapping validation, the
conflict scan and the quiet-channel status reads. It still precedes every
descriptor byte, cache operation and register write. On a channel that is not
quiet, the result is `A733_DMA_RECOVERY` where the baseline returned
`A733_DMA_INVALID`.
[docs/dma-address-width.md](../dma-address-width.md) states both effects. The
baseline drivers fail the new `descriptor_limit` case in
`checks/dma-controller.c` only at that recovery assertion.

**Two targets depend on consumer changes.** The NVMe refusal and the camera's
fixed IOVAs are listed in [docs/dma-address-width.md](../dma-address-width.md)
as changes for the PCIe and camera owners. Until they land, the NVMe path can
still reach a wrong address without an error, and 847,872 bytes below 4 GiB
remain required.
