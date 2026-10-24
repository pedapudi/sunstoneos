# Design: board-test bundles for the single-core paths of this tranche

Status: the Read and returning classes, controlled outcomes and runbook
generation are **implemented** and host-checked. Bundles 1, 2, 3, 5 and 9 are
prepared in `~/.cache/sunstoneos/evidence/board-bundles-20261024/board-bundles-20261024-1/`,
and bundles 1 to 3 passed on port 3-3 on 2026-10-24. The returning apps of
bundles 4, 6, 7 and 8 are **implemented**: bundle 4's image passes in the
secure QEMU model, the sequences of bundles 6 to 8 pass a host model, and the
four bundles are prepared in
`~/.cache/sunstoneos/evidence/board-bundles-20261024/board-bundles-20261024-3/`.
The earlier preparation of the same rows in `board-bundles-20261024-2/` is
superseded and must not run.
All four ran on port 3-3 on 2026-10-24; the [handoff](../handoff.md) records
their results. Bundle 4's prepared walk check was wrong, and the next bundle
4 set must be prepared as section "Samples in bundle 4" states. Bundle 10 stays refused until its runner declares completion counts and
its owner names admission values.
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

Read and Returning are new recovery classes; bundle 10 uses the existing
Terminal class.

- **Read.** One fixed reader, `tools/board-bundles-next-read.py`, is new; the
  tool copies it unchanged into every Read bundle as `board-read.py` and
  hashes it there. It generalizes the coordinator's
  `~/.cache/sunstoneos/evidence/board-session-20261023/state-capture-readl.py`.
  Each bundle supplies only a data file `WORDS.json`. The reader saves the
  128 FEL scratch bytes at `0x71000` with `read`, then issues `readl`. The
  cited lines are those of the A733 build of sunxi-tools,
  `~/.cache/sunstoneos/build/sunxi-a733/fel_lib.c` (revision `b92f4ed` with
  pull request 259 applied). Each command-line `readl` reads one word
  (`fel.c` line 408): it writes a 48-byte A32 stub and the word's slot, the
  same first 52 bytes of scratch whatever the word count, and runs the stub
  in the ROM's context (`aw_fel_readl_n`, lines 352–386). The 128-byte save
  therefore covers every `readl` command. The first FEL write of
  a session also clears `SCTLR.I` in the ROM's context: `aw_fel_write` calls
  `aw_disable_icache` (lines 298–323) because `icache_fix` is set for the
  A733 ([boot.md](../boot.md)). Each condition word is read in its own
  `readl` command and compared with its mask and value; on a mismatch the
  reader skips that word's dependents. The reader restores the scratch bytes
  with `write`, reads them again and compares them. A stalled or aborted read
  leaves the ROM unable to answer, and the board needs a power removal.
- **Returning.** A single-core FreeRTOS image on the returning platform,
  run by `tools/check-a733-kernel.py` as ranks 6 and 9 were. The EL3
  monitor's secure timer forces a return to the ROM 2 s after entry, which
  covers a hang but not a stalled bus access. Each run re-enumerates the
  board twice.

**Gates.** Four blocks have no software gate: CCU, R_CCU, the SID block at
`0x03006000` (`sun60iw2p1.dtsi` lines 1406–1408 name no clock or reset) and
the CPU PLL block at `0x08870000`, whose pinned device-tree node (lines
908–915) names only its oscillator parent and no bus clock or reset.
A record of the same port and lifetime admits their words. In a lifetime
without such a record, a word there is a first read and sits after every
bundle that does not need it. Every other word needs a condition word. The
oscillator word `0x07090160` is the DCXO register of the RTC clock driver
(`ccu-sun60iw2-rtc.c` lines 71 and 78), whose node (`sun60iw2p1.dtsi` lines
901–906) names no gate. The RTC node overlaps the same window with the bus
gate and reset R_CCU `0x20c` bits 0 and 16 (`ccu-sun60iw2-r.c` lines 169–171
and 232; `sun60iw2p1.dtsi` lines 962–970). Gating the word on that R_CCU word
is the conservative choice.

