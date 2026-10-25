# Design: GIC status checks, distributor write waits and EL2 setup for the eight-core images

Status: **implemented and checked on host models; not run on a board.**
The reference comparison behind each change is the CPU reference
backsolve, `docs/reference-backsolve-cpu.md` on branch
`work/backsolve-cpu-1025` (commit `b90375f7`); its ranked list and its
fixes cover the same divergences. Line numbers below are on `bringup` at
`10863b20`, whose code equals `e55f64e0`: the only file that differs
between them is `docs/handoff.md`.

The change corrects six defects in the eight-core startup code and in the
returning diagnostics (the overlays that run one interval of startup on the
board and return to the boot ROM with a report). The tick, the interrupt
exit path, the FreeRTOS port, newlib and the counter clock belong to a
separate performance change.

## 1. Snapshot comparisons ignore read-only status bits

**Change.** The returning restorations take a ten-word snapshot of each
core's redistributor before initialization and again after restoration, and
require the two to match. Four of those words mix writable configuration with
status that hardware changes on its own:

- GICR_CTLR: UWP (bit 31) and RWP (bit 3), write-pending status
  (IHI 0069G 12.11.2);
- GICR_WAKER: Quiescent (bit 31, GIC-600 TRM table 4-24) and
  ChildrenAsleep (bit 2), which follow ProcessorSleep;
- GICR_ISPENDR0: the pending state, which a disabled level-sensitive PPI can
  still assert;
- GICR_PWRR: RDGPO (bit 3) and RDGPD (bit 2), the power state of the
  redistributor group. All eight frames report group 0
  (`GICR_PWRR = n << 8` on frame n, GIC-600 TRM 4.4.5), so one core's
  status bits reflect the power transitions of all eight.

`boot/a733/secondary-local-probe.h` defines one mask per word
(`GICR_CTLR_STATUS`, `GICR_WAKER_STATUS`, `GICR_ISPENDR0_STATUS`,
`GICR_PWRR_STATUS`). One function, `secondary_local_same`, applies them; both
CPU0's restoration (`primary-local-cache-probe.c`, formerly `:97`) and the
secondaries' cleanup (`secondary-local-probe.c`, formerly `:76`) call it. It
is defined in `secondary-distributor-probe.c` because that is the one unit
linked by both overlay builders that run a restoration. The baseline
comparison of GICR_PWRR with the earlier power read uses the same mask. The
host decoders (`tools/secondary-local-host.py`, formerly `:23`;
`tools/all-core-pre-scheduler-host.py`, formerly `:40`; and CPU0's in
`tools/prepare-primary-cache-probe.py`) apply the same masks in the same
before/after equalities, and mask only GICR_CTLR's status bits in the
baseline, as the probes do. The survey builder
(`tools/prepare-secondary-identity-probe.py`) reads the masks from the
header and holds the masking function and the distributor record check
that all three decoders call.

These checks are unchanged:

- the RDGPD = RDGPO stability check before each power request and on the
  baseline power word;
- each wait in a handshake: RWP after the disabling and restoring writes,
  ChildrenAsleep after a ProcessorSleep change;
- the explicit drain checks: each baseline requires nothing enabled, pending
  or active, the secondaries' cleanup requires no pending SGI or PPI after
  disabling them (cleanup error 3), and the workload drain refuses an
  unexpected pending PPI;
- the raw words in every record.

**Reference.** Linux and TF-A poll RWP and ChildrenAsleep and never compare
a GIC register with a saved value (`irq-gic-v3.c:344`–`365`;
`gicv3_helpers.c:36`–`61`, `:310`–`311`). The masked GICR_CTLR comparison
passed ten times in one port 3-3 lifetime
(`docs/designs/workload-bisect.md`).

**Blast radius.** The overlay code of every returning profile that runs a
local restoration: the secondary local, cache, coherence and interrupt
profiles; the pre-scheduler, first-task, task-switch and task-workload
profiles; both return-deadline profiles; and the CPU0 profiles with local
interrupt setup (`tools/prepare-primary-cache-probe.py --el1`, with and
without `--interrupts` and `--context`). No runtime image contains this code.

**Verification.** The instruction model `checks/workload-bisect-probe.py`
gains two injection cases, `restore-pwrr` (CPU0) and
`restore-pwrr-secondary` (core 3): the first GICR_PWRR read after the
restoring priority write returns both group bits inverted. Both deadline
profiles must return normally; `tools/workload-bisect-host.py` requires the
two cases. The `-unfixed` variants, run against the overlays built from
`10863b20`, must reproduce the expected failures: cleanup error 3 on CPU0, or
cleanup error 6 and a parked core 3. The CPU0 host check
(`checks/primary-cache-host.py`) adds `restore-pwrr`, an after-snapshot that
differs only in the PWRR group bits, GICR_CTLR.RWP and
GICR_WAKER.Quiescent and must be accepted, and `restore-group-refused`, one
that differs in the SGI/PPI group word and must be refused.

