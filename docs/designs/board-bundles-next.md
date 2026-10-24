# Design: board-test bundles for the single-core paths of this tranche

Status: **design**. No bundle in this document exists, and none has run.
It extends [the bundle tool design](board-bundles.md) and follows
[performance-architecture.md](../performance-architecture.md). Board state
on 2026-10-24 ([handoff](../handoff.md)): ranks 1 to 6 of the
[board test plan](../audit/board-test-plan.md) passed on both boards. Rank 9,
the native USB channel, passed on port 3-3 twice: `rank-9-usb-v3/` on the
platform revision of the passing kernel check, and `rank-9-usb-v4/` on the
current platform. Both ports were then power-cycled and passed a cold DDR
start. Rank 7 did not return on port 3-1 in two lifetimes, so eight-core
execution is unqualified and rank 8 is blocked. After its reconnect, port 3-1
runs the rank 7 pre-scheduler bundle first; this order then applies to it.

## Bundles one core can run now on port 3-3

Each bundle uses one of two recovery classes:

- **Read.** One fixed reader, `common/board-read.py`, shipped and hashed by
  the tool, generalizes the coordinator's `state-capture-readl.py`. Each
  bundle supplies only a data file `WORDS.json`. The reader saves the 128 FEL
  scratch bytes at `0x71000` with `read`, then issues `readl`. `readl`
  writes a 48-byte A32 stub into that scratch area, runs it in the ROM's
  context and clears `SCTLR.I` there (`aw_fel_readl_n` in the pinned
  sunxi-tools `fel_lib.c`, lines 352–386; [boot.md](../boot.md)). Each
  condition word is read in its own `readl` invocation and compared with its
  mask and value; on a mismatch the reader skips that word's dependents. The
  reader then restores the scratch bytes with `write` and compares them. A
  stalled or aborted read leaves the ROM unable to answer, and the board needs
  a power removal.
- **Returning.** A single-core FreeRTOS image on the returning platform,
  run by `tools/check-a733-kernel.py` as ranks 6 and 9 were. The EL3
  monitor's secure timer forces a return to the ROM 2 s after entry, which
  covers a hang but not a stalled bus access. Each run re-enumerates the
  board twice.

**Gates.** Every word outside CCU and R_CCU needs a condition word. CCU and
R_CCU have no software gate, so a record of the same port and lifetime
admits their words. In a lifetime without such a record, a CCU or R_CCU word
is a first read and sits after every bundle that does not need it.

**Order.** Known results come first, and each first access to a register
block follows every bundle that can run without it, so a stall loses only the
bundles after it. Every Returning run repeats the cache-off helper and survey
that [the runbook README](../board-runbooks/README.md) requires.

| # | Class | Bundle | First access or new risk | Records |
| --- | --- | --- | --- | --- |
| 1 | Read | CCU `+0x580` and `+0x588`, the oscillator word `0x07090160`, CPU-L PLL and mux words `0x08871000` and `0x0887101c` | The CCU and oscillator words were read on port 3-3 by each rank 9 capture; the mimus runtime read the CPU-L words at EL1 | CPU-L boot-clock report for timing records and the memory rows; `MBUS_DFS_EN`; NSI gate and reset |
| 2 | Returning, base `rank-9-usb-v4/` | Fault capture on core 0: a test task loads from `0x4_0000_0000`, above every valid level-1 entry | First board run of `platform_exception`; a translation fault makes no bus access | Controlled outcome below |
| 3 | Returning | Kernel check built with the counter option | First returning-monitor writes of `MDCR_EL3`, `MDCR_EL2` and `HSTR_EL2` | Rank 4's success text; this image becomes the base of bundle 4 |
| 4 | Returning, base bundle 3 | Three stages, each with a stage marker: event counters, the core-0 walk A/B, idle WFI last | The counter option removes the counter traps; WFI may never wake | Samples below |
| 5 | Read | R_CCU `0x1ac` bit 0 as condition, then the policy and status words of domains 1, 4 and 5 at `0x07060000 + 0x1000 × domain` | First read of the PCK600 block | Adoption state |
| 6 | Returning, base `rank-9-usb-v4/` | PMIC service task and power-domain owner, read-only: S_TWI0 divisor and bit time, 32 DCDC2 and DCDC4 snapshots, adoption reads of domains 1, 4 and 5 | The PMIC provider path that `rank-9-usb-v4/` ran with four supply snapshots | Per step: duration, tick interrupts inside it, and at each byte phase, with the interrupt disabled, the TWI interrupt flag and the distributor pending bit of S_TWI0's interrupt (SPI 203 in the device tree); max and distribution against the 110% wire-time bound of [power-domains.md](power-domains.md) |
| 7 | Returning, base `rank-9-usb-v4/` | Domain 1 ON through the owner: delay row `+0xc00`–`+0xc10` read before and after its write, then status read back to back with `CNTPCT` stamps up to the 10 ms bound | First PPU write; domain 1 stays ON until power removal | Time to ON and every intermediate status |
| 8 | Returning, base `rank-9-usb-v4/` | DRAM bus priority: CCU `+0x580`, `+0x584`, `+0x588`, then, only while domain 1 status reads ON, Non-secure reads of the fields of ports 2, 14 and 15, then the table write and readback of those ports only; the USB_PCIE port fields read back unchanged; each access timed with `CNTPCT` | First NSI access; NSI fields persist until power removal | Inherited and final fields, access times, controlled outcomes below |
| 9 | Read | Speed-bin word `0x0300624c`; PLL_NPU and NPU clock words of NPU evidence item 1 | First reads | NPU speed bin |
| 10 | Terminal | NPU single-core image through `tools/check-a733-npu-supervised.py` (NPU items 3 to 7 of [npu-platform.md](npu-platform.md)) | Native USB takeover; ends the board session | Outputs against the simulator; per-job completion counts; domain 1 and NSI state from bundles 7 and 8, which confound its bus timing |

