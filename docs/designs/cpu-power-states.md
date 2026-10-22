# Design: idle cores wait in WFI, and secondary cores power off and on through EL3

Status: **design only**. No code exists, and every register sequence below
stays unqualified on the board until the evidence in the last section exists.
Section numbers refer to `docs/performance-architecture.md`.

Today an idle core never sleeps. The cached eight-core image disables both
FreeRTOS idle hooks, so idle tasks spin. Core 0 alone takes the 1 ms tick, and
its time-slice check in `xTaskIncrementTick` yields every core that runs an
idle task, because the idle-priority ready list always holds all eight idle
tasks: 7 SGI0 interrupts and 7 context switches per tick with no work
(counted). The image's EL3 vectors treat every SMC as a terminal report, so no
core can change power state after startup.

Stage 1 puts idle cores into WFI without EL3 code. Stage 2 adds PSCI-style
`CPU_OFF` and `CPU_ON` to the resident EL3 code for cores 1 to 7. It lands with
its first consumer, the owner of the recognition core pair (cores 6 and 7 in
the A76 admission build, `docs/designs/recognition-dispatch.md`), which turns
the pair off when its session closes and on when it opens.

## Stage 1: idle tasks execute WFI

Both idle hooks call `a733_cpu_idle()` in `platform/a733/cpu_power.c`:
`DSB SY` then `WFI`, as section 1.2 permits. Idle tasks run at EL0, so
`SCTLR_EL1.nTWI` (bit 16) must be set beside `nTWE`, or EL0 `WFI` traps with
exception class `0x01` (Arm DDI0616A.a, section E3.2).

`prvYieldForTask` treats a core running an idle task as preemptible by any
task, so new work reaches an idle core as SGI0; core 0 also wakes on timer
PPI30. The time-slice check changes to yield a core running an idle task only
when more than `configNUMBER_OF_CORES` tasks are ready at idle priority, the
test `configIDLE_SHOULD_YIELD` already uses. Round-robin among application
tasks is unchanged.

## Stage 2: data flow and ownership

Core 0 is never powered off: it takes the tick, renews WDT0 and runs the
supervising task. One EL1 owner per core pair requests changes.

- **EL1 power word.** Each core has one atomic word in kernel `.bss` on its own
  64-byte line: ONLINE, OFF_REQUESTED or OFF_TAKEN. The owner moves ONLINE to
  OFF_REQUESTED by compare-and-swap, and `prvYieldForTask` then skips the core.
  When the core next runs its idle task, the hook moves OFF_REQUESTED to
  OFF_TAKEN and issues a new port SVC that saves the idle task's context into
  its TCB, as a yield does, and issues SMC `CPU_OFF`.
- **Power on.** The owner tries OFF_REQUESTED to ONLINE; success means the core
  never left. Otherwise the owner issues SMC `CPU_ON` and stores ONLINE with
  release ordering. The compare-and-swap closes the race between a late off
  request and an early power-on.
- **Warm resume.** The core enters EL3 at a fixed warm entry, reruns
  `cpu_coherence` and `cpu_continue` for itself, enters EL1 through
  `a733_cached_enter_el1`, and restores `pxCurrentTCBs[core]` through the
  port's first-task restore (SVC 106). The restored hook calls `taskYIELD()`
  once, because `cpu_continue` clears pending SGIs and can discard a yield
  sent during resume.

The service uses PSCI function identifiers and return codes (`CPU_OFF`
`0x84000002`, `CPU_ON` `0xC4000003`; Linux `include/uapi/linux/psci.h` at
`e5f0a698`). The QEMU eight-core model already starts secondaries through
QEMU's PSCI over HVC (`platform/qemu-smp/platform.c`), so the EL1 side runs
there unchanged with HVC in place of SMC. EL3 accepts only the image's fixed
EL1 warm entry as the `CPU_ON` entry and refuses core 0, unknown cores and
other function identifiers. It has no `SYSTEM_RESET` or `SYSTEM_OFF` and never
accesses WDT0, which stays the only reset path in the resident lifetime.

## Register sequences and their sources

