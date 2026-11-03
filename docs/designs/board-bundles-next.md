# Board-test bundles for the single-core paths

**Status** by part:

- **Implemented:** the Read and Returning recovery classes, controlled outcomes,
  runbook generation and rows 1 to 34 in `tools/prepare-board-bundles.py`.
- **Host-tested:** `checks/board-bundles.py`, `checks/board-bundles-apps.py` and
  the QEMU and host models of the returning images.
- **Qualified on the board:** rows 1 to 4 and 6 to 8 on port 3-3, rows 20 to 24
  on port 3-3 and row 28 on port 3-1 (records below).
- **Open:** row 10, which stays refused until its runner declares completion
  counts and its owner names admission values, and the work listed at the end.

The design extends [the bundle tool design](board-bundles.md) and follows
[performance-architecture.md](../performance-architecture.md). The tool
prepares frozen, read-only bundles, runbooks and queue submissions for paths
that one core can run. It never contacts the board. The [board test
plan](../audit/board-test-plan.md) orders ranks 1 to 9 that these rows follow.
Rank 7 (eight-core execution) did not return on port 3-1 in two lifetimes, so
eight-core execution is unqualified and rank 8 is blocked. Each row's frozen
runner, expected result and procedure are in the row table of
`tools/prepare-board-bundles.py`; the predictions and results of each run are
in [the evidence records](../evidence/board-bundles-next.jsonl).

## Recovery classes

Read and Returning are the classes added for single-core paths; row 10 uses
the Terminal class.

- **Read.** One fixed reader, `tools/board-bundles-next-read.py`, is copied
  unchanged into every Read bundle as `board-read.py` and hashed there. Each
  bundle supplies only a data file `WORDS.json`. The reader saves the 128 FEL
  scratch bytes at `0x71000` with `read`, then issues `readl`. Each
  command-line `readl` reads one word: it writes a 48-byte A32 stub and the
  word's slot, the same first 52 bytes of scratch whatever the word count. The command then runs the stub in the ROM's context (`fel.c` and `aw_fel_readl_n` in the
  A733 build of sunxi-tools, revision `b92f4ed`, pull request 259 applied).
  The 128-byte save therefore covers every `readl` command. The first FEL
  write of a session also clears `SCTLR.I` in the ROM's context, because
  `aw_fel_write` calls `aw_disable_icache` when `icache_fix` is set for the
  A733 ([boot.md](../boot.md)). Each condition word is read in its own `readl`
  command and compared with its mask and value; on a mismatch the reader skips
  that word's dependents. The reader restores the scratch bytes with `write`,
  reads them again and compares them. A stalled or aborted read leaves the ROM
  unable to answer, and the board needs a power removal.
- **Returning.** A single-core FreeRTOS image on the returning platform, run by
  `tools/check-a733-kernel.py`. The EL3 monitor's secure timer forces a return
  to the ROM 2 s after entry, which covers a hang but not a stalled bus access.
  Each run re-enumerates the board twice. The tool's `nm` scan of the Monitor
  class applies and also refuses any image that defines `monitor_sync`, the
  uncapped renewal.

**Gates.** Six blocks have no software gate: CCU, R_CCU, the SID block at
`0x03006000`, the CPU PLL block at `0x08870000`, and the SPC (`0x02054000`) and
S_TZMA (`0x07003000`) security controllers. The pinned device tree names no
clock or reset for the SID or CPU PLL blocks, and the vendor BL31 writes both
security controllers on every cold boot with no clock or reset setup
(`sunxi_security.c`). A record of the same port and lifetime admits their words;
in a lifetime without one, a word there is a first read and sits after every
bundle that does not need it. Every other word needs a condition word. The
oscillator word `0x07090160` is the DCXO register of the RTC clock driver and
sits behind the bus gate and reset of R_CCU `0x20c` bits 0 and 16, so it is gated
on that R_CCU word.

**Order.** Known results come first, and each first access to a register block
follows every bundle that can run without it, so a stall loses only the bundles
after it. Every Returning run repeats the cache-off helper and survey that [the
runbook README](../board-runbooks/README.md) requires. No bundle writes DRAM
outside the 128 MiB that every Returning image uses. The user can remove power
every several hours, so a stall costs one reconnect and the bundles after it;
the memory rows still wait for an independent watchdog record or the user's
written acceptance of an uncovered bus stall.