## 2. Distributor write-pending bits are waited for

**Change.** Four places treated a set GICD_CTLR.RWP (bit 31) as a failure.
Each now waits for it to clear, bounded by the deadline it already holds, and
reports a timeout with its own code:

| Site | Before | After; timeout result |
| --- | --- | --- |
| `platform/a733/cpu_continue.c:67`, phase 0 of the per-core controller | refusal, `ERROR_INITIAL` | the phase repeats until RWP reads clear; a timeout fails with `ERROR_DISTRIBUTOR_BUSY` (5), and the controller state keeps the last GICD_CTLR value read, in a field appended after the existing ones so that their layout still matches the frozen native image |
| `boot/a733/secondary-distributor-probe.c:26`, distributor preparation | refusal | wait, then read the configuration; `DISTRIBUTOR_ERROR_RWP_TIMEOUT` (2) in the record's new `error` word, and `DISTRIBUTOR_ERROR_CONFIGURATION` (1) for a wrong configuration |
| `secondary-distributor-probe.c:51`, final restoration check | whole-word equality | equality outside RWP, after the existing wait |
| `platform/a733-smp-cached/irq_table.c:44` and `:77`, SPI routing | refusal before routing; RWP clear required after routing | wait before reading the distributor and after the route writes; `A733_IRQ_TABLE_RWP_TIMEOUT` (2), which the boot patches of the OS, I2S and NPU images report as stop detail `0x311` (a refusal keeps `0x310`) |

The `error` word of the distributor record takes the first reserved word, so
a successful record is unchanged. The host decoders name a nonzero error
word when they refuse a record.

**Reference.** Linux `gic_do_wait_for_rwp` (`irq-gic-v3.c:344`–`359`) and TF-A
`gicd_wait_for_pending_write` (`gicv3_private.h:264`–`268`) poll RWP until it
clears. The backsolve ranks the refusal second among the divergences most
likely to stop a core: in the cached image a refusal parks the core, the
other cores then fail their arrival barrier, and the image stops without a
report. No board record shows this failure; it needs a concurrent
distributor writer. In the current images the two waits in `irq_table.c`
cannot find RWP set: the primary boot waits for RWP after its own
distributor writes immediately before routing, no secondary runs yet, and
the route writes do not set RWP (IHI 0069G 12.9.4). They are kept so that
the routing stays correct if another distributor writer is added.

**Blast radius.** `cpu_continue.c` is linked into every image built from
the cached sources (among them the supervised, observer,
storage-maintenance and devices images) and into the uncached diagnostic. `irq_table.c` is linked only into images that define
an interrupt table: the OS, I2S and NPU images. The returning overlays
contain the distributor probe. The returning profiles still call the
controller of the frozen native image (`762237cd…`), so their per-core
phase 0 keeps the refusal until the native image is refrozen; their
distributor writes finish before any secondary starts.

**Verification.** `checks/a733-cpu-continue.c` adds a distributor that
reports RWP for three reads, which must reach `GIC_READY`, and one whose
RWP never clears, which must fail with `ERROR_DISTRIBUTOR_BUSY` in phase 0,
with no write attempted and the last GICD_CTLR value kept. `checks/a733-irq-table.c` adds a route after five
pending reads, which must return `A733_IRQ_TABLE_ROUTED`, as the nominal
route now must too, and turns its pending-write refusal into
a timeout case that must return `A733_IRQ_TABLE_RWP_TIMEOUT` with no route
write. The returning profiles' models run the changed distributor probe.

## 3. Every core sets up the unused EL2 and zeroes its active priorities

**Change.** `a733_cached_enter_el1` in `platform/a733-smp-cached/continue.S`
gains the steps that the references perform for an operating system entered
at EL1 with EL2 unused:

- beside CNTHCTL_EL2 and CNTVOFF_EL2: VPIDR_EL2 = MIDR_EL1,
  VMPIDR_EL2 = MPIDR_EL1, CNTHP_CTL_EL2 = 0;
- at EL3, after the group enables are cleared and before SCR_EL3 is set:
  SCR_EL3.NS cleared, then ICC_AP0R0_EL1 = 0, an ISB as Linux issues
  (`irq-gic-v3.c:1258`), and the Secure ICC_AP1R0_EL1 = 0;
