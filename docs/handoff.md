# Start here

This section states where SunstoneOS bring-up stands. The sections after it
are the dated record that produced it; read them only for a subsystem's
history.

## Board access

- **Boards:** two Allwinner A733 boards, on USB ports 3-1 and 3-3. Both are
  powered only through USB, and both are in speed bin 4.
- **Power cycles:** the user cannot power-cycle either board until further
  notice (since 2026-10-25).
- **Current lifetimes:** each board holds one lifetime in
  `~/.cache/sunstoneos/evidence/board-session-20261025-36/port-3-<n>`: a
  standard cold DDR start (`s7-ddr`), then two read-only DRAM captures. Until
  the user returns, only images that return to FEL and continue these
  lifetimes may run, with warm-return lists that start from those records.
- **What waits for the user:** anything that needs a fresh lifetime, an
  operator at the board, or a power removal at the end.
- **Who runs boards:** only the coordinating agent operates the boards,
  through `tools/board-test-queue.py` with the coordinator token at
  `~/.cache/sunstoneos/board-test-coordinator.token`.

## Qualified on the boards

- **CPU:**
  - All eight cores ran a timer and interrupt workload at the vendor top
    points for bin 4: A76 at 2002 MHz and 1050 mV (DCDC3), A55 at 1794 MHz
    and 1000 mV (DCDC5), DSU at 1274 MHz (CPU step 6, port 3-3).
  - A return to the boot ROM resets the CPU-L and DSU PLLs but not the CPU-B
    PLL, so every image lowers CPU-B before a successful return.
- **Memory bus:** the MBUS restore to 600 MHz at every EL3 entry passed on both
  boards.
- **GPU:**
  - Power on and off, firmware start, health check and stop, all from the
    open DDK only.
  - One compute job, then eight jobs in one firmware session (80 to 113 µs
    per job), on port 3-1.
  - The GPU runs at the vendor top point, 1,008 MHz at 960 mV.
- **NPU:**
  - Startup in 4.0 ms, with completion signalled by interrupt.
  - ShuffleNetV2 in 2.91 ms at 852 MHz, the vendor top point; the per-job
    overhead is 0.24 ms.
- **microSD:** returning reads at high speed (3.3 V, 50 MHz, 4-bit); all 256
  reads completed.
  - SDR104 is not qualified: its 1.8 V switch stopped at CMD11.
  - Measured PIO throughput is 1.4 to 1.7 MB/s. Read-only DMA is now
    implemented and software-checked; its physical throughput is unmeasured.
- **DRAM training restore:** captures from ten lifetimes fix a per-board
  restore set of 1,036 registers (`restore-set-v3.json`). Initialization without training takes
  36 ms, against 5.1 s to train all four P-states. The restore itself is
  implemented and independently reviewed for RAM-only execution. Seventeen
  native instruction cases, nineteen host cases, both board record producers
  and seven capture-refusal cases pass under
  `~/.cache/sunstoneos/evidence/resume-dram-restore-1025/`. Physical memory
  validation and card-boot integration remain outstanding. The 36 ms figure
  excludes training and does not establish working restored memory.

## Active branches

Autonomous development has resumed. The coordinator preserves both initialized
board sessions while implementations and independent reviews proceed without
hardware access. Each proposed physical test still needs a reviewed return and
failure path compatible with the current session; a normal ROM return alone
does not establish recovery from a fault.

| Work | Branch | Immediate deliverable |
| --- | --- | --- |
| DRAM restoration | `work/resume-dram-restore-1025` | Implement the card-boot loader using verified early single-sector reads and explicit helper storage |
| Radio startup | `work/radio-os-staging-1026` | Stage existing firmware and profiles for the OS while preserving physical wiring requirements |
| GPU recognition | `work/resume-gpu-wait-1026` | Bound caller waits and integrate supervised recorded recognition without relying on watchdog expiry |
| NPU restart | `work/resume-npu-restart-1026` | Add normal worker-mediated rearm while preserving registered objects, domain and SRAM ownership |

Independent reviewers cover CPU/thermal/DRAM, accelerator operation and peripheral
integration. Workers reuse successful evidence and submit focused checks. The
coordinator integrates accepted work, updates the canonical records and pushes
`bringup`. No board command has run during this continuation.

The latest merged work includes paired training-record selection and updates,
read-only microSD DMA, eight-worker supervised OS recognition, and CPU/NPU
performance controls. The supported NPU OS image also rebuilds
successfully against the shared service and thermal changes; its evidence is
under `~/.cache/sunstoneos/evidence/resume-thermal-npu-integration-1025/`.

The branch notes under "Next steps" in each design document say what remains.
- **`work/cpu-top-opp-step6-1025`:** CPU top points, step 6 and its
  analysis. Step 7, the sustained load, needs the 70 °C OS thermal stop and an
  operator.
- **`work/sd-returning-1025`:** returning SD reads, the CMD11 diagnosis and
  the DMA design.
- **`work/dram-fast-boot-1025`:** DRAM captures and analysis, the restore
  design and the `dram-restore-test-1` runbook.
- **`work/perf-modes-1025`:** the original high/low performance-mode work.
  Its reviewed integration is
  now merged from `work/resume-perf-modes-1025`; physical qualification remains
  outstanding and follows CPU step 7.

Most of the other `work/*` branches are superseded or already merged and can
be pruned.

## Known problems and next steps

- **CPU returning image layout is repaired.** Both workload profiles fit
  the existing 16 KiB reservation. The ordinary profile has 3,004 bytes free;
  the top-point profile has 68 bytes free. Both builds and nine instruction
  cases plus eight host-runner cases per profile pass. Evidence is under
  `~/.cache/sunstoneos/evidence/resume-cpu-return-20261025/`.
  A separate reviewed helper now restores DCDC3 and DCDC5 to 800 mV after
  verifying boot clock settings and acquiring the corresponding clock and rail
  ownership. Thirteen actual-provider cases and native variants 33, 21 and 30
  pass under `~/.cache/sunstoneos/evidence/resume-cpu-rails-20261025/`.
  Physical voltage restoration remains unqualified. A failed top-point
  workload can also retain raised clocks, so this layout repair does not
  authorize a top-point run in the current board sessions.
- **GPU failure cleanup is corrected in software.** The reviewed shutdown
  path retains power and ownership when idle cannot be proved. It no longer
  forces reset/OFF after failed idle checks or bypasses idle checks solely on
  a firmware power-off acknowledgement. Three native builds and twenty focused
  ELF-model cases pass under
  `~/.cache/sunstoneos/evidence/gpu-recovery-20261025/`.
  Physical failure recovery remains unqualified. A retained failure ends GPU
  use for that powered lifetime; it does not establish reusable GPU state.
- **NPU interrupt waits are bounded in software.** Pending mask, rearm and
  stop callbacks expire after 10 ms plus one scheduler tick, observed on the
  next worker pass. Expiry retains resources; it does not prove hardware drain.
  Immediate successful callbacks add no timer or MMIO reads. Twelve focused
  FreeRTOS/QEMU cases and the selected integration build pass under
  `~/.cache/sunstoneos/evidence/resume-npu-lifecycle-1025/` and
  `~/.cache/sunstoneos/build/gate-resume-npu-lifecycle-1025/`.
  Physical faults, thermal response, in-place restart and domain OFF remain
  unqualified.
- **OS thermal qualification support is implemented.** The session can lower
  the CPU critical threshold to 70 °C; zero retains the normal policy. A
  bounded binary report preserves the first recognition interval's temperature
  and freshness records without enlarging the transcript. The report is a
  separate USB diagnostic operation, admitted only when the image declares it.
  The CPU-clock/A76 build and focused policy, admission, owner-order and
  report checks pass under `~/.cache/sunstoneos/evidence/resume-thermal-1025/`.
  Physical thermal response remains unqualified. Critical termination can stop
  USB, so successful delivery of a terminal temperature report is not guaranteed.
- **microSD voltage-switch corrections are implemented.** Clock-restart
  completion and flag clearing now occur in one driver poll, and the full
  10 ms completion timeout starts after the 1 ms delay. Focused time-driven
  cases reject the prior scheduling gap, premature deadline and bit-10-only
  completion behavior. Source-backed core/platform checks and both UHS and
  non-UHS compilation pass under
  `~/.cache/sunstoneos/evidence/resume-sd-completion-1025/cmd11/`.
  The README command now includes `--read-record`. No new physical switch ran;
  the next SDR104 lifetime still needs power removal. Read-only descriptor DMA
  is now implemented for up to 256 sectors per
  transfer. It validates descriptor ranges, waits for controller drain and
  publishes received data only after cache synchronization. Focused models,
  affected sanitizer checks, host decoding and native builds pass under
  `~/.cache/sunstoneos/evidence/resume-sd-completion-1025/snapshot/`.
  Physical DMA, cache coherence, SDR104 and throughput remain unqualified.
  The first DMA image cannot use either preserved session: timer/exception
  exits bypass SD stop, and successful return retains SD configuration.
  Its accepted lifetime ends with power removal. The source-backed scheduling
  decision is under
  `~/.cache/sunstoneos/evidence/sd-dma-eligibility-20261026/`.
  The host producer now supports `--profile 2 --plan first-read`: one 4096-byte
  read at sector 0 using the unchanged native binary. Successful decoding
  requires matching card identity and CRC from the accepted PIO reference.
  Focused producer and decoder checks pass under
  `~/.cache/sunstoneos/evidence/sd-first-read-host-1026/`; the existing 240-read
  measurement mode remains available. No physical test ran.
- **Paired DRAM training records are implemented.** Selection chooses the newest
  eligible record; updates write the standby payload before its header and
  preserve the other copy. Thirty-two focused cases, including interruption
  at all 28 sector boundaries, pass under
  `~/.cache/sunstoneos/evidence/resume-dram-record-copies-1025/`.
  The exercised allocation and backup are ordinary-file fixtures. Neither
  physical card has an established training-record allocation or full backup.
  The boot worker is implementing the card hook; physical restoration and
  temperature qualification remain outstanding.
- **CPU/NPU performance modes are integrated in software.** The optional service
  selects the highest operating points for the speed bin by default, with a
  lower-power mode and thermal overrides. It changes NPU clocks only after
  local stop, bounds cleanup waits and retains ownership on uncertain replies.
  CPU-only and combined CPU/NPU native builds and focused owner, planner and
  host checks pass under
  `~/.cache/sunstoneos/evidence/resume-perf-modes-1025/final-snapshot/`.
  A combined CPU/NPU performance build also passes with the new mimus source
  pin and default two workers; actual compile flags, linked worker count and
  image capabilities are recorded under
  `~/.cache/sunstoneos/evidence/integrate-perf-pin-1026/`.
  USB operation 8 carries performance control; operation 7 retains thermal
  reporting. This profile reports GPU state but has no GPU runtime owner.
  Physical switching, thermal response and recovery remain unqualified.
- **Radio and camera native application paths are implemented in software.**
  The radio diagnostic composes actual shared providers, card and transport
  constructors. Its native build, twelve modeled application cases, constructor
  composition and nine producer refusals pass under
  `~/.cache/sunstoneos/evidence/radio-native-20261025/`. Fitted supply topology,
  PM output-latch provenance and inherited PM2 behavior still prevent physical
  admission. Missing authority produces a refusal image before provider startup.
  Normal OS radio ownership now has a separate, reviewed pin contract: it
  preserves live PM outputs from documented readback and initializes inactive
  latches before enabling outputs. Strict returning diagnostics retain their
  complete-latch requirement. Focused pin/admission checks and the native
  radio/headset OS build pass under
  `~/.cache/sunstoneos/evidence/radio-startup-facts-1026/`.
  This removes dormant latch history as a normal OS prerequisite; fitted supply
  wiring, PM2 suitability and host firmware/profile staging remain unresolved.
  The camera stream uses two slots and polls capture during each copied chunk.
  It exports one immutable frame, then reports retained hardware because packet
  drain is unproven. Native/model/host evidence is frozen under
  `~/.cache/sunstoneos/evidence/resume-camera-native-1025/final-snapshot/`.
  The [stop-source audit](camera-stop.md) establishes a parser final-frame
  boundary but leaves receiver, residual traffic and IOMMU retirement unresolved.
  It changes no driver behavior or test evidence. No camera is attached; no
  physical capture or drain is qualified.
- **Do not run GPU set `board-bundles-20261025-31`.** It repeats the qualified
  eight-job session with the older image, which lacks the shutdown on every
  failure path.
- **Mimus acceleration.** Only single-core recognition is board-qualified:
  19.5 s of compute for an 8.87 s recording at the old 494 MHz clock. The next
  steps, in order:
  1. Qualify two, then eight, inference workers at the established top clocks.
     Optional eight-worker native builds and exact QEMU recognition parity now
     pass (`docs/mimus-smp.md`). Independent row work and aligned matrix
     multiplication use eight workers; stages with two scratch contexts remain
     limited to two. Evidence is under
     `~/.cache/sunstoneos/evidence/resume-mimus-eight-20261025/`.
     Eight-worker supervised recorded recognition is now integrated into the OS,
     with A76 admission required. A55 inference runs below OS services; the
     image declares its actual placement and priorities for host verification.
     The combined CPU/NPU performance image, focused FreeRTOS service/retention
     case, 31 host cases and 13 compiler/configuration cases pass under
     `~/.cache/sunstoneos/evidence/resume-os-eight-20261026/`.
     Resident, capture, radio, headset and GPU-linear modes still reject eight
     workers. Physical multicore recognition and service latency remain
     unqualified; two workers remain the default.
     Builds use the local unpublished mimus pin `967747f10dcf038aadc2961b5990d540f6b76455`;
     set `MIMUS_DIR=/home/sunil/git/mimus-worktrees/sunstoneos-eight-workers-1025`.
     The cached `cfg.sh` helper now derives its default from that source record;
     explicit source overrides still work. Argument checks pass under
     `~/.cache/sunstoneos/evidence/configure-source-pin-20261026/`.
     Do not publish the separate mimus repository without user approval.
  2. The first board execution of the exact-sum GPU kernel
     (`docs/gpu-mimus.md`). Its returning diagnostic and host validator are
     implemented, reviewed and frozen under
     `~/.cache/sunstoneos/evidence/gpu-exact-sum-20261025/`. Bundle 34 runs
     two shapes and requires exact sums, tags, guards, cleanup and shutdown.
     Five native caller cases, 204 modeled parity jobs and focused host
     admission/capture cases pass. GPU execution remains unqualified for this
     kernel; no new board test ran during this integration.

  The target in `docs/mimus.md` is 0.44 s of compute for that recording. The
  NPU path is research only (`docs/npu-mimus-linear.md`).

## Rules every agent follows

- **No `rm`:** use `~/.cache/sunstoneos/bin/clean-generated.py`, or move
  files to `~/.cache/sunstoneos/trash/`.
- **Reference first:** derive every board sequence from the vendor sources,
  Linux, TF-A and vendor blobs before a board run. Check a blob's license
  first: the PowerVR firmware and the Vivante libOpenVX forbid disassembly.
- **Vendor top points by default:** default every block to the vendor's
  highest operating point for the chip's speed bin. Lower clocks are optional
  modes, never the default. The user has approved the CPU, GPU and NPU clock
  and voltage changes, and 1.8 V microSD signalling.
- **The gate for every change:**
  - a design note through the performance and simplicity panel;
  - the implementation;
  - an independent three-lens review that converges;
  - measurement;
  - `tools/integration-gate.py` with an explicit `--reason`.

  Then merge into `bringup` without piping `git merge`, checking for unmerged
  paths, and push after a secret scan. No co-author lines.

# Current work

**Development resumed on 2026-10-24 at the user's request.** The board-free
work is merged: the PMIC step bound (`b41460e6`), the returning GPU image, the
NPU profile of the resident OS image, the minor review fixes (`3f7d16b4`) and
the next board bundles. The bound is the board median plus about 20 µs (799 µs,
and 839 µs for the first step), 1,268 permille of the step's own wire time; 110%
stays open for writes. The boards stay untouched until the user reports both
reconnected.

**Port 3-1, third lifetime** (session `board-session-20261024-11/port-3-1`,
set `board-bundles-20261024-11`). Cold DDR and GPU provider readiness passed.
The delay-row probe (`ade71834`) failed like the earlier one: CORE6 status
stayed 0 after the delay row, after PWCR 0 and after policy 8, and the final
poll timed out (`step=38`). The snapshot before any write shows that the CORE6
delay row already holds the vendor values at reset, so a missing delay row is
ruled out. CORE6 `+0x10` reads 1 where TOP5 reads 0, and TOP5's MISR reads
0x100 (its device accepted) where CORE6's reads 0. The claim is held, the
lifetime has ended, and analysis continues on `work/gpu-core6-c-1024`.

**Port 3-1, fourth lifetime** (session `board-session-20261024-14/port-3-1`,
set `board-bundles-20261024-14`, 2026-10-25). After the user's power removal,
cold DDR (12,288 MiB) and GPU provider readiness passed. The wrapper-read
probe returned to the ROM with the expected status 1: CORE6 again did not
reach ON, and all four GPU wrapper reads completed. So the wrapper at
`0x01880000` is reachable with TOP5 ON and CORE6 unacknowledged.

| Address | Vendor name | Value | Meaning |
| --- | --- | --- | --- |
| `0x01880104` | `pwr_qchan_cfg` | `0x0` | Core 0's Q-Channel is not under software control; `q_active`, `q_accept`, `q_deny` and the core-0 clock enable are all 0 |
| `0x01880100` | `pwr_cmp_dly` | `0x0` | Power-complete up and down delays are unset |
| `0x01880018` | `mult_core_cnt` | `0x2` | Two GPU cores |
| `0x0188001c` | `mult_core_id` | `0x10` | Primary core 0, secondary core 1 |

This fits the leading explanation in `docs/gpu-power.md` on
`work/gpu-core6-c-1024`: nothing has brought up the wrapper logic that
answers CORE6's Q-Channel. The next image writes the wrapper in the order
the vendor DVFS source uses, then requests CORE6 ON. The claim closed as
failed with the controlled result, and the queue resumed. The runbook ends
the lifetime after this step, so the next GPU run needs another power
removal.

**Port 3-1, fifth lifetime** (session `board-session-20261025-1/port-3-1`,
set `board-bundles-20261025-3`, branch `work/gpu-core6-c-1024` at
`3e188e6a`). Cold DDR and GPU provider readiness passed. The probe-read image
(bundle 17) did not return within the host's 925 s. Its order was the
power-only ON, read-only reads of wrapper `+0x14` and `+0x10` and SMC0
`0x0a0000b0`, then the GPU core identity as two 32-bit loads (`0x01800024`,
`0x01800020`), as the vendor's probe reads it. The SMC0 and `drm_ctrl`
writes come only after that read returns, so the stall happened at or before
the first identity read, with no write made. The two 32-bit loads alone do
not make the core readable at 400 MHz from PERI0 at 800 mV. The claim is
held, and the board needs a power removal.

**Port 3-1, sixth lifetime** (session `board-session-20261025-3/port-3-1`,
set `board-bundles-20261025-4`): cold DDR and readiness passed. The vendor
clock and reset order probe (bus reset released before the bus gate and core
clock, then the identity as two 32-bit loads, no other new access) did not
return. Reset order, access width and the never-proven reads are ruled out.

**Port 3-1, seventh lifetime: the GPU core answers** (session
`board-session-20261025-4/port-3-1`, set `board-bundles-20261025-5`, bundle
19, branch `work/gpu-core6-c-1024` at `750fa092`). Cold DDR and readiness
passed. The vendor-state image applied the three differences the GPU backsolve
found, in the vendor order:
- the 28 BL31 cold-boot security writes at EL3: SPC ports, `0x07010290` = 7,
  CCU security switch `0x02003f00` = 7, port 14 `0xfffffbff`;
- DCDC4 to 960 mV (selector `0xae` read back, from the boot value `0x9e`);
- the GPU PLL locked at 1008 MHz (`0xf9105300`), with all 10 read-backs passing.

CORE6 then reached status 8 after 3,151 µs, and its MISR showed the device
accept (`0x100`). The two 32-bit identity loads returned `0x00240038` and
`0x006800b7`, BVNC `36.56.104.183`. The run ended `COMPLETE GPU vendor-state`
and the ROM answered. This is the first GPU core register access on the board.
It also shows that CORE6 does power with the vendor state; the earlier
failures to reach status 8 came from the missing vendor state, not from the PPU.
Which of the three differences is necessary is not yet separated.