## Rows

| # | Class | Bundle |
| --- | --- | --- |
| 1 | Returning | Fault capture on core 0: a load from `0x4_0000_0000`, above every valid level-1 entry |
| 2 | Returning | Kernel check built with the counter option; base of bundle 4 |
| 3 | Read | CCU `+0x580`, `+0x588`; R_CCU `0x20c` as condition, then the oscillator word; CPU-L PLL and mux words |
| 4 | Returning | Event counters, core-0 walk A/B and idle WFI in three stages |
| 5 | Read | R_CCU `0x1ac` bit 0 as condition, then the policy and status words of domains 1, 4 and 5 |
| 6 | Returning | PMIC service task and power-domain owner, read-only: S_TWI0 timing, 32 DCDC2 and DCDC4 snapshots |
| 7 | Returning | Domain 1 ON through the owner, with status read back to back up to the 10 ms bound |
| 8 | Returning | DRAM bus priority: gates, Non-secure NSI field reads of ports 2, 14 and 15, table write and readback |
| 9 | Read | Speed-bin word, PLL_NPU, NPU CCU words, domain-4 policy and status, SYSCTRL `0x03000008` |
| 10 | Terminal | NPU single-core image through `tools/check-a733-npu-supervised.py`; refused |
| 11 | Returning | GPU provider readiness, port 3-1 |
| 12 | Returning | GPU TOP5 and CORE6 ON and OFF twice, no GPU register access |
| 13 | Returning | GPU power and identity in two ON/OFF cycles |
| 14 | Read | SID speed bin, CPU-L, CPU-B and DSU PLL and mux words (clock part of record B1 of [cpu-a76-dvfs.md](cpu-a76-dvfs.md)) |
| 15 | Returning | DCDC3 and DCDC5 snapshots (`apps/board-bundles-power/cpu-supply.c`), read-only |
| 16 | Returning | GPU wrapper reads after the default ON |
| 17 | Returning | GPU core identity as the vendor probe reads it, then after the vendor secure configuration |
| 18 | Read | CCMU and PRCM security switches, S_TZMA, SPC port 14 status |
| 19 | Returning | GPU firmware start after the vendor timer record and secure configuration |
| 20-24 | Returning | CPU operating-point steps 1 and 3 to 5 of [cpu-top-opp.md](cpu-top-opp.md) (below) |
| 25 | Returning | GPU firmware start, one health check, firmware stop and OFF |
| 26 | Returning | Bundle 25 again in the same lifetime with no power removal |
| 27 | Read | Clock-audit dump of CCU, PRCM, CPU PLL and RTC clock words for [the clock audit](../clock-audit.md) |
| 28 | Returning | First GPU job: the fixed 32-lane compute job between health check and stop |
| 29 | Returning | Eight timed GPU compute jobs on one context ([job-session design](gpu-job-session.md)) |
| 30-33 | Returning | CPU step 6: top points held across the ROM return, then rail and boot-word reads, then rails restored to 800 mV |
| 34 | Returning | Two GPU integer matrix products with exact sums, cleanup and OFF |

Rows 1 to 9 and 20 to 24 run on port 3-3; rows 11 to 13, 16, 17, 19, 25, 26, 28
and 29 run on port 3-1 after row 11 in the same lifetime. A GPU firmware row
is the last GPU image of its lifetime when its runbook says so. Row 10 runs at 492 MHz,
and at 852 MHz only when row 9's speed bin qualifies and row 6 verified DCDC2 at
800 mV. The approval rule in [docs/hardware.md](../hardware.md) belongs to the
CPU clock driver's entry (CPU-L, CPU-B and their DCDC5 and DCDC3 voltages); the
852 MHz NPU point changes neither, so that rule does not cover it.

## Controlled outcomes

The kernel runner quarantines the instance on any nonzero status. Each
Returning row lists its controlled outcomes, each matched on the full result
record and log. A match passes without quarantine, the runner records the
parsed EL1 values in `session.json`, and the admission patch accepts the
session as a warm-return link carrying those values. Any other nonzero status
still quarantines. A controlled outcome is not a pass for any gate; the
operator applies the gates.

- **Bundle 1.** Status 1; exactly one `FAIL exception` line; `ESR_EL1` class
  `0x25` with a level-1 translation fault in the ISS; `FAR_EL1` equal to
  `0x4_0000_0000`; `ELR_EL1` inside the test function.
