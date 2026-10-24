# Design: board-test bundles for the single-core paths of this tranche

Status: **design**. No bundle in this document exists, and none has run.
It extends [the bundle tool design](board-bundles.md) and follows
[performance-architecture.md](../performance-architecture.md). Board state
on 2026-10-24: ranks 1 to 6 of the [board test plan](../audit/board-test-plan.md)
passed on both boards. Rank 9, the native USB channel, passed on port 3-3
with an image built on the platform revision of the passing kernel check
(`dfa5174`). Rank 7, the all-core first-task diagnostic, did not return on
port 3-1 in two lifetimes, so eight-core execution is unqualified and
rank 8 is blocked. Port 3-1 waits for a power removal; port 3-3 is in the
boot ROM's USB recovery mode (FEL).

## Bundles one core can run now on port 3-3

Each bundle is one of two new recovery classes:

- **Read.** A host script issues 32-bit FEL reads (`sunxi-fel readl`) and
  nothing else, so no board code runs and the ROM keeps the USB controller.
  A read of a block whose clock or reset is off can stall the ROM, and the
  board then needs a power removal. The script therefore reads each word only
  after a word that establishes its access condition, or only when an earlier
  board record already read it.
- **Returning.** A single-core FreeRTOS image on the returning platform
  (`platform/a733/monitor.S`, `monitor.c`, `platform.c`), run by
  `tools/check-a733-kernel.py` as ranks 6 and 9 were. The EL3 monitor's
  secure timer forces a return to the ROM 2 s after entry, which covers a hang
  but not a stalled bus access. Each run re-enumerates the board twice.

The order below starts with known results and puts each first access to a
register block after every bundle that can run without it. A stall therefore
loses only the bundles after it. Every Returning bundle repeats the
cache-off helper and survey that [the runbook README](../board-runbooks/README.md)
requires before each launch.

| # | Class | Bundle | New risk | Records |
| --- | --- | --- | --- | --- |
| 1 | Read | Clock and speed-bin words: the oscillator, speed-bin and CPU-L, CPU-B and DSU PLL and mux words of `a733_cpu_clock_report`; CCU `+0x580`, `+0x584`, `+0x588`; the PLL_NPU and NPU clock words of NPU evidence item 1 | First reads of the CPU-B, DSU, speed-bin and NPU words and of `+0x584` | The boot-clock report that rank 6 lacked and that every timing record and the memory rows need; `MBUS_DFS_EN`; the NPU speed bin |
| 2 | Returning | Kernel check built from the current tree | None beyond rank 4: the kernel check links no pool source, so the new boot-time pool preparation returns at once | The equivalence base: every later Returning image must match its monitor, platform and port objects, apart from a declared overlay |
| 3 | Returning | Fault capture on core 0: a test task loads from an address above `0x3_4000_0000`, where no level-1 entry is valid | First board run of `platform_exception`; a translation fault makes no bus access | The `FAIL exception` line with `ESR_EL1` class `0x25`, `FAR_EL1` and `ELR_EL1` ([fault-capture.md](../fault-capture.md), board item 2 on an A55) |
| 4 | Returning | Idle WFI on core 0: a lowest-priority task runs `a733_cpu_idle()` (`DSB SY`, `WFI`) while a counting task wakes on each tick for 1,000 ticks | A core that never wakes ends at the 2 s deadline with status 2 | Wake-ups per tick and instructions between wake-ups by the timer interrupt (PPI 30) |
| 5 | Returning | CPU event counters on core 0 ([cpu-pmu.md](cpu-pmu.md) items 1 to 4) | A trapped counter access ends in the fault line that bundle 3 proves | `PMCR_EL0`, `PMCEID0_EL0`, `PMCEID1_EL0`; empty-region cycles; cycles per counter tick, compared with bundle 1's CPU-L rate; whether `L2D_CACHE_REFILL` counts |
| 6 | Returning | Core-0 cacheable-walk A/B: one image alternates `TCR_EL1` between uncached and cached walks in five interleaved blocks of the forced-walk loop of [cpu-table-walks.md](cpu-table-walks.md) | Loads only, inside the 128 MiB that every Returning image maps | `CPU_CYCLES` and `DTLB_WALK` per block |
| 7 | Read | Power-domain words: R_CCU `0x1ac` bit 0, then the policy, status and delay-row words of domains 1, 4 and 5 at `0x07060000 + 0x1000 × domain` | First read of the PCK600 block | Adoption state and the delay row before any write |
| 8 | Returning | PMIC service task and power-domain owner, read-only: S_TWI0 divisor and bit time, the DCDC2 and DCDC4 snapshot of supply verification, adoption reads of domains 1, 4 and 5 | PMIC reads repeat the physically observed DCDC1 path of the PMIC provider probe | Supply verification, each service step's duration against the 110% wire-time bound of [power-domains.md](power-domains.md) |
| 9 | Read | NSI port fields `+0x10`, `+0x14`, `+0x18` of ports 2, 14 and 15, read only when `+0x580` bits 31 and 30 are set | First read of the NSI block at `0x02020000`, from the Secure side | Whether boot0 already sets the priority table |
| 10 | Returning | DRAM bus priority, no-table hook reads at EL1 (`SUNSTONE_DRAM_PRIORITY=1`) | A Non-secure access may raise an external abort, which ends in the fault line | Inherited fields from the Non-secure side ([dram-bus-priority.md](dram-bus-priority.md)) |
| 11 | Returning | Domain 1 ON through the owner; domain 5 ON and OFF only if bundle 8 verified DCDC4 enabled at 800 mV and the GPU preconditions of [power-domains.md](power-domains.md) hold | First PPU write; domain 1 stays ON until power removal | Time to ON and OFF, status read back to back during each transition |
| 12 | Returning | DRAM priority table write and readback, with domain 1 ON | NSI fields persist until power removal, so this runs last | Whether Non-secure writes stick |
| 13 | Terminal | NPU single-core image through `tools/check-a733-npu-supervised.py` (NPU items 3 to 7 of [npu-platform.md](npu-platform.md)) at 492 MHz, and at 852 MHz only if bundle 1's speed bin qualifies | Native USB takeover; ends the board session | Startup to READY and outputs against the simulator; per-job completion counts |

