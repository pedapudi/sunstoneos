# Why the all-core first-task diagnostic does not return

Status: the cause is not established, but the order that returns is. The
first-task diagnostic has returned twice, each time after the all-eight-core
pre-scheduler had returned earlier in the same powered lifetime: on 2026-10-18
and, in its corrected form, on port 3-1 on 2026-10-24. It has failed twice,
each time as the first launch of its lifetime to power up a secondary: on
2026-10-23, and in its corrected form in a clean lifetime on 2026-10-24.
Section "The 2026-10-24 sequence on port 3-1 returned" gives the passing
sequence; section "Next board steps" gives the narrowest next diagnostic for
port 3-3. No board was accessed to produce this document.

## Summary

The all-core first-task diagnostic (rank 7 in the board test plan) brings all
eight CPUs to their first FreeRTOS task context, cleans the memory they wrote,
powers the seven secondary CPUs off and returns CPU0 to the boot ROM. It
returned on port 3-1 on 2026-10-18. On 2026-10-23 the same executable bytes,
launched on the same board, did not return: the host saw no new ROM USB
instance within the runner's 10-second window, and the old ROM instance stayed
listed.

The host side of the two attempts is equivalent. Every upload went to the same
address with the same size, and every executed byte is identical. The only
differences are a 16-byte nonce and the per-attempt epoch, generation, lease,
reservation and admission digest. The launcher survey reports the same CPU0
entry state. The 10-second window is more than five times the return time
observed on 2026-10-18.

The powered lifetimes differ. On 2026-10-18 the board ran only CPU
diagnostics, SRAM helpers and read-only observations between the cold DDR
start and the diagnostic. On 2026-10-23 it ran three FreeRTOS images under
the kernel secure monitor first: the single-core kernel check, the
forced-stall recovery case and the returning thermal application.

The diagnostic converts every failed check into an indefinite wait on CPU0. A
refused state check, an expired deadline and a hardware hang therefore produce
the same observation: no return.

The GIC-600 interrupt controller is clocked and out of reset in every
observed lifetime. Two read-only captures on port 3-3, one after the FreeRTOS
images and one in a clean lifetime, read every GIC-600 word as 0. Both used
`sunxi-fel read`, which has the ROM copy memory to the host. A third capture
in the clean lifetime read `GICD_TYPER` both ways: `sunxi-fel read` gave 0,
and `sunxi-fel readl`, which runs a 32-bit load on CPU0, gave `0x007b0408`.
The zero words therefore come from the copy path. The GIC needs no clock or
reset step before the diagnostic.

On 2026-10-24 the corrected diagnostic ran on port 3-1 in a clean lifetime:
power cycle, cold DDR, surveys, a 32-bit state capture that matched port 3-3
word for word, the cache-off helper and a survey. It did not return, and the
old ROM instance no longer answered `sunxi-fel version`, so CPU0 had left
the ROM. No FreeRTOS image had run, and no refusal report was recovered. Without a
returned report, neither entry into the admission nor completion of its reads
is observed. One known difference is that the 2026-10-18 lifetime ran five
native CPU diagnostics first, ending with the all-eight-core pre-scheduler.
Both failing launches were the first launch of their lifetime to request a
secondary power-up. Dependence on earlier diagnostic state and intermittent
failure remain hypotheses; the records do not establish their relative
probability.

The corrected diagnostic makes the state checks before it releases any
secondary. If a check fails, CPU0 records a failure code, the word's address
and the value read, and returns to the ROM, so the host gets a report instead
of a hang. With the state left as it is, the diagnostic runs the same
sequence as before; its instruction-model success report is byte-identical
to the earlier one.

The smaller CPU0 preparation diagnostic now isolates the shared prefix before
a decision about the preserved all-eight-core pre-scheduler proposal. The
latter retains a byte-identical overlay to the one that preceded the
2026-10-18 pass. Its successful return would establish shared startup and
cleanup in that lifetime. A missing return would leave the stop location
unknown; neither outcome alone proves a causal explanation.

## The two attempts sent the same bytes through the same transactions

The runner, `tools/run-native-startup-probe.py`, performed the same 31 FEL
transactions in the same order on both days. The 2026-10-18 runner is commit
`7ca22b0`; the 2026-10-23 runner is `210818b`. Their only change adds the
task-switch and workload profiles; the first-task path, the 15-second
per-transaction timeout and the 10-second return window are unchanged. Each
upload was read back and compared.

| Address | Bytes | Content | Same on both days |
| --- | --- | --- | --- |
| `0x42018000` | 20,992 | Zeroed primary and identity reports, nonce at offset `0x20` | Only the nonce differs |
| `0x41000000` | 74,496 | Native `.text` | Yes |
| `0x41012300` | 1,336 | Native `.rodata` | Yes |
| `0x41200000` | 33 | Native `.data` | Yes |
| `0x41020000` | 15,192 | Returning overlay | Yes |
| `0x42000000` | 16,384 | Zeroed result block | Yes |
| `0x42004000` | 4,096 | Startup contract | Epoch, generation, lease, reservation and admission digest differ |
| `0x52c00` | 128 | Launch request | Epoch, generation, lease and admission digest differ |
| `0x52d00` | 128 | Zeroed launch report | Yes |
| `0x52000` | 1,800 | A32 launcher | Yes |

The contract and request differ only at the byte offsets that
`request_bytes()` in `tools/check-a733-smp-supervised.py` assigns to those five
values. The build identifier and build address are identical. The epoch,
generation, lease and reservation are fresh 63-bit random values on both days.
Both preparation scripts, `all-core-first-task-20261018/prepare-physical.py`
and the bundle's `cpu-prepare.py`, copy the frozen prepared bundle and
regenerate only these values. The native ELF SHA-256 is
`762237cd…` on both days; the rebuilt overlay and segments are byte-identical
(`rank-7-first-task/overlay-comparison.json`).