| Step | Sequence | Source | Board status |
| --- | --- | --- | --- |
| `CPU_ON` | Require PPU status OFF (`0x07052008 + n*0x1000`, bits 3:0 = 0); write and verify entry words (`0x08001004`, `0x08001008 + n*0x1000`) and AArch64 bit 0 of `0x08001000 + n*0x1000`; set mode bit 0 (`0x07050220 + n*4`), then control bit 0 (`0x07050200 + n*4`); wait for status 8 (ON); restore control and mode | Published board monitor helpers at file offsets `0xc4a0`, `0xc298`, `0xc314`; TF-A A733 port `sunxi_pwr_domain_on` (`InfernoEmbedded/arm-trusted-firmware@5bc9605`, `plat/allwinner/common/sunxi_native_pm_ncat2.c`); already implemented as `platform/a733/cpu_start.c` | First power-on passes on every secondary. A second power-on in one boot is **unproven** |
| `CPU_OFF`, GIC | Disable CPU-interface groups; set `GICR_WAKER.ProcessorSleep`; wait for `ChildrenAsleep` | Arm IHI 0069G, `GICR_WAKER`; TF-A `gicv3_rdistif_mark_core_asleep` (same revision) | Passes with delivery disabled (`native-wake-sleep-20261018`) |
| `CPU_OFF`, core | Set `CPUPWRCTLR_EL1.CORE_PWRDN_EN`, `DSB`, `ISB` (Cortex-A76 erratum 2743102); clear mode bit 0; set control bit 1 (GIC wake-up disable); `DSB SY`; `WFI` | TF-A `cortex_a55_core_pwr_dwn`, `cortex_a76_core_pwr_dwn` (`ARM-software/arm-trusted-firmware@0892ad4`); TF-A A733 `sunxi_cpu_off`; monitor helper at `0xc2f0` clears mode bit 0 | Passes in startup diagnostics on every secondary (`secondary-local-probe-20261018/physical-110`). From a running cached scheduler: **unproven** |
| Warm entry, GIC | Clear control bit 1; wake the redistributor; configure SGI0 | `platform/a733/cpu_continue.c`; TF-A `sunxi_pwr_domain_on_finish`; monitor helper at `0xc224` | Passes for cold entry. Warm entry: **unproven** |

A core's private L1 and L2 caches must reach memory before it loses power.
Both TF-A power-down functions issue no cache maintenance before `WFI` and so
rely on the cores cleaning their private caches in hardware. The
implementation cites the power-down sections of the Cortex-A55 r2p0 and
Cortex-A76 r4p1 reference manuals for this; if they do not state it, work
stops and the design returns for review. The published monitor also clears
the low four bits of nine registers at `0x07051000`–`0x07059000` (file offset
`0x4e88`), which may be power policy registers. Their meaning is undocumented,
and this design does not write them.

## Cluster, DSU and redistributor power stay on

The A733 has one DynamIQ cluster holding all eight cores: the user manual's
`CLUSTER_CFG` list (section 5.1.5.4, printed page 250) has `C0_CPU0` to
`C0_CPU7` under one `C0_RST_CTRL`, and figure 5-1 shows one L3 cache and
snoop filter. Cluster or DSU power-down needs core 0 off, which the running
OS never allows, so EL3 never touches cluster power. Redistributors stay
powered (`GICR_PWRR.RDPD` unchanged): their power-on transition is
unqualified, and a powered redistributor latches SGIs sent to an off core.

## Synchronization, memory, barriers and polling

- **EL3 state.** One word per core, on its own 64-byte line in the secure
  section that holds `a733_cached_records`, which EL1 cannot write: ON,
  OFF_PENDING or ON_PENDING. The target writes OFF_PENDING in `CPU_OFF`;
  `CPU_ON` moves OFF_PENDING to ON_PENDING by compare-and-swap with acquire
  and release ordering; warm entry writes ON. Other states return
  `ALREADY_ON` or `ON_PENDING`. `CPU_ON` powers on only after reading PPU OFF.
- **Memory.** Both state arrays are Normal write-back inner-shareable, which
  exclusives require. CPU-to-CPU ordering uses acquire and release only.
- **Barriers.** Plain 32-bit accessors from `a733_mmio.h` inside a sequence;
  the two barrier steps of `cpu_start.c` in `CPU_ON`; in `CPU_OFF`, `DSB` and
  `ISB` after `CPUPWRCTLR_EL1` and one `DSB SY` before `WFI`.
- **Polling.** PPU status and `GICR_WAKER` have no documented completion
  interrupt, so EL3 polls them against `CNTPCT_EL0` deadlines: 100 µs for
  `ChildrenAsleep`, 1 ms for the OFF and ON waits in `CPU_ON`. These design
  limits run only on the owner's admission path; board timing replaces them.

