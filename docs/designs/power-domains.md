# Design: one power-domain owner and one PMIC service task

Status: **implemented and host-modeled**; the section "Implemented state and
measurements" at the end records what exists, the measured values and the
differences from this design. [docs/power-domains.md](../power-domains.md)
describes the component. It completes the planned parts of
[power-authority.md](power-authority.md), whose rail ledger, state word and
lock-free PMIC time reads exist. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md). The
design's baselines were counted on commit `b3b1289` with the host models in
the budget table. One set of figures comes from the board: the service steps
of a read-only run on port 3-3, in section "Board result of the service
steps". They exceed the 110% wire-time target; this design sets a
provisional bound of 155% for a snapshot step at 400 kHz under that run's
conditions and records the provider follow-up that would bring the target
back.

## Ownership and data flow

A PCK600 power domain is one 4 KiB block at `0x07060000 + 0x1000 × domain`.
Six drivers write the same domain sequence: the GPU power controller
(domain 5), the DE_SYS provider of the framebuffer and G2D (domain 1), the NPU
platform (domain 4), the video platform (domains 2 and 3), HDMI and DisplayPort
(domain 10). The accelerator image links the GPU and NPU copies; the GPU
display image links the GPU, framebuffer and HDMI copies.

- **Domain owner.** `drivers/power/a733_power_domains.c` replaces
  `a733_de_sys_power.c` and becomes the only writer of domains 1 and 5;
  domain 4 joins in the NPU stage. A tree check fails if another file writes a
  block the owner holds.
- **Rows.** A consumer registers one static row before the scheduler starts:
  domain and rail token. This package's rows are the framebuffer and G2D
  (domain 1) and the GPU (domain 5). A domain is ON while an admitted row
  requests it. Only domain 5, whose one bus master is the GPU, is ever powered
  off.
- **Row request.** The consumer runs its existing authorization, then stores
  ON in its row with release order and notifies the service. The service
  admits the row, reads the domain-level registers, writes the delay row and
  the ON command if the domain is OFF, and polls status. It publishes the row
  state with release order. The consumer reads that state with one acquire
  load from its existing per-tick state machine: the GPU through
  `a733_gpu_power_poll`, the framebuffer and G2D through
  `a733_power_domain_state`. A blocking `a733_power_domain_wait` and its row
  semaphore are added together with their first blocking caller, the NPU
  startup.
- **Service task.** One task per image runs every PMIC transaction and every
  domain transition. The image's PMIC loop body becomes
  `a733_power_service_step()`, which advances all queued work and returns the
  ticks to block in `ulTaskNotifyTake(pdTRUE, ticks)`. It returns one tick
  while a slot is QUEUED or ACTIVE, the I2C mutex is held, a poll returned
  BUSY, or a domain transition is pending. Otherwise it returns the ticks to
  the session deadline, so a liveness check that expires with time still runs.
  Every park, stop or liveness-revocation path notifies the service.
- **PMIC request.** `a733_pmic_submit` keeps its interface and calls a kick
  hook that notifies the service, which removes the per-tick service poll.
  The hook has its own setter, because checks outside this package build
  `struct a733_pmic_ops` with positional initializers. Submitters keep polling
  `a733_pmic_result` once per tick: the USB2 supply snapshots in the GPU display
  and HDMI audio images, the CPU clock selector writes, PWM, PCIe, the radio,
  ISP0 and JPEG encode. No submitter blocks, so the design adds no slot
  semaphore.
- **Supply verification once per boot.** At provider start plus 5,160 µs (a
  1,000 µs enable delay and a 1,040 mV ramp at 250 µV/µs, board device tree
  lines 2123–2146), the service snapshots DCDC2 and DCDC4. A rail enabled at
  800 mV sets its verified bit in the state word; then the done bit is set.
  Row admission makes one acquire load of the state word and requires the
  supply's verified bit set and every `A733_POWER_PMIC_TRANSFER` bit (provider,
  clock and ledger fault) clear. DCDC2 supplies domains 1 and 4 (`npu-supply`,
  line 2932) and DCDC4 supplies domain 5 (`gpu-supply`, line 3421). A fault
  latched after boot therefore refuses later GPU and DE_SYS starts, as the
  per-start snapshots it replaces did. The GPU's PMIC consumer completes from
  that record with no bus transaction. The ledger accepts selector writes to
  DCDC3 and DCDC5 only; firmware writes to DCDC2 and DCDC4 after boot are
  excluded only by board evidence.
