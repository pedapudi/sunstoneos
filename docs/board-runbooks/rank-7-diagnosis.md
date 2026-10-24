# Why the all-core first-task diagnostic did not return on 2026-10-23

Status: offline diagnosis from saved evidence. No board or USB device was
accessed. The cause is not established. This document ranks the candidate
causes, gives the evidence for and against each, and names the read-only
board observation and the diagnostic correction that separate them.

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
start and the diagnostic. These included an all-eight-core diagnostic that
powered every secondary CPU up and down. On 2026-10-23 the board ran three
FreeRTOS images under the kernel secure monitor before the diagnostic: the
single-core kernel check, the forced-stall recovery case and the returning
thermal application. No secondary CPU had been powered since the cold start,
and the CPU power state was never observed in this lifetime.

The diagnostic converts every failed check into an indefinite wait on CPU0. A
refused state check, an expired deadline and a hardware hang therefore produce
the same observation: no return. The most likely cause is a refused check on
CPU state that differs between the two lifetimes. An intermittent failure of
the diagnostic itself is second. The MBUS clock rate is not a cause: both
lifetimes ran MBUS at the 400 MHz that the ROM programs on every entry, and
the diagnostic does not read or depend on any bus clock.

Running the unchanged diagnostic on port 3-3 is not justified. That board has
the same lifetime history, so the attempt would repeat the suspected condition
and could again end without any report.

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
(`overlay-comparison.json`).

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
   at EL1 parks if `ICC_IGRPEN1_EL1` is nonzero.

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

## Next steps

The port 3-3 queue records a survey taken after the port 3-1 reconnect, so
port 3-1's failed state is lost. The steps below separate hypotheses 1 to 3
without another silent launch.

### Step 1: one read-only FEL capture on port 3-3

Port 3-3 ran the same sequence on 2026-10-23: cold DDR, kernel check, forced
stall and thermal application. Since then it has run only the cache-off helper,
surveys and a read-only CCU capture. Apart from board-to-board differences,
its state should match what the diagnostic met on port 3-1. One `readl` batch
of the following words tests checks 1, 2 and 4 and the redistributor part of
check 3. FEL read the power words, the distributor words, the
CPU0 words and the secondary `TYPER` and `PWRR` words on 2026-10-18. The
secondary `CTLR` and `WAKER` words and the entry configuration words are new
FEL reads. They sit in register blocks that FEL has already read, and CPU0
reads the entry configuration words itself during the diagnostic.

| Words | Addresses | 2026-10-18 value | Required by |
| --- | --- | --- | --- |
| Secondary power status, control, mode (cores 1 to 7) | `0x07052008 + n*0x1000`, `0x07050200 + 4n`, `0x07050220 + 4n` | `0x100`, 0, 0 (`reconnect-20261018-120/cpu-state`) | Low 4 status bits and bit 0 of control and mode clear |
| Secondary entry configuration (cores 1 to 7) | `0x08001000 + n*0x1000`, `+4`, `+8` | 0, 0, 0 (first-task report) | Recorded and restored; no requirement |
| Distributor control and type | `0x03400000`, `0x03400004` | `0x37`, `0x007b0408` (`primary-gic-state-121-20261018`) | `CTLR & 0x70 == 0x30`, RWP clear |
| CPU0 redistributor | `0x03460024` first, then `0x03460000`, `0x03460014`, `0x03470100`, `0x03470200`, `0x03470300`, `0x03470080`, `0x03470d00`, `0x03470c04`, `0x03470400` | `PWRR` 0, all others 0 | `PWRR` powered; `CTLR`, `WAKER`, enable, pending and active zero |
| Secondary redistributor frames (cores 1 to 7) | `0x03460000 + n*0x20000` at `+0x8`, `+0xc`, `+0x24`, `+0x0`, `+0x14` | `TYPER` affinity `n << 8`, last-frame bit on core 7 only; `PWRR` `n << 8`; `CTLR` 0; `WAKER` 6 (`gic600-registers-20261018-089`, first-task report) | Same values |

The secondaries' SGI-bank words are also checked, but FEL has never read them
from CPU0, so they stay out of this batch.

The coordinator compares the result with the table and with the code's
predicates:

- **A word differs.** The differing word names the refusing check. The
  correction is to make the earlier image restore it, or to make the
  diagnostic's preparation establish it before any secondary is released.
  Port 3-1 then gets one corrected attempt after a clean cold start.