- **Bundle 4.** Status 2, interrupt 29, the log markers of the completed counter
  and walk stages and of the WFI stage's start. Its progress record at `0x42000080` holds magic `0x3130535342344242`, stage 3, a wake count below
  1,000 and a last wake stamp more than 48,000 counter ticks (two ticks) older
  than the monitor's `end_counter`. This outcome means the core stopped waking
  from `WFI` on PPI 30. An outcome names such a record through its `progress`
  key; the runner reads its four words from the result page.
- **Bundle 6.** Status 1 after the full summary, with `BOUND permille=...` and
  `FAILED board bundle 6: provisional step bound`: the steps missed the
  provisional bound, or the measured byte was missing or above 110% of nine bit
  times. The steps that need bundle 6 to pass do not run. A supply, snapshot or
  record mismatch prints `FAIL` and is not controlled.
- **Bundle 7.** Status 1 with the owner's FAILED record and its status trace,
  bracketed by `FAILED board bundle 7: domain 1` and `TRACE END board bundle 7`.
  No later bundle writes the PPU in that lifetime. When the owner refuses the
  row before any PPU write (a closed CCU or PPU gate, an unverified DCDC2 or a
  domain whose policy and status disagree), the image ends with status 0 and a
  record that says so. The run is inconclusive, and bundle 8 then finds domain 1
  not ON.
- **Bundle 8.** Status 1 with one `FAIL exception` line whose ISS is a
  synchronous external abort, whose `FAR_EL1` is one of the twelve field words
  the image reads (ports 2, 13, 14 and 15) and whose `ELR_EL1` lies in its access
  function, one outcome per word. The other outcome is status 1 with a readback mismatch and its
  stopping field. An SError from a posted write matches no outcome. After an
  abort, a Read bundle of the same fields gives the Secure view. When domain 1
  status does not read ON or a gate is closed, the image skips every NSI access
  and ends with status 0 and a record that names the skipped stage; the run is
  inconclusive. A Secure-only NSI can return zeros to Non-secure reads, so
  inherited fields that read zero on all three ports are recorded as possibly protected. The write readback then separates the two cases.
- Rows 25 to 29 and 30 to 34 list their own controlled failures in the tool's
  row table.

## Bundle designs

**Bundle 4** (`apps/board-bundles-cpu/main.c`) runs three stages in one task at
priority 1. `a733_cpu_pmu_begin` masks IRQs only for its own register accesses,
so the image masks IRQs around each counter and walk region itself. Each stage
writes a log marker and the stage word of the progress record. The image starts
the WFI stage only before 0.9 s after the monitor's entry, so it ends before the
monitor's 2 s deadline; a later start ends the run with status 0 and an
inconclusive record.

- **Counters.** 64 empty regions (minimum, median, maximum of cycles and
  `INST_RETIRED`); 2,000 ALU instructions, whose `INST_RETIRED` less the empty
  median shows the event's accuracy; a loop of at least 1 ms, whose cycles per
  `CNTPCT` tick give the measured CPU-L rate against bundle 3's configured rate;
  `PMCR_EL0`, `PMCEID0_EL0`, `PMCEID1_EL0`; and one load per line of a 64 KiB
  buffer cleaned and invalidated to the point of coherency, whose
  `L2D_CACHE_REFILL` count shows whether the event counts.
- **Walk A/B.** Blocks of 1,024 iterations of `TLBI VMALLE1`, `DSB ISH`, `ISB`
  and one load. Iteration k loads line k of 2 MiB block k (`base + (k << 21) +
  (k << 6)`), so consecutive loads target different blocks and different cache
  sets and B's descriptor reads hit the caches. No load targets the
  non-cacheable result block or the block that holds the image, its stacks and
  the PMU state. One discarded warm-up block, then twelve blocks in ABBA order: A
  installs `TCR_EL1` `0x200803019` (uncached walks), B `0x200803519` (cached
  walks, the value `platform_init()` installs). Each block records `CNTPCT` and
  all six events. `DTLB_WALK` and `ITLB_WALK` must each equal 1,024, and the
  calibration region before the block (`TLBI VMALLE1`, `DSB ISH`, `ISB`, then end)
  must count no data walk and one instruction walk, or the block is refused
  (`apps/board-bundles-cpu/walk.h`). The switch is: write `TCR_EL1`, `ISB`, `TLBI
  VMALLE1`, `DSB ISH`, `ISB`. No table write follows the final flush of both table
  pages in `platform_init()`, so uncached walks read the same descriptors as
  cached ones; `checks/board-bundles-apps.py` checks this in the source and in
  the linked object's relocations. The image reads CCU `+0x588` and refuses the
  stage when `MBUS_DFS_EN` is set. The run records the CPU-L rate only. The CPU-B and DSU rates that [cpu-table-walks.md](cpu-table-walks.md) asks for with each
  walk run need a Read bundle of their words.