**Order.** Known results come first, and each first access to a register
block follows every bundle that can run without it, so a stall loses only the
bundles after it. Every Returning run repeats the cache-off helper and survey
that [the runbook README](../board-runbooks/README.md) requires.

| # | Class | Bundle | First access or new risk | Records |
| --- | --- | --- | --- | --- |
| 1 | Returning, base `rank-9-usb-v4/` | Fault capture on core 0: a test task loads from `0x4_0000_0000`, above every valid level-1 entry | First board run of `platform_exception`; a translation fault makes no bus access | Controlled outcome below |
| 2 | Returning | Kernel check built with the counter option | First returning-monitor writes of `MDCR_EL3`, `MDCR_EL2` and `HSTR_EL2` | Rank 4's success text; this image becomes the base of bundle 4 |
| 3 | Read | CCU `+0x580` and `+0x588`; R_CCU `0x20c` as condition, then the oscillator word `0x07090160`; CPU-L PLL and mux words `0x08871000` and `0x0887101c` | First reads of the lifetime; these words were read on port 3-3 in earlier lifetimes (the rank 9 captures; the mimus runtime at EL1 for the CPU-L words). Bundles 1 and 2 do not need them, so they come first | CPU-L boot-clock report for timing records and the memory rows; `MBUS_DFS_EN`; NSI gate and reset |
| 4 | Returning, base bundle 2 | Three stages, each with a stage marker: event counters, the core-0 walk A/B, idle WFI last | The counter option removes the counter traps; WFI may never wake | Samples below |
| 5 | Read | R_CCU `0x1ac` bit 0 as condition, then the policy and status words of domains 1, 4 and 5 at `0x07060000 + 0x1000 × domain` | First read of the PCK600 block | Adoption state |
| 6 | Returning, base `rank-9-usb-v4/` | PMIC service task and power-domain owner, read-only: S_TWI0 divisor and bit time, 32 DCDC2 and DCDC4 snapshots, adoption reads of domains 1, 4 and 5 | The PMIC provider path that `rank-9-usb-v4/` ran with four supply snapshots | Per step: duration, tick interrupts inside it, and at each byte phase the TWI controller's interrupt flag, with the interrupt disabled; max and distribution against the 110% wire-time bound of [power-domains.md](power-domains.md) |
| 7 | Returning, base `rank-9-usb-v4/` | Domain 1 ON through the owner: delay row `+0xc00`–`+0xc10` read before and after its write, then status read back to back with `CNTPCT` stamps up to the 10 ms bound | First PPU write; domain 1 stays ON until power removal | Time to ON and every intermediate status |
| 8 | Returning, base `rank-9-usb-v4/` | DRAM bus priority: R_CCU `0x1ac` bit 0 before the domain 1 status read; CCU `+0x580` bits 31 and 30, `+0x584` bits 0 and 16, and `+0x588`; then, only while domain 1 status reads ON and those gates are open and resets released, Non-secure reads of the fields of ports 2, 14 and 15, then the table write and readback of those ports only; the USB_PCIE port fields read back unchanged; each access timed with `CNTPCT` | First NSI access; NSI fields persist until power removal | Inherited and final fields, access times, controlled outcomes below |
| 9 | Read | Speed-bin word `0x0300624c`; PLL_NPU and NPU clock words of NPU evidence item 1 | First reads | NPU speed bin |
| 10 | Terminal | NPU single-core image through `tools/check-a733-npu-supervised.py` (NPU items 3 to 7 of [npu-platform.md](npu-platform.md)) | Native USB takeover; ends the board session | Outputs against the simulator; per-job completion counts; domain 1 and NSI state from bundles 7 and 8, which confound its bus timing |