- **One PPU clock lease.** The owner holds one stable lease of the PPU clock
  (R_CCU `0x1ac` bit 0) through the GPU resource manager until power-off.

## Interrupts and polling

- **PCK600 acknowledgement is polled.** The pinned `pck-600@7060000` node has
  no `interrupts` property (`sun60iw2p1.dtsi` lines 745–753). The PPU has
  `PPU_IMR` (`0x030`) and `PPU_ISR` (`0x038`) in the PCK-600 TRM
  (101150_0005_01) register summary, but their GIC wiring is unknown. The
  service spins up to a provisional 50 µs after the command, then blocks one
  tick per retry until FAILED at 10 ms, the vendor bound
  (`pck600_domains.c` lines 112–121). Section 1.3 permits this tick wait for a
  device without qualified interrupt delivery.
- **PMIC byte phases spin within a bounded step.** The provider does not set
  the S_TWI0 rate. It adopts the bootloader's divisor and admits any rate from
  10 to 400 kHz (`route()` in `drivers/power/a733_pmic.c`). At startup it
  records the bit time from the admitted divisor and its parent clock as an
  immutable field. A snapshot is about 240 bit times and a selector write
  about 320: 0.6 ms and 0.8 ms at 400 kHz, 2.4 ms and 3.2 ms at 100 kHz. The
  service waits for each byte phase (9 bit times) by reading the raw counter
  without the provider lock, then calls `a733_pmic_poll` once. Spinning stops
  after 1 ms in one step, and the step then returns one tick. The interrupt,
  GIC SPI 203 (`sun60iw2p1.dtsi` line 1962), is unqualified.

## Memory placement and cache policy

Owner and service state is static in kernel `.bss`, Normal write-back memory:
the domain table and the rows in consumer storage. The PCK600, CCU and R_CCU
windows are Device-nGnRnE and are verified once with `a733_mapping_check.h`
by the native adapter's init, before the scheduler starts, so the result is
immutable before any task, including the framebuffer's output task that calls
`a733_power_domain_check`, reads it. No buffer is shared with a device, so no cache
maintenance occurs. Transitions occur a few times per boot, so no field gets
its own cache line.

## Synchronization across cores

- Each field has one writer. The consumer writes its row's request word; the
  service writes domain state, row state and verified bits. Readers make one
  acquire load.
- Only the service task touches the PCK600 blocks it owns, so a transition
  takes no owner lock and no platform register lock. The CCU and R_CCU
  accesses are reads.
- The provider's try-lock is held only inside one `a733_pmic_poll`, never
  across a byte wait. `a733_pmic_live` takes no lock: it compares the
  immutable lifetime and makes one acquire load of the state word, so it never
  returns BUSY. A fault it sees is latched by the service's next poll.
  `a733_pmic_result` and `a733_pmic_reap` keep the lock. The work query that
  the service makes reads the fault from the state word as well.
- The service task runs below the image's USB0 and audio tasks, on A55 core 0,
  created with `xTaskCreateStaticAffinitySet` (mask 1); single-core builds use
  `xTaskCreateStatic` at the same priority. A higher-priority task preempts it
  at any point.

| Image (PMIC loop) | PMIC task today | USB0 and audio today | Service task |
| --- | --- | --- | --- |
| GPU (`apps/gpu-render/main.c`) | 2, core 0 | `gpu-usb` 2, core 0 | 1, core 0 |
| Accelerator (`apps/npu-convolution/native.c`) | 4, core 0 | `accelerator-usb` 4, core 0 | 3, core 0, see below |
| GPU display (`apps/gpu-display/main.c`) | 4, any core | USB 3 | 2, core 0 |
| HDMI (`apps/hdmi/main.c`) | 4, any core | output and audio USB 4 | 3, core 0 |
| DisplayPort and G2D (`apps/board/main.c`) | 4, any core | `board-usb0` 4 | 3, core 0 |

In the accelerator image the service at priority 3 on core 0 ties with
`gpu-service` (priority 3, core 0) and preempts the `gpu-compute` client
(priority 2, core 0). While a PMIC request is active, each service step can
spin up to 1 ms on core 0, which delays the compute client by that much per
step and shares the core in turns with the GPU service worker.