- **WFI.** Over 1,000 ticks, the task runs `a733_cpu_idle()` (`DSB SY`, `WFI`)
  with IRQs unmasked, so the tick handler runs on each wake. No other task is
  ready, so the idle task does not run. The task reads `PMCCNTR_EL0`, the event
  counters and `CNTPCT` after each wake, outside any masked region, and stores the
  wake count and stamp in the progress record. The stage records the wake count,
  `CPU_CYCLES` against `CNTPCT`, the shortest and longest `CNTPCT` gap between
  wakes, the event totals and the scheduler ticks. The Cortex-A55 TRM (100442_0200_00,
  section A4.4.1) states that `WFI` gates the clock at the top of the core's clock
  tree but not what the cycle counter does; the stage measures it.

**Bundles 6 and 7** (`apps/board-bundles-power/`) start the PMIC provider, the
rail ledger and the power-domain owner as `rank-9-usb-v4/` started the provider,
with S_TWI0 pins PL0 and PL1 at function 2, pull 1 and drive 1. The provider's
startup and the service session end 1.5 s after the monitor's entry. One service
task at priority 2 runs `a733_power_service_step()`; a client task runs at
priority 1. A recording layer between each native adapter and its user passes
every S_TWI0 access to an observer that classifies the reads of the control
register (flag polls after a START or byte, STOP polls after a STOP). The observer also stores every owner access with its `CNTPCT` stamp. Each image prints its summary and
outcome line first, then detail lines while 256 log bytes remain free, then a
count of the omitted lines.

- **Bundle 6** records the owner adapter's mapping result and the `PAR_EL1`
  translation of the domain-1 block, which the adapter requires to be
  Device-nGnRnE and outer-shareable. A detail line per completed bus phase gives
  the `CNTPCT` span, the scheduler ticks inside it, the completed conditions,
  bytes and STOPs, the flag at the first read of each byte phase and the most
  polls of one phase. The wire time follows from the phases and the bit time (two
  bit times per START, repeated START or STOP, nine per byte). The summary counts
  steps at most 1,000, 1,050, 1,100 and 1,268 permille of their measured wire
  time, then those above 1,268, with ratios rounded up from counter ticks. At the
  park, the service task enters the I2C mutex through the adapter while the provider is idle. It times by `CNTPCT` 256 iterations each of control-register
  reads with `DSB SY` before and after, plain reads with the value discarded,
  dependent plain reads, the adapter's time and read operations, the task check
  and the trace observer. The record names the service's core (`MPIDR_EL1`
  Aff1). The pass bound is `BB_BOUND_PERMILLE` (1,268 of measured wire time) plus
  40 µs for the first step. The byte time is nine bit times or the lower
  stamp of a back-to-back flag spin when that is longer and at most 110% of nine
  bit times ([pmic-access.md](pmic-access.md) gives why a larger byte fails). The
  verdict requires both supplies verified, every snapshot `0xff9e`, 34 snapshots
  counted and at least one recorded step. Re-spins (control-register polls beyond
  the second of a phase) are reported and do not gate the run.
- **Bundle 7** reads the domain-1 words before the request, then lets the owner
  run its sequence. After the owner's ON command reaches the PPU, the layer reads
  the status back to back until it reads ON or 10 ms pass, and records the first
  value and each change with its stamp and the stamp of the read before it. The
  owner starts its own 10 ms bound after that spin, so a domain that never reaches
  ON fails up to 20 ms after the command. Domain 1 stays ON until power removal,
  so the bundle gives one sample per board lifetime.