**S_TWI0 interrupt.** Bundle 6 records the TWI controller's interrupt flag
and leaves out the distributor pending bit of S_TWI0's interrupt (SPI 203 in
the device tree, INTID 235). The monitor keeps `GICD_CTLR.DS` at 0 and assigns
no shared interrupt to Group 1 Non-secure, and a Non-secure read of a Group 0
interrupt's pending bit returns 0. Reading it would need a monitor change to
the interrupt's group and its restoration on return. The flag alone shows
whether the controller raises its interrupt condition at each byte phase,
which is the question this bundle answers; GIC delivery is qualified by the
bundle that first binds the interrupt. The PMIC provider's startup check
reads the enable, pending and active bits of INTID 235 once and requires each
to be 0, as it did in `rank-9-usb-v4/`; the bundle's per-phase record adds no
GIC read.

Bundle 10 runs at 492 MHz, and at 852 MHz only when bundle 9's speed bin
qualifies and bundle 6 verified DCDC2 at 800 mV. The approval rule in
[docs/hardware.md](../hardware.md) line 652 belongs to the CPU clock driver's
entry (lines 640–653), which changes CPU-L, CPU-B and their DCDC5 and DCDC3
voltages. The 852 MHz NPU point changes neither, so that rule does not cover
it. Bundle 10's own runner waits about 400 to 500 s (*estimated*) for the
first of bundles 1 to 9's twelve re-enumerations to leave the guard's 600 s
window. After bundle 10, port 3-3 needs a reconnect, which also power-cycles
it.

**Supervised RAM recovery instead of a watchdog record.** The memory rows
wait for an independent watchdog record for two reasons in
[the base design](board-bundles.md). The first access to untested DDR can
stall the interconnect, which the monitor timer cannot cover. A non-linear
decode can also turn a write to tested DDR into a write to the monitor. Here no bundle
writes DRAM outside the 128 MiB that every Returning image uses, and each
first access is one register block, placed after every bundle that does not
need it. The user can remove power every several hours, so a stall costs one
reconnect and the bundles after it.

### Controlled outcomes

The kernel runner today quarantines the instance on any nonzero status. Each
Returning row instead lists its controlled outcomes, each matched on the full
result record and log. A match passes without quarantine, the runner records
the parsed EL1 values in `session.json`, and the admission patch accepts the
session as a warm-return link carrying those values. Any other nonzero status
still quarantines.

- **Bundle 1.** Status 1; exactly one `FAIL exception` line; `ESR_EL1` class
  `0x25` with a level-1 translation fault in the ISS; `FAR_EL1` equal to
  `0x4_0000_0000`; `ELR_EL1` inside the test function.
- **Bundle 4.** Status 2, interrupt 29, and the log markers of the completed
  counter and walk stages and of the WFI stage's start. The image keeps a
  progress record at `0x42000080`, between the result header and the log; it
  must hold magic `0x3130535342344242`, stage 3, a wake count below 1,000,
  and a last wake stamp more than 48,000 counter ticks (two ticks) older
  than the monitor's `end_counter`. This outcome means the core stopped waking from
  `WFI` on PPI 30. The runner reads the four words from the result page; an
  outcome may name such a record through its `progress` key.
- **Bundle 7.** Status 1 with the owner's FAILED record and its status trace,
  bracketed by `FAILED board bundle 7: domain 1` and `TRACE END board bundle
  7`. No later bundle writes the PPU in that lifetime. When the owner refuses
  the row before any PPU write (a closed CCU or PPU gate, an unverified DCDC2
  or a domain whose policy and status disagree), the image ends with status 0
  and a record that says so; the run is inconclusive, and bundle 8 then finds
  domain 1 not ON.
- **Bundle 8.** Status 1 with one `FAIL exception` line whose ISS is a
  synchronous external abort and whose `FAR_EL1` is one of the twelve field
  words the image reads (ports 2, 13, 14 and 15) and whose `ELR_EL1` lies in
  its access function, one outcome per word; or status 1 with a readback
  mismatch and its stopping field. An SError from a posted write matches no
  outcome. After an abort, a
  Read bundle of the same fields gives the Secure view. When domain 1 status
  does not read ON or a gate is closed, the image skips every NSI access and
  ends with status 0 and a record that names the skipped stage; the run is
  inconclusive and does not admit bundle 10's NSI comparison. A Secure-only
  NSI can return zeros to Non-secure reads, so inherited fields that read
  zero on all three ports are recorded as possibly protected; the write
  readback then separates the two cases.

