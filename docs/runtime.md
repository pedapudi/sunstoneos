# C and C++ runtime on FreeRTOS

The A733 boot CPU runs a tested C++23 runtime check using newlib and libstdc++.
Clang 21 compiles project sources. The pinned AArch64 bare-metal distribution
supplies library headers, static archives, and compiler support routines.
The executable contains no Linux runtime dependency.

## Allocation and task state

`runtime/newlib.c` supplies a bounded heap through `_sbrk`, serializes allocation
by suspending the single-core scheduler, and routes text output to the retained
RAM log. Allocation from interrupt handlers is unsupported. The current image
reserves a 4 MiB heap after its code, static data, and stacks.

FreeRTOS selects a separate newlib reentrancy structure on each task switch.
This keeps `errno` separate between tasks. The `SUNSTONE_NEWLIB` build definition
selects this behavior; the earlier static kernel and USB targets retain their
existing configuration.

Startup clears static storage and calls `runtime_init` before creating tasks.
That function invokes C++ global constructors from the linker’s initialization
array. C++ builds disable exceptions, RTTI, and guards for concurrent local-static
initialization. Required local objects must be initialized before concurrent
use. Scoped destructors and ordinary container allocation are available.
Native C++ allocation operators return null for failed `std::nothrow` requests
and enter monitor recovery on unchecked exhaustion. Applications must use
checked allocation where failure needs recovery.

The syscall adapter supplies text output and returns errors for unavailable file
operations. It does not implement a filesystem, POSIX processes, signals,
threading, or a wall clock. Concurrent stdio, multicore allocator locking,
compiler thread-local storage, and orderly process shutdown remain unqualified.
These limits are separate from the tested newlib task reentrancy.

The single-core library integration is unsuitable for concurrent execution on
multiple cores. The pinned `libc.a` references the global `_impure_ptr`, and
FreeRTOS's default `configSET_TLS_BLOCK` assigns that pointer on task switches.
Two cores would overwrite the same selection even though each task has its own
reentrancy structure. Defining dynamic reentrancy only in application headers
would not change the compiled library's accesses.
The separate [SMP runtime](smp-runtime.md) rebuilds newlib with task-state lookup
and recursive library locks. It passes concurrent allocation, memory-stream
locking, and library-state preservation across migration in QEMU. Its evidence
does not qualify the single-core archive for concurrent use.

## Build and bounded checks

Use the Clang configuration and FreeRTOS source from [build.md](build.md).
The optional C++ dependency is pinned by archive hash in
[sources.lock.json](../sources.lock.json). Fetch it once, then select only the
runtime targets:

```sh
set -e
cache="$HOME/.cache/sunstoneos"
newlib=$(python3 tools/fetch-sources.py aarch64-newlib-toolchain)
cmake -S . -B "$cache/build/qemu-virt" -DNEWLIB_ROOT="$newlib"
cmake --build "$cache/build/qemu-virt" \
  --target a733-cxx-model a733-cxx-check -j 8
llvm-objcopy -O binary "$cache/build/qemu-virt/a733-cxx-check.elf" \
  "$cache/build/qemu-virt/a733-cxx-check.bin"
```

The emulator target uses the same secure-monitor model as the kernel check:

```sh
timeout 20 "$cache/host/usr/bin/qemu-system-aarch64" \
  -machine virt,secure=on,virtualization=on,gic-version=3 \
  -cpu cortex-a55 -smp 1 -m 128M -nographic -monitor none -serial none \
  -semihosting-config enable=on,target=native \
  -device loader,file="$cache/build/qemu-virt/a733-cxx-model.elf",cpu-num=0
```

Require exit status zero and the `PASS C++ runtime:` message. For physical
validation, first satisfy the [DDR and recovery prerequisites](a733-kernel.md).
The shared loader verifies upload contents and ROM identity, waits for automatic
reconnect access, and collects the retained result:

```sh
python3 tools/check-a733-kernel.py \
  --fel "$cache/build/sunxi-a733/sunxi-fel" \
  --image "$cache/build/qemu-virt/a733-cxx-check.bin" \
  --expect 'PASS C++ runtime:'
```

The check runs two tasks for 100 rounds each. Each round allocates a vector,
a 64-byte-aligned object, and a zeroed buffer that is subsequently enlarged.
After a task switch, it verifies contents and task-local `errno`, then frees
the allocations. Additional checks cover global constructors, `std::expected`,
square root, bounded formatting, refusal of oversized C and aligned C++
allocations, and successful allocation after failure. Allocator stress
qualification remains outstanding.

## Evidence

The emulator and one physical session passed on 2026-10-15. The physical result
reports stage 5, status 0, and approximately 0.101 seconds between monitor entry
and completion. The board returned to ROM and regained USB access automatically.
No persistent storage was written.

The physical raw image is 74,056 bytes with SHA-256
`03920786c595d4f836715a37e13a492849be94e6dd220021d06ab503b6de5e09`.
ELF sections contain 70,481 bytes of text/read-only data, 3,560 bytes of data,
and 4,317,392 bytes of reserved RAM, including the 4 MiB heap. The retained
result buffer is a separate 16 KiB reservation.

Raw evidence is under `~/.cache/sunstoneos/evidence/`: the emulator record is
`cxx-runtime-model.json`; the physical directory is
`cxx-runtime-native-allocation-20261015`. The physical directory includes the transaction
log, returned memory, upload readback, and source/toolchain input hashes.
Reuse these results while the relevant inputs remain unchanged.

## Mimus integration

The [mimus application](mimus.md) uses this runtime with external clock and task
identity providers. Its authoritative record covers model declarations,
recorded-audio execution, parity evidence, and remaining qualification.