- **Bundle 6 does not read the GIC pending bit of S_TWI0's interrupt** (SPI 203,
  INTID 235). The monitor keeps `GICD_CTLR.DS` at 0 and assigns no shared
  interrupt to Group 1 Non-secure, and a Non-secure read of a Group 0
  interrupt's pending bit returns 0. The controller's interrupt flag alone shows
  whether it raises its interrupt condition at each byte phase; GIC delivery is
  qualified by the bundle that first binds the interrupt. The PMIC provider's
  startup check reads the enable, pending and active bits of INTID 235 once and
  requires each to be 0.

**Bundle 8** (`apps/board-bundles-dram/`) keeps its sequence in `sequence.c`,
free of FreeRTOS and hardware, so the host model runs it. The gates come first;
the first NSI accesses are the nine field reads of ports 2, 14 and 15, then the
three USB_PCIE fields of port 13 (the `CONFIG_ARCH_SUN60IW2` value of
`MBUS_PMU_USB_PCIE` in the pinned `sunxi-nsi.h`), then `a733_dram_priority()` in
table mode, then port 13 again. Every access is one `DSB SY`-fenced 32-bit access
through one function, timed with `CNTPCT`. An access that the register pair
refuses, as when its trace is full, ends the run as a refused access, never as a
readback mismatch. Its NSI priority writes persist until power removal.

**Bundles 20 to 24** (`apps/board-bundles-power/cpu-opp-step.c`, built with
`-DOPP_BUNDLE=n`) run steps 1 and 3 to 5 of [the top operating-point design](cpu-top-opp.md). Bundle 20 raises CPU-B from 1,014 to 1,404 MHz and back
at the 800 mV of bin vf0400 with cores 6 and 7 off, writing no PMIC register. Bundle 21 raises DCDC3 to 1,050 mV and back with CPU-B held at 1,014 MHz. Bundle 22 raises CPU-L to 1,196 MHz and the DSU to 780 MHz at 800 mV. Bundle 23
raises CPU-B to 2,002 MHz at 1,050 mV with the DSU at 910 MHz, then back. Bundle 24 raises CPU-L to 1,794 MHz at 1,000 mV and the DSU to 1,274 MHz and ends its
lifetime. Each reads a temperature set and the CPU-L, DSU and CPU-B words before
the change and requires every PLL and mux word and both rails to read as before
afterwards. Admission refuses, before any write and with a controlled warm
return, a board that is not at the NORMAL thermal level, not on vf0400, not at
800 mV on DCDC3 and DCDC5, not at 1,014 MHz on CPU-B or with a pattern or LFM
enable set. Each adds a thermal stop at 70 °C: a sensor at or above it before
any write refuses the step, and after a write it lowers what the step raised and
returns STOPPED, a controlled outcome. The row requires an approval record of
schema `cpu-operating-point-approval-v1`, carried as `approval.json`. Bundles 22
and 24 measure CPU0's rate with the PMU code of bundle 4. Bundles 20 to 24 link
the base row's monitor and platform objects, so the base runs first.

## Lifetimes, runbooks and frozen sets

With `--lifetime`, a JSON file names the port, the DDR record of the current
lifetime, its warm-return chain and the order of the run's rows and of external
bundles, each with its record directory. A row's gates are its base and `after`
rows; an external entry names its own `after` rows; any entry may name
controlled outcomes of earlier rows that stop it (`stop_if`). The tool refuses an
order in which a gate row does not come before its dependent. Every runbook then
gives its record directories and its complete `--warm-return` list, and the run
directory receives `runbook.md` with the order, the gates and the chain.
`--check-externals` confirms that an external entry's runbook writes record
directories only under its own stem.

For each prepared row the tool writes `runbook.md` in the format of
`docs/board-runbooks/`: status, bundle files with SHA-256, host checks, numbered
commands with success text, timeouts and admission records, stop conditions, the
controlled outcomes named in the bundle's `controlled.json`, and the rows that
must pass first.

A pass or a listed controlled outcome is a warm return. Any other status,
including the monitor's forced return after a hang (status 2) and a failed
monitor cleanup (status 6), and a run that does not return, ends in the runner's
recorded quarantine of the instance with the claim held until power removal. A
runner refusal before launch makes no device access and quarantines nothing; the
input is repaired and the step resumed. A host timeout of `bundle-run.py` kills
the runner and writes no recovery file, so the operator treats it as a
quarantine.

## Data flow and ownership