### Samples in bundle 4

`apps/board-bundles-cpu/main.c` runs the three stages in one task at
priority 1. `a733_cpu_pmu_begin` masks IRQs only for its own register
accesses and restores `DAIF` before the counted window, so the image masks
IRQs around each counter and walk region itself; a region then takes no tick.
The counter and walk stages take a few tens of milliseconds (*estimated*),
and the WFI stage 1,000 ticks. The image starts the WFI stage only before
0.9 s after the monitor's entry, so that it ends before the monitor's 2 s
deadline; a later start ends the run with status 0 and an inconclusive
record. Each stage writes a log marker and the stage word of the progress
record.

- **Counters.** 64 empty regions (minimum, median, maximum of cycles and
  `INST_RETIRED`); 2,000 ALU instructions, whose `INST_RETIRED` less the
  empty median shows the event's accuracy; a loop of at least 1 ms, whose
  cycles per `CNTPCT` tick give the measured CPU-L rate against bundle 3's
  configured rate; `PMCR_EL0`, `PMCEID0_EL0`, `PMCEID1_EL0`; and one load
  per line of a 64 KiB buffer cleaned and invalidated to the point of
  coherency, whose `L2D_CACHE_REFILL` count shows whether the event counts.
- **Walk A/B.** Blocks of 1,024 iterations of `TLBI VMALLE1`, `DSB ISH`,
  `ISB` and one load (*estimated* 0.5 to 1.5 ms for A at 494 MHz, less for
  B). Iteration k loads line k of 2 MiB block k (`base + (k << 21) +
  (k << 6)`), so consecutive loads target different blocks and the loaded
  lines fall in different cache sets: they stay in L1 and none evicts a
  table line, and B's descriptor reads hit the caches. No load targets the
  non-cacheable result block or the block that holds the image, its stacks
  and the PMU state, which the image checks against its link symbols. One
  discarded warm-up block, then twelve blocks in ABBA order: A installs
  `TCR_EL1` `0x200803019` (uncached walks), B `0x200803519` (cached walks,
  the value `platform_init()` installs). Each block records `CNTPCT` and all
  six events. `DTLB_WALK` and `ITLB_WALK` must each equal 1,024, and the
  calibration region before the block (`TLBI VMALLE1`, `DSB ISH`, `ISB`,
  then end) must count no data walk and one instruction walk, or the block
  is refused (`apps/board-bundles-cpu/walk.h`). Each iteration makes one
  data walk for its load and one instruction walk for the fetch after its
  `TLBI`; after the last iteration, the return path runs from the 2 MiB
  block whose entry that walk filled, so it adds none. The calibration's
  instruction walk is the walk that a block already counts once per
  iteration. The image prepared on 2026-10-24 instead required 1,024 plus
  the calibration's count, so on port 3-3 it refused all 13 blocks, whose
  counts were 1,024 and 1,024 in every block. The next bundle 4 set must
  carry the corrected check. `checks/board-bundles-apps.py` passes on the
  bundles' source revision `74d6aeab` with the current bundle sources. The
  bundle 4 image then differs from the prepared one only in `main.c`, and
  the bundle 8 image is byte-identical. The bundle 6 and 7 images differ in
  their main files, `power.c` and `trace.c`, which carry the next bundle 6's
  additions (section "Records of bundles 6, 7 and 8"). On the `bringup` revision `f3cbec23` the check refuses before
  any image is built, because `platform/a733/platform.c` changed after
  bundle 2 was frozen and the central kernel-check object no longer equals
  bundle 2's. The next set is therefore prepared from `74d6aeab` with the
  correction, or on a new base image. The switch is:
  write `TCR_EL1`, `ISB`, `TLBI VMALLE1`, `DSB ISH`, `ISB`. No table write
  follows the final flush of both table pages in `platform_init()`
  (`platform/a733/platform.c` lines 138–139), so uncached walks read the
  same descriptors as cached ones; `checks/board-bundles-apps.py` checks this
  in the source and in the linked object's relocations. The image reads CCU
  `+0x588` itself and refuses the stage when `MBUS_DFS_EN` is set; bundle 3
  read it clear on port 3-3. Each board is reported separately. The run
  records the CPU-L rate only; the CPU-B and DSU rates that
  [cpu-table-walks.md](cpu-table-walks.md) asks for with each walk run wait
  for a Read bundle of their words (see "Bundles that wait").