## Register sequences and sources

Sources are pinned in `sources.lock.json`: `pck600_domains.c` (SHA-256
`2d9c2b60…`), `ccu-sun60iw2.c` (`1d32bb85…`), `ccu-sun60iw2-r.c` (`9285e1b3…`),
`sun60iw2p1.dtsi` (`1c7313f6…`), `sun60i-a733-orangepi-zero3w.dts`
(`31145367…`) and `vip_drv_device_platform.c` (`a47f85f2…`).

| Step | Registers | Source | Status |
| --- | --- | --- | --- |
| Domain-level checks | CCU `0x5c0` and `0x5e0` bits 5 and 11 (domain 1), 6 and 18 (domain 4), 7 and 16 (domain 5); R_CCU `0x1ac` bit 0 | `ccu-sun60iw2.c` lines 587–601 and 653–677; `ccu-sun60iw2-r.c` lines 144–146 | Host model |
| Adoption | Policy `+0x0` and status `+0x8` bits 3:0 both 0 or both 8 | `pck600_domains.c` lines 87–101; TRM `PPU_PWPR`, `PPU_PWSR` | Host model |
| Delay row, each read back | `+0x170` `0x1f1f1f`; `+0x174` `0x1f1f`; `+0xc00` `0x08080808`; `+0xc04` `0x0808`; `+0xc10` `0x8` | `pck600_domains.c` lines 126–142 and 547–569 | **Unproven.** The TRM names `0x170` and `0x174` (`PPU_DCDR0`, `PPU_DCDR1`); `0xc00`–`0xc10` are absent from it |
| ON or OFF | Policy bits 3:0 to 8 or 0, `DSB SY`, status polled for 10 ms | `pck600_domains.c` lines 104–121 | Host model |
| GPU before ON | MBUS gates set; `0x0b20` bit 31 and `0x0b24` bits 0 and 16 clear | `a733_gpu_power.c`, from `pvr_power.c` ([gpu-power.md](../gpu-power.md)) | Host model |
| Domain 5 OFF | The same three bits reread clear first; status must reach 0 | As above | Host model |
| NPU order | Resets and clocks enabled before the domain request | `vip_drv_device_platform.c` lines 419–428 | Host model |

The GPU keeps its preconditions in `a733_gpu_power.c`; its domain steps become
one row step. Barriers stay on the qualified `a733_mmio_*_dsb_sy_both`
accessors. After qualification, section 1.2 places one `DSB SY` after the
command write, because the spin bound is timed from the command reaching the
PPU.

## Interface for the NPU startup

1. `a733_power_domain_row_init(row, 4, owner, rail_token)` before the
   scheduler starts.
2. `a733_power_domain_check(4)`, which reads and never writes, before the
   first NPU reset or clock write.
3. Resets, module rate and clocks, then the NPU's existing `DSB SY`.
4. `a733_power_domain_request(row)` and `a733_power_domain_wait(row, deadline)`,
   followed by the NPU's existing 2,000 µs settle and readback. The wait takes
   a row semaphore that the service gives after it publishes the row state;
   both are added to the owner in the same change as this caller.

The NPU package removes its delay-row and ON writes, the PCK write offsets in
`a733_npu_platform_native.c`, its DCDC2 snapshot and the direct S_TWI0 path in
`a733_npu_startup.c`.

## Code reused and removed

- **Reused:** the PMIC provider and native adapter, rail tokens and borrow
  pins, the state word (gaining domain fault, verified and done bits),
  `a733_range.h` deadlines and the raw counter time read.
- **Removed:** the PCK600 sequences in `a733_gpu_power.c`,
  `include/a733_de_sys_power.h` with `a733_de_sys_power.c`, the unused
  domain-10 entry, the GPU's DCDC4 snapshot per start and the per-tick PMIC
  loop bodies of the images in the priority table.
- **Moved:** G2D (`apps/g2d/native.c`) and the framebuffer
  (`a733_display_framebuffer.c`) call `a733_power_domain_row_init`,
  `a733_power_domain_request` and `a733_power_domain_state`. Each keeps its
  `domain_authorize` function and runs it immediately before the row request.
  That function checks the designated task, whole-domain exclusion through the
  exclusive DE_SYS, IOMMU_SYS, VI, VE_DEC and VE_ENC clock tokens, the DCDC2
  rail token, the PCK lease and, for the framebuffer, media ownership and
  memory. Their read, write, enter and leave callbacks are deleted.