Bundle 10 runs at 492 MHz, and at 852 MHz only when bundle 9's speed bin
qualifies and bundle 6 verified DCDC2 at 800 mV. The 852 MHz point changes
no voltage and no CPU clock, so the user's approval rule does not cover it.
After bundle 10, port 3-3 needs a reconnect, which also power-cycles it.

**Supervised RAM recovery instead of a watchdog record.** The memory rows
wait for an independent watchdog record because their first DDR access spans
gigabytes and can overwrite the monitor. Here each first access is one
register block, placed after every bundle that does not need it, and the user
can remove power every several hours. A stall therefore costs one reconnect
and the bundles after it.

### Controlled outcomes

The kernel runner today quarantines the instance on any nonzero status. Each
Returning row instead lists its controlled outcomes, each matched on the full
result record and log. A match passes without quarantine, the runner records
the parsed EL1 values in `session.json`, and the admission patch accepts the
session as a warm-return link carrying those values. Any other nonzero status
still quarantines.

- **Bundle 2.** Status 1; exactly one `FAIL exception` line; `ESR_EL1` class
  `0x25` with a level-1 translation fault in the ISS; `FAR_EL1` equal to
  `0x4_0000_0000`; `ELR_EL1` inside the test function.
- **Bundle 4.** Status 2, interrupt 29, the WFI stage marker set and the
  counter and walk stages complete: the core did not wake from `WFI` on PPI 30.
- **Bundle 7.** Status 1 with the owner's FAILED record and its status trace.
  No later bundle writes the PPU in that lifetime.
- **Bundle 8.** Status 1 with one `FAIL exception` line whose ISS is a
  synchronous external abort and whose `FAR_EL1` lies in the NSI block; or
  status 1 with a readback mismatch and its stopping field. After an abort, a
  Read bundle of the same fields gives the Secure view.

### Samples in bundle 4

`a733_cpu_pmu_begin` masks IRQs, so a region takes no tick.

- **Counters.** 64 empty regions (minimum, median, maximum of cycles and
  `INST_RETIRED`); a loop of at least 1 ms, whose cycles per `CNTPCT` tick
  give the measured CPU-L rate against bundle 1's configured rate;
  `PMCR_EL0`, `PMCEID0_EL0`, `PMCEID1_EL0`; whether `L2D_CACHE_REFILL` counts.
- **Walk A/B.** Blocks of 1,024 iterations (*estimated* 0.1 to 0.2 ms at
  494 MHz), each loading from a different 2 MiB block of the 128 MiB map
  other than the non-cacheable result block. One discarded warm-up block,
  then twelve blocks in ABBA order. Each block records `CNTPCT` and all six
  events; `DTLB_WALK` must equal the iteration count, or the block is
  refused. The switch is: write `TCR_EL1`, `ISB`, `TLBI VMALLE1`, `DSB ISH`,
  `ISB`. The stage refuses to run when bundle 1 found `MBUS_DFS_EN` set, and
  each board is reported separately.
- **WFI.** Over 1,000 ticks, a lowest-priority task runs `a733_cpu_idle()`
  (`DSB SY`, `WFI`). The stage records the wake count, `CPU_CYCLES` against
  `CNTPCT`, and `INST_RETIRED` and exceptions taken per wake. The Cortex-A55
  TRM (100442_0200_00, section A4.4.1) states that `WFI` gates the clock at
  the top of the core's clock tree but not what the cycle counter does; the
  stage measures it.

## Bundles that wait

