# Design: one power-domain owner and one PMIC service task

Status: **implemented and host-modeled**; the section "Implemented state and
measurements" at the end records what exists, the measured values and the
differences from this design. [docs/power-domains.md](../power-domains.md)
describes the component. It completes the planned parts of
[power-authority.md](power-authority.md), whose rail ledger, state word and
lock-free PMIC time reads exist. Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md). The
design's baselines were counted on commit `b3b1289` with the host models in
the budget table; no figure comes from the board.

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
windows are Device-nGnRnE and are verified once at service start with
`a733_mapping_check.h`. No buffer is shared with a device, so no cache
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
  immutable lifetime and makes acquire loads of the stage, the failed flag and
  the state word, so it never returns BUSY. A fault it sees is latched by the
  service's next poll. `a733_pmic_result` and `a733_pmic_reap` keep the lock.
- The service task runs below the image's USB0 and audio tasks, on A55 core 0,
  created with `xTaskCreateStaticAffinitySet` (mask 1); single-core builds use
  `xTaskCreateStatic` at the same priority. A higher-priority task preempts it
  at any point.

| Image (PMIC loop) | PMIC task today | USB0 and audio today | Service task |
| --- | --- | --- | --- |
| GPU (`apps/gpu-render/main.c`) | 2, core 0 | `gpu-usb` 2, core 0 | 1, core 0 |
| Accelerator (`apps/npu-convolution/native.c`) | 4, core 0 | `accelerator-usb` 4, core 0 | 3, core 0 |
| GPU display (`apps/gpu-display/main.c`) | 4, any core | USB 3 | 2, core 0 |
| HDMI (`apps/hdmi/main.c`) | 4, any core | output and audio USB 4 | 3, core 0 |
| DisplayPort and G2D (`apps/board/main.c`) | 4, any core | `board-usb0` 4 | 3, core 0 |

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
| Tick waits per snapshot; per selector write | 44 and 56 polls, up to one tick each | 400 kHz: 0, one step of ≤ 0.6 ms and ≤ 0.8 ms; 100 kHz: ≤ 1 ms spin per step, 2 and 3 tick waits | `checks/a733-pmic-cost.c`; `power-domains-service.c` |
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
| Tick waits per snapshot; per selector write | 44 and 56 polls, up to one tick each | 400 kHz: 0 and 0, one step of 687 µs and 885 µs (wire time 642 µs and 828 µs); 100 kHz: 2 and 3, at most 1,000 µs spin per step | 400 kHz: 0, one step of ≤ 0.6 ms and ≤ 0.8 ms; 100 kHz: ≤ 1 ms per step, 2 and 3 |
| BUSY results seen by another caller during one snapshot | `a733_pmic_live`: 211 per snapshot | `a733_pmic_live` 0; `a733_pmic_result` at most 1 with two cores, 0 with one; `a733_pmic_reap` 0 | `a733_pmic_live` 0; result and reap ≤ 1 per caller |
| CPU time USB0 waits behind the service | PMIC task at or above USB0 priority in all five images | 0: the service runs below USB0 in all five images, and the step has no critical section, scheduler suspension or blocking call | 0 on one core and on two |
| Service wakes per second with no work | 1,000 | 0; 2 in the resident board image, which caps the block at 500 ms; one wake per park, stop or row notification | 0 until the session deadline |
| Row-state read by GPU platform poll, framebuffer and G2D | DE_SYS poll while waiting: 344 instructions, a lock entry and 2 register reads | 12 instructions, 1 acquire load, no exclusive access | ≤ 100 instructions, 1 acquire load, no lock |
| Ticks from row request to visible ON, acknowledgement within 50 µs | DE_SYS: 1; GPU: 0 | 1 for the GPU and the framebuffer, at their next poll; G2D reads the same row the same way | 1 for GPU, framebuffer and G2D |
| PMIC result polls by the other submitters | One per tick | One per tick | Unchanged |
| PMIC transactions per row request | GPU: 6 per start (one DCDC4 snapshot) | 0; 12 per boot | 0; 12 per boot |
| Slow acknowledgement | DE_SYS: one caller poll per tick; GPU: a 50 µs spin per poll | 54 µs spin after the command, then one status read per one-tick step; FAILED 10,092 µs after the command, after 10 retries | ≤ 1 tick per retry; FAILED by 10 ms plus 1 tick |
| Register accesses per domain-5 ON | 13 PPU operations by the GPU (14 with the native read-modify-write of the policy word) | 14 PPU accesses by the owner (2 adoption reads, 5 delay-row writes and 5 readbacks, the command, 1 status read), plus 5 domain-level reads | 14, once per transition |
| Files that write the delay row | 6, plus 2 native allowlists | 5, plus 1 native allowlist (the NPU's) | 5 after this package; 4 after the NPU stage |

The 400 kHz step durations exceed the design's 0.6 ms and 0.8 ms by the
difference between the model's wire time and the design's estimate of 240 and
320 bit times, plus 7% for polls and time reads. Supply verification ends
8.4 ms after the service's first step at 400 kHz and 14.9 ms at 100 kHz.

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
- **The GPU row is registered by the image.** The images call
  `a733_power_domain_row_init` on the GPU controller's row after
  `a733_gpu_platform_bind_pmic`, with the platform's DCDC4 consumer token,
  because the GPU platform file is outside this package.
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
- `checks/g2d-power-native.py` emulates the merge-base DE_SYS provider inside
  a built G2D image; it needs its owner to emulate the row instead.