## Failure classes (section 1.8)

| Failure | Class | Retained |
| --- | --- | --- |
| Designated task, clock tokens, PCK lease, media or memory wrong | 1, in the consumer's `domain_authorize` before the request | Nothing; no request stored |
| Verified bit clear; provider, clock, ledger or domain fault bit set; bad rail token; domain-level read wrong; GPU gate or reset open | 1, at admission before any write | Nothing; row uncounted |
| Command and status disagree at adoption | 1; domain FAILED without a write | Nothing; later rows refused |
| Status not reached in 10 ms, or unexpected | 3 | Domain FAILED with fault bit, its rows and tokens, the PPU clock lease |
| PMIC transaction fault or deadline | 3, existing rule | I2C mutex, request, borrow, leases |
| Supply mismatch or timeout | 1 | Done bit with verified bit clear; rows refused |

## Budgets and measurement

The new `checks/power-domains-service.c` runs the service step against a
FreeRTOS shim that counts ticks, wakes and notifications, with one modeled core
and with two, and with S_TWI0 divisors for 400 kHz and 100 kHz.

| Metric | Baseline | Target | Measured by |
| --- | --- | --- | --- |
| Tick waits per snapshot; per selector write | 44 and 56 polls, up to one tick each | 400 kHz: snapshot 0, one step of at most 155% of its wire time (provisional, from one board run; 110% returns after the provider follow-up in section "Board result of the service steps"); selector write at most 1 tick wait in 2 steps; 100 kHz: ≤ 1 ms spin per step, 2 and 3 tick waits | `checks/a733-pmic-cost.c`; `power-domains-service.c`; bundle 6 on the board |
| BUSY results seen by other cores during one snapshot | `a733_pmic_live`: BUSY whenever it meets a poll | `a733_pmic_live` 0; `a733_pmic_result` and `a733_pmic_reap` ≤ 1 per caller | `power-domains-service.c`, concurrent caller on the second core |
| CPU time USB0 waits behind the service | PMIC task at or above USB0 priority in every image in the table | 0 on one core and on two | `power-domains-service.c` |
| Service wakes per second with no work | 1,000 | 0 until the session deadline; park, stop and revocation notify | `power-domains-service.c` |
| Row-state read by GPU platform poll, framebuffer and G2D | DE_SYS: platform register lock and 2 reads per poll | ≤ 100 instructions, 1 acquire load, no lock | `checks/rail-leases-cost.py` (Unicorn) |
| Ticks from row request to visible ON, acknowledgement within 50 µs | DE_SYS: at least 1; GPU: 0 | 1 for GPU, framebuffer and G2D (their next poll), once per start | `power-domains-service.c` |
| PMIC result polls by the submitters named under PMIC request | One per tick | One per tick; unchanged | Not changed |
| PMIC transactions per row request | GPU: 6 per start (one DCDC4 snapshot) | 0; 12 per boot | `checks/power-domains-pck.c` |
| Slow acknowledgement | One caller poll per tick | ≤ 1 tick per retry; FAILED by 10 ms plus 1 tick | `checks/power-domains-pck.c` |
| Register accesses per domain-5 ON | 14 (counted from the GPU steps) | 14, once per transition | `checks/a733-gpu-power.c` trace |
| Files that write the delay row | 6, plus 2 native allowlists | 5 after this package; 4 after the NPU stage | New `checks/power-domains-tree.py` |

The service check also covers the park paths. A park request in the GPU
display image completes only when the PMIC is idle and every domain is ON, OFF
or FAILED with no row transition pending. The GPU finite-return stop and the
GPU liveness park complete before their deadlines.

## Left out

- Domains 2, 3 and 10: their writers are outside this package, and no image
  links two writers of one of them. CLDO2 and CLDO5, which only domain 10
  needs, are not verified.
- Power-off of domains 1 and 4, which have several masters; no caller needs it.
- The S_TWI0 packet engine and both interrupts: delivery is unqualified, and the
  spin is capped at 1 ms per step at a few requests per second.
- Lighter barriers, which need the board qualification in
  [docs/shared-helpers.md](../shared-helpers.md).

## Board evidence required