The return timing excludes the window as a cause. On 2026-10-18 the launcher
upload completed at 21:56:29.4 and the runner read the returned report at
21:56:31.2, so the new ROM instance answered within 1.8 seconds. The
diagnostic itself ran for 126,307 counter ticks (5.3 ms at 24 MHz) under a
startup deadline of 0.996 seconds. On 2026-10-23 the launcher upload completed
at 23:02:25.6 and the runner recorded the failure at 23:02:35.7.

## The launcher survey and the saved memory show no relevant difference

The cache-off launcher surveys taken just before each launch agree in every
recorded field: SCTLR `0x00c50838`, CPSR `0x60000153`, MPIDR `0x81000000`,
LR `0x1a138`, SP `0xf2200`, RMR 0, CNTFRQ 0 and the thirteen saved registers.
All six launcher gates pass on both days. Only the counter differs: 3.50 hours
after power-on on 2026-10-18 and 2.32 hours on 2026-10-23.

The runner saves every DRAM and SRAM region the diagnostic owns before
uploading. A byte comparison of the two days' saved regions gives these
results:

- **Regions the runner overwrites** (native text, rodata, data, overlay,
  result block, launcher, request, launch report) differ widely. On
  2026-10-18 they held the earlier diagnostics' restored contents or the DDR
  test pattern; on 2026-10-23 the kernel and thermal images had loaded code at
  `0x41000000` and written the result block. The uploads replace all of it.
- **Regions the runner does not overwrite** (native `.bss` at `0x41201000`,
  monitor `.bss`, boot stack and translation-table area) differ in 0.6 to 1.3
  percent of their bytes. On both days roughly half their bytes are nonzero
  with the same pattern, which is the DRAM content after cold training. No image in either lifetime left its
  own data there. `a733_cpu_coherence_tables` zeroes and rebuilds both
  translation-table pairs at startup.
- **SRAM stack and scratch** differ by 72 and 8 bytes of residue above the
  launcher's stack and in the FEL scratch area. The launcher does not read
  them.

## The powered lifetimes differ

| Order | Port 3-1, 2026-10-18 (passed) | Port 3-1, 2026-10-23 (did not return) |
| --- | --- | --- |
| 1 | Cold DDR initialization, 12,288 MiB, instance `120` | Cold DDR initialization, 12,288 MiB, instance `002` |
| 2 | Launcher survey | Launcher surveys before and after DDR |
| 3 | **Read-only CPU power-state check: all seven secondaries OFF** | None |
| 4 | CPU0 cached EL3 diagnostic (returns as `121`) | **Single-core FreeRTOS kernel check** (returns as `010`) |
| 5 | **Read-only CPU0 interrupt baseline: all ten local words zero** | Cache-off helper and survey |
| 6 | CPU0 cached EL1 and local interrupt diagnostic (`002`) | **Forced-stall recovery, secure-timer return** (`012`) |
| 7 | CPU0 timer and interrupt diagnostic (`005`) | Idle 85 minutes |
| 8 | CPU0 production context diagnostic (`008`) | **Returning thermal application** (`014`) |
| 9 | **All-eight-core pre-scheduler diagnostic** (`010`) | Cache-off helper and survey |
| 10 | First-task diagnostic: returned as `067` | First-task diagnostic: no return |

On 2026-10-18 launcher surveys, and from step 7 the cache-off helper, also
preceded each launch; the table omits them.

Both lifetimes trained DDR with identical boot0, parameter and wrapper bytes,
and the firmware logs report the same frequencies and training results. The
handoff records a separate 5 V, 3 A supply for board 3-1, and the 2026-10-18
reconnect record confirms it for that day. The thermal application read 40 to 42 °C on all five sensors 31 seconds before the launch.

Three differences follow from the table:

- **FreeRTOS images under the kernel monitor.** No CPU diagnostic in the
  2026-10-18 series followed such an image in the same powered lifetime. The
  port 3-1 queue records for 2026-10-18 contain no kernel-check run.
- **No earlier secondary power-up.** On 2026-10-18 the pre-scheduler
  diagnostic had already powered up every secondary, run it and observed it
  OFF. The pre-scheduler diagnostic and the cold-108 identity diagnostic each
  passed as the first secondary power-up of a lifetime, so a first power-up
  is not by itself a new condition.
- **No observation of the state the diagnostic checks.** Every 2026-10-18
  CPU lifetime began with a read-only check of the seven secondaries' power
  status, control and mode words. The 2026-10-23 rationale states that
  secondaries were OFF at each earlier return, but nothing observed it.

## Every refusal in the diagnostic looks like a hang

`identity_primary_gate` in `boot/a733/secondary-identity-probe.S` calls
`identity_primary_run` and branches to `identity_park`, a permanent
`wfi` loop, whenever the result is not 1. Every check in
`identity_primary_run` returns 0 on failure, and so does every deadline wait.
A refusal therefore leaves CPU0 inside the diagnostic, the ROM never
reinitializes USB, and the host keeps its stale descriptor. That matches the
2026-10-23 observation. The stale descriptor is a host observation, not proof that CPU0 remained
running or that no reset or supply disturbance occurred. No returned report
or electrical measurement identifies the stop mechanism.

The checks that read state left by an earlier image, in execution order:

1. `secondary_distributor_begin` (`secondary-distributor-probe.c`) requires
   `GICD_CTLR & 0x70` equal to `0x30` with the RWP bit clear.
2. For each secondary CPU, `identity_primary_run` requires the power status
   word at `0x07052008 + n*0x1000` to have its low four bits clear, and the
   control and mode words at `0x07050200 + 4n` and `0x07050220 + 4n` to have
   bit 0 clear.
3. On each secondary, `secondary_local_probe` requires its redistributor to
   report the expected affinity, a powered and stable `GICR_PWRR`, `GICR_CTLR`
   0, `GICR_WAKER` 6, and no enabled, pending or active SGI or PPI.
