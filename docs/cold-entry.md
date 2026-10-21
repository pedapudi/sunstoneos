# Cold entry captures the bootloader handoff

The SunstoneOS monitor payload in [cold-entry.S](../boot/a733/cold-entry.S)
implements the first cold-start boundary: a branch past boot0's mutable header,
primary CPU checks, an exception table and a retained RAM report. It then parks.
It does not start FreeRTOS or admit an application. No board has executed this
payload; the current evidence comes from native instruction models and ordinary
file packaging.

The entry uses no stack or data memory before requiring EL3, CPU affinity zero,
an Arm Cortex-A55 and little-endian operation with translation and both caches
disabled. It reuses the existing CPU entry mask. An unexpected privilege, CPU
or cache state parks without writing memory. It does not attempt to repair an
unknown cache state.

After those checks, the entry installs a private exception table and clears its
512-byte report at `0x47008000`. It records CPU identity, control registers,
counter frequency and value, reset vector, and all 264 header bytes. The entry
requires the monitor signature, zero U-Boot/OP-TEE pointers and a replaced DRAM
loadword of at least 128 MiB. These metadata checks do not establish usable
memory capacity, exclusive ownership or remote-processor exclusion. Counter
frequency is recorded even when zero; no timing qualification is inferred.

The report publishes its magic last, after a barrier. Status 1 means entry state
was captured, status 2 means metadata was refused, and status 3 records an
exception. For status 3, only the report header and exception fields are valid;
the fault may precede the ordinary snapshot. Exception capture cannot recover
an unresponsive memory transaction. The payload performs no peripheral access,
CPU power operation, reset request or automatic retry.

| Report offset | Contents |
| --- | --- |
| `0..31` | Magic `0x41533733434f4c44`, version 1, size 512, status; each is a 64-bit word |
| `40..103` | CurrentEL, MIDR, MPIDR, SCTLR_EL3, SCR_EL3, CNTFRQ_EL0, CNTPCT_EL0, RVBAR_EL3 |
| `104` | Captured header length, 264 |
| `128..167` | Exception ESR_EL3, FAR_EL3, ELR_EL3, SPSR_EL3 and vector slot |
| `192..455` | Boot0-mutated monitor header |

The flat image loads at `0x47000000`. Its private report remains inside the
reserved monitor interval and outside the application and device pools.
The linker limits executable bytes below the report. The raw file contains the
original loadword at header offset `0x2c`; boot0 replaces that field before entry.
Entering the unmodified file refuses that metadata.

```sh
python3 tools/prepare-cold-entry.py --output /ordinary/path/new-build
```

The builder creates a new directory containing the native ELF, flat monitor,
source/output hashes and `input.json` for the existing
[microSD container packager](microsd-boot-package.md). The packager creates an
ordinary file; neither tool writes a card. A generated container currently
contains an entry recorder, not a complete standalone OS.

Evidence is under `~/.cache/sunstoneos/evidence/cold-entry-20261020/`.
The 4,164-byte monitor has SHA256
`8aaac1fa719eea8d3ddd161566a74dd9d962ead7768da347e49538e0f383c760`.
The native ELF is
`d274456709da2623cfa907ebdc716613f07cf7b91cbef666b07d0d8295370d5e`.
One invocation of the pinned packer produced the 20 MiB ordinary container
`60561dc7f3a9111749b7935330ac7aaa8bf0f6e2e4f5b44a60b3d06a0fa697c8`.
The packer's source equivalence and individual licensing remain unresolved.

[The instruction check](../checks/cold-entry.py) feeds the actual payload to the
pinned boot0 parser, then executes its main tail from `0x5e34c`, including the
counter delay and cleanup at `0x48e8c`. The observed instructions clear translation
and cache controls and request instruction-cache and branch-predictor
invalidation before the reset request. This interval does not establish data
cache cleaning, system-cache visibility or the state produced by a physical reset.

Eight native entry cases cover the normal snapshot, wrong privilege, wrong core,
enabled data cache, wrong CPU model, unexpected OP-TEE pointer, unchanged
loadword and exception capture. They execute the linked instructions with modeled
system registers. The exception case enters an actual vector directly.
The AArch64 reset-state fixture is independent of the AArch32 cleanup result.
Successful predecessor and entry checks do not prove physical reset semantics.

The next cold-start boundary must establish remote-writer exclusion and memory
publication before constructing application authority. Existing USB-loader
admission records cannot supply those facts for a cold boot. Media loading,
package authentication, DDR training, full boot0 initialization, sustained
FreeRTOS operation and recovery remain outside this evidence.