- The inherited S_TWI0 divisor (`TWI+0x14`) and the bus rate it gives.
- PCK600 time to ON for domains 1, 4 and 5 and to OFF for domain 5, which
  replaces the 50 µs spin bound with the longest time plus 50%.
- GPU and framebuffer starts through the owner with the same register trace as
  the current drivers.
- Reads of `0xc00`–`0xc10` before and after the delay-row write.
- DCDC2 and DCDC4 selectors unchanged after PSCI CPU_ON and over a session; a
  bus capture with no second S_TWI0 master.
- USB0 and audio latency with PMIC requests running, SMP and single core.
- A trace of the status word read back to back during each transition. The
  owner fails on any status other than the starting or the target value,
  while `pck600_domains.c` waits only for the target value.
- The duration of each service step during a DCDC3 selector write. At
  400 kHz the host model at the board's fitted register-access cost predicts
  two steps with one tick wait. DCDC2 and DCDC4 snapshot steps were measured
  (section "Board result of the service steps").
- The cost of a provider register access, divided into the bus access with
  its barriers, the time reads and the provider's software, from the next
  bundle 6; then the step durations again after the provider follow-up.
- Qualified delivery of the S_TWI0 interrupt, GIC SPI 203, for each bus
  phase. It is the condition under which the service blocks on the
  interrupt instead of spinning for each bus phase.

## Changes outside this package

One patch for the primary agent holds these edits; the FreeRTOS configuration
does not change.

- The PMIC loops in the priority table call the service step and create the
  service task at the listed priority and core.
- The GPU display park predicate (`apps/gpu-display/display.c`) also requires
  every domain in ON, OFF or FAILED with no row transition pending. The park
  request there, the GPU finite-return stop (`apps/gpu-finite/finite.c`) and
  the GPU liveness park (`apps/gpu-render/main.c`) notify the service.
- G2D and the framebuffer move to the row interface as listed under
  **Moved**.
- The GPU and NPU native adapters drop their PCK write offsets.

## Implemented state and measurements

Implemented:

- the owner, `drivers/power/a733_power_domains.c`, with rows, supply
  verification, domains 1, 4 and 5, the read-only check and the service step;
  its native adapter `drivers/power/a733_power_domains_native.c`;
- the row header `include/a733_power_domain_row.h`, whose request and state
  calls are inline, so that targets that link only the GPU controller or the
  supply consumer do not need the owner;
- in the PMIC provider: the service binding with its notification hook, the
  work query that reports the pending bus phase and its expected duration,
  the bit time recorded from the admitted divisor, a lock-free
  `a733_pmic_live`, and a count of queued requests;
- `a733_rail_pin` and `a733_rail_unpin` in the rail ledger;
- the GPU controller's domain steps as one row step, with its preconditions
  kept, and the GPU supply consumer completing from the boot record;
- the deletion of `a733_de_sys_power.c` and its header;
- outside this package, in one patch for the primary agent: the five image
  loops, the framebuffer and G2D rows, the GPU display park predicate, the
  stop notifications, the GPU native adapter without PCK offsets, the build
  lists, and the checks that model the GPU controller.

Baselines come from the merge base `dfa5174b2e09ac0dc27df3809183189f7ad00c41`,
extracted with `git archive`, configured with the package's build script and
built for all 68 central targets. The PMIC figures come from
`checks/a733-pmic-cost.c` and `checks/a733-pmic-polls.c` at the merge base, the
DE_SYS and GPU figures from small harnesses over the merge-base drivers, and
the instruction counts from `checks/rail-leases-cost.py --root`. The new
figures come from `checks/power-domains-service.c` (400 kHz: divisor `0x28`;
100 kHz: `0x2a`; a byte with acknowledgement takes nine bit times and a START,
repeated START or STOP two; a time read costs 40 ns and a register access
200 ns), `checks/power-domains-pck.c`, `checks/rail-leases-cost.py` and
`checks/power-domains-tree.py`. Every figure is modeled.

