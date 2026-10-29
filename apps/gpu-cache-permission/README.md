# Read-only GPU output cache diagnostic

This CPU-only returning diagnostic tests the cache-maintenance permission gap
for the integrated OS GPU output region. It does not start the GPU. A physical
pass establishes that the production EL1 cache helper completes DC CIVAC over
the read-only output mapping and preserves CPU-readable initialized bytes.
It does not establish GPU DMA coherence, completion-tag ordering, or recognition.

The current OS maps 0x46c00000–0x47000000 as two 2 MiB blocks with AP=11,
PXN=UXN=1, AF=1, AttrIndx=1 (MAIR=0xff), and inner shareability.
`platform/a733-smp-cached/memory.c` sets those attributes before EL1 entry.
`apps/os/identity.c` clears the first 64 KiB of tags at EL3 with translation
and caching disabled. Existing `checks/device-memory-capacity.py` checks the
production descriptors and read/write translation results. Existing returning
EL1 cache probes make no read-only cache-helper call.

Arm's [DC CIVAC instruction and pseudocode](https://support.arm.com/documentation/ddi0601/2026-09/AArch64-Instructions/DC-CIVAC--Data-or-unified-Cache-line-Clean-and-Invalidate-by-VA-to-PoC?lang=en)
select `CacheOp_CleanInvalidate` at PoC. Translation and trap controls still
apply. EL0 permission rules must not be generalized to EL1; the diagnostic
checks actual EL1 execution, with stage 2 disabled by the existing monitor.
The comparison implementations are TF-A revision
0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e,
[`lib/aarch64/cache_helpers.S:flush_dcache_range`](https://github.com/ARM-software/arm-trusted-firmware/blob/0892ad4db8f8f4c95775cc7d0505b6c12dda1f9e/lib/aarch64/cache_helpers.S), and Linux revision
e5f0a698b34ed76002dc5cff3804a61c80233a7a,
[`arch/arm64/mm/cache.S:dcache_clean_inval_poc`](https://github.com/torvalds/linux/blob/e5f0a698b34ed76002dc5cff3804a61c80233a7a/arch/arm64/mm/cache.S).
Both use CIVAC by cache line and a completion barrier. This image calls the
unchanged `arch/aarch64/device_memory.c:aarch64_device_cache` instead of
introducing another maintenance implementation.

The diagnostic places its logical output at 0x41800000–0x41c00000, inside
the existing 18 MiB image/result ownership interval. Its descriptors have the
same permissions and cache attributes as production. This qualifies the
permission operation, while production address layout, initialization and GPU
DMA visibility require their own physical evidence.

The builder derives local startup, mapping and linker copies from current shared sources. Its EL3
hook runs after monitor initialization, before EL1, and clears 64 KiB of tags
using aligned Device stores. It initializes one 64-byte line after the tags,
one in the second block, and the last output line with known words. Its local
platform copy installs output block descriptors with the exact production attribute bits before
enabling translation. Shared startup, return code and CPU qualification
profiles receive no changes.

At EL1, the client checks both raw block descriptors, read translations at
EL0 and EL1, and write-translation refusal at both levels using AT instructions.
These AT checks do not make a store or trigger a synchronous exception.
It reads all zero tags and all initialized words, calls the existing cache
helper once over all 4 MiB, and repeats the reads. Success prints a fixed
completion line and exits with status 0 through the existing monitor.
The diagnostic introduces no fault recovery; unexpected faults follow the
selected returning protocol's existing handling.

The incremental budget is 65,728 initialized bytes, 131,456 bytes read by the
checks, one helper call over 4 MiB (65,536 CIVAC operations at a 64-byte stride),
and two helper DSB SY barriers. There are no payload copies, GPU MMIO accesses,
GPU IRQs, jobs, or polling loops. The existing platform's timer, GIC, security
setup, result log and normal return remain prerequisites. The host check
measures linked image sizes and verifies the called helper and read-only
translation admission; only a board run can establish physical permission.

The initialization argument requires a loader with caches off, the existing
monitor entry's whole-cache invalidation, and no EL1 writes to output lines
before initialization. This excludes dirty Nonsecure aliases of the Secure
Device stores. The hook ends with DSB SY before monitor entry to EL1.
The monitor sets HCR_EL2 to 0x80000000: stage 2, TPCP/TPC and cache traps are
disabled. Cache maintenance still runs under the installed EL1 translation.

The linker reserves the output as a 4 MiB NOLOAD section and refuses overlap
with image data, either live exception stack, heap, or pools. The ordinary
linker-owned extents end below 0x41800000. No additional DRAM interval is touched.
The frozen host entry uses the existing returning-kernel transport, with strict
component acceptance. Before its ROM-word `readl` commands, the component copy
backs up 128 bytes at 0x71000, restores and compares them on the same ROM
instance, and records the result. Every request checks that instance identity.
A transport failure stops without a restoration attempt; uncertain scratch
retains ownership. The adapter never restores old SRAM after native return. It requires kernel mode and refuses controlled faults or
RAM-input overrides. An unexpected fault or transport failure retains ownership
under the selected protocol. The manifest keeps `execution_admitted=false` until
the coordinator supplies current board admission. This host entry does not
restore the existing 18 MiB DRAM interval or claim that restoration occurred.

Build and run the focused software check with the accepted toolchain:

```sh
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python \
  apps/gpu-cache-permission/build.py --output OUT/image
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python \
  apps/gpu-cache-permission/check.py --bundle OUT/image --output OUT/check.json
```

The builder pins the physically accepted exact-sum baseline and inherited
objects by hash. It captures the new compiler closure outside Git and replaces
objects by unique defining symbols. Unreachable GPU code is discarded by the
linker. The software check executes the linked EL3 initialization, client and
production cache-helper loop. It models architectural registers, translations,
cache effects and the scheduler. Cases cover ordinary completion, writable
mapping, EL0 denial, wrong attributes, uncleared tags and corrupted sums after
maintenance. The nominal case requires exactly 65,728 initialization bytes,
131,456 output bytes read, 65,536 CIVAC instructions and two helper barriers.
Capture checks reject nonzero terminal status, duplicate completion, a failure
line and missing timing. Packaged-entry cases refuse controlled-fault and
RAM-input overrides, including their argparse abbreviations. Scratch cases
cover successful same-instance restoration, readl transport failure with no
subsequent request, and restoration mismatch with retained ownership. A host pass does not qualify physical cache behavior.