- at EL1, after the priority mask and before Group 1 is enabled: the
  Nonsecure ICC_AP1R0_EL1 = 0.

All three EL2 registers are UNKNOWN after a reset. EL1 reads of MIDR_EL1 and
MPIDR_EL1 return VPIDR_EL2 and VMPIDR_EL2, and SunstoneOS derives its core
index from MPIDR. An enabled EL2 physical timer asserts PPI 26, which the
drain checks refuse. A nonzero active priority would mask every interrupt at
or below that priority. The CPU interface has five priority bits
(ICC_CTLR_EL1.PRIbits = 4, Cortex-A55 TRM B4.7), so only the APxR0 registers
exist (IHI 0069G 12.2.1). The three writes follow the order that 12.2.1
requires: ICC_AP0R0, the Secure ICC_AP1R0, then the Nonsecure ICC_AP1R0.
With SCR_EL3.FIQ set, a Group 0 register access from EL1 traps to EL3, so
the Group 0 and Secure Group 1 copies are zeroed at EL3.

The returning profiles run the frozen native entry, which lacks these steps.
`native_el1_cache_probe` (`boot/a733/native-el1-cache-probe.S`), the EL3 code
that calls that entry, mirrors them before the call, as it already mirrors
the Secure priority mask. It zeroes all three active-priority copies there,
selecting each ICC_AP1R0_EL1 copy with SCR_EL3.NS and then restoring
SCR_EL3, because each profile enables Group 1 from different EL1 code.

**Reference.**

- TF-A `init_nonsecure_el2_unused`: VPIDR_EL2 and VMPIDR_EL2 at
  `lib/el3_runtime/aarch64/context_mgmt.c:1154`–`1155`, CNTHP_CTL_EL2 at
  `:1208`.
- The vendor BL31 does the same: VPIDR_EL2 at `monitor.fex@0x4870`,
  VMPIDR_EL2 at `@0x4878`, CNTHP_CTL_EL2 at `@0x4898`
  (sha256 `8af16b86…`, load address `0x48000000`).
- Linux `gic_cpu_sys_reg_init` zeroes ICC_AP0Rn (when Group 0 is accessible),
  then ICC_AP1Rn, then enables Group 1 (`drivers/irqchip/irq-gic-v3.c:1245`–`1281`).
  TF-A writes no active-priority register.

**Blast radius.** Every image built from the cached `continue.S`, among
them the supervised, observer, cached diagnostic, storage-maintenance and
devices images. The
observer and storage-maintenance builds apply patches to `continue.S` and pin
its hash; both patches apply unchanged at new offsets, and the pinned hash in
`cmake/a733-smp-observe.cmake` and `cmake/a733-storage-maintenance.cmake` is
updated. Each core executes 13 more instructions once per entry (counted:
six at EL3 for the active priorities, five for EL2, two at EL1). The mirror
adds 18 instructions once per core to the returning profiles from the
secondary cache profile onwards, and to the CPU0 profiles with
`--el1`. A future refreeze of the native image moves the offsets of
`a733_cached_enter_el1+0xac` and `+0x144` in the patch plan (item 5);
`apply_plan` refuses the build until they are updated.

The uncached diagnostic's continuation (`platform/a733/cpu_continue.S`) is
unchanged; it is not used for interrupt delivery.

**Verification.** The instruction model's CPU interface
(`checks/a733-smp-cached.py`) records each zeroed copy: the Nonsecure
ICC_AP1R0_EL1 below EL3 or when SCR_EL3.NS is set, the Secure copy at EL3
otherwise. It refuses a Group 0 access below EL3 while SCR_EL3.FIQ is set.
The eight-core scheduler run must show all three copies zeroed on every core
before it enables Group 1. The secondary cache and pre-scheduler models
require the same of the mirror before each core's EL1 entry.

## 4. The return-deadline constants have one definition

**Change.** The record address `0x4201b200` appeared five times (the
deadline assembly, the coherent survey's shared-page copy in
`secondary-identity-probe.c`, the builder, the host module and the model),
and its magic number and the deadline three or four times each.
`boot/a733/secondary-local-probe.h` now defines `DEADLINE_RECORD`,
`DEADLINE_RECORD_BYTES`, `DEADLINE_RECORD_MAGIC` and `DEADLINE_TICKS` next to
the status masks, as plain literals, with the C declarations under
`#ifndef __ASSEMBLER__`. `workload-deadline-probe.S` and
`secondary-identity-probe.c` include it. The builder reads every
`#define NAME VALUE` line with a regular expression, as
`checks/memtest-image.py:170` does; the host modules take the values from
the builder, and the model reads the header with the same expression.

