# Concurrent C and C++ runtime in FreeRTOS

The separate SMP runtime target runs C++ containers, newlib allocation, and
memory streams on two emulated Cortex-A55 cores. Task-local library state
survives migration between cores. This implementation is **tested in QEMU**;
physical multicore operation remains untested. [Mimus integration](mimus-smp.md)
passes whole-recording and paced-reference checks with a second inference worker.

## Library state belongs to the running task

The runtime rebuilds [newlib 4.5.0.20241231](https://sourceware.org/pub/newlib/)
with dynamic reentrancy and retargetable locking. The archive is pinned in
[sources.lock.json](../sources.lock.json). Dynamic reentrancy makes library calls
obtain their state through `__getreent()` instead of reading a global task
pointer. This is a supported [newlib integration mechanism](https://sourceware.org/pipermail/newlib/2002/001523.html).

FreeRTOS initializes a separate reentrancy structure in each task's control
block. The usual hook that updates the global pointer is disabled for this
target. Supervisor call 118 returns the running task's structure with IRQs
masked throughout the lookup. Once returned, the pointer remains valid if the
task moves to another core. Before scheduler startup, the lookup returns the
library's global initialization state.

The supervisor-call handler preserves the caller's general registers except
the return value. Its C helpers compile with general registers only, so the
lookup does not overwrite vector registers. This mechanism supplies newlib
task state; compiler-generated C++ `thread_local` storage remains unimplemented.

The rebuilt library also excludes its global-`errno` syscall wrappers.
[Reentrant adapters](../runtime/newlib_syscalls_smp.c) write errors directly to
the supplied task state. The common bounded heap adapter accepts that state
explicitly. A small [newlib patch](../patches/newlib-reclaim.patch) makes task
cleanup distinguish the global initialization structure without reading the
global task pointer. The finite task-deletion check below exercises this cleanup
with the pinned FreeRTOS and newlib in QEMU.

The link rejects references to `_impure_ptr`. Some objects in the prebuilt
C++ library still reference that global pointer and cannot be linked into this
target. The tested containers and allocation operators do not require those
objects. General iostream, locale, C++ threading, and shutdown support remain
outside the tested runtime. Library changes also require application parity
checks before replacing the existing mimus runtime.

## Shared library data uses FreeRTOS mutexes

The [locking adapter](../runtime/newlib_smp.c) supplies all newlib retargetable
lock functions and eight static library locks. It uses recursive FreeRTOS
mutexes, including the allocator lock. A pool supplies up to 32 dynamic locks
without allocating memory while creating a lock. Pool exhaustion fails an
assertion. Closing a dynamic lock returns its slot after verifying that the
mutex is not held.

Call `runtime_smp_init()` before `runtime_init()` and before creating tasks.
The first call initializes the library locks; the second runs C++ constructors.
Only the primary core may use the library before the scheduler starts.
Allocation and other locking library calls from interrupt handlers are unsupported.
Concurrent writes through one `FILE` object are serialized. Direct platform
logging and different streams targeting the same device need separate coordination.

## Build and focused evidence

Use the [existing Clang build environment](build.md) and pinned bare-metal
toolchain. The newlib builder keeps its sources, build, installation, and log
under `~/.cache/sunstoneos`. It records source and compiler identities and
verifies installed output hashes before reusing an existing build.

```sh
cache="$HOME/.cache/sunstoneos"
toolchain=$(python3 tools/fetch-sources.py aarch64-newlib-toolchain)
newlib_smp=$(python3 tools/build-newlib-smp.py --toolchain "$toolchain")
cmake -S . -B "$cache/build/qemu-virt" \
  -DNEWLIB_ROOT="$toolchain" -DNEWLIB_SMP_ROOT="$newlib_smp"
cmake --build "$cache/build/qemu-virt" --target smp-runtime-model -j 8
python3 tools/check-smp.py "$cache/build/qemu-virt/smp-runtime-model.elf" \
  --case runtime --qemu "$cache/host/usr/bin/qemu-system-aarch64" \
  --evidence "$cache/evidence/smp-runtime-check.json"
```

One bounded acceptance session passed on 2026-10-16. Two tasks each complete
128 rounds, synchronizing their starts on opposite cores. Each round checks
containers, aligned allocation, zeroed allocation, resizing, and retained data.
Each task moves to the other core with live allocations, a distinct `errno`,
and a pending `strtok` continuation. The check verifies the destination core and
the same task-state pointer after every move.

Each round opens and closes a private memory stream, exercising dynamic lock
reuse. Both tasks also write to one shared memory stream. The final comparison
requires 256 intact records, with each writer's records in order. Additional
checks cover global construction, bounded heap failure, and allocation after
failure. The image completes 256 migrations within the ten-second host bound.
The shared heap refactor also compiles in the single-core physical runtime.
That build was not executed on the quarantined board.

The accepted ELF SHA-256 is
`4744c9c8010de9bf059c62f6a39503fa99fc2b4f38c1ef97514089c121053184`.
Its sections contain 96,401 bytes of code/read-only data, 2,960 bytes of initialized
data, and 4,354,448 bytes of reserved RAM, including a 4 MiB heap.
The newlib C archive SHA-256 is
`00cc54d2917bb193763518a5c41a9f74e8e79a00f69e4ca95005522d4511ee8d`.
The image, output, library build record, and compiled-input manifest are under
`~/.cache/sunstoneos/evidence/smp-runtime-20261016/`.
`final.json` is the acceptance result; `initial.json` predates the added
destination-core and shared-stream checks.

## Task deletion reclaims library state before static storage reuse

Set `SUNSTONE_SMP_RUNTIME_CLEANUP=ON` in a separate build directory to select
the finite deletion case for `smp-runtime-model`. Run the existing checker with
`--case cleanup`. The default allocation and migration workload is unchanged.

The deletion case creates two tasks sequentially in the same static task and
stack storage. Each task allocates newlib decimal-conversion state, installs
a private cookie stream as its task-local standard output, and migrates to the
opposite core with live library state. It verifies its task-state pointer,
`errno` and `strtok` continuation, then deletes itself.

The idle task runs the real newlib reclamation function. A linker wrapper
publishes completion only after that function returns. The controller verifies
the private stream's close callback, restoration of allocated heap bytes to
a warmed baseline, and subsequent stream allocation and closure. Each recreated
task starts with fresh library state. Ordinary task-opened streams still need
explicit closure; this check uses standard output because newlib's task cleanup
owns that stream.

The fixture requires static allocation only and disabled MPU wrappers. In this
FreeRTOS configuration, the kernel does not access the deleted task's control
block after library reclamation. Those conditions permit reuse after the
wrapper's completion publication. A compile-time guard rejects a configuration
that would require a different reuse boundary.

One session passed on 2026-10-18, with a one-second guest deadline per task and
the existing ten-second host bound. Evidence, the compiled dependency snapshot
and inspection of the actual wrapped destructor are under
`~/.cache/sunstoneos/evidence/smp-runtime-cleanup-20261018/`.
This closes the modeled library-cleanup gap. All eight cores have physically
reached their first task context; sustained all-core scheduling and cache
coherence under load remain unproven. The earlier secondary interrupt-preparation
failure came from requiring all 32 `GICR_IGROUPR0` bits, including read-as-zero
unimplemented PPIs; the [reference audit](a733-multicore-reference-audit.md)
records the cause and the physical correction.
