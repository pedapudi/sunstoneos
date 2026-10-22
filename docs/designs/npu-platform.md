# Design: NPU cold boot, recovery and power management

Status: **design**; no code has changed. Every item below ends implemented and
host-modeled; physical behavior stays unqualified until the board evidence at
the end exists. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md).

The design covers NPU startup ([a733_npu_startup.c](../../drivers/npu/a733_npu_startup.c)),
the local clock and reset controller ([a733_npu_platform.c](../../drivers/npu/a733_npu_platform.c)),
the engine controller ([a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c)),
the PLL controller ([a733_npu_pll.c](../../drivers/npu/a733_npu_pll.c)) and the
convolution application that runs them. It adds four behaviors: startup that
takes its supply and power domain from the power authority, local clock gating
when the NPU is idle, a second operating point, and bounded recovery from a hung
job. It also loads catalogue models from the FAT volume on the microSD card.

## Startup takes its rail and domain from the power authority

The power-authority design (`docs/designs/power-authority.md`, in review on
its own branch) provides two interfaces that startup uses: a rail token from
`a733_rail_acquire`, and a row request to the power-domain owner, which reads
the supply once and turns PCK600 domain 4 on. Startup becomes:

1. Acquire one DCDC2 rail token and the existing clock-registry token
   (exclusive PLL_NPU and NPU-local scopes, stable fabric and reference).
2. If the image runs the PLL controller, bring PLL_NPU to the selected point.
3. Check the local baseline once, then release the AHB, AXI and core resets,
   set the module divider and open the bus and module gates. This keeps the
   clock-before-power order of the pinned Allwinner platform driver.
4. Request domain 4 from the domain owner, wait 2 ms after it reports ON, and
   check the final local state.
5. Open the engine controller, which runs the 208-byte bootstrap command
   stream, and activate the service.

Startup no longer reads the PMIC over S_TWI0, waits its own 5,160 µs settling
interval between two DCDC2 snapshots, or writes PCK600 registers. The PMIC
task's baseline snapshot covers the rail once per boot for every consumer.

**Cold-boot record.** The resident OS image supplies the startup record from
its own boot path at link time. The record names the state that boot leaves
(local gates closed, core, AXI and AHB resets asserted, SRAM reset released,
domain 4 off or on and quiescent, PLL_NPU disabled or locked), and startup
verifies that state against the registers before its first write. Two PLL entry
modes remain: enabling a disabled PLL while DRAM runs, and retuning a locked
PLL. The pre-DRAM enable mode is removed: it needs SRAM-resident code before
DRAM initialization, and no image can run it. The platform's separate
"adopt an ON domain" branch is also removed, because the domain owner reports
the domain state.

**Progress per call.** The bootstrap task today performs one step per tick,
so the platform alone takes 17 polls. A startup call now runs steps until one
waits on hardware:

- the PMIC baseline, after which the PMIC task notifies the waiting task;
- the PLL lock, a spin of three observations 3 µs apart plus 20 µs;
- the domain acknowledgement, for which the owner spins 50 µs and then
  returns PENDING;
- the 2 ms domain settling, the only tick wait (section 1.3 permits a tick
  wait for a hardware wait of at least one tick).

The CCU sequence takes the shared register lock once.

## The service worker owns the NPU after activation

After activation the NPU service worker is the single owner of the engine
controller, the local CCU fields, PLL retuning and gating. The bootstrap task
suspends. All state lives in kernel `.bss` (Normal Write-Back, Inner
Shareable); only the owning task writes it, and no DMA master reads it.

The worker runs on CPU0, the only target of the NPU interrupt (SPI 65, GIC
interrupt 97), so a completion notification wakes a task on the same core with
no inter-processor interrupt. Recognition and other clients on the A76 cores
reach it through the service queue and receive replies by task notification.
Cross-core reads are the thermal level (one acquire load per wake) and lease
status, which is checked once at admission: nothing revokes the DCDC2 token or
domain 4, because the power authority has no DCDC2 writer and never turns
domain 4 off.

**Completion is interrupt-driven.** The worker blocks on its notification with
the job deadline as the timeout. Hang detection therefore costs no polling; the
timeout path runs only when the deadline passes. Images without the interrupt
route keep the one-tick poll as their timeout path.

**Per-job register work.** The configuration readback (MMU page-directory
entry, MMU control and interrupt enable) runs once per job at the start
trigger. It is removed from each completion poll, because only the owner
writes those registers and the native adapter refuses every other writer.