- **WFI.** Over 1,000 ticks, the task runs `a733_cpu_idle()` (`DSB SY`,
  `WFI`) with IRQs unmasked, so the tick handler runs on each wake. No other
  task is ready, so the idle task does not run. The counters run free after
  the last region; the task reads `PMCCNTR_EL0`, the event counters and
  `CNTPCT` after each wake, outside any masked region, and stores the wake
  count and stamp in the progress record. The stage records the wake count,
  `CPU_CYCLES` against `CNTPCT`, the shortest and longest `CNTPCT` gap
  between wakes, the event totals, and the scheduler ticks, which are the
  exceptions taken. The Cortex-A55 TRM (100442_0200_00, section A4.4.1)
  states that `WFI` gates the clock at the top of the core's clock tree but
  not what the cycle counter does; the stage measures it.

### Records of bundles 6, 7 and 8

- **Bundles 6 and 7** (`apps/board-bundles-power/`) start the PMIC provider,
  the rail ledger and the power-domain owner as `rank-9-usb-v4/` started the
  provider, with S_TWI0 pins PL0 and PL1 at function 2, pull 1 and drive 1,
  the values of that run's input. The provider's startup and the service
  session end 1.5 s after the monitor's entry. One service task at priority
  2 runs `a733_power_service_step()`; a client task runs at priority 1. A
  recording layer sits between each native adapter and its user: it passes
  every S_TWI0 access to an observer that classifies the reads of the
  control register (flag polls after a START or byte, STOP polls after a
  STOP), and stores every owner access with its `CNTPCT` stamp. A domain-1
  row exists in both images, so the service verifies DCDC2 and DCDC4 once;
  bundle 6 never requests it.
- **Log order.** Each image prints its summary and its outcome line first,
  then its detail lines while 256 log bytes remain free, then a count of the
  omitted lines (and bundle 7 its closing line). The host model formats
  every line with each field at its widest value: bundle 6 needs at most
  1,364 bytes before its details and bundle 7 at most 721, of 16,127.
- **Bundle 6** records the owner adapter's mapping result and the
  `PAR_EL1` translation of the domain-1 block. The adapter requires the
  block to translate as Device-nGnRnE and outer-shareable; the descriptor's
  shareability field is 0, and the architecture reports Device memory as
  outer-shareable (unpinned), so a refusal here is identified before bundle
  7. A detail line per service step that completed a bus phase gives the
  `CNTPCT` span, the scheduler ticks inside it, the completed conditions,
  bytes and STOPs, the flag at the first read of each byte phase and the
  most polls of one phase; the wire time follows from the phases and the bit
  time (two bit times per START, repeated START or STOP, nine per byte). The
  summary counts the steps at most 100%, up to 105%, up to 110%, up to 155%
  and above 155% of their wire time; 155% is the provisional bound of
  [power-domains.md](power-domains.md), and the 155% bins are in the source
  for the next bundle 6 set. That set also measures the cost of a provider
  register access, as [pmic-access.md](pmic-access.md) plans it. At the
  park, the service task, which is the provider task, enters the I2C mutex
  through the adapter while the provider is idle and times by `CNTPCT` 256
  iterations each of: S_TWI0 control-register reads with `DSB SY` before
  and after each; plain reads with the value discarded (issue throughput);
  plain reads, each address depending on the previous value (latency); the
  adapter's time and read operations; the task check the earlier adapter
  made per access, with IRQs unmasked; and the trace observer, as a flag poll
  that reads the flag clear. The record names the service's core
  (`MPIDR_EL1` Aff1). Every S_TWI0 write of the snapshots is timed in
  place, and these two stamps per write are inside the step times. Once,
  after a byte-phase control write, the control register is read back to
  back until its flag sets, which gives one byte's wire time; that step is
  excluded from the step records and counted. A control-register read has
  no side effect. The `TWI_COST` lines print the windows; the
  `STEPS` line also prints whether that step was excluded and the re-spins,
  flag polls beyond the second of a phase, over the recorded steps, and each
  step line its own re-spins.