4. After all seven secondaries are released, `primary_local_cache_probe`
   requires CPU0's `CNTFRQ_EL0` to be 24 MHz and CPU0's redistributor to have
   `GICR_CTLR`, `GICR_WAKER` and the enable, pending and active words all
   zero. Any other value sets error `0x1001` or `0x1002` and returns 2, which
   `identity_primary_run` treats as failure.
5. In `identity_pre_scheduler_el1` (`all-core-first-task-probe.S`), each core
   at EL1 parks if `ICC_IGRPEN1_EL1` is nonzero. This check cannot refuse:
   every core enters EL1 through `a733_cached_enter_el1`
   (`platform/a733-smp-cached/continue.S`), which first writes 0 to
   `ICC_IGRPEN1_EL3` and so clears both Group 1 enables.

A refusal at check 4 or 5 happens after the secondaries are running. They then
wait for CPU0's completion record until their own deadline and park at EL1.
No later step can power them off, which is why the current design cannot
return after such a refusal.

The kernel monitor (`platform/a733/monitor.c`) restores most of what it
changes, but not all of it. On return it clears the pending and active state
of PPI 29 (the secure timer) and PPI 30 (the tick) only, restores CPU0's
group, modifier, configuration, PPI priorities and enables to their values at
monitor entry, and leaves `GICR_WAKER` awake. It stops the secure and EL1
physical timers. It does not touch the secondaries. CPU0's GIC CPU-interface
and EL2/EL3 control registers are left to the RMR warm reset. The comment in
`primary-local-cache-probe.c` relies on that reset; no board observation
establishes the warm-reset value of `ICC_IGRPEN1_EL1`, `ICC_SRE_EL3`,
`SCR_EL3` or the EL2 controls after a FreeRTOS image.

## The GIC-600 is clocked and out of reset after every cold start

The GIC-600 has one clock gate in the main clock controller (CCU, base
`0x02002000`) and three outer resets in the CPU subsystem. The A733 User
Manual V1.00 and the pinned Linux CCU driver (`ccu-sun60iw2.c`, SHA-256
`1d32bb85…`) give these words:

| Word | Address | Field | Reset value | Read on port 3-3, clean lifetime |
| --- | --- | --- | --- | --- |
| `GIC_CLK_REG` (manual 4.1.6.71) | `0x02002560` | Bit 31 gate, bits 26..24 source, bits 4..0 divider | `0x80000000`: gate on, 24 MHz | `0x84000003`: gate on, 400 MHz peripheral PLL divided by 4 |
| `CPU_PERI_CLK_REG` (4.1.6.72) | `0x02002568` | Same layout | `0x80000000` | `0x84000003` |
| `GENER_CTRL_REG0` (5.1.6.1) | `0x08000000` | Bit 1 `GICCDISABLE` | 0 | 0 |
| `CPU_SYS_RST_CTRL` (5.1.6.2) | `0x0800000C` | Bits 6, 5, 4: GIC outer debug, outer and outer MBIST resets, 1 = de-asserted | `0x7F` | `0x7F` |
| `CPU_DSU_PLL_CLK_REG` (5.1.10.37) | `0x0887301C` | CPU-side GIC clock = DSU clock / M1, bits 3..2 | `0x105` | `0x03000105` |

The driver declares the GIC clock with `CLK_IGNORE_UNUSED` and has no GIC
reset line. The device tree node `interrupt-controller@3400000` names no clock
or reset.

The boot ROM never closes the gate. On every entry the routine at ROM offset
`0x23dc` clears only the source and divider fields of `0x02002560` and
`0x02002568` (helper `0x1d04`), and the clock setup at `0x21cc` then selects
source 4 with divider 4 (helper `0x1cbc`). Neither helper writes bit 31. The
ROM contains no literal for the reset or cluster control words. The
disassembly is `rom-copy-20261016-084055/physical-rom.dis`.

Nothing in this repository writes these words. No source names
`0x02002560`, `0x0800000C`, `0x08868000` (cluster `C0_RST_CTRL`, GIC inner
reset bit 4) or `0x0886800C` (cluster clock-gating override). None of them
appears as a literal in the loaded kernel-check image
(`board-session-20261023/images/a733-kernel-check.bin`), the loaded thermal
image (`board-bundles-20261023/rank-6-thermal/probe.bin`) or the frozen
supervised ELF of the diagnostic. The kernel monitor (`platform/a733/monitor.c`, `monitor.S`) writes
only GIC distributor and redistributor registers, the CPU0 reset vector
`0x08001004` and `RMR_EL3`. The cache-off helper (`boot/a733/cache-off.S`)
clears `SCTLR.I` only. The DDR wrapper runs the vendor boot0, which ran in
every lifetime, including those in which the GIC read correctly.

Every image read the GIC with 32-bit loads and got real values, in every
lifetime:

- On 2026-10-23, the kernel check was the first image after cold DDR on both
  boards. Its monitor read `GICD_CTLR` `0x37` and `GICD_TYPER` `0x007b0408`
  at entry, as did the forced-stall and thermal images that followed
  (`port-3-1/kernel`, `timeout`, `r6-thermal` and the same on `port-3-3`).
- On 2026-10-18, `sunxi-fel readl` read the same two values
  (`primary-gic-state-121-20261018`, `gic600-registers-20261018-089`). Before
  those reads the two lifetimes ran only cold DDR, surveys, a read-only CPU
  power check and one native CPU diagnostic each. None of these steps writes
  a GIC clock or reset.

No step enabled the GIC on 2026-10-18; the gate is on from reset. The
diagnostic's preparation needs no GIC clock or reset step, and none is added:
no source shows a sequence that would be needed.

## A ROM memory copy reads every GIC-600 register as 0