| Metric | Merge base | Now | Target |
| --- | --- | --- | --- |
| Tick waits per snapshot; per selector write | 44 and 56 polls, up to one tick each | Model at 200 ns per register access, 400 kHz: 0 and 0, one step of 687 µs and 885 µs (wire time 642 µs and 828 µs, so 107%). Model at the board's fitted 1,300 ns: snapshot 0, one step of 915 µs (145% of the 630 µs bit-time wire time); selector write 1, in 2 steps. Board, snapshot: one step of 917 µs, 146% of 630 µs (median of 34). 100 kHz model: 2 and 3, at most 1,000 µs spin per step | 400 kHz: snapshot 0, one step of at most 155% of wire time (provisional); selector write at most 1 in 2 steps; 100 kHz: ≤ 1 ms per step, 2 and 3 |
| BUSY results seen by another caller during one snapshot | `a733_pmic_live`: 211 per snapshot | `a733_pmic_live` 0; `a733_pmic_result` at most 1 with two cores, 0 with one; `a733_pmic_reap` 0 | `a733_pmic_live` 0; result and reap ≤ 1 per caller |
| CPU time USB0 waits behind the service | PMIC task at or above USB0 priority in all five images | 0: the service runs below USB0 in all five images, and the owner's code has no critical section or scheduler suspension; the provider's lock waits at activation are one-tick blocks, which USB0 preempts | 0 on one core and on two |
| Service wakes per second with no work | 1,000 | 0; 2 in the resident board image, which caps the block at 500 ms; one wake per park, stop or row notification | 0 until the session deadline |
| Row-state read by GPU platform poll, framebuffer and G2D | DE_SYS poll while waiting: 344 instructions, a lock entry and 2 register reads | 12 instructions, 1 acquire load, no exclusive access | ≤ 100 instructions, 1 acquire load, no lock |
| Ticks from row request to visible ON, acknowledgement within 50 µs | DE_SYS: 1; GPU: 0 | 1 for the GPU and the framebuffer, at their next poll; G2D reads the same row the same way | 1 for GPU, framebuffer and G2D |
| PMIC result polls by the other submitters | One per tick | One per tick | Unchanged |
| PMIC transactions per row request | GPU: 6 per start (one DCDC4 snapshot) | 0; 12 per boot | 0; 12 per boot |
| Slow acknowledgement | DE_SYS: one caller poll per tick; GPU: a 50 µs spin per poll | 54 µs spin after the command, then one status read per one-tick step; FAILED 10,092 µs after the command, after 10 retries | ≤ 1 tick per retry; FAILED by 10 ms plus 1 tick |
| Register accesses per domain-5 ON | 13 PPU operations by the GPU (14 with the native read-modify-write of the policy word) | 14 PPU accesses by the owner (2 adoption reads, 5 delay-row writes and 5 readbacks, the command, 1 status read), plus 5 domain-level reads | 14, once per transition |
| Files that write the delay row | 6, plus 2 native allowlists | 5, plus 1 native allowlist (the NPU's) | 5 after this package; 4 after the NPU stage |

At 400 kHz and 200 ns per register access each request takes one step of
107% of the model's wire time: the step adds polls and time reads to the bus
phases. The design's first estimate of 240 and 320 bit times (0.6 ms and
0.8 ms) is below the model's wire time, which counts nine bit times per byte
and two per condition, so the target is stated relative to wire time. The
board's register accesses cost more, and section "Board result of the service
steps" sets a provisional bound of 155% for a snapshot step;
`checks/power-domains-service.c` checks both access costs. Supply verification
ends 8.4 ms after the service's first step at 400 kHz and 14.9 ms at 100 kHz
in the model.

### Board result of the service steps