**Next steps (2026-10-25 17:10, before the usage-window reset):**
1. CPU steps 2–5 (`cpu-top-opp-20261025/steps2-5-set`) passed independent
   review as safe to run. First fix `cpu-top-opp.md:250-253,260` (a stale rule
   that a CPU-L change ends the lifetime). Add the prediction that a ROM return
   with CPU-L above 494 MHz is untested, and remove power promptly after the
   top-point step. Then run the set on port 3-3 after a power removal.
2. Port 3-1 needs a power removal (the GPU firmware-start image left the GPU
   running). Then run the DRAM training captures on both ports
   (`work/dram-fast-boot-1025`) and GPU firmware stop (a), once that is frozen
   and reviewed.
3. In flight:
   - the MBUS 600 MHz fix and peripheral clocks (`work/clock-audit-1025`);
   - the performance-mode toggle (`work/perf-modes-1025`);
   - the FEL-returning microSD variant;
   - GPU firmware stop and the first command.

**The GPU firmware starts** (set `board-bundles-20261025-20`,
`board-session-20261025-16/port-3-1`, record `s16-gfs-run`;
`work/gpu-fw-start-1025` merged). After cold DDR and readiness, the start
image ran on the vendor-default power path:
- the secure configuration;
- the firmware clock at 1008 MHz;
- the `RGX_CR_TIMER`-based timing record;
- 64-bit registers accessed as two 32-bit halves;
- no `SOFT_RESET2`.

It ended `COMPLETE GPU firmware start: started word 1`, and the ROM answered.
The image leaves the GPU running, so port 3-1 needs a power removal before
its next run. Nothing from the firmware binary was used, only the open DDK
host sources (`docs/designs/gpu-firmware-start.md`).

**MBUS restore qualified on port 3-1** (set `board-bundles-20261025-mbus-qual-2`,
`board-session-20261025-23/port-3-1`, branch `work/clock-audit-1025` at
`4950b025`). After the cold DDR start, a FEL-written pattern (2 × 4 MiB) was
laid down. The first kernel check passed with no `MBUS` line, since MBUS was
still at boot0's 600 MHz. The second, after a ROM return, logged
`MBUS 0x84000000 0x81000000 0`, so it restored 600 MHz from the SRAM window,
and its kernel check passed. The pattern then read back unchanged. Port 3-3 then passed the same steps (`board-session-20261025-24/port-3-3`):
the second kernel check logged `MBUS 0x84000000 0x81000000 0`, and the
pattern verified. The full clock dump (`clock-audit-b`) matched all 49
predicted words, including speed bin `0x496` (code 4); the decode is in
`decoded-clocks.json`. The merge waits on row and index conflicts that are
being resolved on the branch.

**GPU firmware lifecycle passed on port 3-1** (set `board-bundles-20261025-23`,
`board-session-20261025-20/port-3-1`, branch `work/gpu-fw-stop-1025`).
After cold DDR and readiness, one image ran:
- firmware start: started word 1 after 665 µs;
- the compatibility check;
- the health check, executed in 1.9 ms (executed count 0 to 1; the open
  firmware writes no return word, so completion is judged by the count, as
  upstream does);
- forced idle and power-off, both acknowledged;
- the 14-step stop, then OFF and provider park.

The image returned to a responsive ROM with the GPU off, so a firmware run
no longer needs a power removal. Set `board-bundles-20261025-25`
(`board-session-20261025-22/port-3-1`) then ran the whole lifecycle twice in
one lifetime with no power removal between, and both runs completed: the
firmware restarts cleanly. `work/gpu-fw-stop-1025` is merged; its
bundle row is now 25, while frozen sets 21 and 23 call it 20.

**GPU eight-job session passed on port 3-1** (set `board-bundles-20261025-30`,
`board-session-20261025-29/port-3-1`, record `s29-gjs-run`). One firmware
session ran eight 32-lane jobs on one context, timed from the kick to the
timeline word on the 24 MHz counter:
- job 1 took 113 µs, including the MMU cache flush;
- jobs 2 to 5, 7 and 8 took 80 to 83 µs each;
- job 6, the first after the command ring wrapped, took 87 µs.

All 32 output words matched, with no firmware events or resets. Cleanup and
the stop were acknowledged, and the GPU powered off. Preparation took 1.45 ms
and the eight jobs 2.39 ms in total.

**CPU step 6 passed on port 3-3** (`board-session-cpu-top-opp-10/port-3-3`,
set `step6-set-5`). In one lifetime, with an operator present:
- bundle 25 raised CPU-L to 1794 MHz at 1000 mV and the DSU to 1274 MHz, and
  set DCDC3 to 1050 mV with CPU-B kept at 1014 MHz;
- bundle 26 read the boot words after the ROM return;
- the workload relocked CPU-B to 2002 MHz, CPU-L and the DSU, and ran all
  eight cores: 32 rounds each, 32 ticks on CPU0 and 33 SGIs on each
  secondary. It powered the secondaries off, lowered CPU-B to 1014 MHz at
  EL3, and returned with DRAM restored in 7 s;
- bundle 27 read the three boot words and both rails afterwards, at 37 °C at
  most.

The background progress counts (about 2.66 million per A55 core and 3.27
million per A76 core) were far above the predicted 300,000 and 260,000
because the prediction scaled runs of the older native image `762237cd…`,
whose core-ID read was a supervisor call. Against the same image `6fd7af86…`
at the boot rates (`board-session-20261025-8`, 692,000 and 1,590,000), the
counts rose 3.85 and 2.06 times for clock ratios of 3.63 and 1.97
(`docs/designs/cpu-top-opp.md`, "Step 6 passed on port 3-3"). An earlier attempt
(`board-session-cpu-top-opp-8`) stopped before launch because its binding
compared the wrong hash kind; set 5 fixed it.

**Board access is limited until further notice (2026-10-25).** The user cannot
power-cycle either board. Each board holds one long-lived lifetime in
`board-session-20261025-36/port-3-<n>`: a standard cold DDR start (`s7-ddr`,
5,088 ms on 3-3 and 5,102 ms on 3-1, 12,288 MiB) and two read-only four-P-state
DRAM captures (`lc-capture-1`, `lc-capture-2`). Until the user returns, only
returning images that continue these lifetimes may run. Runs that need a
fresh lifetime, an operator or a power removal wait. Those include further
DRAM capture lifetimes, the microSD 1.8 V retry, SD sets and CPU step 7.

**DRAM capture lifetimes on record** (v5 set): each board has two four-P-state,
two P-state-0-only and one initialization-only lifetime. Initialization
without training took 36 ms on port 3-3 (`board-session-20261025-34`),
against about 4.4 s to train P-state 0 and 5.1 s to train all four.

**microSD set 28 on port 3-3.**
- **Lifetime A passed** (`board-session-20261025-30`): high speed at 50 MHz, 4-bit, 3.3 V. All 256 reads completed and the card clock stopped on exit. Throughput was 1.4 to 1.7 MB/s, limited by per-request polling, and DMA is designed in `docs/designs/sd-dma.md` on `work/sd-returning-1025`.
- **Lifetime B stopped** (`board-session-20261025-32`) at the CMD11 voltage switch, before any read, with `SD_REASON=3`, platform stage 13, and the card clock stopped by the module-clock fallback. Its diagnosis is pending.

**Initialization-only DRAM capture, port 3-1** (`board-session-20261025-33`):
boot0 initialized without training (result 7, 12,288 MiB, `dram_tpr13`
0x10061, simple test FAIL, as predicted), and both SRAM captures completed
five stages.

**P-state-0-only DRAM capture, port 3-1** (`board-session-20261025-31`):
training took 4,451 ms, 12,288 MiB, and both captures passed.

**First GPU compute job passed on port 3-1** (set `board-bundles-20261025-29`,
`board-session-20261025-28/port-3-1`, record `s27-gfj-run`). After the cold DDR
start and provider readiness, one power-on did all of the following:
- firmware start in 0.7 ms and the health check in 1.9 ms;
- MMUCACHE, then one 32-lane compute job, with the timeline completed and all
  32 output words equal to the formula;
- no firmware events and no recovery records;
- context cleanup, then the 14-step stop acknowledged in 23 ms, OFF and provider
  park.

The first attempt (set 27) faulted. The job's setup program stored each 64-bit
pointer constant high word first, so the shader's first load went to address
`0x80` and the firmware reset the context. A read-only FEL capture of the
firmware memory identified the fault. Set 29 stores the constants low word first. A failed job now still
ends with the GPU off: an unacknowledged stop runs upstream's hard reset.
`work/gpu-first-job-1025` is merged; the job is bundle row 28.

**P-state-0-only DRAM capture, port 3-3** (`board-session-20261025-25/port-3-3`,
set `dram-training-capture-v5-20261025`, build `ddr-pstate0`): boot0 trained
P-state 0 alone at 2040 MHz (result 7) in 4,441 ms, against about 5,090 ms
for all four P-states, and reported 12,288 MiB, `dram_tpr13` 0x10061 and
`dram_tpr10` 0x925f0000. Both read-only captures completed all six stages.
This is the first reference lifetime of the restore design; its analysis is
on `work/dram-fast-boot-1025`.

**CPU step 6 stopped before the workload on port 3-3**
(`board-session-cpu-top-opp-7/port-3-3`, set `step6-set-2`). Bundle 25 set
CPU-B to 2002 MHz at 1050 mV, CPU-L to 1794 MHz at 1000 mV and the DSU to
1274 MHz, with every temperature below 37 °C. After the ROM return, bundle 26
read both rails held. It also read CPU-L and the DSU back at their 494 MHz
boot words, but the CPU-B PLL still at 2002 MHz (`0xf8804d00`), so it
refused as designed. **The FEL ROM return does not reset the CPU-B PLL.**
Any image that lowers DCDC3 after a top-point bundle in the same lifetime
must first reset or check the CPU-B PLL. The fix and the refreeze are on
`work/cpu-top-opp-step6-1025`. Step 6 still needs an operator at the board.

**Four-P-state DRAM capture, port 3-1** (`board-session-20261025-26/port-3-1`,
v5 set): DDR passed with result 7 for all four P-states, 12,288 MiB and
5,093 ms of training, and both captures passed. This is port 3-1's first
capture lifetime.

**DRAM training capture, port 3-3** (`board-session-20261025-21/port-3-3`,
set `dram-training-capture-v3-20261025`): cold DDR (12,288 MiB), then two
read-only captures, each with all six stages complete. This is the first
lifetime of the capture plan.

**CPU top operating points, steps 2–5 passed on port 3-3** (set
`cpu-top-opp-20261025/steps2-5-set-r2`, `board-session-20261025-19/port-3-3`).
In one lifetime, with CPU0 running and every other core off:
- DCDC3 went to 1050 mV and back to 800 mV, with CPU-B held at 1014 MHz;
- CPU-L reached 1196 MHz and the DSU 780 MHz at 800 mV;
- CPU-B reached 2002 MHz (PLL read back) at 1050 mV, with the DSU at
  910 MHz, then returned to 1014 MHz and 800 mV;
- CPU-L reached 1794 MHz at 1000 mV and the DSU 1274 MHz, left there
  until the power removal that followed.

Every clock word read back as predicted, every ROM return worked,
including returns with CPU-L above 494 MHz, and the hottest sensor read
42 °C. The ROM resets CPU-L and the DSU to 494 MHz on each return, as it
does MBUS, so each image must set its own clocks. It does not reset CPU-B:
the first step 6 run (`board-session-cpu-top-opp-7/port-3-3`) read CPU-B
still at 2002 MHz after bundle 25's return, and the boot ROM's clock setup
writes only the CPU-L and DSU PLLs. Every image that raises CPU-B lowers it
to 1014 MHz before a successful return, the step 6 workload included; a
failed return can leave it raised and ends the lifetime with a power
removal (`docs/designs/cpu-top-opp.md`). A76 execution at 2002 MHz (steps 6
and 7, B1) is still to do. The merge into `bringup` waits on integration
fixes; it is now merged (`work/merge-cpu-top-opp-1025`).

**CPU top operating points, step 1 passed on port 3-3** (set
`cpu-top-opp-20261025/step1-set`, `board-session-20261025-17/port-3-3`,
branch `work/cpu-top-opp-1025`). The kernel check passed. With cores 6 and 7
off, CPU-B went from 1014 MHz to 1404 MHz (PLL `0xf8803600`) and back to
1014 MHz (`0xf8802700`) at 800 mV, with no PMIC write. The clock read-back
words matched, and every sensor read about 39 °C before and after. The
vendor top points for bin 4 are CPU-L 1794 MHz at 1000 mV, CPU-B 2002 MHz
at 1050 mV and DSU 1274 MHz (`docs/designs/cpu-top-opp.md`); steps 2 to 6
climb there.

**Clock audit** (`work/clock-audit-1025`, `docs/clock-audit.md`): MBUS reads
600 MHz after the cold DDR start but 400 MHz after every ROM return, and no
SunstoneOS code writes it. Every image after the first in a lifetime has
therefore run the memory bus at two thirds of boot0's rate, which confounds
the ShuffleNetV2 852 MHz timing. G2D (100 against 300 MHz), the video engine
and JPEG (unread against 624 MHz) and SDIO (25 against up to 208 MHz) are
below the vendor rates. The board's single LPDDR5 chip has no part number in
the schematic, and no vendor DRAM set faster than 2040 MHz exists for its
strap.

**GPU firmware-start step 1 passed on port 3-1** (set `board-bundles-20261025-18`,
`board-session-20261025-14/port-3-1`, branch `work/gpu-fw-start-1025`). The
probe-read on the vendor-default path made all 15 accesses, after CORE6 had
reached status 8 (10,341 µs). The identity was 36.56.104.183 both before and
after the vendor secure configuration, and no SError was pending; the image
then restored, went OFF and parked. The first board reads:
- `drm_ctrl` `0x01880014` = 1 at entry;
- `irq_ctrl` `0x01880010` = 0;
- SMC0 `0x0a0000b0` = 0. Writing 2 (the vendor value) reads back 0, so the
  bit self-clears or the write is ignored. It is named `SMC_DRM_GPU_HW_RST`.

Step 2, the firmware start, is next.

**microSD lifetime A, first SD board run** (set `board-bundles-20261025-14`,
`board-session-20261025-14/port-3-3`). DDR, cache-off, survey, signing and
binding passed. The first launch was refused before launch, because the
runbook's `sd()` gave no `--artifact` and the checker requires
`prepared.json`, `image.elf` and `launch.bin`. The rerun (`sd-uhs-a-hs-2`)
launched, but the host reported `FEL request failed` and then timed out after
180 s with the result unobserved. No new USB device enumerated. The image's
recovery mode is a manual USB reconnect, which on these boards is a power
removal. Under investigation; port 3-3 needs a power removal.

**ShuffleNetV2 on the NPU, timed on port 3-3** (set `board-bundles-20261025-16`,
`board-session-20261025-12/port-3-3`, branch `work/npu-full-model-1025`).
Both starts passed: all four outputs matched, and all three state reads
matched their predictions.
- Inference, doorbell to IRQ: 3.69 ms at 492 MHz and 2.91 ms at 852 MHz, a
  ratio of 0.79 against about 0.72 predicted. The NPU is 99% active, but its
  active cycles rise 1.37× at the higher clock, so about 1.85 ms per inference
  is clock-independent, probably memory traffic.
- Before the trigger: 13–14 µs. IRQ to recorded completion: about 17 µs.
- Each job spends about 56 ms between publication and start, in per-job
  hashing and cache cleaning of the fixed model buffers. That is the largest
  cost by far, and moving it to session open is the next change.
- Both boards are NPU speed bin vf0400 (port 3-3 `0x496`, port 3-1 `0x495`,
  from `bb9-read`). The vendor table has no 1008 MHz row for that bin, and the
  vendor driver runs it at 852 MHz with DCDC2 at 800 mV, so 852 MHz is the
  vendor's top operating point on these boards. `work/npu-full-model-1025` is
  merged. `work/npu-job-overhead-1025` makes 852 MHz the default, moves the
  fixed-buffer hashing to open and close, and simplifies the register path.
  **Qualified and merged** (set `board-bundles-20261025-19`,
  `board-session-20261025-15/port-3-3`). All four ShuffleNetV2 outputs
  matched, and all three state reads matched their predictions.
  - Publication to start: 231–239 µs, down from about 56 ms.
  - Before the trigger: 4.9–5.3 µs, down from 13–14 µs.
  - IRQ to recorded completion: 10.1–12.4 µs, down from about 17 µs.
  - Inference: unchanged, 3.69 ms at 492 MHz and 2.91 ms at 852 MHz.
  - Startup: 4.0 ms.
  A job at the 852 MHz default now costs about 3.3 ms end to end, down from
  about 59 ms.

**NPU interrupt completion and job timing, qualified on port 3-3**
(set `board-bundles-20261025-13`, `board-session-20261025-9/port-3-3`;
`work/npu-followups-1025` merged). Both starts passed, with all four
outputs matching and all three state reads matching their predictions.
- Startup: 4,024 µs at 492 MHz and 4,025 µs at 852 MHz.
- Job start to IRQ97: 22.7 to 23.9 µs at both rates. IRQ to recorded
  completion: about 16 µs.
- NPU cycle counters: active cycles 2,015 to 2,151 at 492 MHz and 3,409 to
  3,731 at 852 MHz, which is about 4.3 µs of activity at both rates. About 90%
  of each job's cycles are idle.
So this test job is too small for the clock to matter: its latency is fixed
overhead (command fetch, DRAM traffic, interrupt delivery). The 852 MHz
comparison needs a full model, such as the ShuffleNetV2 slot.

**Vendor EL3 security setup in every image, qualified on port 3-3**
(set `board-bundles-20261025-10`, session `board-session-20261025-8/port-3-3`).
`work/gpu-vendor-default-1025` is merged: every EL3 entry makes BL31's
cold-boot security writes 9 to 17 (`include/a733_el3_security.h`). The
vendor GPU state (PLL at 1008 MHz, DCDC4 at the OPP voltage, the vendor ON and
OFF order) is the default GPU path. After a cold DDR start:
- the kernel check passed its 100 rounds;
- the read-back matched every prediction: CCU and PRCM security switches 7,
  S_TZMA `0x07003000`/`+4`/`+8` all 0, SPC `0x020540e0` `0xfffffbff`;
- on the native image refrozen with these writes (`6fd7af86`,
  `refreeze-el3-20261025/bundles-22501205`), the pre-scheduler, first task,
  task switch and three deadline-workload runs all returned with DRAM
  restored, and all seven 91-word captures equal the clean capture.
GPU OFF from ON then ran once on port 3-1 (`board-session-20261025-7/port-3-1`,
record `s11-gpo-run`). Cold DDR and readiness passed. The power-only image
completed cycle 1 ON, with PLL_GPU0 locked at 1008 MHz, then reported a
resource-release failure (`GPU resources result=3 error=7 stage=8`). Cycle 2
did not re-enable the PLL, and its ON poll failed. The image returned to the
ROM with status 1, and the claim was closed as failed. Decoding is in progress;
the next GPU run needs a power removal.
The decode showed that OFF had completed. Cycle 2 failed because TOP5 OFF
closes the GPU master gates. The second attempt (set
`board-bundles-20261025-12`, `board-session-20261025-9/port-3-1`, record
`s12-gpo-run`) logged them. Cycle 1 ON and OFF passed again. After TOP5 OFF,
AHB `0x5c0` bit 7 and MBUS `0x5e0` bit 16 read clear (`0x110103ff` to
`0x1101037f`, `0xf1055803` to `0xf1045803`). Cycle 2 then relocked PLL_GPU0,
but TOP5 ON timed out with both gates closed (`error=7 step=48`), and the
image returned to the ROM with status 1. What the vendor stack does to power
TOP5 on again after an OFF is under investigation.
The decode was corrected: TOP5 ON had never been requested. After a TOP5 OFF,
the owner's exact read-back of the delay row failed, because `+0xc10` reads
`0x00ff0008`; the vendor writes that row once and never reads it back. With
the read-back limited to the delay bits (`work/gpu-second-on-1025`), the third
attempt (set `board-bundles-20261025-15`, `board-session-20261025-11/port-3-1`,
record `s13-gpo-run`) got further:
- cycle 2 powered TOP5 up from OFF with both gates closed;
- TOP5 ON reopened both gates (AHB `0x110103ff`, MBUS `0xf1055803`);
- the run then failed with `GPU provider request retained`, and the image
  returned to the ROM. The decode found one remaining cause: the supply
  consumer kept its DCDC4 sample in a provider request slot, while the vendor
  reads the voltage once and keeps nothing open. The fix ends the sample after
  the last OFF (`a733_gpu_platform_supply_end`).
