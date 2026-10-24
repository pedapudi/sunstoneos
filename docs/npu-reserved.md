# Native reservations for the authenticated NPU convolution

The [native constructor](../platform/a733/npu_reserved.c) connects the
[linker-owned device pool](memory-reservations.md) to the existing
[authenticated convolution](npu-convolution.md). It allocates actual backing,
partitions it into command, data and table storage, and calls
`npu_convolution_prepare()`. It returns the same convolution object used by
startup and the FreeRTOS service. The implementation does not provide physical
startup authority or establish numerical NPU execution.

This document assumes familiarity with the convolution service and page-based
memory allocation. The parent lease identifies the complete allocation by pool,
owner, slot and generation. Individual buffer descriptions do not confer release
rights over any part of that parent.

## One reservation contains all session memory

Enable a sufficiently large linker pool in the single-core A733 image.
For example, `-Wl,--defsym=__device_pool_size=2097152` reserves 2 MiB within the
existing image region. The pool's optional maximum is 2 MiB; its default remains
zero. The cached multicore image requires a separate integration.

One parent allocation holds 1,085,440 bytes, comprising 265 complete 4 KiB pages:

| Offset | Bytes | Use | NPU virtual address |
| --- | ---: | --- | --- |
| `0x000000` | 4,096 | Neural-operation descriptors | `0xe0000000` |
| `0x001000` | 4,096 | Coefficients | `0xe0002000` |
| `0x002000` | 524,288 | Application commands and full prefetch tail | `0xe0004000` |
| `0x082000` | 4,096 | Input | `0xe0085000` |
| `0x083000` | 4,096 | Output | `0xe0087000` |
| `0x084000` | 524,288 | Bootstrap commands and full prefetch tail | `0xe0089000` |
| `0x104000` | 20,480 | Private page directory and page table | Not a payload mapping |

The native pool fixes CPU and physical addresses. The constructor derives every
child address from the complete parent extent. The existing NPU memory policy
checks each page's translation, attributes, permissions and disjoint ranges.
It excludes the complete table arena from device payload mappings. The device
address layout preserves the authenticated command relocation profile.

The constructor copies and authenticates the existing compiled model; it does
not change weights, shapes or quantization. The embedded convolution object
contains the private model copy and service metadata outside the pool.
Insufficient contiguous space refuses preparation without allocating a partial
session or modifying the caller's session object.

## The reservation lasts through startup and every job

Zero-initialize the session once and prepare it before scheduling. Serialize
preparation with all other pool allocations and mutations. The session, fatal
callback and callback context must remain valid for the platform lifetime.
All input metadata must lie outside the complete linker pool.

The allocation enters consumer ownership before offline preparation. The pool
names this state `DEVICE`; CPU access remains governed by the convolution's
existing preparation and client protocol. The state prevents the allocator
from releasing buffers at individual job completion.

A model, mapping or preparation failure marks the admitted parent `RETAINED`.
Subsequent calls cannot reinitialize that session. Later convolution faults
continue holding the parent in consumer ownership; they do not necessarily
change the allocator's state to `RETAINED`. Both states refuse allocator release.
No teardown or child-release operation is provided.

Use the returned object throughout the existing runtime sequence:

1. Initialize the [startup sequencer](npu-startup.md) with the image's shared
   providers: counter, clock registry, register lock, GPU/PCK resource manager,
   rail ledger and power-domain owner. Bind its designated bootstrap task.
2. Call `npu_convolution_bind()` with that startup object and the designated
   client task. This constructs the existing deferred FreeRTOS service with the
   reserved backend, MMU image and command catalogue.
3. After scheduling starts, the bootstrap task calls
   `npu_convolution_startup_poll()` once per tick until READY, then activates
   the service. The client uses the existing send, collect, output and reap
   functions. Every job uses the same permanently reserved backing.

These are the existing production APIs. The constructor does not replace their
activation conditions. It cannot infer remote-processor exclusion, internal
SRAM ownership, physical address confinement, supply stability, interrupt
ownership or watchdog recovery from successful allocation or translation.
All remain required before startup permits device commands.

## Focused validation covers the reservation composition

The preparation fixture uses the actual constructor, convolution authenticator,
assembler, MMU builder, native memory policy and AArch64 translation instructions.
Its linker places the pool inside the emulator's existing identity-mapped RAM.
The only address substitution maps an emulated Device page to the NPU register
physical address for admission. The fixture performs no NPU register access.

The three declared modes passed in QEMU: authenticated preparation, model
rejection and insufficient capacity. Successful preparation checks the complete child layout
and command digests, disposes of caller model storage and performs native
cache publication. A real static FreeRTOS client
cannot bind the service without initialized startup authority. The fixture
supplies no startup readiness or drain-success flags.

Model rejection retains the complete parent. Capacity refusal leaves the session
unchanged and creates no allocation. Neighboring CPU storage remains intact.
The fixture does not execute the NPU, synthesize an inference result, run the
existing service lifecycle cases or establish physical cache visibility.

Run the focused composition after relevant inputs change:

```sh
~/.cache/sunstoneos/venv/bin/python checks/npu-reserved.py \
  --freertos /path/to/pinned/FreeRTOS-Kernel \
  --fixtures /path/to/authenticated/convolution-fixtures \
  --qemu ~/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64 \
  --lld ~/.cache/sunstoneos/host/usr/bin/ld.lld-21 \
  --output ~/.cache/sunstoneos/evidence/npu-reserved-local
```

The runner preserves compiler dependencies, fixture bytes, executable identity,
commands and logs. The separate reservation linker check passed 2 MiB acceptance and refusal
of one additional page. Earlier default-layout evidence remains applicable:
only the existing pool-capacity assertion changed. Numerical, startup and
service evidence remains applicable to unchanged behavior.

Evidence is under `~/.cache/sunstoneos/evidence/npu-reserved-20261016/`.
`batch2/selected-modes/results.json` records the successful native modes;
`capacity/results.json` records the linker limit check. The preserved source,
compiler dependencies, fixture and executable identify the tested composition.
The declared model remains SHA-256
`db42d815100417f4b8913896462ed031beafae35fb4ee1c28569ac950a9e8fca`.

The build driver needed linker configuration and dependency corrections before
execution. An initial mode-selector failure occurred before preparation;
patching the selector in the preserved ELF's data selected each case correctly.
The successful image was reused. The final runner incorporates those corrections;
the evidence also preserves the separate driver used for the successful modes.
