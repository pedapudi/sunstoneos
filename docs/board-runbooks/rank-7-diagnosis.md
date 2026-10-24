# Why the all-core first-task diagnostic did not return on 2026-10-23

Status: the cause is not established. The read-only captures on port 3-3 are
complete. The corrected diagnostic is built and passes its offline checks
(`board-bundles-20261023/rank-7-corrected/`; runbook
[rank-7-corrected.md](rank-7-corrected.md)). No board was accessed to
produce this document.

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
the same observation: no return. The two remaining explanations are a refused
state check on state that the FreeRTOS images left, and an intermittent
failure of the diagnostic itself.

The GIC-600 interrupt controller is clocked and out of reset in every
observed lifetime. Two read-only captures on port 3-3, one after the FreeRTOS
images and one in a clean lifetime, read every GIC-600 word as 0. Both used
`sunxi-fel read`, which has the ROM copy memory to the host. A third capture
in the clean lifetime read `GICD_TYPER` both ways: `sunxi-fel read` gave 0,
and `sunxi-fel readl`, which runs a 32-bit load on CPU0, gave `0x007b0408`.
The zero words therefore come from the copy path. The GIC needs no clock or
reset step before the diagnostic.

The corrected diagnostic makes the state checks before it releases any
secondary. If a check fails, CPU0 records a failure code, the word's address
and the value read, and returns to the ROM, so the host gets a report instead
of a hang. With the state left as it is, the diagnostic runs the same
sequence as before; its instruction-model success report is byte-identical
to the earlier one.

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
2026-10-23 observation. The stale descriptor also shows that the SoC did not
reset: a watchdog, thermal or supply reset would have disconnected the USB
device.

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
`0x17000`), so its access width is not known. The GIC architecture supports
only 32-bit and 64-bit accesses to these registers; any other access returns
UNKNOWN data (IHI 0069G, section 12.1.3).

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
and priority words all 0; secondary frame n `GICR_TYPER` `(n << 40) | 0x21`
(with `0x10` added on frame 7), `GICR_PWRR` `n << 8`, `GICR_CTLR` 0 and
`GICR_WAKER` 6. The FEL scratch bytes were restored and compared.

The CPU power, control and mode words read the same values with `read` as
`readl` read on 2026-10-18, so the copy path returns correct values for those
register blocks. For GIC-600 registers the method must be `readl`. The
corrected capture script, `board-session-20261023/state-capture-readl.py`,
reads the same 89 words and the two GIC gate and reset words with `readl`. It
saves and restores the FEL scratch bytes, and it reads the GIC only when the
gate is open and the resets are de-asserted. `state-capture.py` is unchanged
because two queue proposals pin its SHA-256.

## MBUS ran at 400 MHz in both lifetimes

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
2. **The diagnostic does not depend on MBUS or NSI rates.** Its only time
   base is the 24 MHz generic counter: all deadlines are absolute counter
   values. Neither the overlay nor the native ELF contains a CCU fabric
   address in a literal pool. The only native code that builds a CCU
   address, in `a733_usb_prepare`, is unreachable from the overlay's entry.
   DSU and interconnect settings are system-register writes independent of
   the bus clocks. A slower memory bus could stretch the 5.3 ms run, but the
   deadline leaves a margin near 190 times.
3. **Rank: last.** The rate was the same in both lifetimes and nothing in the
   executed path reads it.

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

## Ranked hypotheses

### 1. A state check refused because of state left by the FreeRTOS images

The diagnostic found a word that differs from its required value and parked.
The most specific candidates are the CPU0 checks 4 and 5 above: a pending SGI
or PPI other than 29 and 30, a nonzero `ICC_IGRPEN1_EL1`, or another CPU0
control register that the warm reset does not restore.

For:

- It is the only systematic difference between the lifetimes.
- No CPU diagnostic in the 2026-10-18 series followed a kernel-monitor
  image.
- Monitor cleanup is partial for CPU0's pending and active state, and relies
  on unobserved warm-reset behavior for the CPU-interface and EL2/EL3
  controls.
- A refusal produces the observed failure: no return, no reset, no report.

Against:

- The monitor restores CPU0's enables, groups, configuration and PPI
  priorities. It stops both timers it uses. It leaves `GICR_WAKER` awake,
  which matches the zero the check requires.
- None of the three images touches the secondaries, their redistributors or
  the CPU power controller.
- The distributor control word read by the monitor at each entry was `0x37`,
  the same value the 2026-10-18 baseline recorded.
- No capture with 32-bit loads exists for the state after the FreeRTOS
  images: the only capture of that state used the ROM copy, which reads the
  GIC as 0.

### 2. The diagnostic fails intermittently

A race between cores, a cache-visibility error or a marginal core power-up
fails on some runs regardless of history.

For:

- The 2026-10-18 pass is one run, the 2026-10-23 failure is one run, and no
  other run of this diagnostic exists.
