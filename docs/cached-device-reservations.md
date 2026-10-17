# Cached eight-core device reservations

The cached image can reserve the existing NPU convolution session, GPU firmware
and fixed GPU compute context before task creation. This is offline preparation.
GPU and NPU startup still require their actual platform, power, identity, interrupt,
recovery and isolation providers.

The linker accepts an optional 2 MiB write-back pool after image BSS and an optional
2 MiB non-cacheable pool at `0x41c00000..0x41e00000`. Both belong to the existing
image reservation. Code, ordinary driver state, all EL1 stacks and write-back
allocations end before the non-cacheable block. The secure monitor, its stacks
and the shared translation tables remain in `0x42000000..0x42200000`.
With no pool sizes supplied, the linker reserves no pool bytes.

[The table constructor](../platform/a733-smp-cached/memory.c) runs on the primary
at EL3 with interrupts masked and both translation regimes cold. It builds the
complete EL3 and EL1 profiles and gives the non-cacheable block matching attributes
in both profiles. EL3 retains its nonsecure physical-address selection for that
block. EL1 cannot access the secure monitor or translation tables. Table publication
precedes secondary release and cache adoption; later calls refuse active regimes.
The image does not change cache attributes at runtime.

[The platform hook](../platform/a733-smp-cached/platform.c) is enabled by
`SUNSTONE_A733_CACHED_DEVICES`. Its build supplies immutable
`a733_cached_device_inputs`, the actual constructor dependencies and both pool
sizes. The input structure, firmware, authenticated NPU model and callback storage
remain in the image. The firmware and model bytes must be included in the build
identity. The firmware clock field must agree with the later admitted GPU clock.

The platform also exports `a733_smp_device_preparation_admitted` independently
of that option. Native DMA setup can use the same actual boot guard without
constructing accelerator objects. Removing the conditional compilation leaves
the guard body unchanged. The affected native platform unit compiles and exports
the guard; evidence is in
`~/.cache/sunstoneos/evidence/cached-preparation-guard-20261016/`.
This change has no new complete-image or runtime result. The previously sealed
physical observer bundle remains unchanged.

All eight cores complete the existing pre-scheduler barrier before the primary
calls [the device constructor](../platform/a733-smp-cached/devices.c). Admission
checks actual barrier completion, primary EL1 identity, enabled caches and the
unreleased kernel flag. Admission also checks the startup deadline before
construction and before publishing success. Secondary cores remain behind the kernel release flag.
The primary constructs the actual NPU session, GPU firmware state, MIPS mappings,
compute programs, context, command and planner-sized GPU page tables. Successful
preparation is published with release/acquire ordering. A failed admitted attempt
cannot retry; all acquired parent allocations remain reserved. No child allocation
can release its parent.

These constructors occupy 399 write-back pages and 51 non-cacheable pages.
The write-back pool retains 113 free pages, or 452 KiB. A real CE entropy session
would consume one additional page, leaving 448 KiB. This composition creates no
entropy session or entropy readiness. A 640×480 NV16 camera frame alone requires
600 KiB. Concurrent camera, video, DMA and accelerator use needs additional boot
reservations backed by a qualified DRAM handoff; the initial pools do not establish
capacity for every onboard device.

Cold exclusive ownership, resolved dirty aliases, reset cache state and physical
interconnect coherence remain external boot prerequisites. Register readbacks,
software barriers and retained allocations cannot establish them. The reported
12 GiB capacity remains unqualified. All metadata and mappings remain private and
immutable under the existing cooperative platform contract. Ordinary EL0 access
is not isolated by these mappings. Other pool users remain idle during construction
and serialize every later pool mutation through a common owner or lock.

The [focused instruction fixture](../checks/cached-device-reservations.py) uses
this linker, table constructor, cold enable helpers, actual eight-core barrier
and actual NPU/GPU constructors. Its QEMU reset harness replaces A733 CPU release
and power admission. It executes architectural address translations on every core
and stops before scheduler or device activation. Preparation evidence does not
qualify physical cache coherence, watchdog recovery or device completion.

The focused check passes with eight emulated Cortex-A53 CPUs. Every core executes
read and write translations across both complete pools at EL3 and EL1. The check
also passes cold-regime refusal, protected-range checks, primary/barrier/deadline
admission, actual constructor preparation and release publication. It verifies
that all six parent leases remain device-owned and unreleasable.

Relinking identical objects with the prior and current default linker scripts
produces identical allocated sections, load-segment bytes and geometry, and symbol
values. Both absent pools end at BSS without adding bytes. An image whose BSS and
write-back pool reach into the non-cacheable block fails to link. Raw inputs,
executed ELF and results are retained in
`~/.cache/sunstoneos/evidence/cached-device-reservations-20261016/`.