**Blast radius.** None in code bytes.

**Verification.** The identity profile, whose compiled code does not
change, builds byte-identical to `10863b20`. The deadline profiles' models
check the record against the same values.

## 5. The host module reapplies the builder's patch plan

**Change.** `tools/prepare-secondary-identity-probe.py` defines `profile`
(the profile flags with every implied stage set), `schema`, `sources`,
`report_area`, `extension_address`, `patch_plan(native, hooks, flags)` and
`apply_plan`. `patch_plan` lists every native word that the profile
replaces, as (address, original word, replacement), in application order.
`apply_plan` applies it to the frozen segments, requires each original word
and each patch to land exactly once, and returns the patched segments and
the patch records. The builder writes those results. `load` in
`tools/secondary-identity-host.py` takes the schema, sources and plan from
the builder and reapplies the plan to the frozen base overlay and native
segments. It then requires every built byte and the recorded patch inventory
to match. The host module previously rebuilt the patch list itself, a second
copy that the builder's helpers kept equal by construction.

**Blast radius.** The builder and every host module that loads a returning
build. Build outputs are unchanged.

**Verification.** With the C sources fixed at commit `be0c4926`, the
refactored builder of `94852e47` produces byte-identical overlays, native
segments and ELF files for all eleven profiles. Against the builder of
`10863b20`, every profile's patch list has the same addresses, original
words and order; each replacement differs only where the branch targets a
hook that moved. Every host case of every affected profile passes through
`load`.

## 6. The resident contract has its own magic number

**Change.** The resident contract magic in
`platform/a733-smp-cached/boot.h:9`, `0x4153373352455331`, equalled the
uncached image's result magic (`platform/a733-smp/boot.h:5`). It becomes
`0x4153373352434e31`, which no other file in the tree uses.
`checks/a733-resident-contract.c:50` refuses the new value for a FEL
contract, and the resident build also refuses the uncached result magic.

**Blast radius.** Only builds that define `SUNSTONE_A733_RESIDENT`; the
resident contract check is the only one, and no image does.

**Verification.** `checks/a733-resident-contract.c`, compiled with and
without `SUNSTONE_A733_RESIDENT`.

## The workload models needed a slower modeled counter

The workload instruction models (`checks/all-core-task-workload-probe.py`
and the return-deadline model built on it) advanced the counter 1,000 ticks
per scheduling quantum, in which each core runs 211 to 887 instructions:
about 12,500 CPU0 instructions per 1 ms tick period. The image reprograms
its tick as the previous compare value plus the period, so a tick handled
late fires again at once. With item 3's first 17 probe instructions, CPU0's
worker made one of its 32 rounds while CPU0 took back-to-back ticks; its
compare value had fallen 21 periods behind the counter. Replacing those
instructions with 4, 8, 12, 16, 20 or 24 `NOP`s gives the same stall; with
one `NOP`, or none, the workload completes. The stall therefore depends
only on the number of instructions that precede EL1 entry.

At 500 ticks per quantum the workload completed with 4, 8, 17 and 24
inserted `NOP`s and with the probe code, in 86 to 97 ticks instead of 524
to 870. The models now advance 250 ticks per quantum (`COUNTER_STEP`),
about 50,000 CPU0 instructions per period, and the committed profiles'
workload completes in 38 ticks. Both workload models also fail at once,
with a message that names the saturated model, when CPU0's enabled tick
compare value falls more than one period (24,000 counter ticks) behind the
modeled counter; at 1,000 ticks per quantum that check stops the run at
counter 1,566,024. A core on the board
executes roughly a million instructions per tick period, so the board does
not approach this limit. Programming the tick from the counter, as Linux
does, removes the catch-up behavior; that change belongs to the separate
performance change.

Two model expectations depended on which worker finished last, which the
counter rate changes. In the `stalled-no-tick` case CPU0 can now be waiting
in `identity_park` at EL1 instead of at EL0; the model lets the EL3-routed
deadline interrupt take it there, as the hardware does. The core 3
reproduction cases (`-secondary-unfixed`) require only that core 3 itself
did not reach OFF, since CPU0 may already have powered earlier secondaries
off.

## Budgets and measurements

Budgets follow [docs/performance-architecture.md](../performance-architecture.md):
no hot path changes, and each added step runs once per core per launch.

- **Counted instruction costs.** The runtime entry adds 13 instructions per
  core per entry. The probe mirror adds 18 per core. Phase 0 of the
  controller reads GICD_CTLR once, as before, when RWP is clear. The SPI
  routing reads GICD_CTLR twice more and the counter once more; the
  overlay's distributor preparation reads GICD_CTLR once more.