- **Every word matches.** Hypotheses 3 and the readable part of 1 are
  excluded. What remains is CPU0 system-register state that FEL cannot read,
  and intermittency. Continue with step 2.

### Step 2: make a refusal return a report

Change the overlay so that every check that can fail runs before the first
secondary is released. A failure at that point then returns through the
existing monitor return with a nonzero status and a failure code. CPU0 can
make all of these checks itself:

- Read every secondary's power, control and mode words and redistributor
  frame (checks 2 and 3) from EL3.
- Read its own redistributor words (check 4).
- Read `ICC_IGRPEN1_EL1` for the security state its EL1 will use (check 5).
- Read `CNTFRQ_EL0`.

The existing in-place checks stay. A failure after release still parks,
because running secondaries cannot be brought to an observed OFF from that
point. This changes the overlay bytes, so the build, the three instruction
cases and the nine host cases must be regenerated. It needs no change to the
native ELF.

### Step 3: separate history from intermittency on port 3-1

After its cold DDR start, run on port 3-1, in order:

1. The read-only CPU power-state check from 2026-10-18.
2. The step-1 batch.
3. The cache-off helper and survey.
4. The corrected diagnostic, with no FreeRTOS image earlier in the lifetime.

The outcomes separate the hypotheses:

- **It returns with success.** Lifetime history becomes the leading
  explanation. Rerun the kernel check, forced stall and thermal application,
  then the step-1 batch and the corrected diagnostic again, to find which
  image leaves the refusing state.
- **It returns with a failure code.** The code names the check and the
  state.
- **It does not return in a clean lifetime.** Intermittency or a change in
  the board becomes the leading explanation. The next diagnostic is a
  bounded repeat of the pre-scheduler diagnostic, which shares the power-up
  and cached-entry path.

## Running the unchanged diagnostic on port 3-3 is not justified

The goal's rule requires a concrete correction, or a narrower diagnostic
justified by new evidence, before a failed launch is repeated. The unchanged
diagnostic on port 3-3 meets neither condition:

- **It repeats the suspected condition.** Port 3-3 has the same lifetime
  history as port 3-1: kernel check, forced stall and thermal application,
  with no observed CPU state.
- **A second failure would be just as silent.** It would leave CPU0 parked
  with no report and cost a power removal.
- **It adds new variables.** Port 3-3 has never run any eight-core
  diagnostic, and USB is its only power source. A failure there could not be
  separated from a supply limit.
- **The MBUS observation adds nothing.** Both lifetimes ran MBUS at 400 MHz,
  so it gives no reason to expect a different result.

The step-1 read-only batch on port 3-3 is justified now. A diagnostic launch
on either board should wait for its result and for the step-2 correction.

## Evidence

All paths are under `~/.cache/sunstoneos/evidence/`.

- 2026-10-23 attempt: `board-session-20261023/port-3-1/r7-prepare/` (prepared
  bundle, runtime snapshots and `runtime/session.json`); lifetime records in
  the sibling `ddr/`, `kernel/`, `timeout/`, `r6-thermal/`, `r7-survey/`.
- 2026-10-18 pass: `all-core-first-task-20261018/physical-010/` and
  `warm-010/ready/survey/`; lifetime records `reconnect-20261018-120/`,
  `primary-gic-state-121-20261018/`; queue history in
  `~/.cache/sunstoneos/board-tests.sqlite3`.
- Frozen bundle: `board-bundles-20261023/rank-7-first-task/`, including
  `overlay-comparison.json`.
- MBUS observations: `usb-first-fault-20261018/fabric-064/`,
  `usb-returning-20261018/fabric-056/`,
  `second-board-20261018-3-3/sd-pin-clock-state/`,
  `npu-single-core-20261018/physical-candidate/npu-state/`,
  `second-board-cold-122-20261018/accelerator-state/npu-state/`,
  `crypto-clocks-20261018-082/`, `board-session-20261023/port-3-3/r9-capture/`;
  DDR initialization records for each lifetime under `*/ddr*/`.
- Sources: `boot/a733/secondary-identity-probe.{c,S}`,
  `boot/a733/all-core-first-task-probe.S` at `7ca22b0`,
  `boot/a733/primary-local-cache-probe.c`,
  `boot/a733/secondary-local-probe.c`,
  `boot/a733/secondary-distributor-probe.c`, `platform/a733/monitor.{c,S}`,
  `tools/run-native-startup-probe.py`.