`sunxi-fel read ADDRESS 4` sends the ROM's FEL read request, and the ROM
copies the memory to the host. `sunxi-fel readl ADDRESS` instead uploads a
12-word A32 stub to the FEL scratch area at `0x71000`, executes it, and reads
back the value of one `LDR` (`fel_lib.c`, `aw_fel_readl_n`). The ROM's FEL
copy code lies outside the 68 KiB captured ROM (the dispatcher jumps to
`0x17000`), so its access width is not known. The GIC architecture requires
support only for 32-bit accesses to these registers (and 64-bit accesses to
the 64-bit ones). For any other access it states that "it is UNPREDICTABLE
whether: the access generates an external abort or not; the defined
side-effects of a read occur or not. A read returns UNKNOWN data" (IHI 0069G,
section 12.1.3).

Three port 3-3 captures compare the two methods:

| Capture | Lifetime | Method | GIC words | Power and entry-configuration words |
| --- | --- | --- | --- | --- |
| `port-3-3/state-capture` | After the kernel, forced-stall and thermal images | `read`, 89 words | All 0 | Status `0x100`, control, mode and entry words 0 |
| `port-3-3/state-capture-clean` | Power cycle, cold DDR, surveys | `read`, 89 words | All 0 | Same |
| `port-3-3/gic-clock-capture` | Same clean lifetime | `read` and `readl` | `read`: `GICD_TYPER` 0 before and after. `readl`: the values below | Not read |

The `readl` values in the clean lifetime match 2026-10-18 and satisfy every
predicate of the diagnostic that they cover: `GICD_CTLR` `0x37`,
`GICD_TYPER` `0x007b0408`; CPU0 redistributor `GICR_PWRR`, `GICR_CTLR`,
`GICR_WAKER` and the enable, pending, active, group, modifier, configuration
and priority words all 0; secondary frame n `GICR_TYPER`
`(n << 40) | (n << 8) | 0x21`, with `0x10` added on frame 7 (low word `0x121`
on frame 1, `0x731` on frame 7), `GICR_PWRR` `n << 8`, `GICR_CTLR` 0 and
`GICR_WAKER` 6. The FEL scratch bytes were restored and compared.

The CPU power, control and mode words read the same values with `read` as
`readl` read on 2026-10-18, so the copy path returns correct values for those
register blocks. For GIC-600 registers the method must be `readl`. The
corrected capture script, `board-session-20261023/state-capture-readl.py`,
reads the same 89 words and the two GIC gate and reset words with `readl`. It
saves and restores the FEL scratch bytes, and it reads the GIC only when the
gate is open and the resets are de-asserted. `state-capture.py` is unchanged
because two queue proposals pin its SHA-256.

## MBUS at the failing instruction is unobserved

The rank-9 USB input builder refused the 2026-10-23 port 3-3 capture
(`CCU+0x588` = `0x84000000`: source 4, divider 1, 400 MHz). The 600 MHz
reference it reproduces comes from port 3-3 instance `064` on 2026-10-18,
which is a different board and a different powered lifetime from the one in
which the first-task diagnostic passed.

Every recorded MBUS word fits one rule: boot0's DDR initialization sets MBUS
to 600 MHz (`0x81000000`), and every later entry into the ROM sets it back to
400 MHz.

| Board, instance | Steps since cold DDR | MBUS word |
| --- | --- | --- |
| 3-3, `088` (2026-10-18) | DDR and surveys in the same ROM instance | `0x81000000`, 600 MHz |
| 3-3, `064` (2026-10-18) | DDR and survey in the same ROM instance | `0x81000000`, 600 MHz |
| 3-3, `119` (2026-10-18) | DDR at `117`, PMIC kernel image, ROM re-entry | `0x84000000`, 400 MHz |
| 3-3, `125` (2026-10-18) | DDR at `122`, ROM re-entries | `0x84000000`, 400 MHz |
| 3-3, `056` (2026-10-18) | Warm lifetime with many ROM re-entries | `0x84000000`, 400 MHz |
| 3-1, `082` (2026-10-17) | DDR at `073`, ROM re-entries | `0x84000000`, 400 MHz |
| 3-3, `013` (2026-10-23) | DDR at `004`, kernel, forced stall, thermal | `0x84000000`, 400 MHz |

The captured secure ROM contains the code that resets it. The function at
ROM offset `0x2084` writes `MBUS_CLK_REG` (`0x02002588`): it closes the gate,
clears the divider, selects source 4 (`0x04000000`), sets the update bit 27
and reopens the gate. Every path through `0x2444` reaches it, through
`0x2160` or `0x21cc`. The FEL dispatcher at `0x2514` calls `0x2444` when its
argument is 127, and the normal boot path calls it at `0xba8`. The
disassembly is
`~/.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.dis`.

Answers to the three questions about MBUS:

1. **No image or step set MBUS to 600 MHz before the 2026-10-18 diagnostic
   passed.** Board 3-1's 2026-10-18 lifetime has no MBUS observation. Boot0
   set 600 MHz during cold DDR initialization at instance `120`. The CPU0
   cached EL3 diagnostic then returned through the ROM, and four more
   diagnostics followed, each ending with a ROM entry. Under the rule above,
   MBUS ran at 400 MHz from instance `121` onwards, as it did on 2026-10-23.
2. **The diagnostic does not program MBUS or NSI rates.** Its deadlines use
   the 24 MHz generic counter. Memory and interconnect timing can still
   affect execution. Neither the overlay nor the native ELF contains a CCU fabric
   address in a literal pool. The only native code that builds a CCU
   address, in `a733_usb_prepare`, is unreachable from the overlay's entry.
   DSU and interconnect settings are system-register writes independent of
   the bus clocks. The 5.3 ms passing duration is one measurement; it does
   not establish a worst-case bound for a failed run or another lifetime.