- **Bundle 7** reads the domain-1 words before the request, then lets the
  owner run its sequence. After the owner's ON command reaches the PPU, the
  layer reads the status back to back until it reads ON or 10 ms pass, and
  records the first value and each change with its stamp and the stamp of
  the read before it, before the owner continues. The owner starts its own
  10 ms bound after that spin, so a domain that never reaches ON fails up to
  20 ms after the command. The client reads the same words again after ON.
  Domain 1 stays ON until power removal, so the bundle gives one sample per
  board lifetime.
- **Bundle 8** (`apps/board-bundles-dram/`) keeps its sequence in
  `sequence.c`, free of FreeRTOS and hardware, so the host model runs it.
  The gates come first; the first NSI accesses are the nine field reads of
  ports 2, 14 and 15, then the three USB_PCIE fields of port 13 (the
  `CONFIG_ARCH_SUN60IW2` value of `MBUS_PMU_USB_PCIE` in the pinned
  `sunxi-nsi.h`), then `a733_dram_priority()` in table mode, then port 13
  again. Every access is one `DSB SY`-fenced 32-bit access through one
  function, timed with `CNTPCT`; a span therefore holds the bus access and
  its barriers only, without the address translation and deadline checks
  of the framebuffer's register path. An access that the register pair
  refuses, as when its trace is full, ends the run as a refused access,
  never as a readback mismatch.

## Bundles that wait

- **On the next lifetime of port 3-3.** A Read bundle of the CPU-B and DSU
  PLL and mux words beside the CPU-L words of bundle 3, so that each walk
  run carries all three rates.
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
- **On user approval.** CPU clock raises; DCDC3 and DCDC5 voltage changes,
  including the service-step timing of a DCDC3 selector write, for which
  the power-domain design's host model predicts two steps with one tick wait;
  domain 5 ON and OFF, repeated N times, which needs DCDC4 and is the only
  domain that powers off; WDT0 expiry with fault-record retention, which boots
  from microSD; intentional GPU and NPU hangs. Memory qualification also waits
  for an independent watchdog record or the user's written acceptance of an
  uncovered bus stall. It then runs in its own lifetime after a cold DDR start,
  after bundle 3 alone, because domain 1 and the NSI table persist until power
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
tool builds nothing. `checks/board-bundles-apps.py` builds the images of
bundles 4, 6, 7 and 8 and writes the `build.json` that the tool reads. It
links each image from the exact objects of an image that passed: bundle 2's
for bundle 4, and `rank-9-usb-v4/`'s platform and provider objects for
bundles 6 to 8. It runs from the source tree, outside
`~/.cache/sunstoneos`, because the provider objects record the compile
directory. It first reproduces those objects from this tree: every
object byte-identical, except `monitor.c` in `rank-9-usb-v4/`, whose
instructions are equal because only its line table moved with the counter
option's lines. In `tools/prepare-board-bundles.py`:

- **Read rows** carry their words in the row table, and the tool writes them
  to `WORDS.json`: each entry is an address and either the ungated block that
  holds it or a condition word (address, mask, value) listed earlier. The
  reader's `plan()`, which the tool runs on every Read bundle, refuses a gate
  read after its dependent, a word outside its named block and any key
  outside that schema. A same-lifetime record is a matter of order in the
  plan, so the file carries none. The tool runs no capture script.
