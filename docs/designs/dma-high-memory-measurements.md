# Measurements: camera and NVMe rules for memory above 4 GiB

These measurements compare the camera and NVMe address rules with the
integrated tree they apply to, against the budgets in the
[design](dma-high-memory.md). The rules are recorded in
[docs/dma-address-width.md](../dma-address-width.md). Every figure comes from
a host model, a freestanding object file or a target image build. No board was
used, so no figure below is qualified on hardware.

## Inputs

- **Before:** commit `dfa5174`, the latest integrated tree, before the address
  rules. Its C sources, headers and checks are the same as those of
  `745a6ec`, the baseline named in the design; the two commits differ only in
  project documents.
- **After:** `dfa5174` with the address rules merged in: the fixed
  camera IOVAs, the NVMe address refusal and the named limits.
- **Object files:** each tree's `a733_camera_capture.c`,
  `a733_camera_native.c` (with and without `SUNSTONE_CAMERA_MODEL`),
  `a733_pcie_nvme.c` and `a733_xhci.c`, compiled by `clang-21
  --target=aarch64-none-elf -O2 -march=armv8-a -mstrict-align
  -mgeneral-regs-only -ffreestanding -fno-builtin -mno-outline-atomics` with
  the image's include paths. Function sizes come from `llvm-nm-21`.
  Instructions are compared with `llvm-objdump-21 -d`, after each branch
  target's absolute address is replaced by its offset inside the function.
- **Images:** `sunstone-a733-camera`, `sunstone-a733-pcie-nvme` and
  `sunstone-a733-board`, configured with the project toolchain,
  `SUNSTONE_BOARD_SUPERVISED=ON` and `SUNSTONE_A733_PCIE_NVME=ON`.
- **Camera model:** the after tree's `checks/a733-camera-native.c`, linked once
  with each tree's drivers. Given a file name as a second argument, the model
  writes the address and value of every modeled register write. To count IOMMU
  windows in the before tree, where an allocation can need two, a copy of the
  model prints the window and leaf counts in place of asserting them. A second
  copy maps the camera pool at physical and CPU address `0x483f_0000`, so that
  the allocation crosses a 4 MiB boundary.

Builds, scripts and raw records are under
`~/.cache/sunstoneos/build/dma-high-memory-measure/`: `base-src/` and
`build-base/` hold the before tree, `head-src/` and `build-head/` the after
tree. `tools/objsize.sh SRC OUT` builds the object files,
`tools/compare.py BASE CHANGE` compares them, `tools/camera-model.sh SRC CHECK
OUT MODES` runs the camera model and `tools/elfsyms.sh BUILD OUT` lists image
symbol sizes.

## Results