## Reused and removed code

Reused: `cpu_start.c` for `CPU_ON` with native callbacks; `cpu_coherence` and
`cpu_continue` for warm entry; the port's context save and first-task
restore; `a733_task_context.h` to admit the owner; `a733_range.h` for
deadlines. `platform/a733/cpu_power.S` becomes the one runtime copy of the
shutdown instructions now found only in startup diagnostics. The spinning
idle loop is removed. The single-core monitor `platform/a733/monitor.c` is not
in the eight-core image and keeps its SMC contract.

## Performance budgets

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| SGIs to idle secondaries per tick, no ready task | 7 (counted) | 0 | `sgis` counters in `checks/a733-smp-cached.py` and the QEMU eight-core model, over 64 ticks |
| Instructions per idle secondary between wake-ups | Its full emulation quantum (counted: spin) | 0 in `WFI`; idle loop grows by at most 6 instructions | Per-core instruction count added to the same model |
| Core 0 idle wake-ups per second | 1,000 | 1,000 | Same model |
| `CPU_OFF`, SMC entry to `WFI` | No path | At most 150 instructions, 3 MMIO writes, 1 bounded wait | Host model `checks/cpu-power-states.c`; Unicorn trace of the linked handler |
| MMIO writes in `CPU_ON` | 7 (`checks/a733-cpu-start.py`) | 7 | Same check, warm entry address |
| Instructions, warm reset to idle-task restore | Cold secondary path, not measured | Cold path less its epoch, exclusive-probe and pre-scheduler steps | Instruction model, before and after |
| Added work in `prvYieldForTask` | 0 | One acquire load per candidate core; no barrier or SMC | Disassembly |
| Resident EL3 power service | None | At most 4 KiB text, including the 1,559 bytes of `cpu_start.c`, and 512 B state | `llvm-size` |
| Power-word lines written by more than one core | None exist | 0 | `_Static_assert` on alignment |

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| Call refused by argument or state check | 1 | Nothing |
| `ChildrenAsleep` timeout in `CPU_OFF` | 2 | Nothing: EL3 clears `ProcessorSleep`, returns `DENIED`; the core stays ONLINE |
| OFF or ON not reached by the `CPU_ON` deadline, or `WFI` returns after the power-down writes | 3 | The core, its idle task and stacks: the word stays OFF_TAKEN and the scheduler sends it no work. Tasks pinned only there stop reporting, and in the resident lifetime WDT0 resets the board |

## Left out

- **Power-off per idle period by a governor:** waking a core from the yield
  path would put a bounded EL3 wait inside the kernel lock.
- **GIC hardware wake from OFF, core retention and PPU policy:** the manual
  documents neither the PPU nor the hotplug registers, and the A733 TF-A port
  defines no idle states.
- **L3 partial power-down:** no consumer.
- **Tickless idle on core 0:** services still poll on ticks (section 1.3).
- **`CPU_SUSPEND`, `AFFINITY_INFO`, `SYSTEM_OFF`, `SYSTEM_RESET`, version and feature calls:** no
  consumer.

## Changes outside this package

For the primary agent:

- `platform/a733-smp-cached/FreeRTOSConfig.h`: enable both idle hooks.
- `platform/a733-smp-cached/continue.S`: set `SCTLR_EL1.nTWI`; dispatch the two
  function identifiers from the EL3 synchronous vector to `a733_cpu_power_smc`
  before the fault path; a warm `a733_cached_enter_el1` variant that ends in
  the first-task restore without the ready-flag handshake.
- `patches/freertos-smp-c.patch`: the idle time-slice condition and the power
  word check in `prvYieldForTask`.
- `patches/freertos-smp-asm.patch`: the `CPU_OFF` SVC, which saves context and
  calls `a733_cpu_power_off_el1`.

## Board evidence needed

In scheduled board sessions only:

1. EL0 `WFI` with `nTWI`, woken by SGI0 and PPI30, on all eight cores under
   the scheduler.
2. `CPU_OFF` from the running cached scheduler with PPU OFF observed, and data
   the core wrote before power-down read back by core 0.
3. A second `CPU_ON` of one core in one boot, through warm entry to a restored
   idle task.
4. PPU OFF and ON times and `ChildrenAsleep` time against the 1 ms and 100 µs
   limits.
5. A core that does not return leads to a WDT0 reset in the resident
   lifetime, within the existing watchdog-expiry recovery trial.