## The stage-2 build continues into the slot loader

Assembling `cold-entry.S` with `A733_COLD_STAGE2` produces a second monitor,
built by [cmake/a733-standalone-boot.cmake](../cmake/a733-standalone-boot.cmake)
as the `a733-stage2` target. It keeps the header, gate, exception table and
report above. Only status 1 continues: the monitor sets a stack and zeroes its
storage after the report, then calls the [boot-slot loader](boot-slots.md).
Status 2, status 3 and every fault, including a fault inside the loader,
publish the report and park as the recorder does. The recorder built by
`tools/prepare-cold-entry.py` is byte-identical to the evidence monitor above.

The loader reads and verifies a slot but does not enter it, because the
exclusion and launch-record prerequisites in the preceding paragraph remain
open. Its native SD admission also refuses, so on a board it reports a card
failure and requests the ROM forced-FEL return.

## The parser cleans CPU caches before returning

The pinned boot0 parser enables the data cache through `0x49066` before copying
package items. Its successful return calls `0x49074`, which runs the set/way
clean-and-invalidate loop at `0x47b80` before clearing the data-cache enable bit.
The loop reads cache levels and geometry, skips instruction-only levels, restores
the cache selector to zero and executes `DSB ST` followed by `ISB`. The separate
main-tail cleanup described above occurs later.

[The cache-wrapper check](../checks/cold-boot-cache.py) executes both actual
wrappers, including their Thumb-to-Arm transition. One synthetic cache hierarchy
produces all twenty expected set/way operations before the barrier and cache
disable. System registers, cache effects and the delay remain modeled. The
earlier parser check substituted these wrappers; its unchanged header-mutation
and handoff results retain their original scope.

Evidence is under `~/.cache/sunstoneos/evidence/cold-boot-cache-20261020/`.
This establishes the pinned instruction sequence, not physical publication.
Physical cache geometry, security domains, system-cache coverage and completion
of prior DMA still need separate authority. No bootloader or monitor was changed,
and no additional boot image was built.

## Remote-processor exclusion needs the effective A733 register map

The A733 manual identifies an E902 configuration block at `0x07032000`
(sections 5.2.3 and 5.2.4.8). The shipped monitor agrees with that address.
Its instructions at `0x4800af54..0x4800af9c` perform these writes:

| Address | Operation in the shipped monitor |
| --- | --- |
| `0x0701021c` | Write `0x10003` to the RISC-V configuration reset and bus gates |
| `0x07032204` | Write the E902 entry address `0x40004000` |
| `0x07050008` | Preserve other fields and set bit 0 |
| `0x07010210` | Preserve other fields and enable clock bit 31 |

The manual documents the first, second and fourth registers. The role of
`0x07050008` bit 0 in starting the core is inferred from the instruction sequence;
its reset and outstanding-transaction semantics remain unresolved.
Reversing these writes would not establish a safe shutdown or memory drain.

The published U-Boot directory `drivers/riscv/sun60iw2` supplies a different
configuration base, `0x07130000`, and different clock/reset offsets. Its directory
name does not make those definitions valid for this board. Use the manual and
effective monitor evidence when resolving cold-start ownership.

The [source pins](../sources.lock.json) identify manual revision
`d0dc4d615be00b76e3a0818d0818eff8f69ffa15`, U-Boot revision
`b791be842935b27268ae3d00e943a9075495f30a` and the shipped monitor.
The monitor hash and execution location are documented in
[CPU startup](cpu-start.md#published-firmware-corroborates-the-register-sequence).
The local `remote-source-assessment.json` and `monitor-e902-start.txt` preserve
the compared file hashes, exact instructions and unresolved requirement.
This assessment adds no MMIO operation to the cold-entry recorder.

[Remote-writer exclusion](remote-writers.md) records which bus masters the
board ROM and boot0 start and provides a read-only check for the stage-2
loader. boot0 copies an `scp` item to DRAM without starting the E902, and
neither boot0 nor the board ROM contains a literal address of the E902 start
registers. The check accepts the E902 only when `0x0701021c`, `0x07010210` and
`0x07050008` hold their reset values; it never reads the gated block at
`0x07032000`. The OR100 processor and the crypto engine, which the
board ROM takes out of reset, remain unproven, so the check refuses on every
board until further evidence exists. The loader does not call it yet.