3. **The clean 2026-10-24 lifetime differs.** It contains no native return
   through an RMR reset before the failed launch. The cache-off helper ends
   with `BX LR`, and the survey and capture preserve the same USB instance.
   No record captures MBUS in that lifetime. Under the ROM sequence above,
   it could still have boot0's 600 MHz setting. Neither equality nor a clock
   cause is established for that attempt.

**Where 600 MHz belongs.** Boot0 selects 600 MHz, and a production boot that
never re-enters the ROM keeps it. In a returning
test lifetime the ROM restores 400 MHz on the first return, so a boot step
that raised it would be undone by the next image. Images should therefore
admit the observed fabric configuration and derive rate-dependent values from
it. The USB provider in `drivers/usb/a733_usb_platform.c` already accepts any
fabric word whose source and divider decode to its stated rate, including
source 4. In the rank-9 path the refusal comes from the builder's fixed
requirement of 200, 600 and 600 MHz; the corrected GPU candidate already
derives its MBUS rate from the observed word. Raising MBUS to 600 MHz is a
production performance choice. It belongs in the stage-2 boot path, before
any DMA master starts, using the update-bit sequence.

## The corrected diagnostic did not return in a clean lifetime

The 2026-10-24 launch on port 3-1 (`port-3-1/r7c-prepare/`) followed a power
cycle, a cold DDR initialization (`ddr-2`), surveys, the 32-bit state capture
(`state-capture-readl`), the cache-off helper (`r7c-cache-off`) and a survey
(`r7c-survey`). The runner reported `No new FEL instance; no protocol retry.`
after `exe 0x52000`, with no transport failure. Afterwards `sunxi-fel version`
on the old instance timed out (`port-3-1/r7c-stale-probe/version.txt`), so
the launcher had not refused: a refusal returns to the same ROM FEL loop,
which would still answer.

Every input and step of the three launches compares as follows:

| Item | 2026-10-18 (returned) | 2026-10-23 and 2026-10-24 (no return) |
| --- | --- | --- |
| Runner | `7ca22b0` | `210818b`; current `bringup` |
| FEL transactions to `exe 0x52000` | 31: backups, uploads with readback, launch | Same order, addresses and sizes |
| Launcher at `0x52000` | SHA-256 `2b0641c2…` | Same |
| Native segments | `c29a8952…`, `360d6e73…`, `55b5a6e3…` | Same |
| Overlay | `b6382b8c…` | Same on 2026-10-23; on 2026-10-24 the corrected overlay `33698031…`, which differs only by the pre-release admission |
| Cache-off helper before the launch | Yes, helper SHA-256 `1417834e…` (queue row 165) | Yes, same helper |
| Launcher survey | SCTLR `0x00c50838`, CPSR `0x60000153`, MPIDR `0x81000000`, LR `0x1a138`, SP `0xf2200`, RMR 0, CNTFRQ 0, all six gates pass | Same; on 2026-10-24 the saved `r12` reads `0xcac69d82` instead of 0, a scratch register the launcher saves and restores without reading |
| DDR initialization | Wrapper tool `4a54cdc1…`; same head, body, entry and parameter uploads | Same uploads; same frequencies, training results and 12,288 MiB |
| MBUS | 400 MHz inferred from ROM re-entry; no lifetime capture | 2026-10-23: 400 MHz inferred after ROM re-entry. 2026-10-24: unobserved; no earlier RMR return |
| Native `.bss` and monitor `.bss` | Zeroed by `a733_smp_entry` before use | Same code |
| Lifetime before the launch | Cold DDR, CPU power check, five native CPU diagnostics: CPU0 cached EL3, CPU0 cached EL1 with local GIC, CPU0 timer interrupt, CPU0 production context, all-eight-core pre-scheduler (queue rows 148 to 163) | 2026-10-23: three FreeRTOS images under the kernel monitor. 2026-10-24: none; the state capture only. In both, no secondary had been powered up |

The CPU0 cached EL3 diagnostic returned as the first native launch after
cold DDR on 2026-10-18 (queue row 148). That result demonstrates the native
startup, `a733_cached_prepare` and base publication overlay under those
inputs. It does not locate the 2026-10-24 stop. The preceding 32-bit capture
establishes the sampled register values at capture time; admission execution
and secondary release remain unobserved. CPU0 can park before admission,
stall within a read, or fail later during execution or cleanup.

## The 2026-10-24 sequence on port 3-1 returned

After a power removal, port 3-1 ran in one clean lifetime, in this order and
all with the frozen native ELF `762237cd…`
(`processor-completion-20261024/port-3-1/`, each at `runtime/session.json`):

| Time (UTC−7) | Run | Result |
| --- | --- | --- |
| before 09:22 | Cold DDR (`ddr`), 21-word secondary-power capture (`cpu-off`) | Seven secondaries OFF |
| 09:22 | CPU0-only preparation (`primary`), package `cpu-startup-resolution-20261024/` | Returned |
| 09:26 | 91-word CPU power, CPU configuration and GIC-600 capture (`state-after-primary`) | Recorded |
| 09:30 | All-eight-core pre-scheduler (`pre-scheduler`), package `cpu-diagnosis-20261024/pre-scheduler/` | Returned: eight participants, 256 increments, cleanup, seven secondaries OFF |
| 09:44 | Corrected first-task diagnostic (`first-task`), package `cpu-next-20261024/first-task/` | Returned |
| 09:52 | Task switching (`task-switch`) | Returned |
| 09:58 | A 24-word capture of the GIC clock and resets, the distributor and CPU0's redistributor (`state-after-thermal`) | Every word as expected; one record-only word, `0x0347041c` |
| 09:59 | Full returning workload (`task-workload`) | Passed: 32 floating-point rounds per core, background tasks, SGIs on all cores, CPU0 ticks |
| 10:57, 11:17 | Current-source CPU workload (`9d90b6f9…`), then with one physical timer interrupt on each secondary (`f5ba965d…`) | Passed |