**Barriers.** Each job keeps two ordering points: the barrier callback before
the start write at `0x3a4`, which publishes descriptors written to normal
memory (`DMB OSHST` once the weaker accessors are qualified), and the barrier
after the full-idle read, before output invalidation (`DMB OSHLD`). A `DSB SY`
precedes each timed delay in the reset sequence, because the delay assumes the
write reached the device. Moving the per-access `DSB SY` accessors waits on the
qualification that [docs/shared-helpers.md](../shared-helpers.md) requires.

## Idle gating closes the engine and its local clocks

When the worker has had no job for 100 ms, it closes the controller: interrupt
reporting and the MMU are disabled with readback, and full idle is verified.
It masks the interrupt without stopping its route. It then gates the NPU
locally: module gate off, bus gate off, then AHB, AXI and core resets
asserted. This is the
existing stop sequence from the pinned `npu_clk_uninit`, run as one call under
one register-lock acquisition. Its parent-clock checks are dropped from the
gate path, because closing a gate does not depend on the parent rate.

The next submit reverses the sequence: local release (six writes), then
controller open, which runs the two-pass soft reset (280 µs of delays) and the
bootstrap stream. Domain 4, the PLL and the rail stay on, so the shared SRAM
keeps its contents. The 100 ms threshold keeps the wake cost, about 0.5 ms
modeled, under 1% of the idle time it follows. The worker's existing idle wait
already has a timeout, so gating adds no timer.

Re-opening must not repeat admission work. The MMU tables, bootstrap stream,
models and coefficients have not changed, so a wake cleans no cache lines and
performs no translations.

## A second operating point at 800 mV

The pinned `sun60iw2p1-cpu-vf.dtsi` lists 852 MHz at 800 mV in its 26 MHz
columns for speed bins vf0200, vf0400, vf0500 and vf0501. These bins are the
SID codes `0x02`, `0x04`, `0x05` and `0x15` under the decoder that the CPU clock
driver already uses. Every other bin, and code 0, which an unprogrammed fuse
also reads, stays at 492 MHz. Points that need more than 800 mV are refused,
because DCDC2 also supplies eDP and HDMI and the power authority writes no
DCDC2 voltage.

The 852 MHz profile is a 1,704 MHz oscillator (multiplier 71 on the 24 MHz
reference) with P0 = 2 and module divider 1. The worker chooses the point at
each wake, while the module is gated: the bin's highest point when the thermal
level is Normal, 492 MHz under Throttle, and no work under Critical. A retune
runs only when the point changes, through the existing enabled-retune sequence
under the PLL lease that startup already holds, and rechecks the 13 PLL_NPU
consumer selectors once. The image's ceiling stays 492 MHz until a board run at
852 MHz matches the conformance outputs; raising it changes one constant.

## A hung job is cancelled and the engine restarted

When a job's deadline passes, the controller asserts cancel (`0x3a8` bit 3 for
at least 100 µs), then waits up to 1 ms for the cancel acknowledgement (bit 4)
and full idle (`0x7fffffff`). It then invalidates the job's output range, hands
the buffers back, reports the job as timed out and resubmits the bootstrap
stream. The pinned VIPLite driver recovers the same way when cancellation is
enabled (`vipdrv_task_recover`). Two consecutive recoveries without a completed
job are allowed; the third timeout retains. A completed job resets the count,
as the vendor's `recovery_times` does.

Failure classes (section 1.8):

- **Refused before device access** (invalid command, Critical thermal level,
  submit during a wake that has not finished): an error; the session stays
  usable.
- **Known terminal state** (timeout with cancel acknowledged and full idle):
  the recovery above; nothing is retained.
- **Unknown state** (no cancel acknowledgement or idle, an event bit other than
  completion, an MMIO failure, a third consecutive timeout, a failed CCU
  readback): the controller faults and retains the session memory and MMU
  tables, the clock and rail tokens and the domain row. It retains the register
  lock only if a CCU read-modify-write stopped midway. Unknown events stay in
  this class until board evidence shows what they mean.

## Catalogue models load from the FAT volume

A model in the build-time catalogue can come from a file on the card, by path,
in place of bytes linked into the image. The storage owner task reads the file
through `fatfs_port_load` into the model slot of the session allocation, which
is CPU-owned at that point. Session preparation then checks the size and the
catalogue SHA-256 in its existing single pass. The port's 4 KiB bounce buffer
gives one copy per model byte, and no copy or hash runs per inference. The
catalogue still fixes every admitted digest, so the file supplies bytes and
never identity. Arbitrary uploads belong to the session runtime.

