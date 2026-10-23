# Measurements: camera and NVMe rules for memory above 4 GiB

These measurements compare the camera and NVMe address rules with their parent
commit, against the budgets in the [design](dma-high-memory.md). The rules
are recorded in [docs/dma-address-width.md](../dma-address-width.md). Every
figure comes from a host model, a freestanding object file or a target build.
No board was used.

## Inputs

- **Baseline:** commit `745a6ec`, exported with `git archive`.
- **Change:** the working tree of the commit that adds this record.
- **Object files:** each tree's `a733_camera_capture.c`, `a733_camera_native.c`
  (with and without `SUNSTONE_CAMERA_MODEL`), `a733_pcie_nvme.c` and
  `a733_xhci.c`, compiled by `clang-21 --target=aarch64-none-elf -O2
  -march=armv8-a -mstrict-align -mgeneral-regs-only -ffreestanding
  -fno-builtin -mno-outline-atomics` with the image's include paths. Function
  sizes come from `llvm-nm-21`. Instructions are compared with
  `llvm-objdump-21 -d`, after replacing each branch target's absolute address
  by its offset inside the function.
- **Images:** `sunstone-a733-camera`, `sunstone-a733-pcie-nvme` and
  `sunstone-a733-board`, configured with the project toolchain,
  `SUNSTONE_BOARD_SUPERVISED=ON` and `SUNSTONE_A733_PCIE_NVME=ON`. The change
  also builds `sunstone-a733-camera` with `SUNSTONE_CAMERA_SUPERVISED=ON`.
- **Camera model:** the change's `checks/a733-camera-native.c`, linked once
  with each tree's drivers. Given a file name as a second argument, it writes
  the address and value of every modeled register write.

Builds, scripts and raw records are under
`~/.cache/sunstoneos/build/dma-high-memory/`: `base/` holds the baseline and
`change/` the change. `tools/objsize.sh SRC OUT` builds the object files,
`tools/compare.py BASE CHANGE` compares them, `tools/camera-model.sh SRC CHECK
OUT MODES` runs the camera model and `tools/elfsyms.sh BUILD OUT` lists image
symbol sizes.

## Results

| Metric | Budget | Baseline | Change | Within budget |
| --- | --- | --- | --- | --- |
| DRAM below 4 GiB that width rules require | 184,320 bytes, plus one descriptor page per DMA0 job table | 847,872 bytes, plus one descriptor page per DMA0 job table | 184,320 bytes, plus one descriptor page per DMA0 job table | Yes |
| Master paths that can reach a wrong address with no error | Zero | One: NVMe | Zero | Yes |
| IOMMU 4 MiB windows used by the camera | One at any placement | One at physical `0x4800_0000`; two at `0x483f_0000`, where the allocation crosses a 4 MiB boundary | One at physical `0x4800_0000` and at `0x1_8000_0000` | Yes |
| IOMMU leaf words for the camera | 154 | 154 | 154 | Yes |
| Modeled register writes of one camera capture | Same count and addresses; only three address values differ | 2,348 | 2,348 at the same addresses in the same order | Yes |
| Copies per byte: camera readout, NVMe read | One and one | One and one | One and one | Yes |
| Capture constructor, xHCI capability check | Identical instructions | 1,920 and 532 bytes | Identical instructions | Yes |
| `a733_pcie_nvme_init` | At most 16 bytes larger | 804 bytes | 816 bytes | Yes |
| NVMe poll, read, copy, write rule and command builder | Identical instructions | 520, 120, 308, 732 and 376 bytes | Identical instructions | Yes |
| Native camera constructor | At most 32 bytes larger | 2,020 bytes | 2,052 bytes | Yes |
| Native camera route and DMA admission | No larger | 188 and 288 bytes | 184 and 284 bytes | Yes |
| `.text` of the camera, NVMe and board images | No budget | 205,208; 548,676; 498,736 bytes | 205,232; 548,688; 498,736 bytes | Not budgeted |

The three register values that differ in the capture trace are the route
parameter address (`0x05910050`), from `0x12002000` to `0x04000000`, and the
two plane addresses (`0x05830220`, `0x05830228`), from `0x12002800` and
`0x12015800` to `0x04000800` and `0x04013800`. Each register holds the device
address shifted right by two, so the values are the old physical addresses and
the new IOVAs. With the allocation at `0x1_8000_0000`, the same trace differs
only in the IOMMU directory base of both banks (`0x03900020`, `0x03910020`),
which holds `0x60000`, the table's physical address shifted right by 14.

The NVMe check compares the physical address with the constant six-page size,
`a733_extent_within(0, DMA_ADDRESS_32BIT_LIMIT, physical,
A733_PCIE_NVME_POOL_BYTES)`. The controller addresses only those six pages.
Comparing against the extent's recorded size instead adds 28 bytes, over the
budget, because the size is then loaded from memory and subtracted at run time.

## Host checks

Each check passes on the change:

- `checks/a733-camera-native.py`, cases 0 to 20. Case 19 captures and copies
  an image from an allocation at physical `0x1_8000_0000`. Case 20 places the
  allocation across 2^34 and requires refusal with no fill, no register access
  and the extent still device owned. Against the baseline drivers, cases 0 to
  18 and 20 pass and case 19 fails, because the baseline refuses physical
  addresses at or above 4 GiB. A copy of the fixture that maps its pool at
  `0x483f_0000` fails against the baseline drivers with two IOMMU windows.
- `checks/a733-pcie-trace.py --baseline-rev 745a6ec`: the core, endpoint-test
  and 26 NVMe model cases, and Gen1 write traces identical to the baseline.
  The case that ends the NVMe pool one page above 4 GiB fails when the driver's
  new check is removed.
- `checks/dma-address-width.c`, `checks/a733-camera-capture.py`,
  `checks/a733-xhci-detach.py`, and the supervised native constructor from
  `checks/camera-supervised-native.c`, with the capture and DMA compatibility
  units compiled.

The camera, NVMe and board images, and the supervised camera image, build
without new warnings.