- **Returning rows** are Monitor rows with per-row fields: prerequisite
  `supervised-ram-recovery`, runner `tools/check-a733-kernel.py`, the queue
  timeout below, the equivalence base, and the controlled-outcome file. The
  `nm` scan of the Monitor class applies, and it also refuses any image that
  defines `monitor_sync`, the uncapped renewal. `validate_bundle()` returns a
  platform-equivalence record against the base, and the tool refuses any
  differing object outside the row's declared overlay. Passed objects come
  from the kernel check of ranks 4 and 5, `rank-9-usb-v4/` and bundle 2. The
  queue timeout is computed from the bundle's own copy of the runner.
- **Runbooks.** For each prepared row, the tool writes `runbook.md` in the
  format of `docs/board-runbooks/`: status, bundle files with SHA-256, host
  checks, numbered commands with success text, timeouts and admission
  records, stop conditions, the controlled outcomes named in the bundle's
  `controlled.json`, and the path without the runner patch.
- **Admission.** Rows 1 to 9 admit the value that their preparation step
  returns when it names the row's schema and the row; row 10 keeps
  `admission` at `None` until its owner names the admission values.
- **Plan and memory row.** [docs/audit/board-test-plan.md](../audit/board-test-plan.md)
  lists bundles 1 to 10 after rank 9. The memory row's procedure changes from
  "ranks 1 to 7" to "ranks 1 to 6 and bundle 3", because memory runs use one
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
| Oscillator word, gated on R_CCU `0x20c` | `ccu-sun60iw2-rtc.c` lines 71 and 78; `ccu-sun60iw2-r.c` lines 169–171 and 232; `sun60iw2p1.dtsi` lines 901–906 and 962–970 | Board-read in earlier lifetimes |
| CPU-L words, CPU PLL block without a bus gate | Pinned `ccu-sun60iw2-cpupll.c`; `sun60iw2p1.dtsi` lines 908–915; read by the mimus runtime ([mimus.md](../mimus.md)) | Board-read in earlier lifetimes |
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
| FEL commands from a Read bundle other than two scratch `read`s (save, then compare after restore), `readl`, one scratch `write` and `version` | Not applicable | 0 | Fixed reader hash; fixtures inject a code key into `WORDS.json` and a changed reader |
| Words with no gate, or a gate read after its dependent | Not applicable | 0 | `WORDS.json` scan; fixtures |
| Scratch bytes restored and equal | 128 of 128 (port 3-1 readl capture) | 128 of 128 | Reader record |
| Read bundle duration | 91 words in 18 `readl` invocations plus save and restore: 1.9 s from recovery record to runner log (port 3-1, 2026-10-24) | 5 s per bundle; queue timeout 120 s, no re-enumeration | Session file times |
| Returning images accepted with `monitor_sync`, a USB or CPU-start symbol, or no `monitor_entry` | Not applicable | 0 | `nm` scan; a fixture per case |
| Objects differing from the base outside the declared overlay | 0 for rank 6 | 0 | Equivalence record |
| Returning runs and re-enumerations for bundles 1 to 9 | Not applicable | 6 runs, 12 re-enumerations | Count fields of the guard events |
| Queue timeout per Returning run | Not applicable | 925 s (*estimated*): 615 s guard wait, eight FEL transactions at the runner's 30 s bound, the 10 s return window and 60 s margin | Computed by the tool from the bundle's copy of the runner |
| Objects of bundles 4, 6, 7 and 8 that differ from passed images | Not applicable | Only the declared overlay | Equivalence record of each bundle |
| Log bytes per returning image | 16,127 available after the result header | Summary and outcome line never cut: details stop while 256 bytes remain | Bundle 4 model log (8,343 bytes); host-model bound for bundles 6 and 7 |
| NSI accesses of bundle 8 | 0 | 0 behind a closed gate or an OFF domain; 43 register accesses from zero fields, of which 6 writes, all on ports 2, 14 and 15 | `checks/board-bundles-apps-model.c` |
| Bundle 6 service steps at 400 kHz | 687 µs per snapshot step in the owner's model, 107% of wire time | At most 110% of the wire time of their phases; the board measured 146% (median), and the owner's design sets a provisional bound of 155% for a snapshot step ([power-domains.md](power-domains.md)) | Bundle 6 STEP lines |
| Wall time per board for bundles 1 to 9 | Not applicable | 10 min (*estimated*); no guard window wait, because 12 re-enumerations fit the guard's 12 per 600 s. Both boards share one guard key, so this assumes no other A733 re-enumeration in that window | Session file times |
| Host sandbox, bytes read, copies | [board-bundles-measurements.md](board-bundles-measurements.md) | Unchanged | `checks/board-bundles.py` |