## Budgets

Baselines marked *measured* come from the existing host models
(`checks/npu-platform.c`, `checks/npu-controller.c`) at commit `041c09a`,
compiled on the host with a counting wrapper that runs one poll per modeled
millisecond. The implementation adds these counters to the models and records
the before and after numbers. *Counted* baselines come from reading the code.

| Metric | Baseline | Target | Measured in |
| --- | --- | --- | --- |
| Platform polls from acquisition to READY | 17, measured | ≤ 3, one tick wait (2 ms settle) | `npu-platform.c` |
| Platform register reads and writes at startup | 51 and 12, measured | ≤ 36 and 6; PCK600 words move to the domain owner | `npu-platform.c` |
| Counter-clock reads at startup | 190, measured | ≤ 12, one per step | `npu-platform.c` |
| Register-lock acquisitions at startup | 15, measured | 2 | `npu-platform.c` |
| Startup PMIC traffic | 12 register reads, 6 bus-state reads and a 5,160 µs wait, counted | No bus access; one ledger status read | `npu-startup.c` |
| Bootstrap ticks to service activation, after PMIC READY | About 29, counted | ≤ 4 | `npu-startup.c` tick counter |
| Local gate | 6 polls, 102 reads, 5 writes, 238 clock reads, 6 lock acquisitions, measured | 1 call, ≤ 12 reads, 5 writes, ≤ 6 clock reads, 1 lock | `npu-platform.c` |
| Wake: gated to bootstrap complete | Not possible; stop is terminal | ≤ 0.6 ms modeled, no tick wait, no cache maintenance | `npu-platform.c` with `npu-controller.c` |
| Engine register accesses per job | 11 reads and 2 writes, plus 5 reads per extra poll, measured | 8 reads and 2 writes; none between start and interrupt | `npu-controller.c` |
| `DSB SY` per job in the native adapter | 17, counted | 14 now; 2 `DMB` after qualification | `npu-controller.c` access counts |
| Client wake-ups per job, convolution application | One per tick of execution plus 2 replies, counted | 3: submit, completion, reap | Job case added to `npu-startup.c` |
| Timeout to recovered READY | Not possible; timeout retains everything | ≤ 1.5 ms modeled; nothing retained | `npu-controller.c` |
| Operating-point change | Not available | ≤ 1 retune per wake, lock spin ≤ 0.1 ms, no DCDC2 write | `npu-pll.c` |
| Model bytes from card to session | Linked into the image, 1 copy and 1 SHA-256 pass, counted | 1 copy, 1 SHA-256 pass, none per inference | `npu-platform-models.c`, FAT image on a RAM device |

## Changes owned elsewhere

- **Power authority:** the domain-4 row accepts a request after the NPU's
  local clocks are on, and admits a quiescent domain 4 found on at boot.
- **Session runtime and service** (`npu_service.c`, `npu_session.c`):
  - block on the job deadline while a job runs with the interrupt bound;
  - call gate at the idle timeout and wake before a submit;
  - mask the interrupt for gating without the terminal stop;
  - accept a model already placed in its slot.
- **Native adapter** (`platform/a733/npu_native.c`): verify the register window
  once at initialization in place of an `AT` translation per access.
- **Memory publication:** a re-open with an unchanged mapping generation cleans
  and translates nothing. No reserved CPU or scheduler code changes.

## Not included, with reasons

- **Domain 4 power-off:** the shared SRAM alias (`0x74000`–`0xf3fff`) loses its
  contents, and the power authority keeps the domain on.
- **Points above 852 MHz and bins that need more than 800 mV:** they need
  DCDC2 writes on a rail shared with the display.
- **Disabling PLL_NPU at idle:** each wake would add a lock wait and a consumer
  recheck for a saving that local gating mostly provides.
- **Load-driven frequency changes:** the thermal level is the only consumer
  that needs a lower point.
- **CCU reset of an engine that is not idle:** no evidence shows that its bus
  transactions have drained, so this case retains.

## Board evidence that would confirm the design

- Register dumps after boot of PLL_NPU, CCU `0xb00` and `0xb04` and domain 4
  status, matching the cold-boot record.
- The SID speed-bin code of the attached board.
- Startup to READY and conformance outputs at 492 MHz, then at 852 MHz on a
  qualifying bin.
- One interrupt per job, and a job given a short deadline that is cancelled,
  recovered and followed by a correct inference.
- One hundred gate and wake cycles with unchanged outputs, and the measured
  wake time.