- **On ranks 7 and 8.** Counters and a deliberate fault on an A76; WFI on
  eight cores with SGI 0 wake-ups and the EL0 `WFI` enable; the core-6 walk
  A/B and the dirty-line experiment of the cached launch paths; tasks on
  cores 6 and 7, preceded by a Read bundle of the CPU-B and DSU PLL and mux
  words; PMIC selectors after PSCI CPU_ON; the DRAM latency window,
  whose copy load uses eight cores and a display; the three Terminal rows of
  the integrated OS image already in the tool, including GPU recognition. The
  arbiter row waits for the arbiter.
- **On the GPU, NPU and recognizer images.** The core-0 workload A/B of
  [cpu-table-walks.md](cpu-table-walks.md), with `ITLB_WALK`, during a GPU
  frame, an NPU inference and a recognizer inference. Bundle 4's loop
  measures walk cost only; the owner's revert rule and its 1% contiguous-hint
  trigger need these workloads. Domain 4 ON is bound to bundle 10's records
  once the NPU startup requests it through the owner.
- **On user approval.** CPU clock raises; DCDC3 and DCDC5 voltage changes;
  domain 5 ON and OFF, repeated N times, which needs DCDC4 and is the only
  domain that powers off; WDT0 expiry with fault-record retention, which boots
  from microSD; intentional GPU and NPU hangs. Memory qualification also waits
  for an independent watchdog record or the user's written acceptance of an
  uncovered bus stall. It then runs in its own lifetime after a cold DDR start,
  after bundle 1 alone, because domain 1 and the NSI table persist until power
  removal and would add unrecorded bus state to its timings.
- **The 12 GiB OS map.** `a733_dram_decode_check()` returns 0 until a
  register source exists. After a host trace of boot0's DRAM controller and
  firewall writes, a Read bundle of those registers and the memory
  qualification chain, the cached images test the map; their builder already
  gates it (`platform/a733-smp-cached/memory.c`, line 121).
- **On accessories.** Camera captures below and above 4 GiB, NVMe for the
  PCIe inbound width, and a display.

## Data flow and ownership

Owners build each image under `~/.cache/sunstoneos/build/<package>/`; the
tool builds nothing. In `tools/prepare-board-bundles.py`:

- **Read rows** name a `WORDS.json` that the tool carries: entries of an
  address and a gate (condition address, mask, value, or a same-lifetime
  record for CCU and R_CCU). The tool refuses a gate read after its dependent
  and a file with any key outside that schema. It runs no capture script.
- **Returning rows** are Monitor rows with per-row fields: prerequisite
  `supervised-ram-recovery`, runner `tools/check-a733-kernel.py`, the queue
  timeout below, the equivalence base, and the controlled-outcome file. The
  `nm` scan of the Monitor class applies, and it also refuses any image that
  defines `monitor_sync`, the uncapped renewal. `validate_bundle()` returns a
  platform-equivalence record against the base, and the tool refuses any
  differing object outside the row's declared overlay.
- **Runbooks.** For each prepared row, the tool writes `runbook.md` in the
  format of `docs/board-runbooks/`: status, bundle files with SHA-256, host
  checks, numbered commands with success text, timeouts and admission
  records, stop conditions, the controlled outcomes and the path without the
  admission patch.
- **Admission.** Every new row has `admission` set to `None` until its owner
  names the admission values.
- **Plan and memory row.** [docs/audit/board-test-plan.md](../audit/board-test-plan.md)
  lists bundles 1 to 10 after rank 9. The memory row's procedure changes from
  "ranks 1 to 7" to "ranks 1 to 6 and bundle 1", because memory runs use one
  core and rank 7 qualifies nothing they use.

## Interrupts, memory and synchronization

The tool polls nothing. Read bundles take no board interrupt. Returning
images take PPI 30 as their tick and the monitor's secure timer as the
deadline. Bundle 6 spins within one service step on S_TWI0 byte phases, and
bundle 7 reads PCK600 status back to back up to 10 ms; section 1.3 permits
both for devices without qualified interrupt delivery. No image touches DRAM
outside the 128 MiB that `platform_init()` maps. All bundles run one core.

## Reuse and removal

Reused: `common/bundle-run.py`, `cache-off.py`, `survey.py`, the kernel
runner with `common/primary.patch`, `tools/returning_build.py
--platform-build`, the platform objects of `rank-9-usb-v4/`, the reader logic
of `state-capture-readl.py`, and the tool's sandbox, `nm` scan, snapshot and
submission steps. Removed: rank 7 from the memory row's procedure.

## Register sources and unproven sequences

