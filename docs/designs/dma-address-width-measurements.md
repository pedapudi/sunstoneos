# Measurements: DMA address-width rules

These measurements compare the DMA address-width change with its parent
against the budgets in [dma-address-width.md](dma-address-width.md) and the
DMA defaults of the performance architecture. Every figure comes from a host
model, AArch64 instruction emulation or the object files of a target build.
No board was used.

## Inputs

- **Baseline:** commit `3d576cb`, the integration commit that the change
  branches from.
- **Change:** commit `4b1fce4`.
- **Builds:** each commit was exported with `git archive` and configured with
  the project toolchain (clang, `cmake/aarch64-clang.cmake`). The targets were
  `dma-service-model`, `dma-native-model` and `i2s-stream-model` in the default
  configuration, `sunstone-a733-hdmi` with `SUNSTONE_HDMI_FRAMEBUFFER=ON`
  (links the IOMMU driver), and `sunstone-a733-hdmi` with
  `SUNSTONE_HDMI_AUDIO=ON` (links the DMA0 controller, descriptor builder and
  DMA memory policy).
- **Instruction emulation:** a freestanding harness links
  `a733_dma_controller.c`, `a733_dma.c` and `a733_iommu.c` from each commit
  with the image's C flags (`-O2 -march=armv8-a -mstrict-align
  -mgeneral-regs-only -ffreestanding -fno-builtin`). Unicorn 2.1.4 runs one
  call per scenario and counts every executed AArch64 instruction, callbacks
  included. The callbacks count register reads and writes, barriers, cache
  operations and validation calls. The `submit` code in the harness matches
  the code in the audio image's object, apart from addresses.
- **Host checks:** `checks/dma-controller.c`, `checks/a733-iommu.c` and
  `checks/dma-address-width.c`, built with AddressSanitizer and
  UndefinedBehaviorSanitizer. All pass on the change. The address-width check
  also passes against the baseline IOMMU driver, with the new header supplied
  separately.

Builds, harness, scripts and raw records are under
`~/.cache/sunstoneos/build/dma-address-width-measure/`.

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
| AArch64 instructions per successful submit: copy, four-segment copy, FIFO | No budget; must not be worse than baseline | 1,148; 2,822; 1,017 | 1,155; 2,829; 1,024 | No: seven more in each |
| AArch64 instructions per refused submit: bad chain, bad chain with busy channel | Design accepts a later refusal | 136 (`INVALID`); 136 (`INVALID`) | 744 (`INVALID`); 701 (`RECOVERY`) | Yes, documented |
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
| Code size, `a733_dma_controller.c` `.text` | No budget | 7,248 bytes | 7,216 bytes | Yes |
| Code size, `a733_dma.c` and `dma_memory.c` `.text` | No budget | 1,908 and 1,588 bytes | Identical instructions | Yes |
| Code size, `a733_iommu.c` `.text` | No budget | 6,516 bytes | 6,504 bytes | Yes |
| Image `.text`, `.rodata`, `.data`, `.bss` | No budget | HDMI audio image: 186,608 `.text` | 186,576 `.text`; other sections identical in every image | Yes |

## Findings

**Successful submits execute seven more instructions.** The change removes
ten executed instructions from `validate_transfer`: the two descriptor-limit
compares and their loads. With fewer live values early in `submit`, the
compiler no longer keeps the base of `c->channels` in a callee-saved register
during the conflict scan over the 16 channels. The scan now reloads that base
from the stack once per channel:

```
mul   x9, x23, x8
ldur  x8, [x29, #-0x18]   // added: reload c->channels base
ldr   x19, [x8, x9]
```

The scan runs on every finite and cyclic submit, so it adds 16 loads, and
the net change is seven more instructions per submit, about 0.6 % of a
one-segment copy. The same reload appears in the `submit` code of the audio
image's object file. The design does not record this cost. It does not
affect correctness, bus traffic, barriers or wake-ups. Restoring the baseline
count would need a source change that keeps the scan's base in a register,
for example a local pointer to `c->channels` taken before the scan. Any such
change must be re-measured, because register allocation decides the result.

**Refused bad chains cost more and can enter recovery.** A chain that crosses
the end-of-chain marker is now refused in `a733_dma_build` instead of at the
start of `validate_transfer`. The refusal runs after mapping validation and
the quiet-channel status reads. It still precedes every descriptor byte,
cache operation and register write. On a channel that is not quiet, the
result is `A733_DMA_RECOVERY` where the baseline returned `A733_DMA_INVALID`.
The design states both effects. The baseline drivers fail the new
`descriptor_limit` case in `checks/dma-controller.c` only at that recovery
assertion.

**Two targets depend on consumer changes.** The NVMe refusal and the camera's
fixed IOVAs are listed in the design as changes for the PCIe and camera
owners. Until they land, the NVMe path can still reach a wrong address
without an error, and 847,872 bytes below 4 GiB remain required.