Bundle 13 needs a reconnect of port 3-3 afterwards, which also power-cycles
that board. Port 3-1 runs the same order after its reconnect and a cold DDR
start, unless the rank 7 diagnosis claims it.

## Bundles that wait

- **On ranks 7 and 8.** Event counters and a deliberate fault on an A76
  (core 6); WFI on all eight cores with SGI 0 wake-ups and the EL0 `WFI`
  enable; the core-6 walk A/B and the dirty-line experiment of the cached
  launch paths; tasks on cores 6 and 7 under `A733_A76_ADMITTED`; PMIC
  selectors after PSCI CPU_ON; the DRAM latency window, whose copy load runs
  on cores 0 to 7; the three Terminal rows of the integrated OS image already
  in the tool (NPU service, GPU service, and GPU recognition through the
  dispatcher). The arbiter row also waits for the arbiter itself.
- **On user approval.** CPU clock raises, including A76 operating points;
  DCDC3 and DCDC5 voltage changes; enabling DCDC4 if bundle 8 finds it off;
  WDT0 expiry with fault-record retention and the report after reboot, which
  boots from microSD; intentional GPU and NPU hangs. Memory qualification
  waits for an independent watchdog record or the user's written acceptance
  of an uncovered bus stall.
- **The 12 GiB OS map.** The map is single-core work, but it cannot run now:
  `a733_dram_decode_check()` returns 0 until a register source exists, so
  every image keeps its current map. Three steps come first: a host trace of
  boot0's DRAM controller and firewall writes, a Read bundle of those
  registers, and the memory qualification chain. A Returning image then
  installs level-1 entries 2 to 12 only when the check returns 1, runs with
  SError unmasked, and times `CPU_CYCLES` around the table build.
- **On accessories.** Camera captures below and above 4 GiB; NVMe for the
  PCIe inbound width; a display for the DRAM latency window and GPU output.

## Data flow and ownership

Owners build each image under `~/.cache/sunstoneos/build/<package>/` and
supply its returning app (`main.c`, `build.py --platform-build`, `check.py`),
as `apps/thermal-returning` does. The tool builds nothing. It extends
`tools/prepare-board-bundles.py` with the two classes:

