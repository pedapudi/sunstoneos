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