Conditions: port 3-3 on 2026-10-24, bundle 6 of
[board-bundles-next.md](board-bundles-next.md), raw log
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/bb6-run/kernel.log`.
The single-core returning image runs the provider, the owner and the service
task with the native adapters, whose register accessors issue `DSB SY` before
and after each access. Core 0 ran at 494 MHz (CPU-L, measured by bundle 4 in
the same lifetime). S_TWI0 had the inherited divisor `0x28`: 400 kHz, a bit
time of 2,500 ns. A TWI observer in the provider's accessors counted the bus
phases, and the service task recorded each step's counter span and the
scheduler ticks inside it.

The run recorded 34 steps: two supply-verification snapshots, then 32
client snapshots of DCDC2 and DCDC4 in turn. Each step completed one
snapshot of six read transactions: 12 START or repeated START conditions,
24 bytes and 6 STOPs, 252 bit times, so a wire time of 630 µs. Every
percentage below is relative to that bit-time wire time; the model's own
bytes take 23 µs, so its earlier figure of 107% is relative to 642 µs.

| Metric | Board | Model, 200 ns per access | Model, 1,300 ns per access |
| --- | --- | --- | --- |
| Snapshot step | 917 µs (22,019 counter ticks, median); 916 to 924 µs in steps 2 to 34; 941 µs in step 1 | 687 µs | 915 µs |
| Against 630 µs of wire time | 146% (median); 149.4% in step 1 | 109% | 145% |
| Steps and tick waits per snapshot | 1 and 0 | 1 and 0 | 1 and 0 |

The 287 µs per step beyond the wire time divide as follows.

- **Tick interrupts: none measurable.** Thirty of steps 2 to 34 held one
  scheduler tick and three held none. Their medians, 22,019 and 22,025
  counter ticks, differ by 0.25 µs, and the steps with a tick span 916 to
  919 µs.
- **The service's polling: one extra read per phase, no extra rounds.** In
  steps 2 to 34, each of the 36 phases that wait for the interrupt flag (12
  conditions and 24 bytes) read the control register exactly twice. The
  first read, made right after the phase starts, found the flag clear; the
  second, after the spin, found it set. In step 1, one phase found the flag
  already set at its first read. The spin therefore never ended before the
  controller did, and the service never polled a phase a third time.
- **Register accesses: nearly all of it.** A snapshot makes 211 register
  accesses, 151 reads and 60 writes, counted in the host model; the board's
  3,095 control-register reads, 91 per step, match the model's count. The
  host model reproduces the board's median step when an access costs
  1,300 ns instead of 200 ns.

The 1,300 ns is a fit; the run did not measure how it divides. It covers the
bus access with its two `DSB SY`, and the provider's software around each
access. The disassembled read path of the bundle 6 image is about 220
instructions. It holds two deadline reads, each with an `ISB` and a 64-bit
division of the counter; a task check that masks IRQs and calls two FreeRTOS
functions; the address allowlist; and bundle 6's TWI observer. The owner's
reads in the same run, which use a leaner accessor with the bundle's
recording, are 0.42 to 1.17 µs apart. They are 0.42 to 0.54 µs apart
between CCU reads and 0.63 to 0.96 µs apart (median 0.71 µs) between
consecutive reads of R_CCU and the PPU block, in the `0x07000000` region
that also holds S_TWI0. An
estimated half of the fitted cost is therefore provider software. Section
1.1 and items 4 and 5 of section 2 of
[performance-architecture.md](../performance-architecture.md) prescribe
removing it: one deadline check per bus phase instead of two per access, and
ownership checked once at `enter()` with the per-access revocation load
kept. Production images have no TWI observer, so their accesses cost less
than the fitted figure.

The accesses lengthen the step because of where the service starts each
spin. `pmic()` in `drivers/power/a733_power_domains.c` starts each phase's
spin from a time read made after the poll that started the phase has
returned. The accesses of that poll after the phase-start write, and the
accesses of the next poll before its phase-start write, add to the wire
time. In a copy of the host model, with the spin timed from the provider's timestamp of the
phase-start write and no register access changed, the step takes 856 µs at
1,300 ns per access (136%). With the spin so timed and an access cost of
900 ns, the step takes 776 µs (123%), and a DCDC3 selector write completes
in one step of 998 µs with no tick wait. That timing needs a minimum re-spin
after a poll that finds the flag clear, and one board re-measurement. It is
recorded here as the provider follow-up; this package does not change the
provider. With the current spin timing, a step within 110% would need about
0.3 µs per access in the model. The owner's fastest measured interval,
0.42 µs between CCU reads with recording, already exceeds that, so the
follow-up is needed before the 110% target can return.

The next bundle 6 times S_TWI0 reads with and without barriers and the
provider's time reads, which gives the split, and counts its steps against
the bound below (section "Records of bundles 6, 7 and 8" of
[board-bundles-next.md](board-bundles-next.md)).

**Provisional bound.** At 400 kHz a snapshot completes in one step of at most
155% of its wire time. The bound holds only for this run's conditions: CPU-L
at 494 MHz, one core, the bundle 6 trace hook active, port 3-3 and one
lifetime. Its headroom is judged against step 1, the slowest step at
149.4%, which is 5.6 points below the bound. A step above about 159% (1,000 µs for a 630 µs
snapshot) would reach the service's 1 ms spin limit and split the snapshot
across a tick wait. The bound is replaced after the access-cost
measurement and the provider follow-up above. At 1,300 ns per access, a
selector write, wire time 828 µs in the model, takes two steps with one tick
wait. Until a DCDC3 write is measured on the board or the follow-up lands,
its bound is at most one tick wait in two steps.

Differences from the design, each with its reason:

- **No separate PPU clock lease.** The owner reads R_CCU `0x1ac` bit 0 at
  every admission and transition. Every consumer already holds a stable PCK
  lease of the GPU resource manager before it requests its row, and the
  manager never disables a shared gate it enabled ("shared gate enablement
  persists for the resource manager's OS lifetime"). A lease for the owner
  would need a client slot in every image that no session reserves.
- **Bus-phase spin length.** The service spins for the pending phase's
  expected duration from the provider (two bit times for START, repeated
  START and STOP, nine for a byte) instead of nine bit times for every phase.
  With nine bit times for every phase, a 400 kHz snapshot took three steps in
  the model, because a transaction has three conditions and four bytes.
- **Fail functions do not notify.** The design asks every liveness-revocation
  path to notify the service. `gpu_application_fail` and the board's
  `board_fail` run from interrupt and fault paths, where a FreeRTOS
  notification is not safe. An idle service holds no bus or lock, and a
  pending transfer keeps it on one-tick steps, so it observes a revocation
  within one tick or at its next timed wake, at the latest the session
  deadline. Park and stop requests from task context notify.
- **Idle cap in the resident board image.** The integrated OS image's
  watchdog requires the board PMIC service to report progress within 1 s, so
  that image's service blocks at most 500 ms when idle.
- **The GPU row is registered by the GPU platform.**
  `a733_gpu_platform_bind_pmic` registers the GPU controller's row on the
  power-domain owner bound to the PMIC provider, with the platform's owner and
  its DCDC4 consumer token. `a733_power_domain_row_init` is inline in
  `a733_power_domains.h`, so targets that link the GPU platform without the
  owner still link. The GPU controller's operations, the GPU platform's
  operations and its native adapter have no plain `write` and no PCK
  register bank: every GPU register write is a masked update of the CCU.
- **Fault retention through the work query.** A fault that a client latches
  while the service holds the I2C mutex sets the state word; the work query
  then reports FAILED with RETAIN, and the service polls once more so that the
  provider marks its mutex retained.
- **Mapping check at init.** The native adapter checks the five windows once
  in its init before the scheduler, so a concurrent first access from the
  framebuffer's output task cannot see a check in progress.
- **Verification budget and cleanup.** The 100 ms budget starts when the
  provider is READY after the settle time; a certain failed result is reaped
  and the token released; only the DCDC4 reading is kept, for the GPU supply
  consumer, which reads it after its verified bit.
- **Domain-4 rows are accepted and checked.** The owner accepts domain-4 rows
  for the NPU startup's next stage, and `checks/power-domains-tree.py` fails
  on a domain-4 row outside `checks/` while the NPU platform still writes the
  domain.
- **Merge-base branches in two checks.** `checks/rail-leases-cost-row.c` and
  `checks/power-domains-tree.py` keep a branch for a tree with the DE_SYS
  provider; it serves only their `--root` measurement of the merge base,
  whose figures the table above records.
- **Verification only where a row exists, and a released token.** The rail
  ledger has 32 holder slots for the 31 holders the images create, so a
  permanent verification token could leave a later runtime holder without a
  slot. The service releases the token after a successful verification and
  keeps it only after a failure, when an uncertain request may still borrow
  it. An image that registers no row, such as the integrated OS image,
  verifies nothing and makes no PMIC read at boot.
- **Domain 1 and 4 OFF requests are refused** at the request call, since no
  caller needs those domains off.
- **The GPU check models the owner.** `checks/a733-gpu-power.c` keeps its
  GPU-local cases with a stand-in that answers the row; the real owner's
  trace for `checks/a733-gpu-power-reference.py` comes from
  `checks/power-domains-pck.c`.

Not implemented, with the dependency that blocks each:

- The NPU on domain 4, the blocking `a733_power_domain_wait` and its row
  semaphore: they land with the NPU startup's next stage
  ([docs/power-domains.md](../power-domains.md) lists the NPU owner's
  changes).
- The deinterlace consumer of domain 1 still requires the domain already ON
  and reads its policy and status; it moves onto a row in its own package.