- **Read rows.** The row's capture script declares a module-level literal
  `WORDS`: tuples of an address and either the path of an earlier board record
  that read it or a condition word, mask and value that the script reads
  first. The tool reads the literal with `ast`, as it reads `PREREQUISITES`.
  It refuses a word with neither, a condition read after its dependent, and
  any `sunxi-fel` subcommand other than `readl` and `version`.
- **Returning rows.** The `nm` scan of the Monitor class applies, with its
  USB and secondary-start prefixes. The scan also refuses `monitor_sync`
  without `monitor_renewal_cap`, because an uncapped renewal would let a
  hung image renew past the 2 s deadline. `validate_bundle()` must return a
  platform-equivalence record against bundle 2's ELF, and the tool refuses
  any differing object outside the row's declared overlay. The prerequisite
  is `supervised-ram-recovery`, and the queue timeout is rank 6's 300 s.
- **Runbooks.** For each prepared row, the tool writes `runbook.md` in the
  format of `docs/board-runbooks/`: status, a bundle table with SHA-256,
  host checks, numbered commands with success text, timeouts and admission
  records, stop conditions, and the path without the admission patch. A
  Returning run is

  ```sh
  $PY -B $V/common/bundle-run.py $PORT $S/rN-run 300 "COMPONENT" "EXPECTED" \
    -- $PY -B $B/host/tools/check-a733-kernel.py --fel $FEL --image $B/probe.bin \
       --mode kernel --expect "SUCCESS TEXT" --evidence $S/rN-run \
       --ddr-evidence $D --platform-survey $S/rN-survey/session.json [--warm-return ...]
  ```

  with `$B` the row's bundle and `$D` the DDR record of the current lifetime.
- **Admission.** Every new row has `admission` set to `None`, so the tool
  refuses it until its owner names the admission values and, for bundle 13,
  the completion counts.
- **Plan.** [docs/audit/board-test-plan.md](../audit/board-test-plan.md)
  lists bundles 1 to 13 after rank 9, and the waiting list below them.
- **Memory row.** Its procedure changes from "ranks 1 to 7" to "ranks 1 to 6
  and bundle 1". Memory runs use one core, so rank 7 qualifies nothing they
  use. Bundle 1 supplies the boot-clock report. This change needs the
  coordinator's acceptance.

## Interrupts, memory and synchronization

The tool polls nothing; the outer process waits once on its sandbox. Read
bundles use no board interrupt. Returning images take the EL1 timer
(PPI 30) as their tick and the monitor's secure timer as the recovery
deadline. Bundle 8 spins within one service step on S_TWI0 byte phases, and
bundle 11 polls PCK600 status once per tick after a 50 µs spin, as section 1.3
permits for devices whose interrupt delivery is unqualified. No image touches DRAM outside the 128 MiB
that `platform_init()` maps; the result block stays non-cacheable. All bundles
run one core, so no cross-core ordering is added. On the host, the exclusive
run directory keeps two preparations apart.

## Reuse and removal

Reused: `common/bundle-run.py`, `cache-off.py`, `survey.py`,
`exec-command.py`, the kernel runner with `common/primary.patch`,
`tools/returning_build.py --platform-build`, `platform-equivalence.json`, the
rank 9 capture script's sequential reads, and the tool's sandbox, snapshot
and submission steps. Removed: rank 7 from the memory row's procedure.

## Register sources and unproven sequences

| Sequence | Source | Status |
| --- | --- | --- |
| Oscillator and CPU-L words | Read at EL1 by the mimus runtime ([mimus.md](../mimus.md)); pinned `ccu-sun60iw2-cpupll.c` | Board-read |
| CPU-B, DSU and speed-bin words; NPU clock words | [cpu-clock.md](../cpu-clock.md); [npu-platform.md](npu-platform.md) | First read |
| CCU `+0x580` to `+0x588` | Manual sections 4.1.6.74 to 4.1.6.76; `+0x580` and `+0x588` in the rank 9 capture | `+0x584` first read |
| R_CCU `0x1ac` bit 0 before PCK600 | `ccu-sun60iw2-r.c` lines 144–146; read as 1 on port 3-3 | Board-read |
| PCK600 policy and status | `pck600_domains.c` lines 87–121; PCK-600 TRM `PPU_PWPR`, `PPU_PWSR` | First read |
| Delay row `+0xc00`–`+0xc10` | `pck600_domains.c` lines 126–142 | **Unproven**: absent from the TRM |
| NSI port fields | `sunxi-nsi.c` `nsi_port_setmode`, `setpri`, `setio` | **Unproven**: that a read needs no master power domain, and Non-secure access |
| `MDCR_EL2.HPMN` from `PMCR_EL0.N` | TF-A `0892ad4d` `arch.h`; vendor monitor disassembly | The returning monitor writes no `MDCR_EL2` today |
| `TCR_EL1` change with the MMU on; WFI wake; fault syndrome | Arm DDI 0487, not pinned | **Unproven** by a pinned source |