- **Overlay space** (text bytes, base `10863b20` to head; the limit is
  12,288): local 6,708 to 6,660; cached 8,040 to 8,064; coherent 8,768 to
  8,792; interrupt profile 11,512 to 11,536 (including its 2,048-byte vector
  table); pre-scheduler 10,724 to 10,604; first-task 11,604 to 11,484; task
  switch 10,972 to 10,852; task workload 11,404 to 11,284; both
  return-deadline profiles 12,112 to 11,992. The identity profile and the
  CPU0 cache profile without `--el1` are byte-identical to the base.
- **Native payload segments** of every returning profile differ from the
  base only in patched branch words whose overlay targets moved.
- **Instruction models** (evidence under
  `~/.cache/sunstoneos/evidence/mc-correct-20261025/final-2517b0ae/`): the
  secondary local, cache, coherence and interrupt models; the pre-scheduler
  model; five first-task, two task-switch and two workload cases; nine
  return-deadline cases (success, two stalls, four RWP and two PWRR
  injections) and seven quiet-profile cases; the identity model; the four
  CPU0 profile models; the eight-core scheduler model built from the tree
  (`checks/a733-smp-cached.py`); the controller check
  (`checks/a733-cpu-continue.py`) and the supervised image check. All pass.
  Against the base overlays, `restore-pwrr` and `restore-pwrr-secondary`
  fail, and their `-unfixed` variants reproduce cleanup error 3 on CPU0 and
  cleanup error 6 with core 3 parked (`reproduce-base/`).
- **Host checks**: every returning profile's host check and its evidence
  validator pass, including the deadline host module's requirement of the
  two PWRR cases, and the CPU0 host checks of the cache, local-interrupt,
  interrupt and context profiles, with `restore-pwrr` and
  `restore-group-refused`.
- **Default images** from clean archives of `10863b20` and `2517b0ae`:
  `kernel-check.elf` is byte-identical. `a733-smp-supervised.elf` differs
  in `a733_cached_enter_el1` (the 13 instructions),
  `a733_cpu_continue_poll` (17 more instructions) and its jump table
  (item 2), six routines after the entry that move 52 bytes into alignment
  padding before the EL3
  vectors (every vector table keeps its address), the branches into those
  routines, and `a733_smp_build_id`, which digests the sources
  (`image-comparison.md` in the evidence directory).

- **Integration gate** `mc-correct-1025-38b6e0f9` on `38b6e0f9` (clean
  tree): the supervised, cached diagnostic, uncached diagnostic, observer,
  storage-maintenance and devices images, the SMP kernel and context
  models, the supervised OS variant (which applies the OS boot patch), and
  the kernel, eight-core soak and check-index checks pass. The SMP kernel
  and context models then pass under QEMU
  (`tools/check-smp.py --case kernel`, `--case context`). The I2S image
  built with its changed boot patch at `1428f1a9`; the patch has not changed
  since. The NPU board image was not built: no reviewed NPU catalogue is
  configured on the build host.

## Frozen bundles that need a refreeze

Each frozen bundle runs from its own source copy, so none changes. These
bind sources that differ at the head and need a refreeze before their next
board use:

- Rebuilt by this change and matching `bringup` until now: the
  return-deadline and quiet bundles and every profile build under
  `workload-bisect-20261025/rwp4/`.
- Already stale against `bringup` before this change, which their refreeze
  now includes: `board-bundles-20261023/rank-7-corrected/`,
  `rank-7-pre-scheduler/` and `rank-8-workload/`; the board-qualified
  deadline bundles under `workload-bisect-20261025/rwp3/`;
  `cpu-next-20261024/` (first task, task switch, task workload) and
  `cpu-diagnosis-20261024/pre-scheduler/`; and the CPU0 bundles
  `primary-el1-probe-20261018/`, `primary-interrupt-probe-20261018/` and
  `primary-context-probe-20261018/`.
- Identity survey builds (`workload-bisect-20261025/*/build-identity`):
  bytes unchanged; only their source digests need rebinding.
- The frozen native image (`762237cd…`) still lacks items 2 and 3 at
  runtime; the returning profiles get item 3 from the probe mirror. Its next
  refreeze moves the patch offsets `a733_cached_enter_el1+0xac` and `+0x144`,
  which `apply_plan` will refuse until they are updated.

`refreeze-scan.json` in the evidence directory lists every frozen probe
manifest with sources that differ at the head, including older
diagnostics that were already superseded.