The first-task run of this lifetime is the corrected overlay that did not
return in the clean lifetime of `r7c-prepare`. Its only identified difference
from that launch is the order: here it followed the CPU0-only preparation and
the pre-scheduler.

The two captures bracket the pre-scheduler, the first-task run and task
switching. Thirteen words are common to both, the GIC clock and resets, the
distributor and CPU0's redistributor, and all 13 match. The 78 other words of
the first capture, the secondaries' power-controller words and their
redistributor frames, were not read again after the pre-scheduler. The state
that the pre-scheduler could leave for the first-task run is therefore
unobserved: those 78 words, system registers that FEL cannot read (the DSU
cluster registers and CPU0's registers whose warm-reset value is UNKNOWN),
the secondaries' SGI and PPI enable, pending and active words, and cache
contents. Two passes after the pre-scheduler and two failures without it do
not distinguish an ordering dependence from intermittency. The offline
diagnosis owed is that comparison: the 91 words before and after the first
secondary power-up of a lifetime, which the next diagnostic records.

## Primary-source constraints on the next diagnostic

The pinned [Arm/Linux comparison](../a733-multicore-reference-audit.md)
separates CPU release, redistributor power, wake, CPU-interface access and
interrupt delivery. Those boundaries prevent three unsupported corrections:

- Arm IHI 0069G section 12.1.3 permits unknown data for unsupported access
  sizes. A zero returned by the ROM copy path does not prove a clock fault;
  the matched 32-bit captures supply the relevant GIC observations.
- Section 11.1 explicitly permits SRE writes while the redistributor sleeps.
  The differing Linux and TF-A orderings do not justify changing that order
  to explain an unlocated stop.
- Linux `cpu_psci_cpu_boot()` delegates power-on to firmware. Its kernel
  sequence cannot replace the direct EL3 power and shutdown contracts.

Linux's pinned boot contract also requires instruction visibility and image
publication to the point of coherency. Upload readback alone does not satisfy
that requirement. The pre-scheduler keeps the accepted Nonsecure publication
and owned-line cleanup; a successful report must cover both execution and
cleanup. A synchronous exception handler cannot recover a bus access that
never completes.

The cached Linux sources are pinned by `sources.json` in
`multicore-reference-audit-20261018/`, at revision
`e5f0a698b34ed76002dc5cff3804a61c80233a7a`. The Arm text hashes are in
`sources/cpu-audit/reference-cache.json`, beneath the SunstoneOS cache.

## Hypotheses and evidence limits

### 1. The first-task run depends on state that an earlier native diagnostic establishes

For:

- On port 3-1 on 2026-10-24 the corrected first-task diagnostic returned
  after the CPU0-only preparation and the pre-scheduler, in a clean lifetime
  in which the same overlay had not returned as the first secondary power-up.

- The only passing run followed five native diagnostics in its powered
  lifetime, the last of them the all-eight-core pre-scheduler, which ran the
  same native startup, secondary power-up, cached entry and production
  barrier.
- Both failing runs were the first launch of their lifetime to power up a
  secondary, once after FreeRTOS images and once in a clean lifetime.
- Some state survives both CPU0's RMR warm reset and a secondary's power
  cycle: DSU cluster registers, CPU0 system registers whose warm-reset value is
  UNKNOWN, and GIC redistributor words that no check covers.
  The native image contains a `CLUSTERACTLR_EL1` writer, but its plan selects
  no shared-cluster workaround for the observed DSU revision `0x41`. Its
  presence does not establish a cluster-register change in these runs.

Against:

- No mechanism is identified.
- The pre-scheduler and the sequential identity diagnostic each passed as the
  first secondary power-up of a lifetime, so a first power-up is not by
  itself fatal for the shared path.

### 2. The first-task interval fails intermittently

For:

- One of three first-task launches returned.
- Every launch of a complete eight-core FreeRTOS observer image failed (port
  3-1 queue rows 9, 12, 16, 113 and 141), and their failure stages were never
  located. The first-task interval, from the barrier to the first task
  context on all eight cores, is the part of the startup that those images
  share with it and that the pre-scheduler does not run.

Against:

- The passing run took 5.3 ms against a one-second deadline. No returned
  timing record exists for either failed run, so this does not bound their
  duration or exclude a timing-dependent failure.

### 3. What the comparisons exclude or leave open

- **Earlier FreeRTOS images are not a necessary trigger.** The clean-lifetime
  launch failed without them.
- **Captured secondary power and GIC words match.** Uncaptured state and
  subsequent transitions remain open; the admission produced no report.
- **A clean launcher refusal is unsupported.** The old ROM did not answer,
  but no launcher report establishes where execution stopped.
- **Compared host inputs match.** The native payloads and recorded upload
  transactions agree. The corrected overlay adds admission, and the sessions
  have distinct epochs and lifetime histories. DDR training and upload
  readback do not qualify memory behavior during concurrent cached execution.
- **MBUS is not an established difference.** The 400 MHz value for the passing
  lifetime is inferred from ROM code and intervening returns, not a capture
  from that lifetime.
- **Supply and temperature remain unmeasured during the failing interval.**
  The separate supply rating and earlier thermal readings do not exclude a
  transient fault.

## The corrected diagnostic checks the inherited state before releasing any secondary

The corrected first-task overlay adds one function,
`identity_first_task_admit`, at the end of
`boot/a733/all-core-first-task-probe.S`. The builder
(`tools/prepare-secondary-identity-probe.py --first-task`) links the overlay
with `--wrap=identity_primary_run`, so `identity_primary_gate` calls the new
function before the sequential survey. The function uses no stack and writes
no device register. It reads the startup contract and the report with 64-bit
loads, `CNTFRQ_EL0` as a system register, `GICR_TYPER` with 64-bit loads and
every other GIC and power word with a 32-bit load. It applies to each word
the predicate of the later check that reads the same word. If every word passes, it branches to `identity_primary_run`, and
the run continues as before with the report untouched.

On the first failing word it writes a refusal into the identity report header
at `0x42018100` and returns to the ROM through the base overlay's existing
RVBAR and `RMR_EL3` sequence. No secondary has been released at that point.
In the saved `report.bin`, which starts at `0x42018000`, the refusal fields
are:

| Offset | Field |
| --- | --- |
| `0x130` | State 3 (refused before release); a completed run writes 2 |
| `0x138` | Core whose word failed; 0 for CPU0 and the distributor |
| `0x160` | Failure code |
| `0x168` | Address of the failing word; 0 for a system register |
| `0x170` | Value read |

The host profile (`tools/all-core-first-task-host.py`) decodes the refusal
and raises `Admission refused before any secondary was released: code …
(reason), core …, word … read …`. The runner then restores the saved DRAM,
marks the ROM usable, records that message as the failure and does not
quarantine the instance.

| Code | Word | Required | Later check that applies it |
| --- | --- | --- | --- |
| `0x01` | Startup contract epoch and lease (`0x42004018`, `0x42004070`) | Nonzero | `identity_primary_run` |
| `0x02` | Startup deadline (`0x42004058`) | Counter below it | `identity_primary_run` |
| `0x03` | Identity report `0x42018100`..`0x4201d200` | All zero | `identity_primary_run`, `secondary_distributor_begin`, `primary_local_cache_probe` |
| `0x04` | `CNTFRQ_EL0` | 24,000,000 | `primary_local_cache_probe` |
| `0x10` | `GICD_TYPER` `0x03400004` | Nonzero (distributor readable) | New |
| `0x11` | `GICD_CTLR` `0x03400000` | RWP clear, bits 6..4 equal to `0b011` | `secondary_distributor_begin` |
| `0x12` | `GICR_TYPER` of frames 0 to 7 (`0x03460008 + n*0x20000`) | Affinity n, VLPIS clear, last-frame bit on frame 7 only | `a733_cpu_continue` scan, `secondary_local_probe` |
| `0x20` | Power status `0x07052008 + n*0x1000`, cores 1 to 7 | Low four bits 0 | `identity_primary_run` |
| `0x21` | Power control `0x07050200 + 4n` | Bit 0 clear | `identity_primary_run`, `a733_cpu_continue` |
| `0x22` | Power mode `0x07050220 + 4n` | Bit 0 clear | `identity_primary_run`, `a733_cpu_continue` |
| `0x23` | Secondary `GICR_PWRR` (frame + `0x24`) | RDPD clear, bits 2 and 3 equal | `secondary_local_probe` |
| `0x24` | Secondary `GICR_CTLR` (frame + 0) | 0 | `secondary_local_probe` |
| `0x25` | Secondary `GICR_WAKER` (frame + `0x14`) | 6 | `secondary_local_probe` |
| `0x26` to `0x28` | Secondary `GICR_ISENABLER0`, `ISPENDR0`, `ISACTIVER0` (frame + `0x10100`, `0x10200`, `0x10300`) | 0 | `secondary_local_probe` |
| `0x30` | CPU0 `GICR_PWRR` `0x03460024` | RDPD clear, bits 2 and 3 equal | `primary_local_cache_probe` |
| `0x31` to `0x35` | CPU0 `GICR_CTLR`, `GICR_WAKER`, `ISENABLER0`, `ISPENDR0`, `ISACTIVER0` | 0 | `primary_local_cache_probe` |

The secondary words are checked core by core, `GICR_PWRR` before the other
words of the same frame, and CPU0's frame last.

Other failures still end without a return. They fall into four groups, in
execution order:

- **CPU0 before the admission.** The native startup `a733_smp_boot`
  (`platform/a733-smp-cached/boot.c`) stops in a `wfe` loop through
  `a733_smp_stop` if CPU0's entry state, the startup contract, or the
  watchdog, FEL and redirect words differ from the admission. The base
  overlay `primary_publication_probe` (`boot/a733/primary-publication-probe.S`)
  then branches to `primary_publication_park` if CPU0 is not at EL3, is not
  the boot CPU, has interrupts unmasked or has a different CPU revision or
  cluster identification. It parks again if `probe_production_prepare`
  (`a733_cached_prepare`) leaves `SCTLR_EL3`, `TCR_EL3`, `TTBR0_EL3` or
  `MAIR_EL3` changed or `SCTLR_EL1` and `TCR_EL1` unrestored. A stop inside
  `a733_cached_prepare` also ends without a return.
- **The admission itself.** A GIC or power-controller read that stalls does
  not return.
- **CPU0 after the admission, before release.** `identity_primary_run`
  repeats its own entry checks. It parks if CPU0 is not at EL3, is not the
  boot CPU, has an `SCTLR_EL3` bit of `A733_ENTRY_SCTLR_REJECT`
  (`0x02001005`) set or has interrupts unmasked. It also parks if the
  deadline expires between the admission and these checks. `secondary_distributor_begin`
  parks if a distributor RWP wait reaches the deadline or a readback after
  its distributor writes differs.
- **After release**, when running secondaries cannot be brought to an
  observed OFF: each secondary's checks of its own identity and control
  registers, the power-on, wake and configuration handshakes of
  `a733_cpu_continue`, the cached EL1 entry, CPU0's own continue and EL3
  cache steps, every cleanup readback and every deadline that expires after
  release.

The primary report's state field records how far the base overlay got, and
the native result block records an `a733_smp_stop` status, but the host reads
neither without a return.

The executed bytes change only in the identity extension of the overlay. The
extension grows from 11,096 to 11,736 bytes of its 12,288-byte reservation;
the new function follows every existing hook, so no native patch target moves
and the three native payload segments are byte-identical to the accepted
build. The task-switch and workload profiles are built without the wrap and
are byte-identical to their accepted builds. Their manifests pin the digests
of the two changed sources, so the loaders refuse the frozen workload bundle
(`rank-8-workload/`, "Identity source changed.") until its manifest and
evidence are regenerated.

## Next board steps

Port 3-1 has run the full sequence in one lifetime (section "The 2026-10-24
sequence on port 3-1 returned"). Port 3-3 has never powered up a secondary.
The narrowest next diagnostic there repeats the start of that sequence, in
the same order and with the same frozen packages, and records the state
around the first secondary power-up:

1. The 21-word secondary-power capture and the CPU0-only preparation
   (`cpu-startup-resolution-20261024/`). It writes no GIC or secondary-power
   register.
2. The 91-word capture, the all-eight-core pre-scheduler
   (`cpu-diagnosis-20261024/pre-scheduler/`), which is the first secondary
   power-up of the lifetime, and the same capture again, compared word for
   word with the first. Port 3-1's lifetime did not repeat the secondaries'
   78 words after its pre-scheduler, so this comparison is new.

Both packages bind only to port 3-1. Port 3-3 bindings, with admission
conditions that state port 3-3's lifetime, are frozen with set
`board-bundles-20261025-1`. That lifetime has run returning CPU0 images (the
base kernel check, bundle 6 four times and rank 9) since its cold DDR start,
unlike the clean lifetime that passed on port 3-1, so a pass there would
extend the sequence to that history and a non-return would not separate it
from the first power-up. A pre-scheduler that does not return leaves the
stop location unknown and needs a power removal; it is not retried. After
both return, the first-task, task-switch and task-workload profiles of
`cpu-next-20261024/` complete ranks 7 and 8 on port 3-3 in the same order as
on port 3-1; their port 3-3 bindings follow the same pattern and are not yet
prepared.

## Evidence

All paths are under `~/.cache/sunstoneos/evidence/`.

- 2026-10-23 attempt: `board-session-20261023/port-3-1/r7-prepare/` (prepared
  bundle, runtime snapshots and `runtime/session.json`); lifetime records in
  the sibling `ddr/`, `kernel/`, `timeout/`, `r6-thermal/`, `r7-survey/`.
- 2026-10-18 pass: `all-core-first-task-20261018/physical-010/` and
  `warm-010/ready/survey/`; lifetime records `reconnect-20261018-120/`,
  `primary-gic-state-121-20261018/`, `reconnect-20261018-086/`,
  `gic600-registers-20261018-089/`; queue history in
  `~/.cache/sunstoneos/board-tests.sqlite3`.
- Port 3-3 captures: `board-session-20261023/port-3-3/state-capture/`,
  `state-capture-clean/` and `gic-clock-capture/`; scripts
  `board-session-20261023/state-capture.py`, `gic-clock-capture.py` and
  `state-capture-readl.py`.
- GIC words read by the monitor at each image entry: `result` in
  `board-session-20261023/port-3-{1,3}/{kernel,timeout,r6-thermal}/session.json`.
- 2026-10-24 attempt: `board-session-20261023/port-3-1/r7c-prepare/` and the
  stale-instance probe `r7c-stale-probe/`; lifetime records `ddr-2/`,
  `survey-after-ddr-2/`, `state-capture-readl/`, `r7c-cache-off/`,
  `r7c-survey/`.
- 2026-10-18 lifetime before the pass: `primary-cache-probe-20261018/physical-120/`,
  `primary-el1-probe-20261018/physical-121/`,
  `primary-interrupt-probe-20261018/physical-002/`,
  `primary-context-probe-20261018/physical-005/`,
  `all-core-pre-scheduler-20261018/entry-corrected/physical-008/`.
- Offline revalidation and frozen pre-scheduler package:
  `cpu-diagnosis-20261024/ready-proposal.json`, `snapshot/manifest.json` and
  `offline-validation.json`; scoped model and wrapper evidence are in that
  package. The original bundles remain preserved.
- Bundles: `board-bundles-20261023/rank-7-first-task/` (superseded),
  `board-bundles-20261023/rank-7-corrected/` and
  `board-bundles-20261023/rank-7-pre-scheduler/`.
- MBUS observations: `usb-first-fault-20261018/fabric-064/`,
  `usb-returning-20261018/fabric-056/`,
  `second-board-20261018-3-3/sd-pin-clock-state/`,
  `npu-single-core-20261018/physical-candidate/npu-state/`,
  `second-board-cold-122-20261018/accelerator-state/npu-state/`,
  `crypto-clocks-20261018-082/`, `board-session-20261023/port-3-3/r9-capture/`;
  DDR initialization records for each lifetime under `*/ddr*/`.
- Vendor sources under `~/.cache/sunstoneos/sources/`:
  `camera-research/A733_User_Manual_V1.00.txt` (sections 4.1.6.71, 4.1.6.72,
  5.1.6, 5.1.9 and 5.1.10), `board-kernel/ccu-sun60iw2.c`,
  `board-kernel/sun60iw2p1.dtsi`, `cpu-audit/arm-gic-architecture-ihi0069g.txt`
  (section 12.1.3), `sunxi-tools-b92f4ed3115a520e3ec406697d268239e3f29488/fel_lib.c`;
  ROM disassembly `rom-copy-20261016-084055/physical-rom.dis`.
- Repository sources: `boot/a733/secondary-identity-probe.{c,S}`,
  `boot/a733/all-core-first-task-probe.S`,
  `boot/a733/primary-local-cache-probe.c`,
  `boot/a733/secondary-local-probe.c`,
  `boot/a733/secondary-distributor-probe.c`, `platform/a733/cpu_continue.c`,
  `platform/a733-smp-cached/continue.S`, `platform/a733/monitor.{c,S}`,
  `tools/prepare-secondary-identity-probe.py`,
  `tools/all-core-first-task-host.py`, `tools/run-native-startup-probe.py`.