| Metric | Budget | Before | After | Within budget | Note |
| --- | --- | --- | --- | --- | --- |
| DRAM below 4 GiB that width rules require | 184,320 bytes, plus one descriptor page per DMA0 job table | 847,872 bytes, plus one descriptor page per DMA0 job table | 184,320 bytes, plus one descriptor page per DMA0 job table | Yes | Sum printed by `checks/dma-address-width.c`: xHCI 131,072, NVMe 24,576 and endpoint test 28,672 bytes. The 663,552-byte camera allocation is no longer counted. |
| Master paths that can reach a wrong address with no error | Zero | One: NVMe | Zero | Yes | The NVMe model refuses a pool that ends one page above 4 GiB with no configuration or BAR access and an unchanged pool, and admits one that ends at 4 GiB with zero high halves. |
| IOMMU 4 MiB windows used by the camera | One at any placement | One at `0x4800_0000`; two at `0x483f_0000` | One at `0x4800_0000`, `0x483f_0000` and `0x1_8000_0000` | Yes | Counted from nonzero directory words in every model case that builds tables. |
| IOMMU leaf words for the camera | 154 | 154 | 154 | Yes | Two parameter pages and 76 pages per plane. |
| Modeled register writes of one capture | Same count and addresses; only three address values differ | 2,348 | 2,348, same addresses in the same order | Yes | The three values are listed below the table. Every other model case also writes the same count before and after. |
| Modeled register writes of three frames with two rearms | Same count and addresses; only three address values differ | 2,532 | 2,532, same addresses in the same order | Yes | The rearms write identical values; only the three addresses set before the first frame differ. |
| Copies per byte: camera readout, NVMe read | One and one | One and one | One and one | Yes | `a733_camera_native_copy`, `a733_pcie_nvme_copy` and `a733_pcie_nvme_read` have the same instructions; the `memcpy` and `memset` call counts in each object are unchanged. |
| Barriers in the four objects | Unchanged | 26 in the native camera, none in the others | Same | Yes | No MMIO sequence was added or reordered. |
| Capture constructor, xHCI capability check | Identical instructions | 1,920 and 532 bytes | Identical instructions | Yes | Every function in both objects is identical. |
| `a733_pcie_nvme_init` | At most 16 bytes larger | 804 bytes | 816 bytes | Yes | Three instructions, run once per controller initialization: load the constant `2^32 - 0x5fff`, compare the pool address with it, branch to the refusal. |
| NVMe poll, read, copy, write rule and command builder | Identical instructions | 520, 120, 308, 732 and 376 bytes | Identical instructions | Yes | These run once per 4 KiB command. |
| Native camera constructor `initialize` | At most 32 bytes larger | 2,020 bytes | 2,052 bytes | Yes, at the limit | Eight instructions, run once per camera start. The range check is the same length as the identity and 4 GiB checks it replaces; the growth comes from building each span's device address and physical address separately, where one value served both before. |
| Native camera route and DMA admission | No larger | 188 and 288 bytes | 184 and 284 bytes | Yes | Each compares with a constant IOVA instead of a sum computed from the lease, one instruction fewer. |
| `.text` of the camera, NVMe and board images | No more than the function growth above | 205,208; 548,676; 498,736 bytes | 205,232; 548,688; 498,736 bytes | Yes | The camera image grows by 24 bytes (32 − 4 − 4) and the NVMe image by 12; data and BSS are unchanged. |

The three register values that differ in the capture trace are the route
parameter address (`0x05910050`), from `0x12002000` to `0x04000000`, and the
two plane addresses (`0x05830220`, `0x05830228`), from `0x12002800` and
`0x12015800` to `0x04000800` and `0x04013800`. Each register holds the device
address shifted right by two, so the values are the old physical addresses and
the new IOVAs. With the allocation at `0x1_8000_0000`, the same trace differs
only in the IOMMU directory base of both banks (`0x03900020`, `0x03910020`),
which holds `0x60000`, the table's physical address shifted right by 14.

Other native camera functions have the same sizes before and after. Their
instructions differ only in the source line number passed to the fault
handler (101 before, 109 after) and in the displayed target of `adrp`
relocations, which move with the larger constructor.

The NVMe check compares the physical address with the constant six-page size,
`a733_extent_within(0, DMA_ADDRESS_32BIT_LIMIT, physical,
A733_PCIE_NVME_POOL_BYTES)`. The controller addresses only those six pages.

## Host checks

Each check passes on the after tree:

- `checks/a733-camera-native.py`, cases 0 to 20. Case 19 captures and copies
  an image from an allocation at physical `0x1_8000_0000`. Case 20 places the
  allocation across 2^34 and requires refusal with no fill, no register access
  and the extent still device owned. Against the before drivers, case 19 fails
  at constructor admission, because they refuse physical addresses at or above
  4 GiB; the pool mapped at `0x483f_0000` needs two IOMMU windows.
- `checks/a733-pcie-trace.py --baseline-rev dfa5174`: the core, endpoint-test
  and NVMe model cases, including the pool at 4 GiB and the pool across 4 GiB,
  and Gen1 write traces identical to the before tree. With the driver's new
  check removed, the case across 4 GiB fails.
- `checks/dma-address-width.c`, built as
  [docs/dma-address-width.md](../dma-address-width.md) describes.
- `checks/a733-camera-capture.py` and `checks/a733-xhci-detach.py`.
- `checks/camera-supervised.py`, which builds the supervised camera image,
  compiles the capture and DMA compatibility units and runs the supervised
  native constructor from `checks/camera-supervised-native.c`.

The camera, NVMe and board images build in both trees with the same compiler
warnings, none of them in a changed file.