- **The fourth attempt passed** (set `board-bundles-20261025-17`,
  `board-session-20261025-12/port-3-1`). Readiness passed. The power-only
  image completed two full ON/OFF cycles (`COMPLETE GPU power only: CORE
  status 8, OFF and provider parked`). The identity image then read
  `36.56.104.183` in both of its cycles (four reads) and parked the provider.
  Every step returned to a responsive ROM. GPU power-on, power-off and
  re-power-on now work in the vendor order. The four fixes on
  `work/gpu-second-on-1025`:
  - no master-gate requirement before TOP5 ON;
  - the delay-row read-back limited to the delay bits;
  - the supply sample released after OFF;
  - TOP5 ON reopening the gates by itself.

  The branch is merged into `bringup`. `work/sd-uhs-1025` (read-only UHS-I
SDR104 with 1.8 V PF-bank signalling) is merged after two review rounds; its
board run is `docs/board-runbooks/sd-uhs.md`.

**Reference backsolves (2026-10-25).** At the user's request, CPU, GPU and
NPU bring-up are being derived from the vendor sources and blobs before any
further board run of each. Board runs test only predictions from those diffs.
- GPU (`work/backsolve-gpu-1025`, `docs/reference-backsolve-gpu.md`): the
  kernel GPU driver powers CORE6 itself and never waits for its status. Three
  differences remain: the GPU PLL at 1008 MHz (ours: PERI0 400 MHz, PLL off),
  DCDC4 at 960 mV (ours: 800 mV), and BL31's cold-boot secure setup (SPC ports
  non-secure, CCU security switch `0x02003f00` = 7), which SunstoneOS never
  performs. The next image applies all three in the vendor order, confined to
  one variant.
- NPU (`work/backsolve-npu-1025`, `docs/reference-backsolve-npu.md`): the
  boot ROM routes shared SRAM away from the NPU (SYSCTRL `0x03000008` bit 1)
  and U-Boot routes it back; FEL-loaded images never do. This best explains
  the wrong NPU outputs of the earlier two-job run.
  The read-only state read (set `board-bundles-20261025-6`, bundle 9, 33
  words, no NPU-window access) ran on both ports on 2026-10-25 (`bb9-read`
  in `board-session-20261025-4/port-3-1` and `board-session-20261025-3/port-3-3`).
  All 21 predicted words matched on both boards, including SYSCTRL
  `0x03000008` = 2: bit 1 set, so the shared SRAM is routed to the CPU.
  **The NPU computes correctly with the SRAM routed to it** (set
  `board-bundles-20261025-7`, branch `work/npu-sram-route-1025` at
  `43bbb1e7`, record `nr-npu` in `board-session-20261025-3/port-3-3`). The
  image clears SYSCTRL `0x03000008` bit 1 while the NPU resets are held, as
  U-Boot does at board init, and sets it again at EL3 before the ROM return.
  Both jobs matched the reference: the first output's SHA-256 and every byte
  of the second output. The run ended with status 0, a normal ROM return and
  verified DRAM restoration. The `9e4adabf…` run without the routing step had
  produced wrong, input-independent outputs, so the routing bit is the cause.
  Before the run: IRQ97 captured, DCDC2 enabled at 800 mV (`nr-dcdc2`), fresh
  cache-off and survey. PLL_NPU, domain 4, DCDC2 and the SRAM stay powered.
  **The production NPU startup routes the SRAM** (`work/npu-production-1025`,
  merged 2026-10-25 after two review rounds; [design](designs/npu-sram-route.md)). Every image that links
  the startup now powers domain 4 before any NPU clock, routes the SRAM while
  the NPU resets are held, refuses NPU register access outside READY and
  routes the SRAM back at local stop; the finite image also sets the bit at
  EL3 before its ROM return. The register pass adopts a PLL_NPU that an
  earlier start in the same power lifetime left locked, so a second start
  needs no power removal. Modelled boot to READY is 4.6 ms (was 12.0 ms):
  the owner verifies DCDC2 at its first step without the 5,160 µs settle, the
  bootstrap waits on power-service notifications and ends the 2 ms vendor
  settle exactly, and PLL_NPU starts at once when domain 4 is already ON. An
  852 MHz profile at the unchanged 800 mV is admitted only for speed bins
  whose column lists 800 mV, with the vendor's rate-change order
  ([design](designs/npu-852mhz-plan.md)). The board qualification is the
  [three-start runbook](board-runbooks/npu-starts.md): one cold port 3-3
  lifetime, starts at 492 MHz cold, 492 MHz adopted and 852 MHz by rate
  change, two jobs each, all six outputs predicted to match, startup times
  of about 4.6, 4.4 and 4.6 ms. **Board result** (set `board-bundles-20261025-8`,
  `board-session-20261025-7/port-3-3`): all three starts passed, and all six
  outputs matched the reference. The four state reads matched every predicted
  word, including PLL_NPU at 852 MHz after start 3. Measured startup times
  were 4,024 µs (492 MHz cold), 4,023 µs (492 MHz, PLL adopted) and 4,023 µs
  (852 MHz). No job time was measured: the image never recorded completion,
  because `npu_convolution.c:346` matches the finished command by pointer
  while the controller passes its own copy (`a733_npu_controller.c:162,231`).
  The 75,005 and 77,045 µs values are the publication times since the NPU
  driver started, not durations. The gap between the two publications,
  2,038–2,043 µs, bounds job 1 plus the 1 ms completion polls, the output
  copy and compare, and the submission of job 2 (`timing-2.json` beside each
  record).
- CPU (`work/backsolve-cpu-1025`, `docs/reference-backsolve-cpu.md`): the
  leading divergences are checks that the reference never performs, such as the
  GICR_CTLR status compare (now masked on `bringup`), GICD_CTLR.RWP treated
  as a failure, and exact compares of GICR_PWRR, WAKER and ISPENDR0. Missing
  steps include clearing CNTHP_CTL_EL2, writing VPIDR/VMPIDR_EL2, a timer
  event stream and the GIC-600 erratum 2384374 workaround. The recommended run
  order starts with read-only captures, then port 3-1 rank 7 with the masked
  comparison.

**Multicore quality and performance (2026-10-25).** A design panel
(performance, quality and the CPU backsolve) chose two board-free tranches:
- `work/mc-correct-1025` covers the status-bit masks, the RWP waits, the
  missing EL2 and GIC steps, a single definition per constant, the patch-plan
  de-duplication and the magic collision.
- `work/mc-perf-1025` covers the lock-bookkeeping DSBs, `__getreent` without
  an SVC, lock-free time reads, interrupt-exit barriers, wake placement and
  missed-tick handling, each measured by instruction counts and the
  eight-core soak.

Fair spinlocks, cache-line relayout, `-march` and the IRQ FP save wait for a
supervised eight-core board session.

Merged on 2026-10-25:
- `work/mc-perf-a-1025`: `__getreent` from TPIDRRO_EL0 (96 to 7 instructions)
  and the lock-free counter read, with the measurement tools.
- `work/mc-correct-1025`: status-bit masks, bounded RWP waits, the EL2 and
  active-priority entry steps, single constants, the patch plan and the new
  resident magic. Board qualification: ten deadline-workload runs built from
  `52b408c5` passed in a fresh port 3-3 lifetime
  (`board-session-20261025-5/port-3-3`, `tw1`–`tw10`). All completed with
  DRAM restored and no deadline return, and all eleven 91-word captures equal
  the clean capture. That run does not execute the new `continue.S` entry. It
  runs once the native image `762237cd` is refrozen; the first-task profile on
  the refrozen image then qualifies it. The rank 7 and rank 8 bundles and the
  `cpu-next-20261024` and CPU0 bundles need the refreeze listed in
  `docs/designs/multicore-correctness-1025.md`.
- `work/refreeze-1025`: the native image refrozen from d502f8ea as
  `a8228eaa…`, with twelve returning bundles rebuilt
  (`~/.cache/sunstoneos/evidence/refreeze-20261025/bundles-6cd9d008/`).
  Qualified in a fresh port 3-3 lifetime (`board-session-20261025-6/port-3-3`):
  pre-scheduler, first task, task switch and three deadline-workload runs all
  returned with controls and DRAM restored, and all seven 91-word captures
  equal the clean capture. The new EL1 entry (VPIDR/VMPIDR_EL2, CNTHP_CTL_EL2,
  zeroed active priorities, nTWE/nTWI) and the RWP-waiting controller have
  now run on all eight cores.
- **Performance session on port 3-3** (`board-session-20261025-10/port-3-3`,
  sets `multicore-performance-session-c4eadf9f`, three control and three
  changed runs, alternating):
  - every run completed (32 rounds on every core, no failures), and all
    seven 91-word captures equal the clean capture;
  - A55/A76 counter skew was 3 to 8 ticks;
  - the late-tick change behaved as predicted (2 IRQs for 5 kernel ticks).

  The pass criterion failed: the changed image was lower on every core in
  only 6, 5 and 6 of 8 cores per pair. The median change per core was −7%,
  −12%, −9%, −17%, −44%, −28%, −61% and +2%, with single runs ranging from
  −71% to +361%. The 64-pair figure is dominated by lock contention under
  eight cores, not by barrier cost. The four changes stay off `bringup` until
  an uncontended per-core measurement exists.
- **Contention-free performance session on port 3-3**
  (`board-session-20261025-13/port-3-3`, sets
  `multicore-performance-session-v2`): one core at a time timed 32 samples of
  64 uncontended lock pairs, three control and three changed runs.
  - The changed image was lower in every pairing on all eight cores, and every
    confidence interval was within 5%.
  - Saving: 164 counts per pair on the A55 cores (control about 57,200 per 64
    pairs, changed about 46,500) and 163 on the A76 cores (about 64,000 and
    53,600). That is about 7.4 counts per removed `DSB SY`.
  - All runs completed, and all seven captures equal the clean capture.

  `work/mc-perf-1025` (lock-bookkeeping DSBs, IRQ-exit barriers, same-type wake
  placement, capped late-tick catch-up) is merged.

**Board runs of 2026-10-24 night** (set `board-bundles-20261024-8`, sessions
`~/.cache/sunstoneos/evidence/board-session-20261024-8/port-<PORT>`). The user
reported both boards reconnected after power removal. The two claims held from
the earlier GPU non-returns were closed as failed and both queues resumed.
Cold DDR passed on both boards (12,288 MiB). The set's survey step refused
before device access: its frozen build named the survey runner from before
`575565f8`. `~/.cache/sunstoneos/bin/survey-retention.py` uses the matching
build in `bootstrap-retention-20261024/survey-build` (same binary), and the
surveys then passed. The set's runbooks still call the old survey and need
regenerating.
- **Port 3-1, GPU provider readiness (`3adfa4ad`): passed.** Supplies
  verified, provider parked, no GPU power request, warm return.
- **Port 3-1, GPU power only (`81d4a728`): failed before any CORE6 write.**
  TOP5 came on. Step 8, the CORE6 admission step, read policy 8, status 0 and
  PWCR 0x101 and refused that state (`phase=5 error=5 step=8 last=0x101`), so
  the image reported `FAIL GPU ON poll`. That state is the PPU's power-up state:
  an ON transition started at reset and waits on the GPU's Q-Channel handshake,
  which a clock-gated GPU in reset cannot accept. The vendor sequence writes
  PWCR 0 and then policy 8 unconditionally. TOP5 was left on, because a warm CPU
  reset does not reset the power controller. The monitor returned status 1 to
  the ROM about 2 s after launch. The host runner then held ownership until the
  925 s queue timeout. The claim is held and the lifetime has ended. A corrected
  driver and runner are under review on `work/gpu-core6-1024`, and the next
  port 3-1 run needs another power removal.