| Sequence | Source | Status |
| --- | --- | --- |
| Oscillator and CPU-L words | Pinned `ccu-sun60iw2-cpupll.c`; read by the mimus runtime ([mimus.md](../mimus.md)) | Board-read |
| Speed-bin, PLL_NPU and NPU clock words | [cpu-clock.md](../cpu-clock.md); [npu-platform.md](npu-platform.md) | First read |
| CCU `+0x580` to `+0x588` | Manual sections 4.1.6.74 to 4.1.6.76 | `+0x584` first read |
| R_CCU `0x1ac` bit 0 before PCK600 | `ccu-sun60iw2-r.c` lines 144–146 | Board-read |
| PCK600 policy, status and command | `pck600_domains.c` lines 87–121; PCK-600 TRM `PPU_PWPR`, `PPU_PWSR` | First access |
| Delay row `+0xc00`–`+0xc10` | `pck600_domains.c` lines 126–142 | **Unproven**: absent from the TRM |
| NSI port fields after domain 1 ON | `sunxi_set_nsi_qos_params` takes the master's domain first ([dram-bus-priority.md](dram-bus-priority.md)) | Port 2 follows that order; ports 14 and 15 **unproven**: their masters' domains are not identified |
| Counter controls | `platform/a733/cpu_continue.S` lines 78–96; TF-A `0892ad4d` `arch.h` lines 796 and 816 | Host-modeled |
| `TCR_EL1` switch with the MMU on; fault syndromes | Arm DDI 0487 | **Unproven**: DDI 0487 is listed but not fetched in `checks/cpu-pmu-sources.json`, so no section can be pinned yet |

## Budgets and measurement

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| FEL commands from a Read bundle other than one scratch `read`, `readl`, one scratch `write` and `version` | Not applicable | 0 | Fixed reader hash; fixtures inject a code key into `WORDS.json` and a changed reader |
| Words with no gate, or a gate read after its dependent | Not applicable | 0 | `WORDS.json` scan; fixtures |
| Scratch bytes restored and equal | 128 of 128 (port 3-1 readl capture) | 128 of 128 | Reader record |
| Read bundle duration | 91 words in 18 `readl` invocations plus save and restore: 1.9 s from recovery record to runner log (port 3-1, 2026-10-24) | 5 s per bundle; queue timeout 120 s, no re-enumeration | Session file times |
| Returning images accepted with `monitor_sync`, a USB or CPU-start symbol, or no `monitor_entry` | Not applicable | 0 | `nm` scan; a fixture per case |
| Objects differing from the base outside the declared overlay | 0 for rank 6 | 0 | Equivalence record |
| Returning runs and re-enumerations for bundles 1 to 9 | Not applicable | 6 runs, 12 re-enumerations | Count fields of the guard events |
| Queue timeout per Returning run | Not applicable | 925 s (*estimated*): 615 s guard wait, eight FEL transactions at the runner's 30 s bound, the 10 s return window and 60 s margin | Computed by the tool from the runner |
| Wall time per board for bundles 1 to 9 | Not applicable | 10 min (*estimated*); no guard window wait, because 12 re-enumerations fit the guard's 12 per 600 s. Both boards share one guard key, so this assumes no other A733 re-enumeration in that window | Session file times |
| Host sandbox, bytes read, copies | [board-bundles-measurements.md](board-bundles-measurements.md) | Unchanged | `checks/board-bundles.py` |

The board records replace the instruction-model figures of each component
design. No figure above was measured on the board.

## Left out

- **The xHCI `HCCPARAMS1.AC64` read.** It touches a USB controller.
- **Board contact.** The tool does not submit, claim or run a runner.

## Edits outside the owned files

For `primary.patch`:

- `tools/check-a733-kernel.py`: controlled outcomes as specified above, and
  their acceptance as warm-return links in `validate_platform_admission`.
- `platform/a733/monitor.c`: under a build option that bundles 3 and 4 set,
  write `MDCR_EL3 = 0x10000000`, `HSTR_EL2 = 0` and `MDCR_EL2.HPMN` from
  `PMCR_EL0.N` with `TPM`, `TPMCR` and `HPME` zero, as `cpu_continue.S` does.
  After reset into EL3 the `MDCR_EL2` trap bits are UNKNOWN, so an EL1 counter
  access could trap to an EL2 vector that no image installs.
- `docs/board-runbooks/README.md`: list the new runbooks.

Requests to owners: a single-core form of `a733_cpu_pmu_begin` and `end` that
checks core 0 instead of `vTaskCoreAffinityGet`, which exists only with more
than one core; bundle 4 measures that form. The returning apps of bundle 2
and 4 (CPU fault, counter, table-walk and power-state owners), 6 and 7 (power
domains) and 8 (DRAM). The admission values of every row. The table-walk
owner's acceptance of one interleaved image in place of two images.