- All eight cores run concurrently through shared-memory barriers and
  cache maintenance.
- The two A76 cores are a different core type with their own power-down
  errata sequence in the diagnostic.
- Both complete eight-core FreeRTOS observer images run on port 3-1 on
  2026-10-18 failed, and their failure stages were never located.

Against:

- Each of the seven multicore diagnostics run on 2026-10-18 passed on its
  first physical attempt: sequential identity, local interrupt and cached
  entry, then concurrent coherence, interrupt, pre-scheduler and first-task.
- The executed bytes and inputs are identical.
- The run takes 5.3 ms against a one-second deadline.

### 3. Secondary CPU power or redistributor state differs after this cold start

A secondary was not OFF, had a control or mode bit set, or had a different
redistributor state, so check 2 or 3 refused.

For:

- This lifetime never observed that state.
- Every 2026-10-18 CPU lifetime did, and the first-task rationale asserts it
  without evidence.

Against:

- No image in the lifetime writes those registers.
- Every observed cold start, on both boards, showed status `0x100` and zero
  control and mode words.
- In a clean port 3-3 lifetime, the secondaries' power words and the
  redistributor words read with 32-bit loads satisfy every predicate that
  checks 2 and 3 apply to them.

### 4. A physical fault: supply, temperature or brownout

For:

- Eight cores power up within milliseconds, including the two A76 cores.

Against:

- The SoC did not reset, since the USB descriptor stayed listed.
- The same board on the same supply passed on 2026-10-18.
- The sensors read about 41 °C.

### 5. Host, runner, preparation or timing

Excluded. The transactions, addresses, sizes and executed bytes are
identical. The values that differ are random on both days. The 10-second
window is more than five times the observed return time.

### 6. DRAM or SRAM residue

Excluded. Every region the diagnostic reads before writing is uploaded. The
regions it does not upload hold the cold-DRAM pattern on both days, and the
table area is rebuilt.

### 7. MBUS or NSI clock rate

Excluded, as shown above.

## The corrected diagnostic checks the inherited state before releasing any secondary

The corrected first-task overlay adds one function,
`identity_first_task_admit`, at the end of
`boot/a733/all-core-first-task-probe.S`. The builder
(`tools/prepare-secondary-identity-probe.py --first-task`) links the overlay
with `--wrap=identity_primary_run`, so `identity_primary_gate` calls the new
function before the sequential survey. The function uses no stack and writes
no device register. It reads each word below with a 32-bit load (64-bit for
`GICR_TYPER`) and applies the predicate of the later check that reads the
same word. If every word passes, it branches to `identity_primary_run`, and
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

Some failures still park, because they happen after release, when running
secondaries cannot be brought to an observed OFF. They are each secondary's
checks of its own identity and control registers, the power-on, wake and
configuration handshakes of `a733_cpu_continue`, the cached EL1 entry, every
cleanup readback and every deadline that expires after release. The base overlay's
entry checks (EL3, boot CPU, masked interrupts, CPU revision) run before the
survey and also park; the primary report's state field records how far that
overlay got.

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

Both boards are in clean lifetimes: power cycle, cold DDR and surveys, with
no FreeRTOS image. [rank-7-corrected.md](rank-7-corrected.md) lists the
commands. The order:

1. **Port 3-1, read-only capture.** Run
   `board-session-20261023/state-capture-readl.py`. Every word should match
   the port 3-3 clean values above. A differing word names the inherited state
   before any launch.
2. **Port 3-1, corrected diagnostic.** Run the cache-off helper and a survey,
   bind `rank-7-corrected` to the instance with its `cpu-prepare.py`, and run
   it once. The outcomes separate the hypotheses:
   - **It returns with success.** The diagnostic itself passes in a clean
     lifetime on the board that failed, and lifetime history becomes the
     leading explanation. Then, on the same board: the kernel check, the
     forced-stall case and the thermal application, `state-capture-readl.py`,
     the cache-off helper and a survey, and the corrected diagnostic once
     more.
   - **It returns with a refusal code.** The code, address and value name the
     state; correct its owner before another attempt.
   - **It does not return.** The failure is after release, so it is an
     intermittent failure or a fault that does not depend on the inherited
     state. Remove every power source from port 3-1. The next diagnostic is a
     bounded repeat of the all-eight-core pre-scheduler diagnostic, which
     shares the power-up and cached-entry path.
3. **Port 3-3, only after port 3-1 returns.** Port 3-3 has never run an
   eight-core diagnostic and draws power from USB only, so a failure there
   could not be separated from a supply limit. Repeat steps 1 and 2 on port
   3-3 only if port 3-1 returned with success.

The unchanged bundle `rank-7-first-task/` is superseded and is not run again.

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
- Bundles: `board-bundles-20261023/rank-7-first-task/` (superseded) and
  `board-bundles-20261023/rank-7-corrected/`.
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