The board records replace the instruction-model figures of each component
design. Bundles 1 to 3 recorded only their pass and their words. The figures
of bundles 4 to 8 are recorded in the [handoff](../handoff.md) and in the
designs that own them: [cpu-table-walks.md](cpu-table-walks.md),
[cpu-pmu-measurements.md](cpu-pmu-measurements.md),
[cpu-power-states.md](cpu-power-states.md) and
[power-domains.md](power-domains.md). The other figures in this design are
estimates or model results.

Offline results of the four new images (`checks/board-bundles-apps.py`):

- **Bundle 4.** The secure QEMU model (Cortex-A55, one core) prints the
  three stage markers, 13 walk blocks and 1,000 wakes over 1,000 ticks, and
  exits 0 with an 8,343-byte log; every value prints at a fixed width and
  every line count is fixed, so the board's log has the same length. QEMU
  implements none of the six events, so every walk block is refused there
  and `INST_RETIRED` reads 0. The model whose tick stops before `WFI` prints
  the WFI marker and returns through the secure timer with status 2; its log
  matches the controlled outcome. Image: about 35 KB of text, 3 new objects.
- **Bundles 6 and 7.** About 100 KB of text each and 6 new objects; no QEMU
  model exists for the A733 power devices. The host model checks the TWI
  observer on one read transaction (2 conditions, 4 bytes, 1 STOP, 42 bit
  times), the step distribution, the status spin with an intermediate value
  and with the bound, and the log bound above.
- **Bundle 8.** About 26 KB of text and 4 new objects. The host model runs
  the sequence and the table-mode hook against a register file. Behind a
  closed PPU clock it makes no PPU or NSI access; with domain 1 OFF or in
  transition, or with the NSI gate or reset closed, no NSI access. From zero
  fields it writes six fields and at the table none; it reports a mismatch
  when writes are ignored, a refused access when the register pair refuses
  a readback, and detects a write that reaches port 13. Its log holds
  fixed-width lines for at most 64 traced accesses, under 10 KB in all.

## Left out

- **The xHCI `HCCPARAMS1.AC64` read.** It touches a USB controller.
- **Board contact.** The tool does not submit, claim or run a runner.

## Edits outside the owned files

The counter option in `platform/a733/monitor.c`, the single-core form of
`a733_cpu_pmu_begin` and `end`, and the controlled outcomes with their
warm-return admission are merged. `primary.patch` of the bundle apps holds:

- `tools/check-a733-kernel.py`: the optional `progress` key of a controlled
  outcome (offset, magic, stage, count bound, minimum age) and its match
  against the four words at that offset of the result page and the
  monitor's `end_counter`, also in a warm-return link;
  `checks/kernel-platform-admission.py` gains the link, refusal and runner
  cases.
- `tools/board-bundles-next-read.py`: the start and end time of each FEL
  command in the session record.
- `checks/cpu-pmu.py`: a case for the single-core form, compiled with the
  returning images' configuration.
- `checks/index.json`: the entry of `checks/board-bundles-apps.py`, in
  place of that of `checks/board-bundles-images.py`, the builder of bundles
  1 and 2, which `checks/board-bundles-apps.py` supersedes.
- `docs/board-runbooks/README.md` and `docs/audit/board-test-plan.md`: the
  second prepared set and its runbooks.

The table-walk owner's acceptance of one interleaved image in place of two
images stays open.