## Budgets and measurement

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| FEL commands other than `readl` and `version` in a Read bundle | Not applicable | 0 | `ast` scan; a fixture injects `write` and `exe` |
| Read words without a record or an earlier condition word | Not applicable | 0 | `WORDS` scan; fixtures misorder and omit a condition |
| Re-enumerations per Read bundle | 0 for the 17-word rank 9 capture | 0 | Session record: no new USB instance |
| Read bundle duration | 17 words in 0.12 s (rank 9 capture file times, 2026-10-24) | 1 s per 32 words; 120 s queue timeout | File times |
| Returning images accepted with an uncapped renewal, a USB or CPU-start symbol, or no `monitor_entry` | Not applicable | 0 | `nm` scan; a fixture per case |
| Objects differing from bundle 2 outside the declared overlay | 0 for rank 6 | 0 | Equivalence record, checked by the tool |
| Launch to ROM return per Returning image | 2.9 s and 3.4 s from recovery record to runner log for ranks 6 and 9 (file times) | Image deadline 1.5 s inside the 2 s monitor deadline | Result record counter values |
| Re-enumerations for bundles 1 to 12 | Not applicable | 18 (*estimated*: two per Returning image) | Count of guard reservations |
| Wall time for bundles 1 to 12 | Not applicable | 20 min (*estimated*: the guard admits 12 re-enumerations per 600 s at 15 s spacing, so the last six wait for the first to age out, about 11.5 min of guard pacing) | Replay through `wait_needed` in `~/git/mimus/tools/usb_guard.py` |
| Host sandbox, bytes read, copies, submissions | [board-bundles-measurements.md](board-bundles-measurements.md) | Unchanged | `checks/board-bundles.py` |

The board records replace the instruction-model figures of each component
design, such as zero instructions in `WFI` between wake-ups in
[cpu-power-states.md](cpu-power-states.md). No figure above was measured on
the board.

## Left out

- **Combined images.** Bundles 3 to 6 could share one image and save six
  re-enumerations, but a failure would not name its cause.
- **The xHCI `HCCPARAMS1.AC64` read.** It touches a USB controller.
- **Board contact.** The tool does not submit, claim or run a runner.

## Edits outside the owned files

For `primary.patch`:

- `tools/check-a733-kernel.py`: a `fault` mode that passes on monitor status
  1 when the log's `FAIL exception` line has the given `ESR` class and `FAR`.
  The result record's `esr` and `far` hold the EL3 values of the exit `SMC`,
  so the line is the only copy of the EL1 values. Today any nonzero status
  quarantines the instance. The admission change of
  `common/primary.patch` also accepts that return as a warm-return link.
- `platform/a733/monitor.c`: under a build option that only bundles 5 and 6
  set, write `MDCR_EL2.HPMN` from `PMCR_EL0.N` before EL1 entry, so every
  other image keeps a byte-identical monitor.
- `platform/a733/platform.c`: under a build option, install level-1 entries 2
  to 12 when `a733_dram_decode_check()` returns 1, and unmask SError.
- `docs/board-runbooks/README.md`: list the new runbooks.

Requests to owners: the returning apps of bundles 3 to 6 (CPU fault, power
and counter owners), 8 and 11 (power domains), 10 and 12 (DRAM), the capture
scripts of bundles 1, 7 and 9, and the admission values of every row. The
table-walk owner must also accept bundle 6's single interleaved image in place
of the two images that [cpu-table-walks.md](cpu-table-walks.md) specifies.