Owners build each image under `~/.cache/sunstoneos/build/<package>/`; the tool
builds nothing. `checks/board-bundles-apps.py` builds the base image and the
images of bundles 4, 6, 7 and 8 and writes the `build.json` that the tool reads.
It runs from the source tree, outside `~/.cache/sunstoneos`, because the provider
objects record the compile directory. Each object of an image is one of:

- an object of an image that passed: the kernel check of ranks 4 and 5, bundle 2
  or `rank-9-usb-v4/`. A returning platform object of `rank-9-usb-v4/` is used
  when this tree's object equals it. `monitor.c` is used when its instructions are equal. A provider object is used when the unit recompiles byte-identical;
- an object of the base image of the same set (origin `set-base`): the central
  kernel check with `monitor.c` compiled with the counter option. Bundle 4 links
  every base object except `checks/kernel.c`, and bundles 6 to 8 link its
  `platform.c`. The base runs first, and its pass makes those objects board-run;
- a declared new unit: the bundle's own files and, in bundles 6 and 7, the PMIC
  provider, its native adapter, its consumer and the GPU resource adapter.

With `--usb`, the check also records the origin of every object of the native USB
image that `apps/usb-returning/build.py --platform-build` built from the same
tree, and refuses a platform or provider object that belongs to no passed image.
In `tools/prepare-board-bundles.py`:

- **Read rows** carry their words in the row table, and the tool writes them to
  `WORDS.json`: each entry is an address and either the ungated block that holds
  it or a condition word (address, mask, value) listed earlier. The reader's
  `plan()`, which the tool runs on every Read bundle, refuses a gate read after
  its dependent, a word outside its named block and any key outside that schema.
  The tool runs no capture script.
- **Returning rows** are Monitor rows with per-row fields: prerequisite
  `supervised-ram-recovery`, runner `tools/check-a733-kernel.py`, the queue
  timeout, the equivalence base and the controlled-outcome file.
  `validate_bundle()` returns a platform-equivalence record against the base, and
  the tool refuses any differing object outside the row's declared overlay. A row
  may name a base row; it is prepared only after that row in the same run, and
  each `set-base` object must equal the base bundle's object of the same unit. A
  row's `after` list orders its runbook only. The queue timeout is computed from
  the bundle's own copy of the runner.
- **Admission.** Rows 1 to 9 admit the value that their preparation step returns
  when it names the row's schema and the row; row 10 keeps `admission` at `None`
  until its owner names the admission values.
- **Survey.** The returning SRAM survey before each returning run calls
  `tools/run-launch-survey.py` through `common/bundle-run.py` with the survey
  build in `bootstrap-retention-20261024/survey-build`. That runner refuses a
  build whose manifest names another runner (`host_sha256`) or survey source
  (`source_sha256`). The tool refuses inputs with a returning row before it
  creates a run directory unless the survey build passes the runner's hash
  checks, and `checks/board-bundles.py` repeats that check offline.

The memory row's procedure requires ranks 1 to 6 and bundle 3, because memory
runs use one core and rank 7 qualifies nothing they use.

## Interrupts, memory and synchronization

The tool polls nothing. Read bundles take no board interrupt. Returning images
take PPI 30 as their tick and the monitor's secure timer as the deadline. Bundle
6 spins within one service step on S_TWI0 byte phases, and bundle 7 reads PCK600
status back to back up to 10 ms; section 1.3 of the performance architecture
permits both for devices without qualified interrupt delivery. No image touches
DRAM outside the 128 MiB that `platform_init()` maps. All bundles run one core.

## Register sources and unproven sequences

