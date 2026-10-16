# Reserved NPU memory and cache publication

The NPU memory policy is **implemented** and passes a host check with separate
CPU and device memory. It compiles for AArch64. The [native adapter](npu-native.md)
also passes a FreeRTOS emulator check. Production service serialization,
board integration, and physical memory transfers remain
unimplemented or untested. The policy performs no MMIO and is excluded from
default builds.

The [interface](../include/a733_npu_memory.h) binds private device page tables to
reserved command, input, output, weight, and scratch allocations. The
[implementation](../drivers/npu/a733_npu_memory.c) supplies memory publication
and completion operations for the [controller core](npu-controller.md).
Publication makes CPU-written data visible to the device before submission.
Completion makes device-written data visible to the CPU after the controller
confirms completion and bus idle.

## Admission binds CPU, physical, and device addresses

Initialization copies up to 64 buffer descriptions and sorts them by device
address. Each description includes CPU and physical bases, a device virtual
base, length, and role. Exactly one bootstrap command allocation is required.
Other roles are application commands, device-readable data, and
device-readable/writable data.

Buffer bases and lengths must align to 4 KiB. Device addresses fit 32 bits;
physical addresses fit 40 bits. CPU, physical, and device ranges must each be
disjoint, including read-only aliases. Command allocations cover the controller's
full 524,280-byte prefetch span. Device address zero and the internal-SRAM
window at `0x400000..0x47ffff` remain unmapped.

The caller supplies dedicated, pinned RAM with stable mappings and no aliases
outside the registry. Initialization checks every covered CPU page against its
declared physical address. The translation callback must verify read permission,
Normal write-back memory attributes (`0xff`), and inner shareability (`3`).
Tables and device-written buffers also require CPU write permission.
These checks cannot establish that memory belongs exclusively to the caller;
the platform must enforce allocation ownership.

The private [MMU builder](npu-mmu.md) creates 4 KiB device mappings from the copied
buffer descriptions. The table arena is excluded from payload mappings, including
unused arena capacity. Initialization checks metadata overlap and all mappings
before building tables. Refusal leaves the memory object and arena unchanged.

The broader MMU builder supports larger pages and read-only physical aliases.
This publication policy currently accepts only disjoint buffers with 4 KiB pages.
Internal SRAM translation requires separate implementation.

## Publication and completion retain ownership on failure

Command lookup returns the registered allocation's device base, full readable
length, and a token identifying its private description. Publication checks
that token and the bootstrap or application-command role. It rejects substituted
addresses, lengths, tokens, and MMU-image descriptions.

Before cache maintenance, publication rechecks all CPU translations, including
the entire reserved table capacity. It then walks the CPU table image and
compares every used entry and padding byte against the private registry.
The walk detects extra mappings, altered permissions or addresses, incorrect
root pointers, and modified absent entries. It does not rely on a checksum.

Publication cleans the table image and every device-readable allocation.
It cleans and invalidates every device-written allocation. The cache callback
must complete maintenance to the point of coherency, where CPU and device
accesses observe the same memory, and use a supported maintenance stride.
A final barrier orders publication before controller submission.

After the controller proves an event or cancellation acknowledgment and full
bus idle, completion rechecks the live CPU mappings. It invalidates every
device-written allocation and orders CPU visibility. The controller still
requires result consumption before releasing a job; the memory policy's IDLE
state alone does not authorize reuse.

A validation or cache-operation failure latches a memory fault. Subsequent
publication, completion, command lookup, and initialization calls refuse the
instance without invoking callbacks. Partial cache maintenance retains the
entire session. External recovery must prove device quiescence before disposal.

The memory object, tables, command streams, and allocation descriptions remain
private and immutable through successful controller close. All buffers remain
reserved for that session. Payload bytes may change only when controller
ownership permits CPU access. Callers must serialize operations and prevent
other CPU or device access during publication and execution.

The policy validates memory relationships, not command semantics. A native model
loader and privileged service must still validate commands, relocations,
register permissions, and termination. The native adapter supplies CPU
translation/cache callbacks and bounded timekeeping. FreeRTOS service ownership,
power/clocks, and watchdog recovery remain integration requirements.

## Focused evidence

The [host check](../checks/npu-memory.c) connects the real controller core,
MMU builder, and memory policy to a register model with separate CPU/device
copies. An independent device table walker resolves the published mappings.
The modeled operation copies and transforms bytes; it does not emulate NPU
commands or establish numerical inference support.

The check covers:

- Bootstrap and application submission, event arrival before bus idle, output
  visibility after idle, result consumption, shutdown, and neighboring guards.
- Physical addresses above 4 GiB, directory crossings, the final virtual page,
  read-only commands, unmapped pages, and disposal of caller metadata.
- Changed CPU mappings, missing permissions, wrong memory attributes,
  substituted commands or images, and corrupted table entries or padding.
- Failure at each publication cache operation, failed completion maintenance,
  retained faults, and refusal without further callbacks.
- Atomic admission refusal for aliases, SRAM overlap, address overflow,
  invalid roles, insufficient command/table capacity, and invalid alignment.

One AddressSanitizer/UndefinedBehaviorSanitizer host run passed, followed by one
AArch64 target compile. No physical device request occurred. Unchanged DMA,
mimus, model-inspection, and standalone MMU/controller evidence remains reusable.
Local logs, commands, input identities, and artifacts are under
`~/.cache/sunstoneos/evidence/npu-memory-20261016/`.

The AArch64 object has SHA-256
`4cbe693d63ffea31b4adae64c5615f277a0756bcf5c982824345945aafda1494`.
It contains 3,568 bytes of code and constants and no static data. It references
the MMU builder, `memcpy`, and `memset`. These are object measurements;
production service memory use and publication latency remain unmeasured.

Reproduce the focused host check with:

```sh
clang-21 -std=c11 -O1 -g -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  checks/npu-memory.c drivers/npu/a733_npu_memory.c \
  drivers/npu/a733_npu_mmu.c drivers/npu/a733_npu_controller.c \
  -o /tmp/npu-memory-check
/tmp/npu-memory-check
```

After configuring the AArch64 toolchain as described in [build.md](build.md),
compile the isolated object with:

```sh
cmake -S . -B "$HOME/.cache/sunstoneos/build/qemu-virt"
cmake --build "$HOME/.cache/sunstoneos/build/qemu-virt" --target a733-npu-memory
```