- **Port 3-1, second lifetime** (session
  `board-session-20261024-9/port-3-1`, after the user's power removal). Cold DDR
  and GPU provider readiness both passed. The corrected power-only image
  (`4ade8fdf`, set `board-bundles-20261024-9`) admitted CORE6's power-up state
  but still failed:
  - after the PWCR 0 write, status was still 0 after 51 µs;
  - after the policy 8 write, status was still 0 after 51 µs;
  - the final status poll timed out (`phase=5 error=6 step=27`);
  - MISR read 0 before any write and after reset release;
  - IDR0 read `0x10130101`.

  CORE6 therefore needs a prerequisite outside the PCK and the GPU clock bits.
  The fixed runner exited as soon as the board returned. The claim is held,
  the lifetime has ended, and analysis continues on `work/gpu-core6-b-1024`.
- **Port 3-3, base kernel check (`88bce618`): passed** all 100 rounds.
- **Port 3-3, bundle 6 (`2291f78e`): passed.**
  - **Step bound:** median 1,243 permille against the 1,268 bound; the first
    step took 807 µs against an 840 µs limit.
  - **XADDR write:** read back unchanged.
  - **New windows (24 MHz counter ticks, 41.7 ns each):**
    - 256 plain XADDR writes, with one DSB per 32: 1,233 ticks, about 200 ns
      per write.
    - 256 whole provider polls with the flag clear: 7,652 ticks, about
      1.25 µs per poll.
    - Service-loop work between polls: 1,687 ticks per window.
  - **Domains:** 1, 4 and 5 are all policy 8 and status 8.
  - The port 3-3 lifetime continues; its last warm return is `s7-bb6-run`.
- **Port 3-3, clock words (`board-bundles-20261024-1/clock-words`): passed.**
  All six words read, none skipped. Every word, including PLL_CPU_L
  `0xf8802600` and the CPU-L mux `0x03010000`, equals the 2026-10-23 reading of
  the same board. CPU-L therefore ran at the same 494 MHz that the bundle 6
  bound assumes.

- **Port 3-3, set `board-bundles-20261024-10`:** two more bundle 6 runs in
  the same lifetime, both passed with warm returns, and both carrying a window
  that times each completing poll. Snapshots and `TWI_WRITE` lines match
  between the runs.
  - **DSB SY pairs (`s10-bb6-dsb-run`):** median 796.4 µs (1,265 permille);
    first step 823.6 µs against an 838.8 µs limit.
  - **Plain S_TWI0 barrier placement (`s10-bb6-light-run`):** median 794.6 µs
    (1,262 permille); first step 819.3 µs. This qualified the placement for
    snapshots, and it merged at `7f74583a`. It saves 7.4 µs inside the polls
    of each step but only 1.9 µs of median, because each phase costs whole
    microseconds.
  - **The window's own cost:** about 12 µs per step, which leaves 3 to 6
    permille of margin. Compile it out of the default bundle 6 image before the
    next timing run.
  - **Still to do:** rerun rank 9 on the merged adapter. 110% still needs the
    S_TWI0 packet engine, plus a cut to the roughly 38 µs of step work outside
    the polls.

- **Port 3-3, set `board-bundles-20261024-12`:** bundle 6 from the merged
  tree (`22f8dada`), with plain S_TWI0 reads and the completing-poll window
  compiled out (now the default; `BB6_COMPLETE_WINDOW=1` builds it). It passed
  with a warm return (`s12-bb6-run`): median 1,224 permille against the 1,268
  bound; first step 808.0 µs against an 846.4 µs limit. Snapshots and
  `TWI_WRITE` match the earlier runs.
  - **Rank 9 is not yet rerun on the merged adapter.** The native USB image
    links the central `monitor.c` object built without the counter option, and
    no board run has executed that object since `rank-9-usb-v4`. The chosen
    route is to link the board-qualified base monitor object (the
    counter-option build) into the native USB image.

- **Port 3-3, set `board-bundles-20261024-13`:** rank 9 (native USB) on the
  merged adapter. The image links the board-qualified base monitor object
  (`3eaf49b9`) in place of the central one (`probe.bin` `d474710c`). It passed
  with complete OUT/IN observation, quiescence and a responsive ROM return
  (`s13-r9-usb`). This qualifies the plain S_TWI0 placement in the native USB
  image. The port 3-3 lifetime now holds six warm returns.

- **Port 3-3, set `board-bundles-20261025-1`** (same lifetime): all steps
  passed.
  - **Bundle 14 (`bb14-read`):** 15 FEL words, none skipped. SID bin word
    `0x0300624c` = `0x496`; CPU-B PLL `0xf8802700`, CPU-B mux `0x03000000`;
    CPU-L PLL `0xf8802600` as before; DSU PLL `0xf8802600`, mux `0x03010105`.
  - **Bundle 15 (`u1-bb15-run`):** DCDC3 and DCDC5 both enabled at selector
    `0x9e`, 800 mV, read through the qualified provider with no write.
  - **Rank 7 on port 3-3, the first secondary power-up of a lifetime:**
    - The 91-word state before the run matched every clean value.
    - The CPU0-only preparation returned (`u1-r7p-prepare`), and all 91 words
      were unchanged after it.
    - The all-eight-core pre-scheduler then returned (`u1-r7s-prepare`):
      completed mask `0xfe`, restored controls, secondaries OFF, DRAM
      restored.
    - The capture after it equals the capture before it in all 91 words
      (`u1-r7-state-comparison.json`). So the first secondary power-up
      leaves no residue in the captured power, CPU configuration and GIC
      words.
    - The coordinator accepted the rewritten port 3-3 admission conditions,
      with this lifetime's earlier cached DRAM images named as a confound.
    - The hold on ranks 7 and 8 is lifted. The first-task, task-switch and
      workload profiles for port 3-3 come next.

- **Port 3-3, set `board-bundles-20261025-2`** (same lifetime, records
  `u2-*`): ranks 7 and 8 in order, each launch followed by the 91-word
  capture.
  - **First task (`u2-ft-prepare`) and task switch (`u2-ts-prepare`):** both
    returned on all eight cores. The capture after each equals the clean
    capture in all 91 words; no secondary-power word changed.
  - **Thermal reading (`u2-th-run`):** returned; the hottest sensor read
    42.3 °C, below the 90 °C admission limit for the workload. The capture
    after it is unchanged.
  - **Full returning workload (`u2-tw-prepare`): did not return.** This is
    the first board execution of the workload on port 3-3 (32
    floating-point rounds per core, tick and SGI handlers, background
    tasks). The same overlay and native image returned on port 3-1 on
    2026-10-24 (`processor-completion-20261024/port-3-1/task-workload`);
    only the contract and request differ. Two returning bisection profiles
    return CPU0 to the ROM with a record at the startup deadline plus 0.5 s
    or at any earlier CPU0 stop ([design](designs/workload-bisect.md),
    [runbook](board-runbooks/rank-8-workload-bisect.md)); their return path
    has not run on a board. The launcher started at
    00:47:55 on 2026-10-25, and no new FEL instance appeared in the
    following 6.7 hours; the USB device kept its pre-launch number. The
    journal records `launch_attempted` with no transport failure. Nothing
    after the launch was read, so the stopping point is unknown.
  - The claim (`ac52d411…`) was held until the reconnect of 2026-10-25 and
    then closed as failed. The timer step and rank 8 did not run.
    B1 and the CPU clock and voltage runs stay blocked behind this result.
- **Port 3-3, 2026-10-25 lifetime** (session
  `board-session-20261025-1/port-3-3`, USB power only, bundles from
  `workload-bisect-20261025`): the workload with a CPU0 return deadline
  (`all-core-task-workload-deadline-v1`, same native image `762237cd…`).
  - Cold DDR, then the 91-word capture, equal to the clean one.
  - Runs 1 to 4 (`tw-deadline-prepare`, `tw2`–`tw4`) completed normally, with
    DRAM restored. Each capture after them equals the clean one.
  - **Run 5 (`tw5-prepare/runtime`) failed and returned through the
    deadline unit,** about 12 s after launch. This is the first board
    return of that path. Reason 3, stage 1: a failed survey at EL3 before
    the workload proper. `elr_el3` was `0x41022f10`, `elr_el1` `0x41023328`
    and `x30` `0x4102175c`. Every core reported power `0x108` with no
    pending, active or enabled SGI/PPI. The secondaries were not stopped and
    DRAM was not restored. The record, `report.bin` and
    `owned-writable-after.bin` are saved.
  - So the failure is intermittent within one lifetime (one in five) and
    occurs during startup, not in the floating-point rounds. Decoding is in
    progress on `work/workload-bisect-1025`.
  - The claim `5a49569e…` was closed as failed after the user's reconnect.
  - Run 5 decoded as CPU0's local restore comparing GICR_CTLR while RWP
    (bit 3) was still set. `work/workload-bisect-1025` (`1c885abf`) adds a
    bounded RWP wait on CPU0 and on every secondary, as Linux does.
- **Port 3-3, second 2026-10-25 lifetime** (session
  `board-session-20261025-2/port-3-3`, bundles `workload-bisect-20261025/rwp-fix`):
  after cold DDR and a clean 91-word capture, the RWP-corrected deadline
  profile failed on its first run (`tw1-prepare`) with the same record
  class: reason 3, stage 1, a deadline return about 5 s after launch. So the
  RWP wait does not remove the failure; decoding is in progress. The host
  runner again stayed alive after saving the record and was stopped. The
  claim `e86e3137…` was closed as failed after the user's reconnect.
  - The decode showed RWP read clear in the wait and set again at the
    comparison, with no CPU0 write in between; why is unexplained by the
    GIC specification. Linux and TF-A never compare GICR_CTLR. Head
    `26b497a2` masks the volatile status bits 31 (UWP) and 3 (RWP) in every
    GICR_CTLR comparison and keeps the waits. `fb6f5a69` also exits the host
    runner after a quarantined deadline return.
- **Port 3-3, third 2026-10-25 lifetime** (session
  `board-session-20261025-3/port-3-3`, bundles `workload-bisect-20261025/rwp3`):
  the status-masked deadline profile ran ten times in one lifetime
  (`tw1-prepare` to `tw10-prepare`). All ten completed, with controls and
  DRAM restored and no deadline return. Each of the eleven 91-word captures
  equals the clean capture. This qualifies the full returning eight-core
  workload with the status-masked check on port 3-3. The frozen rank 7 and
  rank 8 bundles still carry the unmasked comparison and need a refreeze
  from the branch.

The mimus branch `port/sunstoneos` (`9c57286b7`) was pushed to
`github.com/pedapudi/mimus` on 2026-10-24 with the user's approval; mimus
`main` is unchanged.

The user approved physical CPU clock and voltage changes on 2026-10-24: the A76
operating-point runs A1, A2 and A3 of `docs/designs/cpu-a76-dvfs.md` and the
DCDC3 selector write, each still gated on its own prerequisites.
On 2026-10-25 the user also approved GPU clock and voltage changes, such as
the vendor operating point of 1008 MHz from the GPU PLL at 960 mV. Each needs
its own design, review and approval record naming the rail and values.
On 2026-10-25 the user also approved NPU clock changes (852 MHz first, at the
current 800 mV) and switching microSD signalling to 1.8 V for SDR104, the
standard UHS-I voltage switch. The user's approval covers NPU voltage as well
as NPU clock.

**Performance policy (user, 2026-10-25).** Every block defaults to the highest
operating point the vendor uses for the chip's speed bin, with the vendor's
voltage: CPU clusters, DSU, GPU, NPU, DRAM, buses and storage. Lower operating
points are optional modes, for example for power saving or thermal limits,
and never the default or a cap.
Approval record for the GPU vendor-state probe (set `board-bundles-20261025-5`,
bundle 19, 2026-10-25): AXP8191 DCDC4 selector `0x15` = `0x2e` (960 mV) from
the 800 mV power-on value, then GPU PLL word `0xe9105300` (1008 MHz). This is
the first board use of the PMIC selector-write path; only the snapshot path has
run before. The coordinator accepted it after an independent review found the
write bounded to DCDC4 with that code and an exact read-back.

## Review of the paused tranche (2026-10-24 evening)

A four-area review (CPU, NPU, GPU and power, integration) of `f03060ee..b4d0eadb`
found no blocking defect. Its fixes merged at `019b26f6` after a three-lens
review and a selected integration gate (`gate-review-fixes-1024`). They
change behaviour that the paused GPU and NPU work must account for when it
resumes:

- Resident images no longer accept staged workload 2 (`OS_STAGED_WORKLOAD`
  in `apps/os/os.h`).
- NPU SUBMIT fills the output buffer with `0xc35a3ca5`, and the host reports
  which bytes kept it, so a job that writes nothing is detected.
- `apps/gpu-power-id` has a power-only variant (`build.py --power-only`) that
  never reads the GPU register window; run it before any identity probe.
- The GPU CORE status poll spins up to 50 µs before returning AGAIN; the
  monitor publishes an overridden status in `replaced_status`; a CPU
  qualification drained-barrier timeout is reason 9; an inherited CPU
  cluster spends its grant change at the first clock write.
- Configure refuses a `MIMUS_DIR` that is not the pinned `9c57286b7` or has
  changes; `tools/integration-gate.py` builds per checkout, locks, and
  refuses a dirty tree unless `--allow-dirty`.

The paused `gpu-provider-ready-1024` work may also edit
`drivers/gpu/a733_gpu_power.c` and this file; merge `bringup` into it first.

## Resume checkpoint

Main worktree: `/home/sunil/git/sunstoneos-worktrees/bringup`.
The last published revision is `fa9bb2fc`. Local commit `12613f42` integrates
reviewed live GPU/NPU shared-owner checks; it is not pushed yet. Its four
sanitizer cases passed, covering overlapping admission, each domain's timeout
and global PMIC transport retention. No production defect was found. Reuse
`accelerator-shared-owner-20261024/snapshot/manifest.json`; do not rerun it for
this pause or documentation commit. The associated power-domain documentation
is saved with this handoff. Check-index aggregation remains pending.

**Returning GPU image (merged 2026-10-24):** the finite GPU image that runs
one job and returns builds from current objects; the shared monitor and
`platform/a733/platform.c` are configure inputs. The image is `9005499a…`.
- **Return quiet check:** it requires TOP5 and CORE6 policy and status 0, at EL1
  and again at EL3.
- **Write-back span:** a read-only ELF marker holds the span
  `0x48000000..0x48088000`. A malformed marker is refused. An image without the
  marker is accepted with the historical `0x86000` span only if it lacks the
  typed allocator.
- **Clean deadline:** it is checked once per 4 KiB page, so the normal case
  issues 312 ISBs rather than about 19,600.
- **Checks:** all 29 return cases, the 9 marker cases and the linked lifecycle
  cases pass.
- **Evidence:** `gpu-full-integration-20261024/merged/`, with closure snapshot
  `snapshot-2`.

The image is not physically eligible. Running it needs a single-core admission
record granting exclusive CORE6 and TOP5 control, confirmed supply, clock and
isolation, and a reviewed return and recovery plan. Every GPU single-core
profile, including the default one, lists `platform/a733/platform.c` as a
configure input, so editing it re-runs configuration and changes the build
identity.

**NPU profile of the resident OS image (merged 2026-10-24):** an optional,
feature-gated supervised profile runs one NPU application client. It reserves
`0x47400000..0x47c00000` and links only the model the client loads.
- **Client privilege:** the bootstrap and client entries run at EL1t only
  when `os_npu_start` creates them; unregistered tasks are refused.
- **Report:** read-only USB vendor operation 6 returns a fixed 104-byte report,
  decoded by `apps/os/npu-report.py`. It distinguishes pending, matched,
  mismatch (the `0xc35a3ca5` fill shows bytes the job never wrote), refused
  (thermal stop) and retained. A retained report stays in RAM, because USB0
  sends nothing after a board failure.
- **Faults and defaults:** critical faults retain every NPU resource. A
  manifest without an `npu` field means OFF.
- **Budget:** CODE use is 995,209 bytes, with 1,100,912 free; static state is
  140,329 bytes.
- **Default images:** the default `sup` and `res` `.text` is identical to
  builds without the profile.
- **Checks:** all 32 `checks/os-npu.py` cases pass. Evidence:
  `os-npu-20261024/check-8/`.

- **Launcher:** for an image whose link check records the NPU profile,
  `tools/check-a733-os-supervised.py` requests operation 6 once after a
  completed capture with at least 100 ms of the window left, and records the
  decoded report and its classification. This is host-checked only
  (`checks/os-image-launch.py`).

Open items:
- The `os-supervised-software` evidence must be regenerated before the next
  OS launch: the launcher's hash and its source set, which now includes
  `apps/os/npu-report.py`, changed.
- `npu_admission` in `apps/os/prepare.py` is untested against real NPU entry
  records.
- No NPU job of this image has run on a board.

The latest reviewer finding is a concrete privilege requirement:
`a733_counter_clock_now` masks DAIF, so the registered NPU application client
must execute at EL1. Verify/finalize the narrow
`os_npu_client_task(NULL)` privilege-hook correction and test the actual task
frame SPSR. Unregistered tasks must remain refused. The older NPU board image
had the same problem; its `apps/npu-board/main.c` bootstrap and client now
start at EL1t only while `board_application_start` creates them, which
`checks/npu-board-providers.py` checks against the linked first-frame SPSR.
That image remains unqualified.
A proposed read-only vendor opcode 6 exposes a separate fixed 104-byte NPU
report; its implementation/review is not complete. Verify actual framed USB
length, immutable completion publication and preservation of mimus protocol
semantics. Numerical mismatch must be reported separately from transport or
hardware failure. Critical faults retain resources; no full OFF/restart is claimed.
NPU-only behavior is feature-gated, and old manifests without NPU metadata mean
OFF. Reconcile only genuinely affected GPU-profile code after integration.
Untracked NPU sources and `checks/os-npu.py` are part of this unfinished work;
do not remove them.

**CPU audit:** the shutdown/publication review found no justified correction.
Existing checks cover all-core capture/drain before cache cleaning and retained
fatal exits. No tests were repeated. Idle/wake and induced-fault recovery still
need physical qualification.

**Review follow-ups, not yet fixed:** a review of `f03060ee..b4d0eadb`
found these in addition to the corrections already committed:

- `apps/npu-finite/thermal.c` and `apps/npu-board/client.c` each implement
  the NPU's thermal admission and stop gate (claim, stop, stopping); one
  shared gate should serve both.
- The NPU convolution and finite units select profiles with many `#ifdef`
  blocks; per-profile units would replace them.
- `irq_quiet()` in `apps/gpu-finite/finite.c` repeats four mapping checks on
  every tick it polls; one check at admission is enough.
- `apps/cpu-runtime-qualification/terminal.S` runs its cache-cleaning pass
  on all eight cores; whether each core needs it, and its cost, are
  unreviewed.
- Reopening the GPU mimus session (`platform/a733/gpu_mimus.c`) rebuilds its
  immutable programs instead of reusing them.
- The OS GPU service reopens the GPU inside Begin after a thermal stop, and
  `apps/os/gpu.c` polls CLOSE once per tick; both need an event-driven form.
- `healthy()` in `platform/a733/gpu_backend.c` calls the external live check
  on every call; its cost on the job path is unmeasured.
- The GPU return check must also require CORE6 OFF. The paused
  `gpu-provider-ready-1024` work already adds it.
- The CPU runtime qualification reads the EL3 alias before it validates the
  descriptors; the read should follow validation.

On resume, finish and review the two isolated implementations, integrate their
commits, regenerate the check index once, update canonical status/issues and
push after the normal scan. Preserve all existing freezes and evidence. Both
board sessions below still require the previously requested physical recovery;
board-free work does not authorize probing them.


**Engine revision:** `bringup` now builds against mimus `9c57286b7`, the
exact-sum hook and the AArch64 epilogue default, on the local branch
`port/sunstoneos` in `~/git/mimus-worktrees/sunstoneos`. That revision is
not on the mimus remote yet; publishing it needs the user's approval.

**Port `3-3` needs power removal when available:** disconnect and reconnect
its USB cable, which is its only power source. The corrected finite GPU image
launched but did not return to ROM within 90 seconds. Do not probe or retry it.
The earlier runner and wrapper PIDs `3892073` and `3892068` have ended.
The queue still retains claim
`f5e77284cf63a0b25af8681a8798031c6a976a84166256a8eaa31d162a7c771c`.
Evidence is under `processor-completion-20261024/port-3-3/gpu-corrected-guard/`.
Source diagnosis continues without board access; the failed execution boundary
is unknown. The leading, unproven hypothesis is that the image predates
CORE6 power control (`9bd65b98`) and its first GPU register read stalled the
CPU with the core domain unpowered
([gpu-power.md](gpu-power.md#an-unpowered-gpu-core-is-the-leading-hypothesis-for-both-failed-gpu-runs)).

**Port `3-1` also needs USB power removal:** the GPU power/identity probe
launched from `/dev/bus/usb/003/047`, identity `[7, 1642, 1097006, 0, 0]`,
but did not return within 30 seconds. Do not probe or retry it. The earlier
runner and wrapper PIDs `3974830` and `3974825` have ended. The queue retains claim
`a7c35b3b0e5c0bce0d06cfb9db6039b8830898ec4115509636115de998db0cc8`.
Evidence: `processor-completion-20261024/port-3-1/gpu-power-id-corrected/`.
All six FEL transactions succeeded, including launch; no native report locates
the failure. This image also predates CORE6 power control, and the same
unproven hypothesis applies. After recovery, the
[power-only GPU diagnostic](../apps/gpu-power-id/README.md#the-power-only-variant-runs-before-any-identity-probe)
runs before any identity probe; it is built and host-checked, and no board bundle is frozen. A preceding command-path error stopped before board access and
was closed as blocked; it did not require physical recovery. USB is the only
power source. A reconnect request is pending while software work continues.
Host metadata checked at 21:49 UTC still identifies both failed USB instances;
process exit does not establish physical recovery. Neither claim was released.
Evidence: `processor-completion-20261024/resume-host-state-214930.json`.

**Physical progress before that failure:** the current CPU workload passed on
all eight cores, followed by a separate revision that delivered one physical
timer interrupt to each secondary. Both restored controls, confirmed seven
CPUs OFF, returned to ROM and verified restoration of 18 MiB. GPU and NPU
entry checkpoints also returned normally before provider construction.
The DCDC2 and DCDC4 snapshots each reported an enabled rail programmed to
800 mV, without changing voltage.

The numerical NPU image then completed two copied/reaped jobs and local
CLOSE/stop, parked its provider, returned to ROM and restored 18 MiB.
Numerical comparison failed. The captured second output has 100 zero bytes
followed by 300 `ff` bytes, and its digest matches the first job's digest.
The numerical test is recorded as failed. Its verified retirement permitted
further unrelated work; PLL_NPU, domain 4, DCDC2 and NPU SRAM remained powered
and externally owned. No second NPU startup was authorized. The subsequent
GPU probe was admitted with those retained resources explicitly preserved.

**Current priority: CPU, GPU and NPU robustness**, including power and thermal
management. Complete startup, repeated operation, interrupt delivery, bounded
failure handling and safe shutdown before performance tuning. Model checks do
not establish physical operation.

**Board-free work at pause:** rebuild the finite GPU and OS GPU-linear profiles
with current dependency layouts; integrate the existing five-model NPU runtime
into an explicit supervised OS profile; and check simultaneous GPU/NPU use of
the shared power owners. The CPU shutdown/publication audit found no new defect
or uncovered software requirement, so its accepted checks were not repeated.
The NPU design reserves `0x47400000..0x47c00000` after the cold/stage-2 monitor,
using existing mappings and a session-bound ownership record. Implementation
and its focused evidence are still in progress.
The live shared-owner composition now passes four focused cases covering
simultaneous admissions, domain-local timeouts and global PMIC failure. It
preserves NPU ownership through local stop and GPU shutdown; no production
defect was found. Evidence: `accelerator-shared-owner-20261024/`.

**Open work:**

1. Qualify actual scheduler idle/wake behavior and complete CPU power/thermal
   integration. Image `f5ba965d…` passed 32 floating-point rounds per core,
   background scheduling, SGIs on all cores, 32 CPU0 timer ticks and one timer
   interrupt on each secondary. CPU0 remains the global FreeRTOS tick owner.
   Evidence: `processor-completion-20261024/port-3-1/cpu-local-timer/runtime/session.json`.
   The production thermal owner now keeps startup throttling until all five
   channels have valid readings and policy permits NORMAL; focused checks pass.
   The optional CPU clock path also rechecks thermal permission immediately
   before clock writes, closing a reproduced late-THROTTLE frequency increase.
   Fourteen focused cases pass; native code grows by 36 bytes with no new
   state or register access. Evidence: `cpu-late-thermal-20261024/`.
   Repeated cold startup, sustained operation, induced-fault recovery, actual
   idle wakeup, runtime core removal and thermal transitions remain open.
   The reviewed idle/wake image is ready at
   `cpu-idle-wake-20261024/snapshot/`; it has not run on hardware.
2. Locate the GPU failure before another power attempt. The entry-only image
   `8974f81a…` passed CPU startup, local memory preparation and normal return.
   The subsequent `fd3d5839…` power/identity/OFF probe did not return. Its exact
   uploads match, and the new constructors reach scheduler start in the
   instruction model; neither result establishes physical driver progress.
   The reviewed candidate at `gpu-provider-ready-20261024/` verifies actual
   PMIC/provider readiness and both boot supplies, then parks before any GPU
   power request. Software checks establish no GPU/PCK register access, token
   or pending request; they make no claim about inherited domain power state.
   The failed probe reuses an older monitor without the newer restoration
   readback checks; preserve that source distinction in diagnosis.
   Eight CPU startup/runtime objects match the passing DCDC4 image; the
   comparison found no additional justified CPU correction. Audit:
   `gpu-power-id-diagnosis-20261024/cpu-common-audit.json`. The uploaded zero
   result block was initialization, not a post-execution observation.
   A separate source-backed shutdown fix now initializes and verifies five
   PCK delay registers before the first OFF request after inherited ON.
   Repeated-transition and eighteen injected-failure cases pass under
   `gpu-first-off-delays-20261024/`; this does not locate the failed launch.
   No additional clock-update wait is justified by the effective callbacks.
   The official Zero 3W current-kernel recipe selects BXM and disables
   open-PVR. The pinned build/configuration chain is now documented in
   [GPU power](gpu-power.md); a particular installed image remains unverified.
   The native path now implements the selected reference's internal GPU core
   control under exclusive LOCAL ownership. Static admission precedes handshake
   control changes; CORE ON is checked after clock/reset progress, and CORE OFF
   before teardown. Uncertainty retains ownership. Focused controller, native
   access and shared-owner checks pass, including thirteen linked-client cases.
   The corrected power/identity image `20726f8b…` and draft proposal are under
   `gpu-core-power-20261024/`. Its source and artifact review passed, but fresh
   admission must explicitly cover CORE6; the earlier TOP5-only grant is insufficient.
   The unchanged provider-readiness image remains the first physical test.
   This source correction does not establish the previous failed instruction
   or qualify physical core-domain acknowledgement.
   The integrated GPU caller now performs its existing bounded CLOSE before
   removing the recognition hook after an invalid completed result. Only a
   matching STOPPED result permits CPU fallback; uncertainty remains fatal and
   retains ownership. Seven native caller cases and focused hook checks pass
   under `gpu-terminal-retire-20261024/`; normal jobs gain only a retired-state
   guard. Physical shutdown remains unqualified.
3. Diagnose the NPU numerical mismatch from exact runtime memory. Image
   `9e4adabf…` passed startup, two jobs, local retirement and ROM return, but
   all 400 output bytes differ from the reference. Frozen model, input and
   reference match the compiler export; no source-backed correction has been
   identified. The next useful observation is the stopped runtime allocation,
   command/descriptor/MMU data and metadata before host restoration, in a new
   admitted lifetime. The existing run already restored that memory.
   Evidence: `processor-completion-20261024/port-3-1/npu-numerical/runtime/session.json`
   and `npu-numerical-diagnosis-20261024/output-audit.json`.
   A reviewed derived continuity record preserves status 1 and the numerical
   failure while proving normal retirement; the original record is unchanged.
   Finite thermal supervision is now implemented and software-checked; its
   physical qualification remains open. The reviewed host at
   `npu-stopped-capture-20261024/snapshot/sources/` conditionally captures the
   audited numerical image after safe retirement and before RAM restoration.
   Four fixed DRAM reads preserve 1,085,880 bytes of metadata and runtime
   buffers, and a fifth reads the 101,376 bytes of NPU SRAM the model uses.
   Capture failures retain ownership; correct numerical execution remains open.
   The reviewed `apps/npu-finite/analyze-capture.py` compares that capture
   with the accepted commands, tensors and MMU mappings. Nineteen synthetic
   cases pass; no physical capture exists yet. It reports incomplete evidence
   separately and does not infer engine behavior from matching RAM bytes.
   No repeat startup is valid with the retained PLL.
   The [lifecycle audit](npu-startup.md) distinguishes observed local retirement
   from unqualified domain power-off. The scoped test already supplies shared-SRAM
   ownership; the remaining source gap concerns A733 low-power handshakes and
   ordering. Linux asynchronous runtime PM does not establish a synchronous order.
   A bounded search of A733 suspend, management-firmware and remote-processor
   sources found no additional OFF/drain contract. Its evidence is in
   `npu-low-power-source-20261024/source-search.json`; this is not proof that
   such a contract is unavailable elsewhere. The stopped-memory capture remains
   the next useful numerical observation.
4. Qualify power and thermal behavior after reliable numerical execution. The
   optional CPU-clock path now lowers and restores supported inherited rates
   under a bounded grant while preserving voltage. Missing authority prevents
   changes. GPU healthy CLOSE→OPEN and the actual OS thermal caller are
   implemented and checked; automatic idle parking and fault recovery remain
   open. Fixed operating targets are 400 MHz/800 mV for GPU and
   492 MHz/800 mV for NPU; this is not a performance qualification. NPU local CLOSE
   retains its domain, PLL and rail pin; power-off and in-place restart remain
   unresolved. Late shared-domain acknowledgements now retain ownership.
5. Keep the PMIC timing evidence. Its provisional snapshot bound is 1,268
   permille of measured wire time, with 40 µs extra for the first step. Exact
   comparisons and focused checks pass; the 110% target remains open. Refreeze
   the revised trace and row text before the next physical timing measurement.
6. Physical CPU clock and DCDC3 voltage changes are approved within
   `docs/designs/cpu-a76-dvfs.md`; other PMIC rails are not. The mimus branch
   `port/sunstoneos` at `9c57286b7` is published.

Both finite accelerator host parsers now use their actual native monitor
identifiers, with producer-derived regression evidence. The earlier fixtures
had duplicated incorrect parser constants. That defect would reject a returned
result; it does not explain the GPU non-return. The base single-core monitor
also now reports cleanup failure when interrupt restoration cannot be verified;
its existing host retains ownership on that result. These new monitor bytes
still need physical qualification.

The finite GPU host permits 60 seconds of native execution plus 30 seconds for
ROM discovery. Explicitly discardable, exclusively owned RAM avoids unnecessary
preservation of the 802 MiB reservation. Failure retains ownership; discarded
contents are never described as restored. The default restoration mode remains
available. Physical thermal transitions, repeated power transitions and
concurrent CPU/GPU/NPU operation remain completion requirements.

The integrated OS now supports explicit supervised workload 2: staged recorded
recognition after USB0 startup, without an external USB disk. Workloads 0 and 4
retain their storage-overlap, attachment and acknowledgement requirements.
The recorded-only native build and focused runtime, admission and linked-entry
checks pass. Evidence: `os-staged-recorded-20261024/freeze-final/manifest.json`.
Its frozen image `ea6eec1c…` derives from `f0d99be9`; CPU clock control is unlinked
and the older shared power-transition code is unused in this mode. The image
still requires physical entry review and one bounded capture, followed by USB
power removal. It has not run on hardware. See the [launch procedure](os-image.md#first-bounded-launch-with-recorded-audio).

The completed integration also includes bounded USB storage stall recovery,
radio clock admission, the camera frame-enable timing correction and optional
punctuation handling in paced recognition. Their component evidence remains
valid; do not repeat broad suites because they were merged. Defer new peripheral
breadth while the processor and accelerator robustness work progresses.

Continue in `/home/sunil/git/sunstoneos-worktrees/bringup` toward the
[accepted goal](goal.md). Read `AGENTS.md` and the relevant device skill before
new work. The [hardware matrix](hardware.md) remains the complete support
inventory; component documents hold APIs, source provenance and detailed evidence.

`bringup` contains the [implementation audit](audit/README.md), nine rounds of
fixes, features and consolidation based on it, WPA3-Personal host support, and
the first packages of a performance-gated completion tranche for the CPU, GPU,
NPU and DDR. New work follows the
[performance architecture](performance-architecture.md). The audit record
lists each package, the findings it addressed and the findings that remain
deferred with their reasons. Every change was checked with host builds,
emulators and host models. All 67 central build targets and nine integrated
OS image variants build, and the PWM and crypto build options configure. The
QEMU kernel check, the eight-core emulator soak, the mimus parity check, the
shared-helper check and the dashboard tests pass. No board test ran during
the audit or any round, so nothing they added is physically tested or
qualified. That audit evidence applies to its recorded revisions; later physical results
are listed below. Check Git before publishing subsequent integrations.

## Sequence toward the complete OS

1. **Establish reliable processor operation.** Run the prepared all-core
   idle/wake check after board recovery. For GPU, first qualify provider and
   supply readiness before any GPU register access, then resolve power/reset,
   identity, firmware startup and one checked numerical workload in order.
   For NPU, inspect stopped command, mapping and tensor memory to locate the
   observed numerical mismatch. Source-backed corrections precede retries.
2. **Finish processor lifecycle integration.** Qualify repeated operation,
   shared power ownership, thermal refusal and shutdown, supported power
   transitions, and representative concurrent CPU/GPU/NPU work. Safe local
   task completion alone does not establish device power-off or DMA drain.
3. **Run the integrated OS and mimus.** Preserve the existing model and
   transcript parity. Combine recorded-input recognition, storage, services
   and management in one image, then qualify persistent boot and backup-first
   updates. Existing standalone transcription is partial completion evidence.
4. **Complete the hardware inventory.** Qualify the implemented native paths
   for storage, USB, radios, buses, audio, display, cameras, codecs and other
   listed hardware. Independent work can proceed alongside processor diagnosis.
   Missing cameras, a DSI panel, microphones and other required accessories
   remain physical qualification dependencies. Unpopulated eMMC and UFS media
   are excluded from the current boards' applicable functions.
5. **Optimize the completed system.** Measure end-to-end mimus recognition
   against the 0.05-times-audio target with the required model and formatting.
   Investigate 0.01 times only as the bounded stretch target. Defer replacing
   opaque dependencies until hardware functionality and performance tuning.

The dependency graph and hardware inventory define the full scope; this order
prioritizes shared blockers without requiring every device task to wait for the
processors. Reuse accepted evidence and run checks only for changed behavior or
an unresolved requirement.

## Board state and recovery

The current identities, recovery need and retained processes are listed at the
top of this document. Both boards now use USB as their only power source.
A lost connection remains unavailable until confirmed power removal and
reconnection. A cached FEL descriptor alone does not establish recovery.
Continue with the other board and offline development while recovery is pending.

Session access for both ports lasts until the next host reboot. After a reboot,
the user runs `sudo python3 tools/usb-access.py enable-session --port <port>
--user sunil` for each port; `python3 tools/usb-access.py check-session --port
<port>` confirms access without communicating with the board.

### Earlier board evidence

On 2026-10-23 both boards passed the first five ranks of the board test plan,
after the host reboot had reset both: the boot ROM answered as SoC `0x1903`,
the returning SRAM survey restored SRAM byte for byte, boot0 trained 12,288 MiB
of LPDDR5 at 2,040, 1,165, 816 and 408 MHz, the single-core FreeRTOS kernel
check built from the current tree passed 100 rounds, and the forced-stall case
returned to the ROM through the monitor's secure timer. That kernel run is the
first board execution of the boot table flush in `platform/a733/platform.c`.
After a warm ROM return the instruction cache is on, so every native launch is
preceded by the returning cache-off helper and a fresh survey on the current
USB instance. Evidence and the coordinator's queue scripts are in
`~/.cache/sunstoneos/evidence/board-session-20261023/`.

Rank 6, the returning thermal application, then passed on both boards: two
complete sets from all five sensors read 40.7–42.2 °C on port `3-1` and
42.5–44.0 °C on port `3-3`, below the 90 °C throttle trip. Rank 7, the
accepted all-core first-task diagnostic, launched on port `3-1` and did not
return to the ROM, although its overlay and payload are byte-identical to the
build that passed on that board on 2026-10-18; the board needed a power
removal, after which a cold DDR start passed again. The two lifetimes differ
at least in the MBUS clock: a read-only capture today shows `+0x588` =
`0x84000000`, decoded as 400 MHz, where the 2026-10-18 lifetime ran it at
600 MHz. The same capture stopped rank 9 on port `3-3` before any launch,
because its input requires 600 MHz. Rank 7 is not repeated on either board
until an offline diagnosis names a concrete correction or a narrower
diagnostic. The rank 6 to 9 bundles and runbooks are in
`~/.cache/sunstoneos/evidence/board-bundles-20261023/`.

A read-only capture of the CPU power, CPU configuration and GIC-600 words on
port `3-3` returned the expected CPU power words. Its GIC-600 words read as
zero only because that capture used `sunxi-fel read`, which has the ROM copy
memory with an access width no source establishes; GICv3 returns unknown data
for unsupported access sizes. Read with 32-bit loads (`sunxi-fel readl`), the
GIC is clocked (`GIC_CLK_REG` `0x84000003`), out of reset (`CPU_SYS_RST_CTRL`
`0x7f`) and in its 2026-10-18 state (`GICD_CTLR` `0x37`, `GICD_TYPER`
`0x007b0408`), also in a clean lifetime. Read GIC registers only with
`readl`. On 2026-10-24 the same capture on port `3-1`, in its clean
lifetime, matched port `3-3` word for word. The corrected rank 7 diagnostic,
which checks the startup contract, the GIC and the secondary power words
before any secondary starts and returns a refusal code instead of parking,
then launched on port `3-1` in that clean lifetime and again did not return:
no new FEL instance appeared within 300 s. The board had run no FreeRTOS
image since its cold DDR start, so state left by earlier images does not
explain the non-return, and no admission check refused. CPU0 stopped either
in a parking path before or after the admission (listed in
`docs/board-runbooks/rank-7-diagnosis.md`) or after release. Rank 7 and rank 8
are not repeated on either board until an offline diagnosis names a narrower
diagnostic. Rank 9, the native USB channel,
then ran on port `3-3` with a builder that accepts MBUS at 400 MHz: the image
returned to the ROM with a controlled refusal at USB takeover: its 32-bit
store to `0x04100040` (observed `0x00029940`) did not clear bit 6, which is
the PCTL SoftConn bit (the D+ pull-up), not the device-session bit. Every
pinned source writes PCTL with an 8-bit store. On 2026-10-24 the corrected
bundle `rank-9-usb-v3/`, which clears SoftConn with one 8-bit store and
checks it clear 150 ms later, passed rank 9 on port `3-3`: the image took
over USB0 from the ROM, enumerated, received the 4-byte OUT request and sent
the 40-byte IN response (`native_usb.functional_success` true), quiesced and
returned to a responsive ROM (`board-session-20261023/port-3-3/r9-v3-usb/`).
This is the first board run of the native USB device channel. The image
used the platform revision of the passing kernel check. In the same
lifetime, `rank-9-usb-v4/`, rebuilt on the current platform with the PCTL
byte store only in the shared native USB adapter, passed the same exchange
and returned (`r9-v4-usb/`). The current platform's 12 GiB EL1 map code was
present but installs no high map, because the DRAM decode check still
returns 0.

In the same lifetime on 2026-10-24, port `3-3` passed the first three
single-core bundles of `docs/designs/board-bundles-next.md`
(`board-bundles-20261024/`):

- **Fault capture:** one load from `0x4_0000_0000` took a level-1
  translation fault before any bus access. The platform printed one `FAIL
  exception` line (`ESR` `0x96000005`, `FAR` `0x400000000`, `ELR`
  `0x4100360c`, core 0, task `fault`), the monitor returned status 1 and the
  ROM answered again (`bb1-run/`). This is the first board run of the fault
  path, and the runner accepted it as a controlled outcome.
- **Kernel check with the monitor counter option:** the monitor's
  `MDCR_EL3`, `HSTR_EL2` and `MDCR_EL2` writes ran, all 100 kernel rounds
  passed and the ROM answered again (`bb2-run/`).
- **Clock words:** six words read with no gate closed, and the 128 FEL
  scratch bytes restored (`bb3-read/`): CCU `+0x580` `0xc3000000`, `+0x588`
  `0x84000000` (MBUS at 400 MHz, `MBUS_DFS_EN` clear), R_CCU `0x20c`
  `0x00010001`, oscillator word `0x983fb0f7`, CPU-L PLL `0xf8802600` and
  CPU-L mux `0x03010000`.

Later in the same lifetime, port `3-3` passed the second set of single-core
bundles (`board-bundles-20261024-1/` and `-3/`):

- **Event counters, walk A/B and WFI (bundle 4, `bb4-run/`):**
  - **Counters:** an empty counter region retires exactly 20 instructions and
    a fixed 2,000-instruction region exactly 2,000.
  - **Walk A/B:** in the core-0 walk comparison, an iteration with uncached
    table walks takes 693 cycles and one with cached walks 436 (median over
    6 blocks each, spread below 0.2%); uncached walks make 7 bus accesses
    per iteration against 4. Each block counted exactly 1,024 data and 1,024
    instruction walks. The bundle's per-block check marked every block
    refused because it expected the calibration region's one instruction
    walk to recur per block; the raw counts are valid. The table-walk
    design's revert rule keeps cached walks for this loop; its core-0
    workload runs are still required. The corrected check
    (`apps/board-bundles-cpu/walk.h`) requires 1,024 of each. The next
    bundle 4 set must carry it and must be prepared from the bundles' source
    revision `74d6aeab` with the correction, or on a new base image: at the
    current revision `platform/a733/platform.c` no longer reproduces bundle
    2's object.
  - **WFI:** over 1,000 tick wakes in 1.0 s the cycle counter advanced
    841,417 cycles, so the A55's cycle counter stops in WFI.
  - **CPU-L clock:** the core ran at 494 MHz.
- **Power-domain words (bundle 5, `bb5-read/`):** domains 1, 4 and 5 read
  policy and status `0x8` (ON) after boot, before any SunstoneOS request.
- **PMIC service and power-domain owner, read-only (bundle 6, `bb6-run/`):**
  - **Supply:** S_TWI0 runs at 400 kHz, and supply verification found DCDC2
    and DCDC4 at 800 mV, unchanged.
  - **Mapping:** the domain 1 controller is mapped outer-shareable, as the
    adapter requires.
  - **Owner budget missed:** all 34 recorded service steps exceeded the
    owner's bound of 110% of wire time: 146% at the median, 149% at most
    (the first step). Tick interrupts add nothing measurable. The host model
    matches the board at about 1.3 µs for each of a snapshot's 211 register
    accesses. The owner's leaner reads in the same region take 0.63 to
    0.96 µs, so an estimated half of that is provider software per access
    (two deadline reads, a task check and, in this image, the bundle's trace
    hook); the next bundle 6 measures the split. The
    accesses add to the step because the service times each phase's spin
    from the end of its poll rather than from the phase start, a provider
    follow-up. The power-domain design sets 155% as a provisional bound for
    a 400 kHz snapshot step under this run's conditions.
- **Domain 1 ON (bundle 7, `bb7-run/`):** the owner found domain 1 already
  ON and adopted it without a transition, so no switch-on time was measured
  in this lifetime.
- **DRAM bus priority (bundle 8, `bb8-run/`):** the NSI block answers
  Non-secure EL1 reads. The table write on ports 2, 14 and 15 changed their
  fields from `0x40` to `0x68`, `0x7c` and `0x7c` and read back correctly,
  and the USB_PCIE port fields stayed unchanged. The written priorities
  persist until port `3-3` loses power.

Still in that lifetime, port `3-3` then passed steps 1 to 4 of the fifth
prepared set (`board-bundles-20261024-5/`, records `set5-*`):

- **New base (`set5-bb2-run/`):** the kernel check built from the current
  tree with the monitor counter option, including the interrupt mask that
  `platform_init` now restores after pool preparation, passed 100 rounds and
  returned. It is the equivalence base for later single-core images.
- **Bundle 4 with the corrected walk check (`set5-bb4-run/`):** all stages
  passed and no walk block was refused; WFI again counted about 845,000
  cycles over 1,000 wakes.
- **Bundle 6 on the changed PMIC provider (`set5-bb6-run/`):** the median
  service step fell from 146% to 123.6% of wire time, and the cold first
  step from 941 µs to 809 µs, inside the provisional 155% bound. The byte
  time measured nominal (22.5 µs) with no re-spins. The access-cost windows
  give, per access: a bare S_TWI0 read about 378 ns, the barrier about 41 ns
  more, the new adapter read path about 462 ns, a time read 144 ns, the
  task check 76 ns and a write about 358 ns. The 110% target remains open: the service step follows write cost,
  and writes without a trailing barrier were not measured. The proposed
  replacement for the provisional 155% bound remains under the review
  described at the top of this document.
- **Bundle 7 (`set5-bb7-run/`):** the owner again found domain 1 ON and
  adopted it.
- **Native USB on the changed provider (`set5-r9-v6-usb/`, from
  `board-bundles-20261024-6/`):** the OUT/IN exchange completed, the task
  quiesced and the ROM answered again, so rank 9 is requalified on the
  PMIC access-path change. The returning thermal image of rank 6 does not
  link the provider and needs no rerun.

On 2026-10-23, after the rank 9 refusal, both ports were power-cycled and
passed a fresh cold DDR start (`ddr-2/`). Port `3-3` has stayed in that
lifetime since: every 2026-10-24 run above, from `r9-v3-usb/` through
`set5-r9-v6-usb/`, is a warm return within it. Port `3-1` ran the corrected rank 7
diagnostic in its own `ddr-2` lifetime and waits for a power removal. Earlier identities and admission
records supply no current authority. The physical steps follow the
[ranked RAM-only board test plan](audit/board-test-plan.md);
[continuation](#continuation-and-evidence-reuse) gives the complete order.
Until then, continue the [offline work](#next-offline-work).

Neither board has populated eMMC or UFS. Do not probe or require qualification of
these absent devices. Preserve both microSD cards and their native support;
writes to a card follow [section 4 of the goal](goal.md), starting with a host
backup of that card.
No MIPI camera, display or microphone is attached. OV5640 is the first sensor
target; the DSI panel is undecided. Use recorded audio for mimus meanwhile.

| Board | Power and access facts | Continuation requirement |
| --- | --- | --- |
| Original, host port `3-1` | Corrected rank 7 did not return on 2026-10-24. USB-C is its only power source; unplugging the cable removes power. The 2026-10-18 all-core pass is historical evidence. | Remove every power source and reconnect in FEL. Then follow the cold DDR and narrower pre-scheduler plan at the top of this document; do not repeat the failed launch. |
| Second, host port `3-3` | Latest recorded run `set5-r9-v6-usb/` completed the native USB exchange, quiesced and returned to a responsive ROM on 2026-10-24. USB is its only power source. | Continue eligible independent device work with current admission and frozen inputs. Reuse the passing thermal, kernel and USB evidence while its relevant inputs match. |

Per-port permissions, locks, queues and failure records remain independent.
A read-only queue inspection on 2026-10-24 found both queues bound to their
ports. Port `3-1` retained one claimed entry, one ready entry and four blocked
entries; port `3-3` had one blocked entry and no claimed entry. These records
do not establish a live process or physical recovery. Preserve the failed
session's ownership until the coordinator resolves recovery; do not rebind or
release it merely to resume work. Follow the [queue procedure](board-test-queue.md)
and verify current permissions and ownership before a launch.
The separate ESP32 mimus keyboard is unrelated: do not use it as the Orange Pi
console or send desktop keyboard events.

The earlier USB guard refusal in
`usb-first-fault-20261018/physical-064/runtime/` is historical. The subsequent
byte-store correction and successful native USB exchanges are recorded above;
full USB modes and device-specific consumers still need their own qualification.

## Integrated OS image

The [integrated OS image](os-image.md), `sunstone-a733-os`, runs the mimus
board application together with a continuous thermal service. The board
application already combines the USB0 device service, the USB2 host with
keyboard and read-only storage, the Type-C and PMIC owners and EL0 recognition.
The image routes its interrupts through one fixed table and starts its
applications from one fixed list. It builds in two lifetimes:

- **Supervised.** The image accepts the bounded FEL contract, keeps WDT0
  disabled and ends with a terminal halt that needs manual power removal. A
  FEL launcher and session preparation exist. Preparation requires a reviewed
  entry record built from physical evidence, which does not exist yet.
- **Resident.** The [stage-2 loader](boot-slots.md) enters the image after a
  cold boot from microSD with WDT0 armed in its four-second mode. A supervising
  task renews the watchdog only while every listed service reports progress
  within its window, so a stalled service lets the watchdog reset the board.
  The loader admits the SD controller state that boot0 leaves, but its three
  entry prerequisites refuse: remote-writer exclusion, watchdog recovery after
  a cold boot and a comparison of boot0's entry state. This lifetime cannot
  run on a board until all three exist.

Optional builds add I2S microphone capture with the DMA0 interrupt in the
table, the board's Wi-Fi station with crypto engine entropy for WPA2, or
Bluetooth headset recognition. With both the radio and the headset, the
board's radio owner owns the AIC8800 module and loads its firmware once, and
the Bluetooth application attaches to that session as a second client with
its own UART1, H4/HCI and wake-pin leases. The `SUNSTONE_A733_OS_MSBC` option,
with `SUNSTONE_BT_SBC_DIR` naming the pinned libsbc sources, adds wideband
speech to any headset build.
[The recorded-audio launch](os-image.md#first-bounded-launch-with-recorded-audio)
states which variant admits board workload 2, staged recorded recognition
without a USB disk. The image builds in nine variants:

| Variant | Options added to `SUNSTONE_A733_OS=ON` | Needed before a board run |
| --- | --- | --- |
| Supervised | None | The reviewed entry record, and a passed USB0 channel test |
| Resident | `SUNSTONE_A733_OS_RESIDENT` | The stage-2 entry prerequisites, a qualified watchdog-expiry recovery and measured service windows |
| Resident with I2S audio | `SUNSTONE_A733_OS_RESIDENT`, `SUNSTONE_A733_OS_AUDIO` | As resident, plus a connected microphone and a reviewed audio entry record with the resident DMA facts |
| Resident with radio | `SUNSTONE_A733_OS_RESIDENT`, `SUNSTONE_A733_OS_RADIO` | As resident, plus the radio profile and firmware as a slot asset at `0x43000000` and a reachable access point; any radio failure, including an absent access point, resets the board |
| Resident with radio and audio | `SUNSTONE_A733_OS_RESIDENT`, `SUNSTONE_A733_OS_RADIO`, `SUNSTONE_A733_OS_AUDIO` | The requirements of resident with I2S audio and of resident with radio |
| Supervised headset | `SUNSTONE_A733_OS_HEADSET`, with `SUNSTONE_BT_SECURITY_DIR` naming the pinned Zephyr security sources | Preparation that stages the Bluetooth profile and firmware, a paired, reviewed headset, and a qualified faster SDIO mode for firmware loading ([remaining OS work](os-image.md#not-yet-integrated)) |
| Resident headset | `SUNSTONE_A733_OS_RESIDENT`, `SUNSTONE_A733_OS_HEADSET` | As resident, plus the Bluetooth profile and firmware as a slot asset and a paired, reviewed headset; any headset failure resets the board |
| Radio with headset | `SUNSTONE_A733_OS_RADIO`, `SUNSTONE_A733_OS_HEADSET` | Preparation that stages both profiles and the shared firmware in one span, a reachable access point, a paired headset, and a qualified faster SDIO mode for firmware loading ([remaining OS work](os-image.md#not-yet-integrated)) |
| Resident radio with headset | `SUNSTONE_A733_OS_RESIDENT`, `SUNSTONE_A733_OS_RADIO`, `SUNSTONE_A733_OS_HEADSET` | As resident with radio, plus the Bluetooth profile at `0x43008000` and a paired headset; a radio or headset failure resets the board |

A Bluetooth failure retains only the Bluetooth client's resources at the
module, but the image still ends the session on any headset failure, also
while the radio is up. The headset build excludes the microphone, because the
recognizer takes one capture source. A link check and host models of
interrupt delivery, supervision and admission pass for every combination of
lifetime and options. No board has run any of them. Display, camera and NPU remain separate
images. The optional resident GPU linear build links the GPU service and the
local mimus exact-sum hook; physical GPU recognition remains unqualified.

## CPU evidence and next physical decision

Multicore implementation and review may be delegated. The coordinator alone
runs board tests.
[Arm/Linux source audit](a733-multicore-reference-audit.md) and
[startup records](a733-smp-supervised.md) preserve the architectural decisions.

On 2026-10-18, all eight CPUs reached their initial FreeRTOS task context
and returned safely. Actual construction and restoration record each core's idle-task pointer,
stack, argument and exception state. The production barrier and 256 atomic
increments also pass. All writers finish before cleaning the 145-page writable
interval, or 9,280 cache lines per CPU. CPU0 restores controls; all seven
secondaries acknowledge shutdown and reach independently observed OFF. Saved
DRAM restoration and responsive ROM return pass. Reuse
`all-core-first-task-20261018/physical-010/runtime/session.json`.
That physical diagnostic excludes task bodies, timer interrupts and application
SGIs; it does not qualify sustained multicore scheduling. The corrected rank 7
launch failed to return on 2026-10-24, so the earlier pass supplies no current
launch authority. The recovery and narrower pre-scheduler plan above governs
the next physical attempt.

The following extensions pass software checks only:

| CPU workload | Accepted scope and required preservation | Evidence |
| --- | --- | --- |
| Context switching | Actual idle yield, context save/selection/restoration and pinned-worker entry on all eight cores; wrong arguments retain cores. USB observer is suppressed. Worker bodies and timer/SGI work remain excluded. | `all-core-task-switch-20261018/build-no-observer/`, `host/software.json`, `host-root-review.json` in that evidence directory |
| Full returning workload | Each core completes 32 floating-point rounds, background work and SGIs; CPU0 services the production timer. Final-writer stop, interrupt cleanup, Nonsecure memory publication and matched shutdown are reviewed. Timing/coherence remain modeled. | `all-core-task-workload-20261019/frozen/`, `host-frozen/`, `host-root-review.json` |
| [Eight-core emulator soak](smp.md#eight-emulated-cores-pass-a-scheduling-soak) | Ten QEMU trials pass a cross-core queue ring, shared-mutex exclusion, migration through all cores with FP context and preemption of a non-yielding task on every core. QEMU models identical Cortex-A55 cores and uses the QEMU platform, not the A733 cached platform. | `smp8-soak.json` |
| [Resident contract](a733-smp-cached.md#resident-contract) and [interrupt table](a733-smp-cached.md#a-fixed-interrupt-table-serves-images-with-several-device-owners) | Cold-boot contract with an armed four-second watchdog, no FEL return and no diagnostic deadline; EL3 route programming and per-core dispatch for listed shared peripheral interrupts. Host checks cover admission and every refusal. The integrated OS image uses both. | Not frozen; relevant checks: `checks/a733-resident-contract.c` and `checks/a733-irq-table.c` |

The full-workload damaged-round-count refusal reuses its saved completed
checkpoint. Host checks validate workload results, epochs, task contexts,
timer-priority restoration, saved RAM and retained transport failure. Do not
repeat the earlier unobserved full launch unchanged. After recovery, use the narrower
[pre-scheduler diagnostic](board-runbooks/rank-7-pre-scheduler.md) identified
at the top of this document. Ranks 7 and 8 remain blocked pending its result
and a concrete correction or justified diagnostic.

Preserve the accepted returning CPU diagnostic ELF
`762237cd0f023a2c6895167b347352bb383e1012b5c39d1f8f0139278a68dba3`.
Rebuilding requires new symbol and patch bindings; documentation changes do not
justify replacing the frozen artifact.

The [CPU clock and voltage driver](cpu-clock.md) and its thermal coupling
exist and pass host and instruction models. The driver moves CPU-L and CPU-B
among the pinned operating points, raises a cluster only at the thermal
policy's NORMAL level after a reading from that cluster's sensor, and lowers
raised clusters to 1014 MHz at THROTTLE. Every transition needs a session
grant record, and no image grants a transition: no image carries a grant or
calls the driver. The user reviewed the CPU clock plan and agreed with the
design on 2026-10-21. Any physical change still needs the user's approval for
each change. CPU-L therefore stays at
the 494 MHz that boot0 leaves.

## Integrated component status

All rows below describe implemented, software-checked paths. They do not grant
physical admission. Modeled hardware, scheduler, cache, transport and numerical
boundaries remain those of each referenced record. Evidence paths are relative
to `~/.cache/sunstoneos/evidence/`. The hardware matrix covers remaining onboard
functions and modes beyond these bounded applications.

| Component and canonical specification | Current scope and remaining limit | Accepted evidence |
| --- | --- | --- |
| [Integrated OS image](os-image.md), [stage-2 loader](boot-slots.md) | Mimus board application plus a continuous thermal service, one interrupt table and a fixed start list, in the nine variants [above](#integrated-os-image). Resident renewal requires progress from every listed service within 1 s windows (3 s for recognition). Link check and host models pass for every variant, including each service stalling or never reporting. The loader admits the SD controller state that boot0 leaves; its entry prerequisites refuse on a board. Service windows are unmeasured. | Not frozen; relevant checks: `checks/os-image-link.py`, `checks/os-image-model.c`, `checks/os-image-stage2.py`, `checks/os-image-audio.py`, `checks/os-image-radio.py`, `checks/os-image-headset.py`, `checks/os-image-identity.py` and `checks/os-image-launch.py` |
| [PCIe endpoint COPY](pcie-test-application.md) | One trusted direct `pci-epf-test` endpoint: selected BAR/iATU, 4,096-byte DMA COPY, one-shot MSI and immutable copied USB output. Hardware/scheduling are modeled. Requires endpoint-specific DMA-before-unmap and data-before-status/MSI authority; none exists yet. All resources remain retained; physical DMA/MSI, all-core startup and recovery remain unqualified. | `pcie-endpoint-20261019/root-integration.json`; `root/review.json` in that directory |
| [PCIe NVMe block read](pcie-nvme.md) | Supervised image `sunstone-a733-pcie-nvme`: one admitted 4096-byte block from a direct NVMe controller with polled completion, then controller shutdown and port stop with PERST held; copied USB0 result. Failures retain the port's leases and the queue and data pages. Linked-image and host cases pass against a modeled controller; no NVMe device has been attached. | Not frozen; relevant checks: `checks/pcie-nvme-fixture.py`, `checks/pcie-nvme-native.py` and `checks/pcie-nvme-host.py` |
| [GPU→HDMI copied USB observations](gpu-display.md#copied-usb-observations) | Two immutable 720p converted frames and ordered 0→1→0 replay; GPU CLOSE precedes HDMI start. Typed USB authority and full-session digest bind copied hashes/16 samples per frame, three presentations and healthy GPU/HDMI/DE/PMIC terminal state. Final `70041393…`: fifteen framing cases; earlier components/eight root cases reused via 102 unchanged objects, seven SRAM cases separately. Durable host capture uses 19 s/native 20 s. Effects remain modeled; no full-pixel, visible-output, physical or concurrent GPU/HDMI qualification. All resources retained. | `gpu-display-capture-20261020/root-integration.json`; host: `gpu-display-observation-host-20261020/`, [capture scope](gpu-display-preparation.md#bundle-and-evidence-limits) |
| [Finite HDMI PCM](hdmi-audio.md), [copied USB observation](hdmi-audio-preparation.md#optional-copied-usb-observation) | Stereo S16LE/48 kHz, 1–48,000 frames via DMA0→I2S3→HDMI. Original `432b7c10…` models finite playback. Observation `0c661c29…` adds identity-bound 512-byte USB replies, with actual TinyUSB/publisher cases and host refusal/durability/readiness checks; prior audio disposition seeded, no PCM/DMA replay. Four boot assertions passed before reporting-only failure; repaired source unexecuted. Resources retained; common health failure can prevent fault retrieval. No physical USB/sound, serializer drain or SMP qualification. | `hdmi-audio-usb-20261020/root-integration.json`; earlier finite scope: `hdmi-audio-nc-20261020/root-integration.json` |
| [DisplayPort implementation](displayport-native.md) | Four-lane 1.62 Gb/s, 720p60 RGB8/C-or-E profile. Final `91646247…` passes linked constructor/PD/controller/PHY/AUX/DE lifecycle and retained shutdown, twelve host cases and one linked HS USB packet-completion case with modeled effects. Nominal schedule defers ticks under the register mutex; no contention/SMP guarantee. AUX APB source/rate authority remains required. COMB0 excludes USB3/PCIe; resources retained. No trained physical link or visible-output qualification. | `displayport-native-20261020/root-integration.json`; earlier admission/boot/launcher checks retain their image scopes; [preparation limits](displayport-preparation.md) |
| [DSI immutable-frame replay](display-scanout.md#caller-uploaded-immutable-frame-replay) | Two authenticated XRGB8888 uploads selected 0→1→0 through the register command queue (RCQ) at modeled 720p/1080p. Fresh RCQ completion plus later DE frame-end bind copied results; full USB delivery precedes next request. Frames/command pages stay immutable: no retirement, recycling or arbitrary new-frame streaming. No attached panel, physical pixels, electrical qualification or shared startup execution. | `display-frame-present-20261019/root-integration.json`; `root/layout-review.json` in that directory |
| [SPI3 polling/deferred IRQ](spi-application.md#deferred-interrupt-software-evidence) | One authenticated FIFO-sized WRITE_READ on PE0–3; version 2 uses CPU0 INTID59, ISR mask/notify and owner-only completion. Final `149f7635…`: five linked native cases and separate host/root checks. Fourteen clock-parent cases remain separate. Frozen workload-2 image excludes the differing Type-C task. Register/device/scheduler effects modeled; resources retained, physical exchange/IRQ/timing/recovery unqualified. | `spi-irq-20261020/root-integration.json`, `root-frozen/manifest.json` |
| [UART2 FIFO/IRQ/DMA](uart-application.md#finite-dma-transfers) | Finite 1–1,024-byte DMA echo: both channels active before requests; success requires stopped DMA, requests off, true TEMT/RX-empty/idle, copied echo, REAP/CLOSE and UART release. Known-live faults cancel boundedly; uncertainty retains. DMA0 parent/allocation stay owned for board lifetime. Device/cache/RTOS effects are modeled; physical wiring, DMA visibility and inherited startup remain unqualified. | `uart-dma-20261019/root-integration.json`, `root/layout-review.json`; earlier FIFO/IRQ evidence remains separate |
| [I2S duplex/repeated service](i2s-application.md), [cyclic DMA](i2s-stream.md) | Separate TX/RX channels; copied slots survive cancellation until delivery/ACK. Fixed cyclic TX and checked-copy RX/duplex are integrated. Current source copies a cyclic RX period only while one whole period separates it from the DMA writer; that guard postdates the evidence, so rerun `checks/i2s-cyclic.py`. Continuity, cache visibility and wire drain remain unqualified; RX ordering blocker is below. | `i2s-duplex-20261018/integration.json`; `i2s-repeated-20261018/root-integration.json`; `i2s-hardware-cyclic-20261019/root-integration.json` |
| [Runtime I2S PCM/file input](i2s-application.md#complete-file-input-retains-unknown-serial-completion) | Finite TX leases accept BEGIN_FILE/END_INPUT with a short final period. Normal EOF requires exact DMA consumption and FIFO empty; wire completion is unknown. TX/GEN, clocks, pins, catalogue and buffers remain retained; REAP/CLOSE refuse. Explicit CANCEL may truncate; prefix-plus-guards mode remains separate. Native/provider and client/host checks model hardware; physical playback/drain remain unqualified. | `i2s-file-playback-20261019/root-integration.json`; earlier refills: `i2s-runtime-pcm-20261019/` |
| [Repeated keyboard/mimus/storage](../apps/mimus-board/README.md) | Workload 4 rearms only after transcript/sector/trigger delivery and matching ACK-response completion. Each generation needs fresh neutral→Enter and generation-bound storage overlap during unchanged EL0 recognition. Nonwrapping `UINT32_MAX` limit and original session deadline bound reuse; workload 0 remains version-1 one-shot. Lifecycle/transport/hardware/scheduler effects modeled; earlier arithmetic parity reused. Coordinator captures 1–32 generations/default 1 within 59 s/native 60 s; durable results precede ACK, failures preserve earlier output without retry/relaunch. Host fixtures/readiness only. Final `05c4b4d9…` unchanged; physical concurrency/capture/recovery/performance unqualified. | `mimus-repeated-board-20261020/root/integration.json`; host: `mimus-board-supervised-host-20261020/root-integration.json`, `checks-fixed/result.json`, `caller-entry.json` |
| [Healthy Wi-Fi reconnect](radio-session.md#explicit-healthy-reconnect-preserves-the-existing-service), [link-loss recovery](radio-session.md#clean-link-loss-is-recovered-without-disposing-sockets) | Same AP in the frozen evidence; current source also recovers onto another AP of the same network, and board transfers repeat after a timeout during link loss (not frozen). Fixed address or DHCP after socket/IP-task retirement. Fixed OPEN emits PASS; WPA2 owner/session completion is verified from terminal RAM despite reporting assertion/no PASS, corrected fixture unexecuted. Separate actual-IP-task DHCP new-lease/timeout cases pass with association/peer modeled; native board consumer build only. A clean unsolicited link loss now takes the interface offline with sockets kept open and reassociates until the recovery deadline; one host composition covers it. Loss with outstanding work, or a second loss during recovery, retains the radio. No board-poll/startup/physical claim. | `radio-reconnect-20261020/root-integration.json`; DHCP: `radio-dhcp-reconnect-20261020/root-integration.json`; link loss: not frozen; rerun `checks/radio-reconnect.py --fixture loss` |
| [IPv4 TCP listener](board-radio.md#an-admitted-listener-verifies-one-copied-application-exchange) | One admitted peer and copied 1–256-byte exchange each way; backlog one, fenced child/listener disposal. Seven actual-stack model, four app and five host cases pass; native image linked, not executed. Pinned MIT stack uses generated IPv4 reply-checksum correction. Radio-only workload 3 retains inactive older USB host inputs; physical networking, entropy and wire drain remain unqualified. | `net-inbound-20261020/root-integration.json`, `closure.json`, `bringup-impact.json`; stack: [radio-network](radio-network.md) |
| [Repeated Bluetooth recognition](bluetooth-native.md#repeated-headset-capture-and-recognition) | CVSD copied input and delivery-owned transcripts; unchanged recognizer remains EL0. Optional mSBC decodes 16 kHz speech through the same path. Current source also attaches the application to a Wi-Fi module owner and admits a resident lifetime, and a stop stored during a capture poll now closes cleanly (not frozen; rerun `checks/bluetooth-headset.py --repeated`). Transparent eSCO, receive bounds, acoustic quality and session timing remain physically unqualified. | `bluetooth-repeated-recognition-20261019/root-integration.json`; `bluetooth-msbc-20261019/root-integration.json` |
| [Bluetooth caller PCM](bluetooth-native.md#caller-pcm-playback-and-duplex) | mSBC playback-only and recognition duplex; two immutable 2,400-byte periods, full USB delivery and ticket ACK before reuse. END seals admission; CANCEL can truncate audio. Credits/disconnect account for buffers, not rendering or wire drain. Image-specific case boundary is below. | `bluetooth-pcm-duplex-20261019/root-integration.json`, `profile-gate-delta.json` |
| [Read-only SD](storage-read-application.md) | Legacy one-bit 400 kHz, four-bit default up to CSD/25 MHz, or required high speed at 50 MHz. USB-layout reconciliation was accepted at `894051d`; later headers do not invalidate that frozen image. No writes or physical mode qualification. | `sd-bus-modes-20261019/`; `sd-shared-usb-20261019/root-integration.json` |
| [Optional eMMC reads](emmc-read-application.md) | SDC2/SDC3 identification, EXT_CSD and copied reads pass instruction/host checks. Neither current board has a qualification target. The single-core image retains resources and has no qualified ROM return. | `emmc-native-20261018/` |
| [High/Baseline H.264](h264-native.md#high-profile-runtime-clips-and-presentation-ordering) | High profile 100, progressive 8-bit 4:2:0: CABAC/scaling/weights, I/P/B/reference-B; the frozen image holds two full-HD or four HD short-term references, four active list entries, five reordered copied-output slots. Current source admits three full-HD references in host models; it needs a new freeze. Twelve full-HD + twelve refB-HD + twelve four-ref-HD pictures complete in models; normal mode decodes three/copies two. Physical decode/cache/confinement/drain remain unqualified. Current source adds long-term references in P pictures, memory-management operations, POC and frame_num wrap and Baseline/Main streams, checked with header-rewritten clips in models (not frozen; rerun `checks/h264-high-modes.py`). Long-term B references, unrestricted streaming and broader encoding remain required. Separate Baseline scopes retained. | `h264-high-20261019/root-integration.json`, `closure.json`, `root/layout-review.json`; Baseline: `h264-fullhd-20261019/` |
| [VGA/legacy H.264](h264-native.md) | VGA admits 256 prepared 640×480 pictures, 512 KiB/AU; legacy 48×32 IDR/P remains separate. Input refill, reference retirement and copied-output ACK are distinct. Corrected launcher and supervisor-policy evidence are bounded as below. Physical pixels, reference visibility, confinement and drain remain unqualified; no reclamation. | `h264-vga-20261019/root-integration.json`; `h264-launcher-20261019/root-integration.json`; `h264-supervisor-entry-20261019/result.json` |
| [DI301 WEAVE/motion/fixed TNR](deinterlace-native.md), [host](deinterlace-preparation.md) | Modes 1/2 retain their original scopes. Mode 3 requires reset-held entry and uses two 640×480 NV12 frames with zero history, copying planar YUV420; each guarded flag buffer is 307,200 bytes. Native plan/allocation/resumed completion/serializer and host/SRAM cases pass with hardware/pixels/timing modeled. One allocation replay repaired an omitted checkpoint and reproduced proof/table bytes; completion then resumed without replay. One image/launcher. Image `9a4c1c38…` adds a selectable TNR strength (not frozen). Resources retained; numerical/physical filtering, gain feedback, film detection, repeated jobs and drain remain unqualified or required. | `deinterlace-tnr-20261020/root-integration.json`; prior: `deinterlace-motion-20261020/`, `deinterlace-native-20261020/` |
| [Recorded Bayer ISP0](camera-isp0.md), [host](isp0-preparation.md) | One 640×480 BGGR12→NV16 frame via TOP/ISP600/VIPP0/camera DMA0, with guarded masters 1/2 and immutable copied output. Final `32eb5b93…`: bounded-call nominal, classified failures, serializer/host/allocation and separate boot/SRAM checks. Pixels/device effects modeled; no numerical, full TinyUSB/DCD, scheduler or physical proof. GPL defaults/setters, no opaque tuning/runtime blob; all resources retained after ACK. Sensor-dependent ISP1 path stays separate. | `isp0-native-20261020/`; `root-integration.json` binds the final closure |
| [HEVC decode](hevc-native.md) | One uploaded two-picture clip, Main profile 8-bit 4:2:0 640×480, IDR then P, for decode on the VE with copied NV12 output. `sunstone-a733-hevc` builds; host models check parsing, storage and register programming, and the IDR picture's register writes match a vendor-library trace. Preparation refuses a launch bundle until physical allocation and page-table evidence exists. No numerical or physical decode result. | Not frozen; relevant checks: `checks/hevc-decode-model.py`, `checks/hevc-native-access.py` and `checks/hevc-image.py`; earlier source-only scope: `hevc-host-source-20261020/root-integration.json` |
| [H.264 encoding](h264-encode-native.md) | Two caller 640×480 NV12 frames, Baseline level 3.1/CAVLC/QP 26 IDR→P; immutable copied outputs with delivery-gated ACK. All input/reference/output resources retained. Actual provider/IOMMU/copy/USB and 140 SDK-reference register-write checks pass with entropy/completion modeled; no opaque CPU library linked. Final image `2e4a93b0…`. Current source encodes sequences of 1 to 30 pictures with ring reuse, and its 2,128 writes for 30 pictures match the library (not frozen; rerun `checks/h264-encode-native.py --case gop --frames 30` and `checks/h264-encode-registers.py`); the supervised capture tool still captures two pictures. Real entropy, reconstruction, numerical encoding, visibility and drain unqualified. Rate control and broader encoding remain required. | `h264-encode-20261020/root/integration.json`; [source provenance](../apps/h264-encode/sources.json) |
| [JPEG encoding](jpeg-encode-native.md) | One retained caller 640×480 NV12 job, stride 640/quality 75; copied JPEG ≤1 MiB. Inherited PCK3, exclusive encoder lease and IOMMU master 3. Provider/IOMMU/copy checks model completion and entropy; native image linked, not executed. No numerical, burst, visibility, drain or physical claim; all resources retained. Frozen workload 2 uses private USB0, with eight inactive radio/USB-host input deltas. Broader encoding remains required. | `jpeg-encode-native-20261020/root/integration.json`, `closure-summary.json`, `commit-binding.json` |
| [JPEG](jpeg-application.md) | Authenticated supplied baseline YCbCr420 image and copied pixels with dimensions; independent oracle optional. Current source admits up to 512 KiB of input and 640×480 planes, and version 3 sessions decode up to eight images in order; the supervised tool still sends one image, and neither change is frozen. Pixel correctness and confinement remain physically unqualified. | `jpeg-runtime-input-20261018/` |
| [Crypto](crypto-application.md#fixed-cold-clock-provisioning-for-caller-requests) | Caller SHA-256/AES-128 ECB; version 3 adds fixed cold CE400 clock/reset provisioning after authenticated reset-held/gated admission. Eighteen owner cases and two separate SRAM cases pass; SHA device output/RTOS effects modeled. One build, prior arithmetic/version-1/2 evidence reused. Current source adds AES-192 and AES-256 keys with CBC, CTR and GCM, SHA-1, SHA-224, SHA-384, SHA-512 and HMAC in the services (not frozen; rerun `checks/a733-ce-modes.py`); the application's request format still offers only SHA-256 and AES-128 ECB. All resources retained; physical exclusion, secure access, operation and drain remain unqualified. | `ce-clock-20261020/root-integration.json`; caller inputs: `crypto-runtime-input-20261018/` |
| [USB HID/CPU IRQ](usb2-host.md) | LS/FS/HS HID copies four reports without desktop events. Supervised board option routes CPU0 INTID187 to the existing owner; other profiles poll. IRQ delivery, DMA/drain, power and attachment reuse remain unqualified. | `usb-host-interrupt-20261018/root-integration.json`; `usb-host-cpu-irq-20261019/root-integration.json` |
| [Gen1/companion leaf](usb2-host.md#one-gen1-hub-or-the-usb2-companion-root-serves-one-leaf) | One selected root, direct or one self-powered hub/leaf: workloads 6/8 Gen1 MSC/boot keyboard, 10/12 USB2 companion keyboard/HS MSC. Typed topology binds the unchanged 592-byte session. Actual stack/copied-client/host plus six identity/layout and two route cases model hardware/IRQ. Native linked, not physically executed. Root/topology/class, ≥900 mA and hub-power admission remain required; no physical bundle. Simultaneous roots and wider USB remain required. Current USB2 source also admits two hub tiers, bus-powered hubs and USB Audio Class 1 capture, which no workload selects. Mimus remains USB2. | `usb-ss-hub-20261020/root/integration.json`, `handoff.md`; earlier direct HID: `usb-ss-hid-20261020/` |
| [USB hub/dual leaf](usb2-host.md) | Workload 4: one self-powered HS USB2 hub, one LS/FS/HS HID plus one HS MSC. Joint immutable output requires both results and fresh attachment. Existing IRQ187 route retained; derived profiles keep workloads 0–3. Hub/TT, power, DMA and drain remain unqualified. | `usb-host-dual-leaf-20261019/root-integration.json`; earlier single leaf: `usb-host-hub-20261019/` |
| [USB MSC/Gen1 BOT](usb2-host.md) | GET_MAX_LUN endpoint reset/dequeue precedes LUN0 fallback and copied READ10; eleven HS cases model hardware, SS recovery is source-reviewed only. Other stalls/uncertainty retain. Workload 6 Gen1 read-only LBA0 refuses USB2 fallback and requires authenticated ≥900 mA plus image-bound COMB0/rail/clock/route/source-switch admission; none exists. The TinyUSB adapter now tears down a disconnected hub child, or a device behind an adapter, through the xHCI detach path (not frozen); a Type-C detach or bulk stall still ends the session. Wider modes and drain remain required. Frozen recovery image retains the older clock policy. | `usb-msc-stall-20261020/root-integration.json`; Gen1: `usb-host-superspeed-20261019/` |
| [LRADC0 raw observations](adc-application.md#lradc0-raw-observations-require-separate-electrical-authority) | ADC version 2 copies two fresh channel-0 raw codes, then configured disable with permanent retention. Final `7f390582…` passes composed/driver/host checks with analog/RTOS/USB effects modeled; raw 0/63 establish no measured value or sample rate. Physical use is blocked by nominal 1.8 V R23 pull-up versus documented 0–1.35 V range, without overrange authority. No key mapping or voltage conversion. | `lradc-20261020/root-integration.json`, `root-frozen/manifest.json`; original and corrected fixtures retain their scopes |
| [Thermal](thermal-application.md), [ADC](adc-application.md), [finite USB](usb.md) | Finite applications and return adapters integrated. Thermal/ADC retain configuration and leases; ADC needs fitted-divider electrical admission. The fixed [thermal policy](thermal.md#a-fixed-board-policy-throttles-and-stops-work) ends a thermal session at the critical level. The CPU clock driver lowers raised clusters at the throttle level, but no image runs it, so no image reduces load there. Physical operations/return remain unqualified. | `thermal-adc-returning-source-20261018/integration.json`; `usb-returning-20261018/integration.json` |
| [RTC source/alarm](rtc-application.md) | Separate source initialization and external-source alarm paths. Source admission precedes alarm use; success requires verified Secure interrupt-state restoration, not only a completion message. | `rtc-source-returning-20261018/integration.json`; `rtc-alarm-returning-20261018/integration.json` |
| [NPU runtime/IRQ](npu-application.md), [service](npu-service.md) | Retained same-model SUBMIT/READ/copied output/REAP/CLOSE; IRQ97 owner alone acknowledges. Bootstrap plus two finite inferences require three notified completions and stopped receipt. Source-off CLOSE/GIC stop precede release; retained runtime keeps OS/USB live without ROM return. | `npu-interrupt-service-20261019/root-integration.json`; `npu-runtime-input-20261019/dma-reconcile/root-integration.json` |
| [NPU two-stage graph](npu-two-stage.md) | Fixed two fused convolution/ReLU stages, 64 input/eight output bytes, internal SRAM and one graph completion. CPU/SDK references pass; native output/service are modeled. No mimus replacement or physical inference claim. | `npu-two-stage-20261019/root-integration.json` |
| [NPU SH/NT graph](npu-relative-shift.md) | Fixed FP32 four-head/128-step relative shift, external pool cleared before each job, chunked copied input/output and healthy CLOSE. Two reused-network SDK outputs match FP32 bits; native replies/IRQ/cache/transport are modeled. No unchanged-encoder acceleration. | `npu-relative-shift-20261019/root-integration.json` |
| [Complete NPU NN graph](npu-complete-nn.md) | Published 125-operation ShuffleNetV2 graph with coefficients/pool; two copied jobs and CLOSE. Both SDK input patterns saturate to `[0,255]`, with undocumented classes: no input-sensitive accuracy, physical inference or mimus acceleration claim. | `npu-complete-nn-20261019/root-integration.json` |
| [Combined GPU/NPU](accelerator-composition.md) | Trusted GPU compute and complete NPU graph share providers and one copied USB owner. Interleaved results/independent CLOSE retain the session deadline; shared faults revoke both. Native and host preparation/capture checks model admission, CPU/RTOS, arithmetic and hardware; NPU outputs remain saturated. Positive bundle substitutes physical NPU validation; production refuses synthetic proofs. Supervisor stops at readiness before queue/USB. Native linked; physical execution unqualified. | `gpu-npu-composition-20261020/root/integration.json`; host: `accelerator-host-20261020/root-integration.json`, `results.json`, `supervisor-entry.json` |
| [G2D fill/copy/blend/2× enlargement](g2d.md), [host](g2d-preparation.md) | Exact 2× ARGB8888 enlargement uses a source crop at half the even destination dimensions. Current `f77be8b2…`: three pinned geometries, five native refusals, one job/copy, checkpointed HS TinyUSB/ACK and separate SRAM/host checks. Before ACK, authenticated output must keep outside-rectangle/padding bytes zero; interpolation remains unchecked. One build/launcher, no prior matrix replay. Earlier success/blend scopes remain separate. Image `08e56fc5…` adds general scaling, image-over-image blending and RGB order conversion with model checks (not frozen). All resources retained; physical pixels/DMA/cache/reuse remain unqualified; rotation and YUV conversion remain required. | `g2d-scale-20261020/root-integration.json`; prior: `g2d-success-20261020/`, `g2d-blend-20261020/` |
| [GPU programs/live input](gpu-compiled-programs.md) | Trusted sealed program, same-shape copied input, exact-sequence READ and CLOSE. Separate multi-dispatch and true 1D/2D grid profiles admit up to 31 workgroups/992 elements with 32 lanes. Numerical execution and device-cache visibility remain modeled/unqualified. | `gpu-dispatch-geometry-20261019/`; `gpu-workgroup-grid-20261019/root-integration.json`; `gpu-runtime-input-20261019/dma-header-integration/root-integration.json` |
| [GPU indexed textures/depth/blend](gpu-render.md#depth-testing-and-source-over-blending) | Through 1080p with trusted shaders; per-frame depth OFF/LESS/LEQUAL/ALWAYS plus writes and straight-alpha source-over. Shared GEOM+FRAG_PR then final FRAG retains both rings through final completion. Old single-final-fragment ownership results are historical; legacy-profile-2 sequencing is separately checked. GPU/firmware and selected app/USB boundaries are modeled. Current source adds graphics profile 4: state batches, stencil, perspective texturing, five blend equations, sampler modes and lending a finished frame to a display consumer (not frozen). Physical graphics, arbitrary shaders and blend factors, multisampling and display use of a lent frame remain required. | `gpu-depth-blend-20261019/root-integration.json`, `closure.json`, `root/layout-review.json`, `reuse.json` |
| [GPU compute cleanup](gpu-compute-service.md), [render cleanup](gpu-render.md#healthy-close-cleans-firmware-objects-before-stopping) | Exact successful context/render-target/free-list receipts precede healthy stop; partial progress stays retained and late reconstruction faults through shutdown. Seven compute/eight render composition cases model firmware/MMIO/isolation. Separate full native builds `dae5a387…`/`6c45bdc3…` compile/link their recorded layouts only; no runtime-result transfer. Default backing/VM stay retained and reopen is refused; optional standalone disposal has separate scope below. Physical cleanup remains unqualified. | `gpu-compute-cleanup-20261020/root-integration.json`, `gpu-render-cleanup-20261020/root-integration.json`; builds: `gpu-compute-cleanup-native-20261020/`, `gpu-render-cleanup-native-20261020/` |
| [Standalone GPU disposal/reopen](gpu-render.md#standalone-sessions-can-dispose-and-reconstruct-child-resources) | Optional sole-client single-core profile releases/rebuilds seven child parents with fresh generations/epochs; two global firmware parents retained. Original `e26b9632…` native leaf checks model stopped admission; final `780d9544…` depth app/TinyUSB protocol models service completion, with thirteen host cases. Two builds, 87/89 objects unchanged after app budget fix. Separate 15 s DISPOSE/rebuild budgets; triangle/version-8 texture runtime and pending texture OPEN remain unchecked; compute-only has separate evidence below. Physical reuse/visibility/SMP and mixed-device reclamation remain unqualified. | `gpu-resource-disposal-20261020/root-integration.json`, `closeout.json`, `final-frozen/manifest.json` |
| [Compute-only disposal/reopen](gpu-compute-service.md#compute-only-disposal-and-reopening) | Image `06315568…` executes actual three-parent release/reconstruction with package/global parents retained, plus seventeen app/TinyUSB exchanges with GPU/service completion modeled. Pending OPEN holds old identity; READY publishes coherent new epoch/result for immediate READ. Two 65-word outputs use pages of 32, 32 and 1 word; healthy host replay and four tail faults retain exact 256-byte prefixes. Physical reuse/cache/SMP and mixed-device disposal remain unqualified. | `gpu-compute-lifecycle-20261020/root-integration.json`, `final-frozen/manifest.json` |
| [GPU returning application](gpu-application.md), [NPU returning application](npu-application.md) | Workers stop and PMIC parks before cleanup. Reviewed Nonsecure publication and matched host restoration are preserved separately from retained live-input profiles. Physical inference and ROM return remain unqualified; critical frozen scopes are below. | `gpu-returning-repeat-fix-20261019/root-review.json`; `gpu-returning-host-20261019/root-host-source-review.json`; `npu-finite-return-20261018/frozen/`; `npu-finite-host-20261018/root-host-source-review.json` |

## Blockers and image-specific evidence boundaries

- **I2S cyclic RX/duplex:** the completed-package counter does not explicitly
  order destination writes and cursor updates, so physical admission requires
  `cyclic-rx-completion-and-reuse-ordering-established`. Only a board
  measurement can meet it: destination writes must trail the counter, and the
  DMA writer must lead the counter and cursor, by less than one period each.
  No such record exists, and CPU-cache review alone is insufficient. The
  receive path keeps one whole period between each checked copy and the
  writer, so the application's four-period ring gives its client one period,
  128 ms, to return each copy. Preserve
  `i2s-hardware-cyclic-20261019/root-cache-review.json` and
  `identity-reuse-clarification.json`; standalone-client cases model service calls.
- **Camera streaming:** the first DMA status interval now opens immediately
  before the OV5640 frame-enable write. Delayed setup, uncertain enable and
  callback reentry pass focused host checks; later gaps still suppress
  delivery ([camera capture](camera-capture.md)). Native ring allocation,
  slot synchronization, stop/drain and application delivery remain missing.
  Physical frame timing and address latching need a camera module.
- **Bluetooth:** seven composed/eight native operation cases retain ELF
  `36bfa257…`; final `388f9f93…` changes five objects for exact profile guards.
  Its final-image case covers version admission and copied PCM commit only.
  Earlier repeated-recognition USB cases also retain their earlier image;
  `identity-reuse.json` binds the changed finished/status case. Pinned libsbc 2.0
  is CPU code under LGPL-2.1-or-later; preserve source/notices/relinkable objects
  via [the manifest](../apps/bluetooth/msbc-source.json). Default CVSD omits it.
- **H.264:** the historical `862fae444a481eb9505d8ba7ef3835760e208764663c1e7aef8459f8b9be43ac`
  omits VE counter-read permission and cannot publish completed output. Its
  substituted-read compositions are historical only. VGA `24144b2a…` includes the
  correction and real lower IOMMU policy checks; normal-mode evidence stops after
  frame one with terminal status pending. Use the corrected AArch32 launcher in
  new VGA bundles; old frozen bundles reject version 2. Launcher tests stop at
  modeled reset handoff; the 59-second supervisor-policy sentinel stops before
  queues, evidence creation or USB. Full-HD `b48123e3…` has separate private
  contract/sealing evidence; neither path executes shared startup or proves pixels.
- **GPU:** the corrected fixed-return candidate is
  `2ab2cb1b3bac150d5eb695793ab0eb39bb453afc7fbd7ecb2f595ebfca3fd08d`.
  Publish through Nonsecure EL1 mappings before SMC; earlier EL3-only publication
  results are superseded. Root review passes; physical admission remains false.
  The historical two-job
  image has the NEW-only callback defect and is superseded for repeated submit.
  Keep `gpu-returning-application-20261019/ns-publication/` and
  `gpu-returning-host-20261019/` within their publication/host scopes. Native
  compute/render cache ordering implements the pinned firmware's documented
  per-job flushing contract and matched completion UPDATEs. No missing host-side
  cache command is identified. Physical device-cache visibility remains
  unqualified; CPU maintenance alone does not establish it. Source comparison:
  `gpu-cache-visibility-20261020/assessment.json`.
  Corrected shared rendering uses FRAG_PR (204) at 2n−1, then final FRAG (203)
  at 2n; geometry completion n and final fragment 2n gate output/reuse. Older
  single-final-fragment ownership cases are superseded. Compute evidence retains
  its separate scope.
- **NPU returns:** preserve `npu-finite-quiescence-20261018/integration.json`,
  `npu-supply-returning-20261018/integration.json` and
  `npu-finite-host-20261018/transport-frozen/`. SRE tasks run at EL1t; cached SMP
  task/exit rules do not transfer. Stop communication after transport failure;
  retain ownership. Root retains architectural return review.
- **Supervisor entry:** host-only corrections admit DSI and UART result keys.
  The actual caller reaches a readiness sentinel before queue or device access:
  `dsi-supervisor-entry-20261019/result.json`, `frozen/manifest.json`. This
  establishes neither native startup nor hardware operation; H.264 was not rerun.
  UART entry passes the same boundary under `uart-supervisor-entry-20261019/`.
  Its `caller-audit.json` confirms admitted keys for all 21 existing production
  callers, including both conditional GPU profiles; device cases were not repeated.
- **Physical device preparation:** DSI needs a selected panel and image-bound
  electrical/PLL, rail/bank-voltage, wiring, isolation and fetch-padding facts;
  model-only authority is refused. USB Gen1 has no authenticated physical source
  record. Type-C cutoff cannot remove an external hub's supply. Combined mimus
  needs fresh DDR/model-memory, power, DMA and recovery admission; its injected
  native transcript does not establish physical concurrent recognition/storage.
  The DSI image also requires PCK domains 0, 1, 2, 3, 8 and 9 to be ON at
  entry and powers none of them; the power-domain owner can power domain 1
  for a registered row, but the DSI image registers none, and the owner has
  no entry for the other five domains. The radio board test waits for a
  physically qualified four-bit SDIO profile
  ([remaining OS work](os-image.md#not-yet-integrated)).
- **Shared-helper migration:** the consolidation round kept every barrier
  and the order of interrupt-mask and scheduler calls, but three code
  differences are covered only by emulation. Register reads that return
  through a pointer store the value after the trailing `DSB SY`; native
  adapters run their instance checks before reading `CurrentEL` and `DAIF`;
  and a failed read translation skips the write translation. The maintenance
  watchdog's mapping check is also no longer inlined. The first board run of
  each affected adapter should confirm them.

## Continuation and evidence reuse

1. Check session access on both ports before each board session. Board work
   and the [next offline work](#next-offline-work) proceed in parallel; a
   pending reconnect pauses only the board it names.
2. Inspect the selected port's queue binding and retained ownership before a
   launch. Both queues were already bound in the 2026-10-24 snapshot above.
   Match each frozen image, launcher and admission record to its evidence;
   regenerate only records whose relevant inputs changed. Preserve the accepted
   returning CPU ELF and its bindings.
3. Continue from each board's recorded result in the
   [ranked board test plan](audit/board-test-plan.md). Port `3-1` first needs
   power removal and the narrower pre-scheduler plan above. Port `3-3` has
   passed the native USB channel and can continue eligible peripheral work.
   Each test runs once under the port lock with frozen inputs and a chosen
   evidence directory. Stop at the first transport failure and await explicit
   recovery; never retry a failed launch unchanged.
4. The passing USB0 channel on port `3-3` makes peripheral paths that report
   through USB0 eligible, one bounded test per concrete question. The first run of the
   integrated OS image uses the supervised variant; its launcher exists, and it
   needs a reviewed entry record built from physical evidence first. Record the
   measurements that the rounds left open where a test reaches them:
   `ID_AA64ISAR0_EL1` on every core, the CPU PLL pattern and low-frequency-mode
   words that boot0 leaves, the longest pause of each resident service loop,
   PMIC BUSY durations, mimus load and recognition time, the DMA0 and GPU
   interrupt delivery, whether a headset stop during capture closes cleanly,
   whether the target headset stays connected to an audio gateway that
   advertises no call feature, whether its voice button starts a recording,
   and whether the AIC8800 module is still admitted when Bluetooth fails
   first.
5. Tests that risk the connection come last in a board's connection window,
   after the ranks above pass on that board. They include card writes, after a
   host backup of that card; the stage-2 loader, a read-only snapshot of the remote-writer
   registers after a cold boot, and the resident lifetime; intentional
   watchdog expiry; CPU clock or PMIC voltage changes, each with the user's
   approval; and tests that need a camera, display, microphone, headset, PCIe
   endpoint, NVMe device or access point. Keep the failed
   historical UFS probe disabled; absent eMMC/UFS are not current qualification
   blockers.
6. Qualify unchanged-model mimus and representative concurrent OS workloads.
   After every required hardware function works, optimize complete recognition
   to at most 0.05 times audio duration; investigate 0.01 times as a bounded stretch.
   Report loading and live latency separately. Reproducible boot images,
   recovery/update procedures and physical qualification remain goal requirements.

Evidence reuse follows these rules:

- Reuse accepted checks while their relevant inputs match; a commit,
  documentation edit or handoff is not a rerun reason. Follow
  [selective validation](validation.md) and the [reference index](reference-index.md).
  Tables above select continuation records; their linked component documents
  preserve full evidence inventories. A “not frozen” label identifies missing
  artifact preservation, not a requirement to rerun otherwise valid checks.
- Preserve each accepted ELF, full source/object closure, generated inputs and
  evidence scope. The fix rounds rebuilt many images: a frozen image keeps its
  recorded evidence and scope, and a rebuilt image needs new evidence. Never mix
  current objects into a frozen image. An integration audit record matches only
  its named revision; do not rebuild old images merely to catch up.
- Shared USB headers change embedded layouts even in consumers whose USB host
  path is inactive. Future builds compile affected consumers consistently.
- The shared launcher correction `6407e26` preserves the exact legacy
  preprocessed templates and the combined binary; it adds no physical launch
  evidence.
- The fix packages left their check outputs under
  `~/.cache/sunstoneos/build/audit-*/`. Those are working outputs; freeze
  evidence before treating a new image as accepted. The radio
  fixes are the exception; their evidence is under
  `~/.cache/sunstoneos/evidence/audit-radio-20261020/`.
- Configure a fresh CMake build directory with the compiler and all dependency
  paths supplied together; changing the compiler in an existing cache can clear
  those paths. Save a complete resumable execution state before an
  allocation-only fixture exits.

### Boot chain, DMA authority and clock references

The [microSD container packager](microsd-boot-package.md) creates regular files
only; one actual packer invocation and seventeen host checks pass. Root audit:
`microsd-boot-package-20261020/wrapper-root-integration.json`. The
[cold-entry recorder](cold-entry.md) supplies a guarded 4,164-byte native
monitor that captures header/exception state and parks. Actual boot0 parser,
cleanup/main-tail and eight AArch64 entry cases pass with independent reset-state
models; one ordinary-file container contains the actual monitor. Evidence:
`cold-entry-20261020/frozen/manifest.json`, `instructions/result.json`.
The actual parser cache wrappers separately execute twenty synthetic set/way
operations before disabling the data cache under `cold-boot-cache-20261020/audit.json`.
The [stage-2 loader](boot-slots.md) admits the SD controller state that boot0
leaves and builds the integrated OS image's resident contract in host and
instruction models. On a board its three entry prerequisites refuse, so it
enters no image. The [remote-writer check](remote-writers.md) reads the reset,
clock and DMA-enable state of every DRAM bus master without writing, but it
cannot prove the OR100 processor or the crypto engine excluded, and no boot
image calls it. Memory ownership, physical boot, media writes and recovery are
not established either, so standalone OS boot remains incomplete. Packer
license/source equivalence remains unresolved. The coordinator owns physical
admission; implementation and review may be delegated.

DMA fabric admission accepts correctly normalized 24 MHz references from
supported raw oscillators and retains REF in every admitted profile. Ten focused
platform/clock-registry sanitizer cases pass under
`dma-clock-reference-20261020/frozen/manifest.json`; no native image or physical
DMA qualification is added. [DMA1 applicability](dma.md#dma1-request-routing-remains-unresolved)
is source-blocked after the bounded primary-source search: the CPUX request table
does not establish CPUS DRAM encoding. No DMA1 implementation, build, descriptor
or launch is enabled.
DMA-copy, UART DMA, I2S and HDMI audio host admission require external session/
configuration-bound channel, controller and descriptor/data Nonsecure access
authority. Thirty additional consumer host cases and twenty source matches are
accepted under `dma-consumer-security-20261020/root-integration.json`, without
native/runtime changes. Production rejects model authority; no safe DMA security
register probe or physical permission is established. Earlier images retain
their runtime scopes; HDMI audio USB observation has its separate scope above.

The [provider clock record](provider-clock-reference.md) covers normalized REF24
admission in TWI11/USB2/radio/CE/I2S/PWM, the video engine and PCIe.
[SPI](spi-platform.md) has separate parent checks. Raw S_TWI1, LED source 0 and
S_SPI source 0 retain their recorded restrictions. [HDMI](provider-clock-reference.md#hdmi-native-evidence)
binds raw 24/26 MHz TOP input separately from normalized 24 MHz SFR and retains REF.
Actual compiled-object raw-26 720p prepare/start/retained-stop and saved-state
refusals pass under `hdmi-ref26-20261020/root-integration.json`. Calculated pixel
rates are 73.9375/147.875 MHz; 1080p has source/host evidence only. Physical timing
and output remain unqualified; the earlier GPU image retains its frozen scope.
Other function checks do not qualify images or physical clocks.

## Next offline work

Every item below is offline work with host builds, emulators and host models.
Each item states what is left and why the rounds left it; the
[audit record](audit/README.md) gives the details per package. A change to an
image's sources changes its identity: rebuild the image and record new
evidence, and never mix new objects into a frozen image.

### Shared-helper copies

The consolidation round moved a private copy only when a shared header
function returns the same result for every input. These copies remain:

| Copy | Reason it remains and what unblocks it |
| --- | --- |
| [docs/shared-helpers.md](shared-helpers.md) and the comment in `include/aarch64_device_memory.h` | The packages did not own them. The migrated-caller list and the remaining-caller table still describe the code before the consolidation round for every directory except `drivers/net` and `drivers/bluetooth`. The document still names the removed `a733_ce_separate()` and `a733_dma_separate()` and the removed NPU cache enum, and its HDMI-audio rule accepts one shareability value where the code accepts 2 or 3. |
| Task-context check and fenced accesses in `drivers/storage/a733_mmc_platform_native.c` | `checks/a733-storage-native.py` builds its host model by rewriting this file's exact inline assembly. Replace those text rewrites with a host model of the shared headers first. |
| `drivers/usb/a733_usb_platform.c` and `a733_usb_platform_native.c` | `checks/a733-usb-platform.py` copies a fixed source list without the shared headers. Add the headers to it, then reapply the migration, which was implemented and reverted. |
| GIC accesses and one mapping check in `apps/storage-read-single-core/monitor.c` and `runtime.c` | The DSI scanout image's patches use their lines as context. Regenerate those patches in the same change. |
| Register accessors and deadline guards in the AArch32 device and storage launchers | Their host build tools put only the launcher's own directory on the include path, and prepared bundles record the launcher hash. The tools must change, and the launchers need qualifying again. |
| Fenced accesses in `drivers/gpu/gicv3_spi.c` | About 15 board applications compile it, and each needs a before-and-after comparison. |
| The radio-profile page loop in `apps/board/main.c` | Every single-core `main.patch` and the OS image's `main.patch` are built from this file and must be regenerated in the same change. |
| `clock_ok()` in `drivers/uart/a733_uart.c`, `time_ok()` in `drivers/audio/a733_i2s_platform.c`, four samplers in the crypto engine driver and one overlap test in `drivers/npu/nbg_relocation.c` | None: the final reviews found that these have header equivalents and were missed. Move them with the method in [docs/shared-helpers.md](shared-helpers.md#migration-rule). |
| The overlap test in `drivers/storage/fatfs_port.c` | It runs without first checking that both ranges fit, as the removed copy did. Adding the span check changes behavior, so it needs its own change and checks. |
| Task-context checks in CPU startup and the FreeRTOS port | Coordinate with the assigned multicore implementation owner and reviewer. |

### Build recipes and patches

| Item | Reason it remains |
| --- | --- |
| Hooks in `apps/board/main.c`, `session.c` and `identity.c` that replace the single-core session, main and identity patches (the rest of supervisor-apps-8) | The board owner holds these files. The patches now apply without fuzz. |
| One shared patch function for the other standalone projects (the rest of build-system-13) | The remaining copies are in the root `CMakeLists.txt`, `platform/*` and other applications. |
| Folding the board, thermal and RTC branches of the shared single-core monitor into one parameterized path | The DSI scanout image patches the shared monitor, runtime and board linker script. It must move to the shared recipe or to hooks first. |
| The standalone storage-read single-core and eMMC read projects | They fail to build in the board application's TinyUSB sources: `hcd.h` is not found and `TUSB_SPEED_SUPER` is undeclared. |
| Path maps in standalone projects | Standalone projects do not get the root `-ffile-prefix-map` options, so their objects embed absolute paths. |

### Board application, contracts and watchdog

| Item | Reason it remains |
| --- | --- |
| One shared contract version for the extended images (supervisor-apps-3 with bringup-gaps-11) | It changes the contract layout in `platform/a733-smp-cached/boot.h`, every application's boot record, extended-boot patch and preparation script, and the EL3 writers, so every board and extended image changes identity. |
| Sealing the board session once (supervisor-apps-6) | Seven native checks call an application's session preparation without the board seal; the storage-read snapshot is the input to an SD session patch; the DisplayPort and H.264 encoder images prepare their session before the board's. |
| Core affinity for the board service tasks (the rest of supervisor-apps-12) | The Type-C and PMIC tasks yield while an I2C transfer is in flight, so pinning both to one core risks starving it, and the benefit depends on multicore recognition. |
| The USB0 `stop()` loop in `apps/board/usb0.c` | It still delays one tick per pass, because `cmake/a733-bluetooth.cmake` requires its exact text. |
| Shared WDT0 values (security-misc-7) | [docs/watchdog.md](watchdog.md#launchers-arm-the-watchdog-for-runtime-images) lists the five writers. The launcher build tools copy only each launcher's files, a host check compares the SMP and observe launchers as text, and the expiry probe's binary hash is pinned. |
| A host case for the resident supervising task's suspension | `checks/board-park.py` runs only the PMIC task; the resident path was checked by disassembly. |

### Devices and services

| Area | Next deliverable | Reason it remains and owner |
| --- | --- | --- |
| Stage-2 entry | Supply the three entry prerequisites in `boot/a733/stage2/admission.c`: remote-writer exclusion, watchdog recovery after a cold boot, and a comparison of boot0's EL3 entry state with the FEL-launched reset state. Call the remote-writer check before the loader reads the card. Add a faster loader SD profile; the 400 kHz one-bit profile reads about 25 KB/s, so 14 MB of models take about ten minutes. | The coordinator runs its board tests. Exclusion needs a source that documents an OR100 reset or removes OR100 from the A733, and a crypto engine idle status or control-flow proof. Do not repeat the E902 search or infer safety by reversing startup writes. |
| OS image services | Carry a CPU clock grant record in the OS session, by default one that grants nothing, and check it with `a733_cpu_clock_grant_load`. Start the CPU clock driver after the thermal policy, with an owner task that polls within the driver's 10 ms margin, and add a reduced-load response to THROTTLE. Report measured cycle timing with each recognition. Stage the radio and Bluetooth profiles and firmware in supervised preparation, in one span when both are built. Keep the session running after a headset failure while the radio is up. | Application owner. A grant that permits a physical change needs the user's approval for that change. |
| Radio and Bluetooth | Alternate access points in the board profile; a native consumer of the A2DP sink. | Radio and Bluetooth owners. WPA3 association needs SAE external authentication and protected management frames; the SAE exchange itself is implemented and host-checked. |
| I2S receive | An eight-period cyclic receive ring for directions 3 and 5 in `apps/i2s`, and the launch condition in `apps/i2s/prepare.py` restated as a measured margin. | Audio owner. `I2S_PERIODS` also sizes the finite, duplex and USB transfers, so it must not simply be raised. |
| USB host | Adoption of a new Type-C attachment after VBUS removal; Bulk-Only Transport reset and bulk clear-halt recovery; a board workload and wire format for audio capture. | USB host owner. Storage recovery needs changes to the pinned TinyUSB storage class and to bulk-endpoint recovery in the core. |
| Display | Register domain-1 rows on the power-domain owner (`drivers/power/a733_power_domains.c`) in the DSI and deinterlace images, and give the owner a domain-9 entry for DSI; an application that streams frames through the replay ring; display use of the GPU's lent frame. | Display owner. DSI also needs PCK domains 0, 2, 3 and 8 for its IOMMU group, and domain 8 is USB2. |
| Camera | A CAM2 binding: a port field in the camera boot record and `apps/camera/prepare.py`, the PK10–PK19 data pins and MCLK2, and a sourced deskew value for receiver port 1. Continuous capture in the native application. | Camera owner. The coordinator's frame-enable interval fix is host-checked; native ring ownership, synchronization and stop/drain still need integration. |
| GPU | Compute input and output beyond one 4 KiB page, arbitrary trusted shaders, blend factors, clipping, mipmaps and multisampling. | GPU owner. Multi-page compute changes the package wire format, so it is a package of its own. |
| NPU | Complete the FIFO-direct USB path for the [existing runtime board image](designs/npu-board.md), resolve the recorded check failures, then obtain physical inference evidence. | NPU owner. The image already shares the board provider resources and uses extended memory; do not recreate it. Any mimus offload needs an accuracy evaluation. |
| Codecs, G2D and DI301 | Supervised capture of H.264 sequences longer than two pictures in `tools/check-a733-h264-encode-supervised.py`; H.264 frame-level rate control; supervised capture of JPEG sequences; repeated JPEG encode; an extended uncached pool for full-HD JPEG; G2D rotation once `g2d_rotate.c` is pinned, and YUV conversion; repeated DI301 jobs in one session, with gain feedback and film detection. | Video and graphics owners. Rate control needs a rate-controlled reference run of the pinned library to check against. Long-term B references stay refused until hardware evidence shows how the engine treats them. |
| PCIe, storage, serial and timing | Interrupt completion in the NVMe image, which needs GIC routing for INTID 184 or 185; reads of more than one block; a consumer of the FAT filesystem, with its FatFs pin added to `sources.lock.json`; an S_PWM clock-registry scope; GPADC window admission in the native adapter once an application selects windows; an image that runs the SNTP client with the RTC service; the new crypto modes in the crypto application's request format. | Each component owner. |

### Checks and stale documents

The rounds also reported defects in files the packages did not own. Each
file's owner fixes them:

- `tools/check-a733-pwm-supervised.py` refuses 600 ticks, the fan's full speed.
- These checks fail on their current images or fixtures: the storage-read
  native, binding and service checks with a fresh fixture,
  `checks/jpeg-supervised-host.py`, `checks/jpeg-sealing-native.py`,
  `checks/gpu-display-launcher.py`, `checks/usb-host-superspeed-native.py`,
  `checks/monitor-application-native.py`, `checks/crypto-runtime-native.py`,
  the I2S application's native parent-and-adapter case, the NPU convolution
  model's startup mode, `checks/gpu-draw.py`,
  `checks/a733-gpu-resources-emulate.py`, the TWI case of
  `checks/provider-clock-reference.py`, the native step of
  `checks/clock-wide.py`, the setup of the RTC checks and the final host step
  of `checks/bluetooth-application.py`.
  `checks/board-application.c` lacks HID and xHCI mocks.
- `checks/dma-interrupt.py`, `checks/i2s-interrupt.py` and
  `checks/i2s-application.py` use the unanchored source filter that made
  `checks/dma-pool-session.py` drop the generated FreeRTOS port when its
  output path contained `checks/`. The first two fail to link in the same
  way. Anchor their filters as that check now does.
- The NPU startup and clock-handoff models have no runner or check-index
  entry. They need `-M virt,gic-version=3` and at least two cores.
- `checks/npu-reserved.py` does not pass `TINYUSB_DIR`, and the session and
  reserved modes of `checks/npu-session.py` need catalogue artifacts that are
  not in the cache. `checks/radio-reconnect-wpa.py` names a worktree path that
  does not exist.
- The GPU display fixtures need regenerating with
  `checks/gpu-display-preparation.py`, because their trusted-program record
  pins the hash of `drivers/gpu/pvr_render_texture.c`. The SPI and monitor
  native fixtures from 2026-10-18 and the returning builds' default frozen
  baseline are stale.
- `checks/crypto-application-package.py` needs the bound queue. Both PCIe
  `prepare.py` scripts still accept a 250 ms startup bound. The camera record
  version must change in five files at once. Sixty-eight sanitizer checks
  lack `-fno-sanitize-recover=all`.
- Documents that are now stale: `include/a733_gpu_resources_native.h`,
  `docs/mmc-platform.md` and `docs/hdmi-native.md` call the PMIC provider
  read-only, although it accepts the CPU clock driver's DCDC3 and DCDC5
  selector writes. `docs/board-radio.md` says the poll budget covers the
  largest 4,096-byte receive, `docs/camera.md` says CAM2 still needs a native
  TWI9 controller, and `docs/pcie.md` says no image links the function
  client.

Use implementation workers and a separate focused correctness reviewer.
Keep each native path and its host preparation consistent before building. Use
separate worktrees and coordinate shared interfaces before concurrent edits.
Only the coordinator runs board tests.
Resume checks from saved state and reuse successful evidence while its relevant
inputs match. Defer performance optimization and recurring measurements until
required hardware functionality is established. Update canonical hardware and
component records and existing issues for accepted changes.

## Worktree cleanup preserves unfinished work

A cleanup on 2026-10-20 removed 327 of 364 registered worktrees. Each removed
checkout had integrated or patch-equivalent commits and no local files beyond
verified disposable Python bytecode. All branch references were preserved, and
the external source, build and frozen-evidence directories remain intact. The
fix rounds have since added one worktree per package; 129 worktrees are
registered now. Round five reviewed and integrated the `hevc-decode` sources;
that worktree and the unfinished `h264-interframes` worktree remain available. Do not discard retained changes as part of another cleanup
without review.

The cleanup inventory, branch identities, removal journal and final result are under
`~/.cache/sunstoneos/evidence/worktree-cleanup-20261020-204010/`.
`plan.json` records each original path, branch and commit; `result.json` records
the completed cleanup. Recreate a removed checkout at its recorded path with
`git worktree add <path> <branch>` when an old build or reference requires it.
Removing checkouts does not invalidate their frozen test evidence or justify
rebuilding their images.

The [historical handoff](handoff-history-20261019.md) preserves older integration,
recovery and validation context; it supplies no current launch authority.