| Sequence | Source | Status |
| --- | --- | --- |
| Oscillator word, gated on R_CCU `0x20c` | `ccu-sun60iw2-rtc.c`, `ccu-sun60iw2-r.c`, `sun60iw2p1.dtsi` | Board-read |
| CPU-L words, CPU PLL block without a bus gate | Pinned `ccu-sun60iw2-cpupll.c`; read by the mimus runtime ([mimus.md](../mimus.md)) | Board-read |
| Speed-bin, PLL_NPU and NPU clock words | [cpu-clock.md](../cpu-clock.md); [npu-platform.md](npu-platform.md) | First read |
| CCU `+0x580` to `+0x588` | Manual sections 4.1.6.74 to 4.1.6.76 | `+0x584` first read |
| R_CCU `0x1ac` bit 0 before PCK600 | `ccu-sun60iw2-r.c` | Board-read |
| PCK600 policy, status and command | `pck600_domains.c`; PCK-600 TRM `PPU_PWPR`, `PPU_PWSR` | First access |
| Delay row `+0xc00`-`+0xc10` | `pck600_domains.c` | **Unproven**: absent from the TRM |
| NSI port fields after domain 1 ON | `sunxi_set_nsi_qos_params` takes the master's domain first ([dram-bus-priority.md](dram-bus-priority.md)) | Port 2 follows that order; ports 14 and 15 **unproven**: their masters' domains are not identified |
| Counter controls | `platform/a733/cpu_continue.S`; TF-A `0892ad4d` `arch.h` | Host-modeled |
| `TCR_EL1` switch with the MMU on; fault syndromes | Arm DDI 0487 | **Unproven**: not fetched in `checks/cpu-pmu-sources.json`, so no section is pinned |

## Qualified figures

| Item | Figure | Record |
| --- | --- | --- |
| Bundles 1 to 3 | passed on port 3-3 | `board-bundles-next-bundles-1-3-port-3-3` |
| Bundle 4 (counters, walk, WFI) | ran on port 3-3 | `board-bundles-next-bundle-4` |
| Bundle 6 (PMIC step bound) | passed with median 1,243 permille | `board-bundles-next-bundle-6` |
| Bundles 7 and 8 | ran on port 3-3 | `board-bundles-next-bundles-7-8` |
| Bundles 20 to 24 | passed on port 3-3 | `board-bundles-next-cpu-operating-point-steps` |
| Bundle 28 | passed on port 3-1 | `board-bundles-next-gpu-rows` |
| Offline budgets and image models | host results | `board-bundles-next-budgets-and-models` |

## Open work

1. Rank 7 and rank 8 gating: counters and a deliberate fault on an A76; WFI on
   eight cores with SGI 0 wake-ups and the EL0 `WFI` enable; the core-6 walk A/B
   and the dirty-line experiment of the cached launch paths; tasks on cores 6 and
   7 after a Read bundle of the CPU-B and DSU PLL and mux words; PMIC selectors
   after PSCI CPU_ON; the DRAM latency window with eight cores and a display; the
   three Terminal rows of the integrated OS image; the arbiter row.
2. A Read bundle of the CPU-B and DSU PLL and mux words beside the CPU-L words of
   bundle 3 on the next lifetime of port 3-3, so that each walk run carries all
   three rates.
3. The core-0 workload A/B of [cpu-table-walks.md](cpu-table-walks.md) with
   `ITLB_WALK` during a GPU frame, an NPU inference and a recognizer inference;
   domain 4 ON binds to row 10 once the NPU startup requests it through the owner.
4. The A76 operating-point runs A1 to A3 and the DCDC3 selector write of
   [cpu-a76-dvfs.md](cpu-a76-dvfs.md), on a board where ranks 7 and 8 have passed
   and the per-board B1 record exists, with an approval record of schema
   `a733-os-cpu-clock-approval-v1` that names that board, image, session and B1
   record. The user approved the physical clock and voltage changes.
5. User approval is needed for other CPU clock raises and DCDC5 voltage changes;
   domain 5 ON and OFF repeated N times (needs DCDC4); WDT0 expiry with
   fault-record retention (boots from microSD); intentional GPU and NPU hangs. The
   memory rows run in their own lifetime after a cold DDR start, after bundle 3
   alone, because domain 1 and the NSI table persist until power removal.
6. The 12 GiB OS map: `a733_dram_decode_check()` returns 0 until a register
   source exists. After a host trace of boot0's DRAM controller and firewall
   writes, a Read bundle of those registers and the memory qualification chain,
   the cached images test the map (`platform/a733-smp-cached/memory.c` gates it).
7. Camera captures below and above 4 GiB, NVMe for the PCIe inbound width and a
   display need accessories.
8. Row 10 needs its runner to declare `COMPLETION_COUNTS` and its owner to name
   admission values; the table-walk owner's acceptance of one interleaved image in
   place of two images stays open.

## Left out

- **The xHCI `HCCPARAMS1.AC64` read.** It touches a USB controller.
- **Board contact.** The tool does not submit, claim or run a runner.
